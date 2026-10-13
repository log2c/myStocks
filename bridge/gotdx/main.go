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
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"unsafe"

	"github.com/bensema/gotdx"
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
	Force        bool            `json:"force"`
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
}
type point struct {
	Time   string   `json:"time"`
	Price  float64  `json:"price"`
	Avg    *float64 `json:"avg"`
	Volume float64  `json:"volume"`
}
type bar struct {
	Time   string  `json:"time"`
	Open   float64 `json:"open"`
	High   float64 `json:"high"`
	Low    float64 `json:"low"`
	Close  float64 `json:"close"`
	Volume float64 `json:"volume"`
	Amount float64 `json:"amount"`
}

var mu sync.Mutex
var mainClient, exClient *gotdx.Client
var calendarPath string
var mainContractPattern = regexp.MustCompile(`^[A-Z]+0$`)
var mainlandPattern = regexp.MustCompile(`^[0-9]{6}$`)

func client(extended bool) *gotdx.Client {
	slot := &mainClient
	if extended {
		slot = &exClient
	}
	if *slot == nil {
		opts := []gotdx.Option{gotdx.WithTimeoutSec(3), gotdx.WithAutoSelectFastest(true), gotdx.WithFuturesCalendarPath(calendarPath)}
		if activeProxy.enabled() {
			opts = append(opts, gotdx.WithTimeoutSec(20))
		}
		opts = append(opts, proxyOptions()...)
		if extended {
			*slot = gotdx.NewMACEx(opts...)
		} else {
			*slot = gotdx.NewMAC(opts...)
		}
	}
	return *slot
}
func reset(extended bool) {
	slot := &mainClient
	if extended {
		slot = &exClient
	}
	if *slot != nil {
		(*slot).Disconnect()
		*slot = nil
	}
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
func quotes(req request) map[string]any {
	result := make([]quote, 0)
	errors := make([]string, 0)
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
		for offset := 0; offset < len(codes); offset += 80 {
			end := min(offset+80, len(codes))
			reply, err := client(extended).MACSymbolQuotes(markets[offset:end], codes[offset:end],
				gotdx.MACFieldBitmap(gotdx.MACFieldPreClose, gotdx.MACFieldClose, gotdx.MACFieldSpeedPct))
			if err != nil {
				reset(extended)
				errors = append(errors, err.Error())
				continue
			}
			for _, item := range reply.Stocks {
				key := fmt.Sprintf("%d.%s", item.Market, strings.ToUpper(item.Symbol))
				stocks := originals[key]
				delete(originals, key)
				for _, original := range stocks {
					q := quote{Code: original.Code, Name: item.Name, Price: field(item.Values, "close"), RiseSpeed: field(item.Values, "speed_pct")}
					if q.Name == "" {
						q.Name = original.Name
					}
					pre := field(item.Values, "pre_close")
					if q.Price == nil {
						errors = append(errors, original.Code+": missing price")
						continue
					}
					if pre != nil && *pre > 0 {
						q.Change = number(*q.Price - *pre)
						q.Pct = number((*q.Change) / (*pre) * 100)
					}
					result = append(result, q)
				}
			}
		}
		for _, stocks := range originals {
			for _, s := range stocks {
				errors = append(errors, s.Code+": TDX returned no quote")
			}
		}
	}
	sort.Strings(errors)
	return map[string]any{"quotes": result, "errors": errors}
}
func timeline(req request) (any, error) {
	s, err := resolveNamed(req.Code, req.Name)
	if err != nil {
		return nil, err
	}
	// Ask for two sessions, then use the last available trading day. This also
	// handles weekends and holidays without guessing a trading calendar.
	reply, err := client(s.extended).MACTickCharts(s.market, s.code, 0, 2)
	if err != nil {
		reset(s.extended)
		return nil, err
	}
	points := make([]point, 0)
	if len(reply.Charts) == 0 {
		return nil, fmt.Errorf("TDX returned no timeline for %s", req.Code)
	}
	sort.SliceStable(reply.Charts, func(i, j int) bool { return reply.Charts[i].Date < reply.Charts[j].Date })
	last := len(reply.Charts) - 1
	for last >= 0 && len(reply.Charts[last].Ticks) == 0 {
		last--
	}
	if last < 0 {
		return nil, fmt.Errorf("TDX returned an empty timeline for %s", req.Code)
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
			points = append(points, point{Time: date + " " + tick.Time, Price: tick.Price, Avg: avg, Volume: float64(tick.Vol)})
		}
	}
	if len(points) == 0 {
		return nil, fmt.Errorf("TDX returned an empty timeline for %s", req.Code)
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
	if req.Count > 800 {
		return nil, fmt.Errorf("K-line count exceeds 800")
	}
	reply, err := client(s.extended).MACSymbolBars(s.market, s.code, req.Period, 1, uint32(req.Start), uint32(req.Count), types.AdjustNone)
	if err != nil {
		reset(s.extended)
		return nil, err
	}
	if len(reply) == 0 {
		return nil, fmt.Errorf("TDX returned no K-line for %s", req.Code)
	}
	bars := make([]bar, 0, len(reply))
	for _, b := range reply {
		bars = append(bars, bar{b.DateTime.Format("2006-01-02 15:04:05"), b.Open, b.High, b.Low, b.Close, b.Vol, b.Amount})
	}
	sort.SliceStable(bars, func(i, j int) bool { return bars[i].Time < bars[j].Time })
	return map[string]any{"bars": bars}, nil
}
func dispatch(req request) (any, error) {
	switch req.Operation {
	case "capabilities":
		return map[string]any{"abi": 1, "authentication": false, "operations": []string{"quotes", "timeline", "klines", "search", "hot_boards", "members", "breadth"}}, nil
	case "search":
		return search(req)
	case "hot_boards":
		return hotBoards(req)
	case "members":
		return members(req)
	case "breadth":
		return marketBreadth(req)
	case "quotes":
		return quotes(req), nil
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
	mu.Lock()
	defer mu.Unlock()
	if err := configureProxy(req.Proxy); err != nil {
		data, _ := json.Marshal(map[string]any{"error": err.Error()})
		return C.CString(string(data))
	}
	if req.CalendarPath != calendarPath {
		reset(false)
		reset(true)
		calendarPath = req.CalendarPath
	}
	result, err := dispatch(req)
	if err != nil {
		result = map[string]any{"error": err.Error()}
	}
	data, err := json.Marshal(result)
	if err != nil {
		data = []byte(`{"error":"invalid numeric response"}`)
	}
	return C.CString(string(data))
}

//export MyStocksTdxFree
func MyStocksTdxFree(value *C.char) { C.free(unsafe.Pointer(value)) }
func main()                         {}
