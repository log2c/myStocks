package main

import (
	"encoding/json"
	"github.com/bensema/gotdx/proto"
	"github.com/bensema/gotdx/types"
	"math"
	"strings"
	"testing"
	"time"
)

func TestResolveMarketMappings(t *testing.T) {
	cases := []struct {
		input    string
		market   uint8
		code     string
		extended bool
	}{
		{"1.600519", 1, "600519", false},
		{"0.000001", 0, "000001", false},
		{"0.920002", 2, "920002", false},
		{"116.00700", 31, "00700", true},
		{"100.HSI", 27, "HSI", true},
		{"124.HSTECH", 27, "HZ5017", true},
		{"113.M0", 29, "ML8", true},
		{"114.AU0", 30, "AUL8", true},
		{"114.AU2610", 30, "AU2610", true},
		{"115.MA0", 28, "MAL8", true},
		{"8.IF0", 47, "IFL8", true},
		{"133.USDCNH", 10, "USDCNH", true},
	}
	for _, c := range cases {
		t.Run(c.input, func(t *testing.T) {
			got, err := resolve(c.input)
			if err != nil || got.market != c.market || got.code != c.code || got.extended != c.extended {
				t.Fatalf("resolve(%q)=%+v,%v", c.input, got, err)
			}
		})
	}
	for _, code := range []string{"", "1", "1.", "1.123", "1.ABCDEF", "90.BK0420", "100.SPX", "133.EURUSD"} {
		if _, err := resolve(code); err == nil {
			t.Fatalf("unverified %q accepted", code)
		}
	}
}
func TestMissingAndNonFiniteNumbers(t *testing.T) {
	values := map[string]any{"zero": float32(0), "speed": float64(-1.25), "nan": math.NaN(), "inf": math.Inf(1), "text": "0"}
	for _, key := range []string{"missing", "nan", "inf", "text"} {
		if field(values, key) != nil {
			t.Fatalf("%s must remain unavailable", key)
		}
	}
	if p := field(values, "zero"); p == nil || *p != 0 {
		t.Fatal("zero must not become missing")
	}
	if p := field(values, "speed"); p == nil || *p != -1.25 {
		t.Fatal("speed must stay in percent units")
	}
}
func TestCapabilitiesDoNotClaimAuthentication(t *testing.T) {
	result, err := dispatch(request{Operation: "capabilities", User: json.RawMessage(`{"userId":"test","token":"secret"}`)})
	if err != nil {
		t.Fatal(err)
	}
	data, _ := json.Marshal(result)
	if strings.Contains(string(data), "secret") || result.(map[string]any)["authentication"] != false {
		t.Fatal("unimplemented authentication or secret disclosed")
	}
	if _, err := dispatch(request{Operation: "login"}); err == nil {
		t.Fatal("login must not pretend to succeed")
	}
}
func TestUnknownCodesProduceExplicitPartialErrors(t *testing.T) {
	result := quotes(request{Stocks: []stock{{Code: "90.BK0420"}, {Code: "100.SPX"}}})
	if len(result["quotes"].([]quote)) != 0 || len(result["errors"].([]string)) != 2 {
		t.Fatalf("unexpected result: %#v", result)
	}
}

func TestNativeCodesDoNotReuseEastMoneyMarkets(t *testing.T) {
	for _, test := range []struct {
		code     string
		market   uint8
		symbol   string
		extended bool
	}{
		{"tdx:main:1:881319", 1, "881319", false},
		{"tdx:ex:49:02800", 49, "02800", true},
		{"TDX:EX:48:08300", 48, "08300", true},
		{"tdx:ex:12:A_NDX", 12, "A_NDX", true},
	} {
		got, err := resolve(test.code)
		if err != nil || got.market != test.market || got.code != test.symbol || got.extended != test.extended {
			t.Fatalf("invalid native mapping: %+v %v", got, err)
		}
	}
	for _, code := range []string{"tdx:ex:256:00700", "tdx:main:x:881319", "tdx:foo:1:600519", "tdx:ex:31:", "tdx:ex:31:00700\nCONNECT"} {
		if _, err := resolve(code); err == nil {
			t.Fatalf("accepted malformed native symbol %q", code)
		}
	}
}
func TestLegacyBoardMappingRequiresUniqueName(t *testing.T) {
	old := boardLists
	oldExpiry := boardsExpires
	defer func() { boardLists = old; boardsExpires = oldExpiry }()
	boardsExpires = time.Now().Add(time.Hour)
	boardLists = map[uint16][]proto.MACBoardListItem{types.BoardTypeAll: {{Market: 1, Code: "881319", Name: "半导体"}}}
	got, err := resolveNamed("90.BK1036", "半导体")
	if err != nil || got.code != "881319" {
		t.Fatalf("legacy named match failed %v", err)
	}
	boardLists[types.BoardTypeAll] = append(boardLists[types.BoardTypeAll], proto.MACBoardListItem{Market: 1, Code: "880491", Name: "半导体"})
	if _, err := resolveNamed("90.BK1036", "半导体"); err == nil {
		t.Fatal("ambiguous board name must not choose arbitrary code")
	}
}
func TestSearchUsesCachedNativeBoardCode(t *testing.T) {
	old := catalog
	oldExpiry := catalogExpires
	defer func() { catalog = old; catalogExpires = oldExpiry }()
	catalog = []catalogItem{{Code: "tdx:main:1:881319", Name: "半导体", Kind: "板块"}, {Code: "tdx:ex:31:00700", Name: "腾讯控股", Kind: "扩展市场"}}
	catalogExpires = time.Now().Add(time.Hour)
	result, err := search(request{Keyword: "半导体"})
	if err != nil {
		t.Fatal(err)
	}
	items := result.(map[string]any)["items"].([]catalogItem)
	if len(items) != 1 || items[0].Code != "tdx:main:1:881319" {
		t.Fatal("search emitted an incompatible code")
	}
}
func TestDistributionBinBoundaries(t *testing.T) {
	for _, test := range []struct {
		pct float64
		bin int
	}{{-10, 0}, {-9, 0}, {-8, 1}, {-6, 2}, {-4, 3}, {-0.1, 4}, {0, 5}, {0.1, 6}, {3, 7}, {5, 8}, {7, 9}, {9, 10}} {
		if distributionBin(test.pct) != test.bin {
			t.Fatalf("wrong distribution bucket %v", test)
		}
	}
}

// tradingHours is any moment outside the opening auction; these cases only
// exercise the "not trading yet" state.
var tradingHours = time.Date(2026, 5, 27, 10, 0, 0, 0, chinaZone())

func TestPreOpenQuoteUsesPreviousClose(t *testing.T) {
	q := quoteFromValues("1.600519", "Stock", map[string]any{
		"close": float64(0), "pre_close": float64(18.5), "speed_pct": float64(-100),
	}, tradingHours)
	if !q.ResetData || q.Price == nil || *q.Price != 18.5 || q.PreClose == nil || *q.PreClose != 18.5 {
		t.Fatalf("reset quote must carry the previous close: %+v", q)
	}
	if q.Pct != nil || q.Change != nil || q.RiseSpeed != nil {
		t.Fatal("reset quote must not carry intraday change sentinels")
	}
	for _, pre := range []any{nil, float64(0), math.NaN()} {
		q = quoteFromValues("1.600519", "Stock", map[string]any{"close": float64(0), "pre_close": pre}, tradingHours)
		if !q.ResetData || q.Price != nil || q.Pct != nil {
			t.Fatal("unavailable previous close must not become a zero price or -100 percent")
		}
	}
	q = quoteFromValues("1.600519", "Stock", map[string]any{
		"close": float64(21), "pre_close": float64(20), "speed_pct": float64(0),
	}, tradingHours)
	if q.ResetData || q.Price == nil || *q.Price != 21 || q.Pct == nil || *q.Pct != 5 || q.Change == nil || *q.Change != 1 || q.RiseSpeed == nil || *q.RiseSpeed != 0 {
		t.Fatal("live quotes must retain prices, changes and valid zero rise speed")
	}
}

func TestUnopenedMainlandQuotesIgnoreRetainedPrices(t *testing.T) {
	for _, code := range []string{"1.000001", "0.399001", "tdx:main:1:000001", "tdx:main:0:399006", "1.600519", "1.510300", "tdx:main:1:881319"} {
		t.Run(code, func(t *testing.T) {
			values := map[string]any{
				"close": float32(18), "pre_close": float32(18.5),
				"open": float32(0), "vol": uint32(0), "speed_pct": float32(-2),
			}
			q := quoteFromValues(code, "Unopened", values, tradingHours)
			if !q.ResetData || q.Price == nil || *q.Price != 18.5 {
				t.Fatalf("unopened quote must use previous close despite retained price: %+v", q)
			}
			if q.Pct != nil || q.Change != nil || q.RiseSpeed != nil {
				t.Fatalf("unopened quote must hide retained intraday changes: %+v", q)
			}
			delete(values, "pre_close")
			q = quoteFromValues(code, "Unopened", values, tradingHours)
			if !q.ResetData || q.Price != nil || q.Pct != nil {
				t.Fatal("unavailable previous close must not fall back to a retained price")
			}
		})
	}
}

func TestOpeningAuctionQuoteUsesMatchedPrice(t *testing.T) {
	auction := time.Date(2026, 5, 27, 9, 20, 0, 0, chinaZone())
	values := map[string]any{
		"close": float32(19), "pre_close": float32(18.5),
		"open": float32(0), "vol": uint32(0), "speed_pct": float32(-2),
		"server_update_time": uint32(92015),
	}
	q := quoteFromValues("1.600519", "Auction", values, auction)
	if q.ResetData || q.Price == nil || *q.Price != 19 {
		t.Fatalf("auction quote must keep the indicative match price: %+v", q)
	}
	if q.Change == nil || *q.Change != 0.5 || q.Pct == nil || math.Abs(*q.Pct-2.7027) > 0.001 {
		t.Fatalf("auction quote must compute change against the previous close: %+v", q)
	}
	if q.RiseSpeed != nil {
		t.Fatal("auction quote must not carry a rise speed")
	}
}

func TestIndexesAndBoardsStayOnPreviousCloseDuringAuction(t *testing.T) {
	auction := time.Date(2026, 5, 27, 9, 20, 0, 0, chinaZone())
	// They are computed from constituents, so a pre-open snapshot is still the
	// previous close, and treating it as a match price would invent a change.
	for _, code := range []string{"1.000001", "0.399006", "tdx:main:1:000001", "tdx:main:1:881319"} {
		t.Run(code, func(t *testing.T) {
			q := quoteFromValues(code, "Index", map[string]any{
				"close": float32(3681), "pre_close": float32(3648),
				"open": float32(0), "vol": uint32(0), "server_update_time": uint32(92015),
			}, auction)
			if !q.ResetData || q.Price == nil || *q.Price != 3648 || q.Pct != nil {
				t.Fatalf("non-auction series must keep the previous close: %+v", q)
			}
		})
	}
}

func TestAuctionWindowRequiresAnAuctionStamp(t *testing.T) {
	auction := time.Date(2026, 5, 27, 9, 20, 0, 0, chinaZone())
	values := map[string]any{
		"close": float32(19), "pre_close": float32(18.5),
		"open": float32(0), "vol": uint32(0),
		"server_update_date": uint32(20260526), "server_update_time": uint32(150001),
	}
	q := quoteFromValues("1.600519", "Holiday", values, auction)
	if !q.ResetData || q.Price == nil || *q.Price != 18.5 || q.Pct != nil {
		t.Fatalf("a snapshot stamped with the last session's close is not a live auction: %+v", q)
	}

	// Either stamp is enough: the date alone proves the snapshot is today's even
	// if the update time has not advanced yet.
	values["server_update_date"] = uint32(20260527)
	q = quoteFromValues("1.600519", "Auction", values, auction)
	if q.ResetData || q.Price == nil || *q.Price != 19 {
		t.Fatalf("today's date must be enough to accept the auction snapshot: %+v", q)
	}
	values["server_update_date"] = uint32(20260526)
	values["server_update_time"] = uint32(92015)
	q = quoteFromValues("1.600519", "Auction", values, auction)
	if q.ResetData || q.Price == nil || *q.Price != 19 {
		t.Fatalf("an auction-time stamp must be enough: %+v", q)
	}

	// A snapshot with no server stamp stays usable; the clock alone decides.
	delete(values, "server_update_date")
	delete(values, "server_update_time")
	q = quoteFromValues("1.600519", "Auction", values, auction)
	if q.ResetData || q.Price == nil || *q.Price != 19 {
		t.Fatalf("a stamp-less auction snapshot must remain usable: %+v", q)
	}

	// Hong Kong and futures have their own opening mechanics and are untouched.
	q = quoteFromValues("116.00700", "Tencent", map[string]any{
		"close": float32(380), "pre_close": float32(382.4), "open": float32(0), "vol": uint32(0),
	}, auction)
	if q.ResetData || q.Price == nil || *q.Price != 380 {
		t.Fatalf("extended-market quotes must not go through the mainland auction path: %+v", q)
	}
}

func TestMainlandOpeningAuctionWindow(t *testing.T) {
	for _, tc := range []struct {
		at   time.Time
		want bool
	}{
		{time.Date(2026, 5, 27, 9, 14, 59, 0, chinaZone()), false},
		{time.Date(2026, 5, 27, 9, 15, 0, 0, chinaZone()), true},
		{time.Date(2026, 5, 27, 9, 29, 59, 0, chinaZone()), true},
		{time.Date(2026, 5, 27, 9, 30, 0, 0, chinaZone()), false},
		{time.Date(2026, 5, 27, 10, 0, 0, 0, chinaZone()), false},
		{time.Date(2026, 5, 27, 14, 57, 0, 0, chinaZone()), false},
		{time.Date(2026, 5, 30, 9, 20, 0, 0, chinaZone()), false},
		{time.Date(2026, 5, 31, 9, 20, 0, 0, chinaZone()), false},
		{time.Date(2026, 5, 27, 1, 20, 0, 0, time.UTC), true},
	} {
		if got := mainlandOpeningAuction(tc.at); got != tc.want {
			t.Fatalf("mainlandOpeningAuction(%s) = %v, want %v", tc.at, got, tc.want)
		}
	}
}

func TestOpeningStateDoesNotHideLiveQuotes(t *testing.T) {
	for _, tc := range []struct {
		name   string
		code   string
		values map[string]any
	}{
		{"opened with no volume", "1.000001", map[string]any{"open": float32(20), "vol": uint32(0)}},
		{"auction traded", "1.600519", map[string]any{"open": float32(0), "vol": uint32(100)}},
		{"missing open", "1.000001", map[string]any{"vol": uint32(0)}},
		{"missing volume", "1.000001", map[string]any{"open": float32(0)}},
		{"invalid open", "1.000001", map[string]any{"open": math.NaN(), "vol": uint32(0)}},
		{"Hong Kong", "116.00700", map[string]any{"open": float32(0), "vol": uint32(0)}},
		{"Hong Kong index", "tdx:ex:27:HSI", map[string]any{"open": float32(0), "vol": uint32(0)}},
		{"futures", "114.AU0", map[string]any{"open": float32(0), "vol": uint32(0)}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			tc.values["close"] = float32(21)
			tc.values["pre_close"] = float32(20)
			tc.values["speed_pct"] = float32(0)
			q := quoteFromValues(tc.code, "Live", tc.values, tradingHours)
			if q.ResetData || q.Price == nil || *q.Price != 21 || q.Pct == nil || *q.Pct != 5 || q.Change == nil || *q.Change != 1 || q.RiseSpeed == nil || *q.RiseSpeed != 0 {
				t.Fatalf("live or incomplete opening state must preserve the quote: %+v", q)
			}
		})
	}
}
