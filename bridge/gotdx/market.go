package main

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"time"

	"github.com/bensema/gotdx"
	"github.com/bensema/gotdx/types"
)

func changeFields(v map[string]any) (*float64, *float64) {
	price, pre := field(v, "close"), field(v, "pre_close")
	if price == nil || pre == nil || *pre <= 0 {
		return nil, nil
	}
	return number(*price - *pre), number((*price - *pre) / (*pre) * 100)
}

type rankItem struct {
	Code          string   `json:"code"`
	Name          string   `json:"name"`
	DetailFs      string   `json:"detailFs"`
	WatchCode     string   `json:"watchCode"`
	Price         *float64 `json:"price"`
	Pct           *float64 `json:"pct"`
	Change        *float64 `json:"change"`
	MainNetInflow *float64 `json:"mainNetInflow"`
	YearPct       *float64 `json:"yearPct"`
	MarketCap     *float64 `json:"marketCap"`
	Turnover      *float64 `json:"turnover"`
}

func rankFields() [20]byte {
	return gotdx.MACFieldBitmap(gotdx.MACFieldClose, gotdx.MACFieldPreClose, gotdx.MACFieldMainNetAmount, gotdx.MACFieldYTDPct, gotdx.MACFieldTotalMarketCapAb, gotdx.MACFieldTurnover)
}
func hotBoards(req request) (any, error) {
	kind := types.BoardTypeHY
	if req.Kind == "concept" {
		kind = types.BoardTypeGN
	}
	list, err := boards(kind)
	if err != nil {
		return nil, err
	}
	if len(list) == 0 {
		return nil, fmt.Errorf("TDX returned no boards")
	}
	// Fetch every board before sorting; sorting only a first page would omit leaders.
	items := []rankItem{}
	for start := 0; start < len(list); start += 80 {
		end := min(start+80, len(list))
		markets := []uint8{}
		codes := []string{}
		for _, b := range list[start:end] {
			markets = append(markets, uint8(b.Market))
			codes = append(codes, b.Code)
		}
		reply, err := client(false).MACSymbolQuotes(markets, codes, rankFields())
		if err != nil {
			reset(false)
			return nil, err
		}
		if len(reply.Stocks) != end-start {
			return nil, fmt.Errorf("TDX returned an incomplete board ranking")
		}
		for _, b := range reply.Stocks {
			code := canonical(b.Market, b.Symbol, false)
			change, pct := changeFields(b.Values)
			items = append(items, rankItem{Code: code, Name: b.Name, DetailFs: code, WatchCode: code, Price: field(b.Values, "close"), Pct: pct, Change: change, MainNetInflow: field(b.Values, "main_net_amount"), YearPct: field(b.Values, "ytd_pct")})
		}
	}
	metric := func(i rankItem) *float64 {
		if req.SortField == "pct" {
			return i.Pct
		}
		return i.MainNetInflow
	}
	sort.SliceStable(items, func(a, b int) bool {
		x, y := metric(items[a]), metric(items[b])
		if x == nil {
			return false
		}
		if y == nil {
			return true
		}
		if req.SortOrder == "asc" {
			return *x < *y
		}
		return *x > *y
	})
	limit := int(req.Count)
	if limit < 1 {
		limit = 10
	}
	if len(items) > limit {
		items = items[:limit]
	}
	return map[string]any{"items": items}, nil
}
func members(req request) (any, error) {
	code := strings.TrimPrefix(req.Code, "b:")
	s, err := resolveNamed(code, req.Name)
	if err != nil {
		return nil, err
	}
	if s.extended || !strings.HasPrefix(s.code, "88") {
		return nil, fmt.Errorf("TDX board membership requires a TDX board code")
	}
	count := uint32(req.Count)
	if count == 0 {
		count = 300
	}
	if count > 10000 {
		return nil, fmt.Errorf("member count exceeds 10000")
	}
	reply, err := client(false).MACBoardMembersQuotesDynamic(s.code, count, uint16(gotdx.MACSortChangePct), 1, rankFields())
	if err != nil {
		reset(false)
		return nil, err
	}
	items := []rankItem{}
	for _, b := range reply.Stocks {
		code := fmt.Sprintf("%d.%s", b.Market, b.Symbol)
		change, pct := changeFields(b.Values)
		items = append(items, rankItem{Code: code, WatchCode: code, Name: b.Name, Price: field(b.Values, "close"), Change: change, Pct: pct, MarketCap: field(b.Values, "total_market_cap_ab"), Turnover: field(b.Values, "turnover"), YearPct: field(b.Values, "ytd_pct")})
	}
	return map[string]any{"items": items}, nil
}

var breadthCache map[string]any
var breadthExpires time.Time
var breadthHistory []map[string]any

func marketBreadth(req request) (any, error) {
	if !req.Force && breadthCache != nil && time.Now().Before(breadthExpires) {
		return breadthCache, nil
	}
	// 880001 covers the complete A-share market. These fields are counts for
	// boards; on ordinary equities the same field bits encode orderbook sizes.
	bitmap := gotdx.MACFieldBitmap(gotdx.MACFieldClose, gotdx.MACFieldUpCount, gotdx.MACFieldDownCount, gotdx.MACFieldLimitUpCount, gotdx.MACFieldLimitDownCount, gotdx.MACFieldServerUpdateDate, gotdx.MACFieldServerUpdateTime)
	reply, err := client(false).MACSymbolQuotes([]uint8{1}, []string{"880001"}, bitmap)
	if err != nil {
		reset(false)
		return nil, err
	}
	if len(reply.Stocks) != 1 {
		return nil, fmt.Errorf("TDX returned no market breadth")
	}
	v := reply.Stocks[0].Values
	up, down, limitUp, limitDown := field(v, "up_count"), field(v, "down_count"), field(v, "limit_up_count"), field(v, "limit_down_count")
	if up == nil || down == nil || limitUp == nil || limitDown == nil {
		return nil, fmt.Errorf("TDX market summary lacks required fields")
	}
	// The membership total gives the universe size; no equity quotes are summed
	// on top of index turnover, which would double-count the same trades.
	count, err := client(false).GetMACBoardMembersQuotesDynamic("880001", 0, 0, 1, 1, gotdx.MACFieldBitmap(gotdx.MACFieldClose))
	if err != nil {
		return nil, err
	}
	flat := int(count.Total) - int(*up) - int(*down)
	if flat < 0 {
		return nil, fmt.Errorf("TDX market breadth counts exceed membership total")
	}
	distribution, err := breadthDistribution()
	if err != nil {
		return nil, err
	}
	updateDate, updateTime := field(v, "server_update_date"), field(v, "server_update_time")
	if updateDate == nil || updateTime == nil {
		return nil, fmt.Errorf("TDX market summary lacks trading timestamp")
	}
	updated, err := time.ParseInLocation("20060102 150405", fmt.Sprintf("%08d %06d", int(*updateDate), int(*updateTime)), chinaZone())
	if err != nil {
		return nil, fmt.Errorf("TDX returned invalid trading timestamp")
	}
	now := updated.UnixMilli()
	point := map[string]any{"timestampMs": now, "riseCount": int(*up), "fallCount": int(*down), "limitUpCount": int(*limitUp), "limitDownCount": int(*limitDown)}
	day := updated.Format("2006-01-02")
	if len(breadthHistory) > 0 && breadthHistory[0]["date"] != day {
		breadthHistory = nil
	}
	point["date"] = day
	if len(breadthHistory) > 0 && breadthHistory[len(breadthHistory)-1]["timestampMs"] == now {
		breadthHistory[len(breadthHistory)-1] = point
	} else {
		breadthHistory = append(breadthHistory, point)
	}
	if len(breadthHistory) > 500 {
		breadthHistory = breadthHistory[len(breadthHistory)-500:]
	}
	result := map[string]any{"upCount": int(*up), "downCount": int(*down), "flatCount": flat, "limitUpCount": int(*limitUp), "limitDownCount": int(*limitDown), "lastUpdatedAtMs": now, "distribution": distribution, "overviewTimeline": append([]map[string]any(nil), breadthHistory...)}
	breadthCache = result
	breadthExpires = time.Now().Add(30 * time.Second)
	return result, nil
}
func chinaZone() *time.Location {
	z, err := time.LoadLocation("Asia/Shanghai")
	if err != nil {
		return time.FixedZone("CST", 8*3600)
	}
	return z
}
func breadthDistribution() ([]map[string]any, error) {
	bitmap := gotdx.MACFieldBitmap(gotdx.MACFieldClose, gotdx.MACFieldPreClose)
	reply, err := client(false).MACBoardMembersQuotesDynamic("880001", 10000, uint16(gotdx.MACSortCode), 1, bitmap)
	if err != nil {
		reset(false)
		return nil, err
	}
	if len(reply.Stocks) != int(reply.Total) {
		return nil, fmt.Errorf("TDX breadth distribution is incomplete")
	}
	bins := make([]int, 11)
	for _, q := range reply.Stocks {
		_, pct := changeFields(q.Values)
		if pct == nil {
			continue
		}
		bins[distributionBin(*pct)]++
	}
	labels := []string{"<-9%", "-9~-7%", "-7~-5%", "-5~-3%", "-3~0%", "0%", "0~3%", "3~5%", "5~7%", "7~9%", ">9%"}
	result := []map[string]any{}
	for i, n := range bins {
		result = append(result, map[string]any{"bucket": labels[i], "value": n})
	}
	return result, nil
}
func distributionBin(p float64) int {
	switch {
	case p <= -9:
		return 0
	case p <= -7:
		return 1
	case p <= -5:
		return 2
	case p <= -3:
		return 3
	case p < 0:
		return 4
	case math.Abs(p) < 0.000001:
		return 5
	case p < 3:
		return 6
	case p < 5:
		return 7
	case p < 7:
		return 8
	case p < 9:
		return 9
	default:
		return 10
	}
}
