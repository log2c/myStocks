package main

import (
	"encoding/binary"
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/bensema/gotdx/proto"
)

func TestTimelineReconstructsFullWidthMinuteVolumes(t *testing.T) {
	// Captured 510300 minute data. The upstream SDK splits each uint32
	// volume into Vol/Unknown; exercise that parser before our projection.
	volumes := []uint32{1355308, 421948, 419112, 0, math.MaxUint32}
	prices := []float32{4.229, 4.238, 4.234, 4.234, 4.235}
	data := make([]byte, 71+14*len(volumes))
	binary.LittleEndian.PutUint16(data[0:2], 1)
	copy(data[2:24], "510300")
	binary.LittleEndian.PutUint32(data[24:28], 20261015)
	binary.LittleEndian.PutUint32(data[44:48], math.Float32bits(4.294))
	binary.LittleEndian.PutUint16(data[64:66], 1)
	binary.LittleEndian.PutUint16(data[69:71], uint16(len(volumes)))
	for i, volume := range volumes {
		row := data[71+14*i : 71+14*(i+1)]
		binary.LittleEndian.PutUint16(row[:2], uint16(570+i))
		binary.LittleEndian.PutUint32(row[2:6], math.Float32bits(prices[i]))
		binary.LittleEndian.PutUint32(row[6:10], math.Float32bits(4.235))
		binary.LittleEndian.PutUint32(row[10:14], volume)
	}
	parser := proto.NewMACTickCharts(nil)
	if err := parser.ParseResponse(&proto.RespHeader{}, data); err != nil {
		t.Fatal(err)
	}
	reply := parser.Response()
	reply.VolUnit = 100 // Quote lot size must not scale minute volumes.
	if first := reply.Charts[0].Ticks[0]; first.Vol != 44588 || first.Unknown != 20 {
		t.Fatalf("unexpected upstream word layout: %+v", first)
	}
	result, err := timelineResponse("1.510300", reply)
	if err != nil {
		t.Fatal(err)
	}
	points := result.(map[string]any)["points"].([]point)
	if len(points) != len(volumes) {
		t.Fatalf("missing points: %+v", points)
	}
	for i, volume := range volumes {
		if points[i].Volume != float64(volume) {
			t.Fatalf("minute %d: volume=%v want=%d", i, points[i].Volume, volume)
		}
	}
	if points[0].Time != "2026-10-15 09:30:00" || points[1].Time != "2026-10-15 09:31:00" || points[2].Time != "2026-10-15 09:32:00" {
		t.Fatalf("minute alignment changed: %+v", points[:3])
	}
	if points[0].Price >= 4.294 || points[1].Price <= points[0].Price || points[2].Price >= points[1].Price {
		t.Fatal("510300 minute prices must produce green, red, green bars")
	}
}

func TestKlineHistoryLimitIncludesMA250Warmup(t *testing.T) {
	_, err := klines(request{Code: "1.510300", Period: 4, Count: 1050})
	if err == nil {
		t.Fatal("K-line request must reject more than 800 visible days plus 249 warm-up days")
	}
}

// Captured 2026-10-20 TDX values: 创业板指 daily volume 151827968 shares over
// 56896388 万股 of float, which the market data reports as 2.67%.
func TestTurnoverMatchesMarketDataForIndex(t *testing.T) {
	floatWan := 56896388.0
	got := turnoverPercent(151827968, 4, mainlandLotsVolume(symbol{market: 0, code: "399006"}), &floatWan)
	if got == nil || math.Abs(*got-2.668) > 0.01 {
		t.Fatalf("index turnover must divide shares by the quote float count: %v", got)
	}
	// Weekly bars carry lots, so the same series needs the extra factor of 100.
	// The market data reports 14.90% for the week ending 2026-10-16.
	weekly := turnoverPercent(8480319, 5, mainlandLotsVolume(symbol{market: 0, code: "399006"}), &floatWan)
	if weekly == nil || math.Abs(*weekly-14.90) > 0.02 {
		t.Fatalf("weekly index turnover must scale lots to shares: %v", weekly)
	}
}

// Captured 2026-10-20 贵州茅台: daily volume 3413565 shares over 125008 万股
// float, reported as 0.27%.
func TestTurnoverMatchesMarketDataForEquity(t *testing.T) {
	floatWan := 125008.15625
	s := symbol{market: 1, code: "600519"}
	if mainlandLotsVolume(s) {
		t.Fatal("an equity must not take the index volume scale")
	}
	daily := turnoverPercent(3413565, 4, false, &floatWan)
	if daily == nil || math.Abs(*daily-0.273) > 0.005 {
		t.Fatalf("equity daily turnover: %v", daily)
	}
	// Weekly equity bars are lots; the market data reports 1.42% for the week
	// ending 2026-10-16.
	weekly := turnoverPercent(178080, 5, false, &floatWan)
	if weekly == nil || math.Abs(*weekly-1.425) > 0.01 {
		t.Fatalf("equity weekly turnover: %v", weekly)
	}
}

func TestTurnoverStaysUnavailableWithoutFloatShares(t *testing.T) {
	if got := turnoverPercent(1000, 4, false, nil); got != nil {
		t.Fatal("turnover must stay unavailable without a float count")
	}
	zero := 0.0
	if got := turnoverPercent(1000, 4, false, &zero); got != nil {
		t.Fatal("a zero float count must not produce infinite turnover")
	}
	if got := turnoverPercent(0, 4, false, &zero); got != nil {
		t.Fatal("a bar without volume must not report turnover")
	}
}

func TestMainlandVolumeScalesByInstrumentClass(t *testing.T) {
	for _, tc := range []struct {
		code string
		lots bool
	}{
		{"1.000001", true},  // 上证指数
		{"0.399001", true},  // 深证成指
		{"0.399006", true},  // 创业板指
		{"1.881319", true},  // 半导体 board
		{"0.000001", false}, // 平安银行, same digits as the Shanghai index
		{"1.600519", false},
		{"0.300750", false},
		{"1.510300", false}, // ETF
		{"0.159915", false},
		{"116.00700", false}, // Hong Kong takes its own path
	} {
		s, err := resolve(tc.code)
		if err != nil {
			t.Fatalf("%s: %v", tc.code, err)
		}
		if got := mainlandLotsVolume(s); got != tc.lots {
			t.Fatalf("%s: lots=%v want %v", tc.code, got, tc.lots)
		}
	}
}

func TestHongKongBarsUseLotVolumes(t *testing.T) {
	for _, code := range []string{"116.00700", "100.HSI"} {
		s, err := resolve(code)
		if err != nil {
			t.Fatalf("%s: %v", code, err)
		}
		if !hongKongVolume(s) {
			t.Fatalf("%s must take the lot volume scale", code)
		}
	}
	s, _ := resolve("1.600519")
	if hongKongVolume(s) {
		t.Fatal("a mainland equity must not take the Hong Kong volume scale")
	}
}

// The reported market cap is authoritative for an index: its close is points,
// so close*shares is not a market value.
func TestIndexSharesReproduceReportedMarketCap(t *testing.T) {
	cap := 1.6243908291310838e13
	got := indexShares(shareCounts{MarketCap: &cap}, 2715.955810546875)
	if got == nil {
		t.Fatal("index market cap must produce a share count")
	}
	if value := *got * 2715.955810546875 * 10000; math.Abs(value-cap)/cap > 1e-9 {
		t.Fatalf("share count must reproduce the reported market cap: %v", value)
	}
	if indexShares(shareCounts{}, 100) != nil {
		t.Fatal("a missing market cap must not invent a share count")
	}
}

func TestKlineBarsCarryStatisticsFields(t *testing.T) {
	// The chart header reports amount, turnover and market value from a bar,
	// so those fields must survive the projection into JSON.
	b := bar{
		Time:     "2026-10-19 00:00:00",
		Open:     10.0,
		High:     10.8,
		Low:      10.1,
		Close:    10.6,
		Volume:   2000,
		Amount:   number(2.4e8),
		Turnover: number(3.0),
		PreClose: number(10.2),
	}
	encoded, err := json.Marshal(b)
	if err != nil {
		t.Fatal(err)
	}
	var decoded map[string]any
	if err := json.Unmarshal(encoded, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded["amount"] != 2.4e8 || decoded["turnover"] != 3.0 || decoded["preClose"] != 10.2 {
		t.Fatalf("bar statistics missing from JSON: %s", encoded)
	}
	// A bar without statistics must serialize nulls, not zeros, so the chart
	// can tell "no data" from a genuine zero.
	empty, err := json.Marshal(bar{})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(empty), `"amount":null`) {
		t.Fatalf("missing statistics must stay null: %s", empty)
	}
}
