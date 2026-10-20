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
