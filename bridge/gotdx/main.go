// Package main exposes a versioned JSON C ABI. No Go pointers cross the ABI.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"math"
	"reflect"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"

	"github.com/bensema/gotdx"
	"github.com/bensema/gotdx/proto"
	"github.com/bensema/gotdx/types"
)

type stock struct {
	Code string `json:"code"`
	Name string `json:"name"`
}
type request struct {
	Operation string  `json:"operation"`
	Stocks    []stock `json:"stocks"`
	Code      string  `json:"code"`
	Period    uint16  `json:"period"`
	Start     uint16  `json:"start"`
	Count     uint16  `json:"count"`
	// Kept at the application boundary for future authenticated protocols.
	// gotdx currently has no account authentication API; never claim login.
	User         json.RawMessage `json:"user"`
	CalendarPath string          `json:"calendarPath"`
	Proxy        proxyConfig     `json:"proxy"`
	Keyword      string          `json:"keyword"`
	Name         string          `json:"name"`
	Kind         string          `json:"kind"`
	SortField    string          `json:"sortField"`
	SortOrder    string          `json:"sortOrder"`
	// PushIntervalMs is how long the push channel waits for a server-sent frame
	// before asking again, so the price cadence follows the app's polling setting.
	PushIntervalMs int `json:"pushIntervalMs"`
	// IncludeBreadth adds the market summary board to the quote batch, so the
	// floating window's breadth row costs nothing beyond one more symbol.
	IncludeBreadth bool         `json:"includeBreadth"`
	Force          bool         `json:"force"`
	Servers        serverConfig `json:"servers"`
}
type symbol struct {
	market   uint8
	code     string
	extended bool
}
type quote struct {
	Code      string   `json:"code"`
	Name      string   `json:"name"`
	Price     *float64 `json:"price"`
	Pct       *float64 `json:"pct"`
	Change    *float64 `json:"change"`
	RiseSpeed *float64 `json:"riseSpeed"`
	PreClose  *float64 `json:"preClose"`
	ResetData bool     `json:"resetData"`
}
type point struct {
	Time   string   `json:"time"`
	Price  float64  `json:"price"`
	Avg    *float64 `json:"avg"`
	Volume float64  `json:"volume"`
}
type bar struct {
	Time     string   `json:"time"`
	Open     float64  `json:"open"`
	High     float64  `json:"high"`
	Low      float64  `json:"low"`
	Close    float64  `json:"close"`
	Volume   float64  `json:"volume"`
	Amount   *float64 `json:"amount"`
	Turnover *float64 `json:"turnover"`
	PreClose *float64 `json:"preClose"`
}

var calendarPath string
var mainContractPattern = regexp.MustCompile(`^[A-Z]+0$`)
var mainlandPattern = regexp.MustCompile(`^[0-9]{6}$`)

// withHot runs one operation on the family's interactive lane; bulk work goes
// to the lane that is allowed to be slow.
func withHot(extended bool, fn func(*gotdx.Client) error) error {
	return withClient(extended, laneHot, fn)
}

func withBulk(extended bool, fn func(*gotdx.Client) error) error {
	return withClient(extended, laneBulk, fn)
}

func withScan(extended bool, fn func(*gotdx.Client) error) error {
	return withClient(extended, laneScan, fn)
}

func buildClientOptions(extended bool) ([]gotdx.Option, []*proxyTunnel, map[string]string) {
	timeout := 3
	if proxyActive() {
		// A proxy hop adds a full round trip to every exchange.
		timeout = 20
	}
	opts := []gotdx.Option{gotdx.WithTimeoutSec(timeout), gotdx.WithFuturesCalendarPath(calendarPathSnapshot())}
	serverOpts, tunnels, remote := serverOptions(extended)
	return append(opts, serverOpts...), tunnels, remote
}
func number(n float64) *float64 {
	if math.IsNaN(n) || math.IsInf(n, 0) {
		return nil
	}
	return &n
}
func field(values map[string]any, key string) *float64 {
	switch v := values[key].(type) {
	case float64:
		return number(v)
	case float32:
		return number(float64(v))
	case int:
		return number(float64(v))
	case uint32:
		return number(float64(v))
	}
	return nil
}

// EastMoney identifiers are not TDX market IDs. Only explicit mappings are
// accepted; an unknown symbol must never return a different instrument's data.
func resolve(code string) (symbol, error) {
	if strings.HasPrefix(strings.ToLower(strings.TrimSpace(code)), "tdx:") {
		return parseCanonical(code)
	}
	parts := strings.SplitN(strings.ToUpper(strings.TrimSpace(code)), ".", 2)
	if len(parts) != 2 || parts[1] == "" {
		return symbol{}, fmt.Errorf("invalid watch code: %s", code)
	}
	s := symbol{code: parts[1]}
	switch parts[0] {
	case "0", "1", "2":
		n, _ := strconv.Atoi(parts[0])
		s.market = uint8(n)
		if !mainlandPattern.MatchString(s.code) {
			return s, fmt.Errorf("invalid mainland symbol: %s", code)
		}
		if s.market == 0 && (strings.HasPrefix(s.code, "92") || strings.HasPrefix(s.code, "8") || strings.HasPrefix(s.code, "4")) {
			s.market = types.MarketBJ.Uint8()
		}
	case "116":
		s.market = types.ExCategoryHKMainBoard
		s.extended = true
	case "100", "124":
		aliases := map[string]string{"HSI": "HSI", "HSCEI": "HSCEI", "HSTECH": "HZ5017"}
		mapped, ok := aliases[s.code]
		if !ok {
			return s, fmt.Errorf("no verified TDX mapping for %s", code)
		}
		s.market = types.ExCategoryHKIndex
		s.code = mapped
		s.extended = true
	case "113":
		s.market = types.ExCategoryDLFutures
		s.extended = true
	case "114":
		s.market = types.ExCategorySHFutures
		s.extended = true
	case "115":
		s.market = types.ExCategoryZZFutures
		s.extended = true
	case "133":
		if s.code != "USDCNH" {
			return s, fmt.Errorf("no verified TDX mapping for %s", code)
		}
		s.market = types.ExCategoryBasicFX
		s.extended = true
	case "8":
		s.market = types.ExCategoryCFFEXFutures
		s.extended = true
	default:
		return s, fmt.Errorf("no verified TDX mapping for %s; select EastMoney for this instrument", code)
	}
	if s.extended && (parts[0] == "113" || parts[0] == "114" || parts[0] == "115" || parts[0] == "8") && mainContractPattern.MatchString(s.code) {
		s.code = strings.TrimSuffix(s.code, "0") + "L8"
	}
	return s, nil
}

// mainlandOpeningAuction reports whether the opening call auction is running.
// Mainland instruments report open=0 and vol=0 until continuous trading starts,
// so during this window the snapshot's last price is the price being matched
// rather than the previous session's close.
func mainlandOpeningAuction(now time.Time) bool {
	now = now.In(chinaZone())
	if wd := now.Weekday(); wd == time.Saturday || wd == time.Sunday {
		return false
	}
	minute := now.Hour()*60 + now.Minute()
	return minute >= 9*60+15 && minute < 9*60+30
}

// snapshotFromAuction checks the server's own stamps. The local clock only says
// an auction window is open; a weekday holiday keeps the last session's
// snapshot, and that must not be mistaken for an auction publishing prices now.
// Either stamp is enough to prove the snapshot is today's: the date advances
// when the server rolls to the new session, the time advances while it quotes.
func snapshotFromAuction(values map[string]any, now time.Time) bool {
	date := field(values, "server_update_date")
	clock := field(values, "server_update_time")
	if date == nil && clock == nil {
		// Without any server stamp the clock alone has to decide.
		return true
	}
	n := now.In(chinaZone())
	if date != nil && *date > 0 && int(*date) == n.Year()*10000+int(n.Month())*100+n.Day() {
		return true
	}
	return clock != nil && *clock >= 91500 && *clock < 93000
}

// hasOpeningAuction reports whether the instrument is matched by the 09:15 call
// auction. Indexes and boards are calculated from their constituents and never
// auction, so their pre-open snapshot stays the previous close.
func hasOpeningAuction(s symbol) bool {
	return !s.extended && !mainlandLotsVolume(s)
}

// Normalize each instrument independently: other markets may still have live quotes.
func quoteFromValues(code, name string, values map[string]any, now time.Time) quote {
	q := quote{Code: code, Name: name, Price: field(values, "close"),
		PreClose: field(values, "pre_close"), RiseSpeed: field(values, "speed_pct")}
	// Mainland quotes may retain a nonzero last price while the server has
	// already cleared today's open and volume. Outside the opening call auction
	// that retained price is the previous session's close and must not produce a
	// fictitious daily change; during the auction it is the price being matched.
	open, volume := field(values, "open"), field(values, "vol")
	unopened := false
	auctionable := false
	if open != nil && *open == 0 && volume != nil && *volume == 0 {
		s, err := resolve(code)
		unopened = err == nil && !s.extended
		auctionable = err == nil && hasOpeningAuction(s)
	}
	auction := unopened && auctionable && mainlandOpeningAuction(now) && snapshotFromAuction(values, now)
	if (q.Price != nil && *q.Price == 0) || (unopened && !auction) {
		q.ResetData = true
		q.Price = nil
		q.RiseSpeed = nil
		if q.PreClose != nil && *q.PreClose > 0 {
			q.Price = q.PreClose
		}
	} else if auction {
		// 涨速 describes continuous trading, which has not started yet.
		q.RiseSpeed = nil
	}
	if !q.ResetData && q.Price != nil && q.PreClose != nil && *q.PreClose > 0 {
		change := *q.Price - *q.PreClose
		q.Change = number(change)
		q.Pct = number(change / *q.PreClose * 100)
	}
	return q
}

// breadthSymbol is the market summary board. Its quote carries the up/down and
// limit counts the summary row shows, so it rides the same batch as the
// watchlist instead of costing a separate full-market request.
const breadthSymbol = "880001"

func quotes(req request) map[string]any {
	result := make([]quote, 0)
	errors := make([]string, 0)
	var breadth any
	for _, extended := range []bool{false, true} {
		markets := make([]uint8, 0)
		codes := make([]string, 0)
		originals := make(map[string][]stock)
		for _, item := range req.Stocks {
			s, err := resolveNamed(item.Code, item.Name)
			if err != nil {
				if !extended {
					errors = append(errors, err.Error())
				}
				continue
			}
			if s.extended != extended {
				continue
			}
			key := fmt.Sprintf("%d.%s", s.market, s.code)
			if _, exists := originals[key]; !exists {
				markets = append(markets, s.market)
				codes = append(codes, s.code)
			}
			originals[key] = append(originals[key], item)
		}
		withBreadth := req.IncludeBreadth && !extended
		if withBreadth {
			markets = append(markets, 1)
			codes = append(codes, breadthSymbol)
		}
		// Symbols whose exchange failed are reported through the transport error
		// only. Saying "no quote" for data the server never answered implies the
		// instrument is delisted, which sent users chasing the wrong problem.
		unanswered := make(map[string]bool)
		for offset := 0; offset < len(codes); offset += 80 {
			end := min(offset+80, len(codes))
			fields := []gotdx.MACFieldSelector{gotdx.MACFieldPreClose, gotdx.MACFieldOpen, gotdx.MACFieldClose,
				gotdx.MACFieldVol, gotdx.MACFieldSpeedPct, gotdx.MACFieldServerUpdateDate, gotdx.MACFieldServerUpdateTime}
			if withBreadth {
				// On ordinary equities these bits encode orderbook sizes; only the
				// 880001 row reads them, and the extra bytes are negligible.
				fields = append(fields, gotdx.MACFieldUpCount, gotdx.MACFieldDownCount,
					gotdx.MACFieldLimitUpCount, gotdx.MACFieldLimitDownCount)
			}
			var reply *proto.MACSymbolQuotesReply
			err := withHot(extended, func(c *gotdx.Client) error {
				var err error
				reply, err = c.MACSymbolQuotes(markets[offset:end], codes[offset:end], gotdx.MACFieldBitmap(fields...))
				return err
			})
			if err != nil {
				errors = append(errors, err.Error())
				for i := offset; i < end; i++ {
					unanswered[fmt.Sprintf("%d.%s", markets[i], strings.ToUpper(codes[i]))] = true
				}
				continue
			}
			now := time.Now()
			for _, item := range reply.Stocks {
				key := fmt.Sprintf("%d.%s", item.Market, strings.ToUpper(item.Symbol))
				if withBreadth && item.Market == 1 && strings.EqualFold(item.Symbol, breadthSymbol) {
					// The summary row degrades on its own: a failed universe lookup
					// leaves the previous counts on screen, so this must not surface
					// as a quotation error the user has to dismiss.
					if snapshot, err := breadthCounts(item.Values); err == nil {
						breadth = snapshot
					}
					continue
				}
				stocks := originals[key]
				delete(originals, key)
				for _, original := range stocks {
					q := quoteFromValues(original.Code, item.Name, item.Values, now)
					if q.Name == "" {
						q.Name = original.Name
					}
					if q.Price == nil && !q.ResetData {
						errors = append(errors, original.Code+": missing price")
						continue
					}
					result = append(result, q)
				}
			}
		}
		for key, stocks := range originals {
			if unanswered[key] {
				continue
			}
			for _, s := range stocks {
				errors = append(errors, s.Code+": TDX returned no quote")
			}
		}
	}
	sort.Strings(errors)
	response := map[string]any{"quotes": result, "errors": errors}
	if req.IncludeBreadth {
		response["breadth"] = breadth
	}
	return response
}

// breadthCounts turns the summary board's quote into the four counts plus the
// universe size. The counts are free with the batch; the size changes only on a
// listing or delisting, so it is cached far longer than the counts.
func breadthCounts(values map[string]any) (map[string]any, error) {
	up, down := field(values, "up_count"), field(values, "down_count")
	limitUp, limitDown := field(values, "limit_up_count"), field(values, "limit_down_count")
	if up == nil || down == nil || limitUp == nil || limitDown == nil {
		return nil, fmt.Errorf("TDX market summary lacks required fields")
	}
	total, err := breadthUniverse()
	if err != nil {
		return nil, err
	}
	flat := total - int(*up) - int(*down)
	if flat < 0 {
		return nil, fmt.Errorf("TDX market breadth counts exceed membership total")
	}
	updatedAtMs := int64(0)
	if date, clock := field(values, "server_update_date"), field(values, "server_update_time"); date != nil && clock != nil {
		if stamp, err := time.ParseInLocation("20060102 150405",
			fmt.Sprintf("%08d %06d", int(*date), int(*clock)), chinaZone()); err == nil {
			updatedAtMs = stamp.UnixMilli()
		}
	}
	return map[string]any{
		"upCount": int(*up), "downCount": int(*down), "flatCount": flat,
		"limitUpCount": int(*limitUp), "limitDownCount": int(*limitDown),
		"lastUpdatedAtMs": updatedAtMs, "total": total,
	}, nil
}

var (
	breadthUniverseMu    sync.Mutex
	breadthUniverseValue int
	breadthUniverseAt    time.Time
)

// The universe size only moves when an instrument lists or delists, so a half
// hour of staleness is invisible while it keeps the summary row free.
const breadthUniverseTTL = 30 * time.Minute

func breadthUniverse() (int, error) {
	breadthUniverseMu.Lock()
	cached, at := breadthUniverseValue, breadthUniverseAt
	breadthUniverseMu.Unlock()
	if cached > 0 && time.Since(at) < breadthUniverseTTL {
		return cached, nil
	}
	var reply *proto.MACBoardMembersQuotesDynamicReply
	if err := withBulk(false, func(c *gotdx.Client) error {
		// The Get* call does not connect on its own, and this lane may never have
		// been used before; the MAC* wrapper would connect but rewrites a zero
		// count into a full-market page, which is the request being avoided.
		if err := c.ConnectMAC(); err != nil {
			return err
		}
		var err error
		// One row is enough: the reply carries the membership total beside it.
		reply, err = c.GetMACBoardMembersQuotesDynamic(breadthSymbol, 0, 0, 1, 1,
			gotdx.MACFieldBitmap(gotdx.MACFieldClose))
		return err
	}); err != nil {
		return 0, err
	}
	total := int(reply.Total)
	if total <= 0 {
		return 0, fmt.Errorf("TDX returned no market universe size")
	}
	breadthUniverseMu.Lock()
	breadthUniverseValue, breadthUniverseAt = total, time.Now()
	breadthUniverseMu.Unlock()
	return total, nil
}
func timeline(req request) (any, error) {
	s, err := resolveNamed(req.Code, req.Name)
	if err != nil {
		return nil, err
	}
	// Ask for two sessions, then use the last available trading day. This also
	// handles weekends and holidays without guessing a trading calendar.
	var reply *proto.MACTickChartsReply
	if err := withHot(s.extended, func(c *gotdx.Client) error {
		var err error
		reply, err = c.MACTickCharts(s.market, s.code, 0, 2)
		return err
	}); err != nil {
		return nil, err
	}
	return timelineResponse(req.Code, reply)
}

func timelineResponse(code string, reply *proto.MACTickChartsReply) (any, error) {
	points := make([]point, 0)
	if len(reply.Charts) == 0 {
		return nil, fmt.Errorf("TDX returned no timeline for %s", code)
	}
	sort.SliceStable(reply.Charts, func(i, j int) bool { return reply.Charts[i].Date < reply.Charts[j].Date })
	last := len(reply.Charts) - 1
	for last >= 0 && len(reply.Charts[last].Ticks) == 0 {
		last--
	}
	if last < 0 {
		return nil, fmt.Errorf("TDX returned an empty timeline for %s", code)
	}
	day := reply.Charts[last]
	date := strings.ReplaceAll(day.Date, "-", "")
	if len(date) == 8 {
		date = date[:4] + "-" + date[4:6] + "-" + date[6:8]
	}
	for _, tick := range day.Ticks {
		avg := number(tick.Avg)
		if tick.Avg <= 0 {
			avg = nil
		}
		if tick.Price > 0 {
			// In 0x123E each minute volume occupies four little-endian
			// bytes. The SDK exposes the low word as Vol and the high word
			// as Unknown. Recombine them; these are already per-minute
			// values, not cumulative totals or values scaled by VolUnit.
			volume := uint32(tick.Vol) | uint32(tick.Unknown)<<16
			points = append(points, point{Time: date + " " + tick.Time, Price: tick.Price, Avg: avg, Volume: float64(volume)})
		}
	}
	if len(points) == 0 {
		return nil, fmt.Errorf("TDX returned an empty timeline for %s", code)
	}
	return map[string]any{"points": points, "preClose": number(day.PreClose)}, nil
}
func klines(req request) (any, error) {
	s, err := resolveNamed(req.Code, req.Name)
	if err != nil {
		return nil, err
	}
	if req.Count == 0 {
		req.Count = 240
	}
	// The chart loads this full depth so panning and zooming out reach real
	// history: 800 periods (the largest configurable view) plus 249 prior
	// closes for the MA250 warm-up.
	if req.Count > 1049 {
		return nil, fmt.Errorf("K-line count exceeds 1049")
	}
	var reply []proto.MACSymbolBar
	if err := withHot(s.extended, func(c *gotdx.Client) error {
		var err error
		reply, err = c.MACSymbolBars(s.market, s.code, req.Period, 1, uint32(req.Start), uint32(req.Count), types.AdjustNone)
		return err
	}); err != nil {
		return nil, err
	}
	if len(reply) == 0 {
		return nil, fmt.Errorf("TDX returned no K-line for %s", req.Code)
	}
	// Share counts turn a bar's close into a market value. They are a
	// best-effort lookup: indexes, boards and futures simply have none.
	counts := readShareCounts(s)
	// The SDK divides by the bar's own FloatShares. That is right for equities
	// and Hong Kong bars, but mainland index and board bars carry a denormal
	// there, so their SDK turnover is unusable. Recompute whenever the
	// quote-level count is available, and always for those series.
	indexBars := mainlandLotsVolume(s)
	lots := indexBars || hongKongVolume(s)
	bars := make([]bar, 0, len(reply))
	for _, b := range reply {
		turnover := number(b.Turnover)
		if indexBars || counts.Float != nil {
			turnover = turnoverPercent(b.Vol, req.Period, lots, counts.Float)
		}
		bars = append(bars, bar{
			Time:     b.DateTime.Format("2006-01-02 15:04:05"),
			Open:     b.Open,
			High:     b.High,
			Low:      b.Low,
			Close:    b.Close,
			Volume:   b.Vol,
			Amount:   number(b.Amount),
			Turnover: turnover,
			PreClose: number(b.PreClose),
		})
	}
	sort.SliceStable(bars, func(i, j int) bool { return bars[i].Time < bars[j].Time })
	totalShares, floatShares := counts.Total, counts.Float
	if indexBars {
		// An index close is points, not a price, so no share count turns it into
		// a market value. Only the reported market cap does; express that as the
		// count which reproduces it, and drop the raw count that would not.
		totalShares = indexShares(counts, bars[len(bars)-1].Close)
		floatShares = nil
	}
	return map[string]any{"bars": bars, "totalShares": totalShares, "floatShares": floatShares}, nil
}

func isFinite(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }

// TDX reports K-line volume in a unit that depends on both the period and the
// instrument class. Mainland equities and ETFs send daily volume in shares and
// weekly/monthly volume in lots of 100. Index, board and Hong Kong series are
// already in lots at every period, so they need one more factor of 100.
// Turnover divides a share count, so both factors matter.
func barVolumeInShares(volume float64, period uint16, lots bool) float64 {
	scale := 1.0
	if period != 4 { // weekly and monthly
		scale = 100.0
	}
	if lots {
		scale *= 100.0
	}
	return volume * scale
}

// TDX indexes and boards follow fixed code ranges per market. Market 1 holds
// both 上证指数 (000001) and 平安银行 (000001, market 0), so the market is part
// of the test rather than the code alone.
func mainlandLotsVolume(s symbol) bool {
	if s.extended {
		return false
	}
	if strings.HasPrefix(s.code, "88") {
		return true
	}
	if s.market == 1 {
		return strings.HasPrefix(s.code, "000")
	}
	return s.market == 0 && strings.HasPrefix(s.code, "399")
}

// Hong Kong bars report volume in lots too, with a real share count rather
// than the denormal mainland indexes carry.
func hongKongVolume(s symbol) bool {
	if !s.extended {
		return false
	}
	return s.market == types.ExCategoryHKMainBoard || s.market == types.ExCategoryHKIndex
}

// turnoverPercent is traded shares over the float count, in percent. floatWan
// counts 10k shares, so floatWan*100 is a hundredth of the share count and the
// quotient already lands in percent.
func turnoverPercent(volume float64, period uint16, lots bool, floatWan *float64) *float64 {
	if floatWan == nil || *floatWan <= 0 {
		return nil
	}
	shares := barVolumeInShares(volume, period, lots)
	if !isFinite(shares) || shares <= 0 {
		return nil
	}
	return number(shares / (*floatWan * 100.0))
}

// An index close is points, not a price, so close*shares is meaningless there.
// The reported total market cap is authoritative; return the share count that
// reproduces it so the header's market value stays honest.
func indexShares(counts shareCounts, close float64) *float64 {
	if counts.MarketCap == nil || *counts.MarketCap <= 0 || !isFinite(close) || close <= 0 {
		return nil
	}
	return number(*counts.MarketCap / close / 10000.0)
}

// shareCounts holds outstanding share counts in 10k shares, used to derive
// market value and turnover. A missing count stays null rather than zero.
type shareCounts struct {
	Total     *float64 `json:"total"`
	Float     *float64 `json:"float"`
	MarketCap *float64 `json:"-"`
}

// Share counts change only on corporate actions, and the K-line chart
// throttles its own fetches to 30 seconds. Cache per symbol so a refresh costs
// no extra round trip on the hot lane.
var (
	shareCountMu    sync.Mutex
	shareCountCache = map[string]shareCounts{}
)

func readShareCounts(s symbol) shareCounts {
	key := fmt.Sprintf("%d.%s", s.market, s.code)
	shareCountMu.Lock()
	cached, ok := shareCountCache[key]
	shareCountMu.Unlock()
	if ok {
		return cached
	}
	var counts shareCounts
	if err := withHot(s.extended, func(c *gotdx.Client) error {
		counts = fetchShareCounts(c, s)
		return nil
	}); err != nil {
		return shareCounts{}
	}
	if counts.Total == nil && counts.Float == nil {
		// Do not cache a failure: the next refresh should retry.
		return counts
	}
	shareCountMu.Lock()
	shareCountCache[key] = counts
	shareCountMu.Unlock()
	return counts
}

func fetchShareCounts(c *gotdx.Client, s symbol) shareCounts {
	var counts shareCounts
	reply, err := c.MACSymbolQuotes([]uint8{s.market}, []string{s.code},
		gotdx.MACFieldBitmap(gotdx.MACFieldTotalShares, gotdx.MACFieldFloatShares, gotdx.MACFieldTotalMarketCapAb))
	if err != nil || len(reply.Stocks) == 0 {
		return counts
	}
	values := reply.Stocks[0].Values
	counts.Total = field(values, "total_shares")
	counts.Float = field(values, "float_shares")
	counts.MarketCap = field(values, "total_market_cap_ab")
	return counts
}

func dispatch(req request) (any, error) {
	switch req.Operation {
	case "capabilities":
		return map[string]any{"abi": 1, "authentication": false, "operations": []string{"quotes", "timeline", "klines", "search", "warm", "hot_boards", "members", "breadth", "server_list", "server_probe", "prepare_servers", "push_start", "push_stop", "push_poll"}}, nil
	case "server_list":
		return listServers(), nil
	case "server_probe":
		return testServers(req)
	case "search":
		return search(req)
	case "warm":
		// Prime the directory in the background so the first stock search does
		// not wait for a full scan of every market.
		warmCatalog()
		return map[string]any{"started": true}, nil
	case "hot_boards":
		return hotBoards(req)
	case "members":
		return members(req)
	case "breadth":
		return marketBreadth(req)
	case "quotes":
		return quotes(req), nil
	case "push_start":
		return pushStart(req)
	case "push_stop":
		return pushStop()
	case "push_poll":
		return pushPoll()
	case "timeline":
		return timeline(req)
	case "klines":
		return klines(req)
	default:
		return nil, fmt.Errorf("unsupported operation: %s", req.Operation)
	}
}

//export MyStocksTdxRequest
func MyStocksTdxRequest(input *C.char) (output *C.char) {
	defer func() {
		if recover() != nil {
			output = C.CString(`{"error":"gotdx panic while processing request"}`)
		}
	}()
	if input == nil {
		return C.CString(`{"error":"null request"}`)
	}
	var req request
	if err := json.Unmarshal([]byte(C.GoString(input)), &req); err != nil {
		return C.CString(`{"error":"invalid request JSON"}`)
	}
	// Requests are independent: each operation takes its lane lock, so a
	// directory scan can no longer hold every quotation behind it.
	result, err := processRequest(req)
	if err != nil {
		result = map[string]any{"error": err.Error()}
	}
	data, err := json.Marshal(result)
	if err != nil {
		data = []byte(`{"error":"invalid numeric response"}`)
	}
	return C.CString(string(data))
}

// prepareMu makes route preparation exclusive. It only mutates committed
// settings and validates one node, so plain requests never wait on it.
var prepareMu sync.Mutex

func processRequest(req request) (any, error) {
	if req.Operation == "prepare_servers" {
		prepareMu.Lock()
		defer prepareMu.Unlock()
		return prepareServers(req)
	}
	// Settings previews never alter active connections or account/proxy state.
	// push_poll and push_stop only read or stop local state, so the high-rate
	// poll the UI drives must not pay for config commits or failover.
	if req.Operation == "server_list" || req.Operation == "server_probe" || req.Operation == "capabilities" ||
		req.Operation == "push_poll" || req.Operation == "push_stop" {
		return dispatch(req)
	}
	if err := applyConfig(req); err != nil {
		return nil, err
	}
	return dispatchWithFailover(req)
}

// applyConfig commits the settings carried by a request. Every caller in one
// process sends the same settings, so a concurrent commit is a no-op; the lock
// exists only to keep the globals consistent, and it is never held while a
// socket is being closed.
func applyConfig(req request) error {
	configMu.Lock()
	proxyChanged, err := setProxyLocked(req.Proxy)
	if err != nil {
		configMu.Unlock()
		return err
	}
	servers, err := normalizeServers(req.Servers)
	if err != nil {
		configMu.Unlock()
		return err
	}
	mainChanged := !reflect.DeepEqual(activeServers.Main, servers.Main)
	exChanged := !reflect.DeepEqual(activeServers.Ex, servers.Ex)
	activeServers = servers
	calendarChanged := req.CalendarPath != calendarPath
	calendarPath = req.CalendarPath
	configMu.Unlock()

	if proxyChanged {
		catalogMu.Lock()
		catalog, catalogWarning = nil, ""
		boardLists = make(map[uint16][]proto.MACBoardListItem)
		breadthMu.Lock()
		breadthCache = nil
		breadthMu.Unlock()
		catalogMu.Unlock()
		resetRoute(false)
		resetRoute(true)
		dropClient(false)
		dropClient(true)
		return nil
	}
	if mainChanged || calendarChanged {
		resetRoute(false)
		dropClient(false)
	}
	if exChanged || calendarChanged {
		resetRoute(true)
		dropClient(true)
	}
	return nil
}

//export MyStocksNetworkProxy
func MyStocksNetworkProxy(input *C.char) (output *C.char) {
	defer func() {
		if recover() != nil {
			output = C.CString(`{"error":"network proxy initialization failed"}`)
		}
	}()
	if input == nil {
		return C.CString(`{"error":"null proxy settings"}`)
	}
	result, err := networkProxyRequest([]byte(C.GoString(input)))
	if err != nil {
		result = map[string]any{"error": err.Error()}
	}
	data, _ := json.Marshal(result)
	return C.CString(string(data))
}

//export MyStocksTdxFree
func MyStocksTdxFree(value *C.char) { C.free(unsafe.Pointer(value)) }
func main()                         {}
