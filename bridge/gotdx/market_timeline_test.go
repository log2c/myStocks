package main

import (
	"encoding/binary"
	"encoding/json"
	"math"
	"testing"
	"time"

	"github.com/bensema/gotdx/proto"
)

func breadthTime(text string) time.Time {
	t, err := time.ParseInLocation("2006-01-02 15:04:05", text, chinaZone())
	if err != nil {
		panic(err)
	}
	return t
}

func breadthChart(code string, days ...proto.MACTickChartDay) *proto.MACTickChartsReply {
	return &proto.MACTickChartsReply{Market: 1, Code: code, Charts: days}
}

func TestBreadthTimelineColdStartAfterClose(t *testing.T) {
	// Exercise the upstream wire parser too: these statistical instruments use
	// the float Price/Avg slots for paired counts, including valid zero counts.
	parse := func(code string, up, down float32) *proto.MACTickChartsReply {
		data := make([]byte, 71+240*14)
		binary.LittleEndian.PutUint16(data[:2], 1)
		copy(data[2:24], code)
		binary.LittleEndian.PutUint32(data[24:28], 20261015)
		binary.LittleEndian.PutUint16(data[64:66], 1)
		binary.LittleEndian.PutUint16(data[69:71], 240)
		for i := 0; i < 240; i++ {
			minute := 570 + i
			if i >= 120 {
				minute += 90 // Skip lunch, retain 09:30..11:29 and 13:00..14:59.
			}
			row := data[71+14*i : 71+14*(i+1)]
			binary.LittleEndian.PutUint16(row[:2], uint16(minute))
			binary.LittleEndian.PutUint32(row[2:6], math.Float32bits(up))
			binary.LittleEndian.PutUint32(row[6:10], math.Float32bits(down))
		}
		parser := proto.NewMACTickCharts(nil)
		if err := parser.ParseResponse(&proto.RespHeader{}, data); err != nil {
			t.Fatal(err)
		}
		return parser.Response()
	}
	points, err := projectBreadthTimeline(parse("880005", 430, 5108), parse("880006", 36, 0),
		breadthTime("2026-10-15 18:00:00").UTC(), 5600)
	if err != nil || len(points) != 240 {
		t.Fatalf("cold start needs the entire server history, got %d points: %v", len(points), err)
	}
	if points[0].TimestampMs != breadthTime("2026-10-15 09:30:00").UnixMilli() ||
		points[120].TimestampMs != breadthTime("2026-10-15 13:00:00").UnixMilli() {
		t.Fatal("history must use China trading time, including the lunch gap")
	}
	if points[239].RiseCount != 430 || points[239].FallCount != 5108 || points[239].LimitUpCount != 36 || points[239].LimitDownCount != 0 {
		t.Fatalf("wrong statistical field mapping: %+v", points[239])
	}
	points = appendBreadthClose(points, breadthTime("2026-10-15 18:00:00"), 431, 5107, 37, 26)
	if len(points) != 241 || points[240].TimestampMs != breadthTime("2026-10-15 15:00:00").UnixMilli() || points[240].RiseCount != 431 {
		t.Fatal("closing snapshot must complete the 15:00 point without using the wall clock")
	}
	points = appendBreadthClose(points, breadthTime("2026-10-15 18:01:00"), 432, 5106, 37, 26)
	if len(points) != 241 || points[240].RiseCount != 432 {
		t.Fatal("repeated closing updates must not duplicate the 15:00 point")
	}
	data, err := json.Marshal(points)
	if err != nil {
		t.Fatal(err)
	}
	var rows []map[string]any
	if err := json.Unmarshal(data, &rows); err != nil || rows[240]["limitDownCount"] != float64(26) || rows[0]["timestampMs"] == nil {
		t.Fatal("breadth history must preserve the C++ snapshot JSON contract")
	}
}

func TestBreadthTimelineAlignsDatesAndMinutes(t *testing.T) {
	riseFall := breadthChart("880005",
		proto.MACTickChartDay{Date: "2026-10-14", Ticks: []proto.MACTickChartItem{{Time: "09:30:00", Price: 100, Avg: 200}}},
		proto.MACTickChartDay{Date: "2026-10-15", Ticks: []proto.MACTickChartItem{
			{Time: "13:00:00", Price: 300, Avg: 200},
			{Time: "09:30:00", Price: 200, Avg: 300},
			{Time: "09:30:00", Price: 201, Avg: 299},
		}},
		proto.MACTickChartDay{Date: "2026-10-16", Ticks: []proto.MACTickChartItem{{Time: "09:30:00", Price: 400, Avg: 100}}})
	limits := breadthChart("880006",
		proto.MACTickChartDay{Date: "2026-10-14", Ticks: []proto.MACTickChartItem{{Time: "13:00:00", Price: 99, Avg: 88}}},
		proto.MACTickChartDay{Date: "2026-10-15", Ticks: []proto.MACTickChartItem{{Time: "09:30:00", Price: 20, Avg: 10}}})
	points, err := projectBreadthTimeline(riseFall, limits, breadthTime("2026-10-15 16:00:00"), 600)
	if err != nil || len(points) != 2 || points[0].RiseCount != 201 || points[1].RiseCount != 300 {
		t.Fatalf("history must select, sort and deduplicate one trading day: %+v %v", points, err)
	}
	if points[0].LimitUpCount != 20 || points[1].LimitUpCount != -1 || points[1].LimitDownCount != -1 {
		t.Fatal("missing limit samples must stay unknown, never borrow another day's counts")
	}
	points, err = projectBreadthTimeline(riseFall, nil, breadthTime("2026-10-15 11:00:00"), 600)
	if err != nil || len(points) != 1 || points[0].LimitUpCount != -1 {
		t.Fatal("intraday queries must exclude future minutes and preserve partial rise/fall data")
	}
	weekend, err := projectBreadthTimeline(riseFall, limits, breadthTime("2026-10-17 16:00:00"), 600)
	if err != nil || len(weekend) != 1 || weekend[0].RiseCount != 400 {
		t.Fatal("non-trading days must use the latest available trading day")
	}
	if len(appendBreadthClose(weekend, breadthTime("2026-10-17 16:00:00"), 500, 100, 0, 0)) != 1 {
		t.Fatal("a new calendar date must not be appended to an earlier trading day's series")
	}
}

func TestBreadthTimelineRejectsInvalidSamples(t *testing.T) {
	day := proto.MACTickChartDay{Date: "2026-10-15", Ticks: []proto.MACTickChartItem{
		{Time: "invalid", Price: 10, Avg: 20},
		{Time: "08:00:00", Price: 10, Avg: 20},
		{Time: "12:00:00", Price: 10, Avg: 20},
		{Time: "09:30:00", Price: math.NaN(), Avg: 20},
		{Time: "09:31:00", Price: 10, Avg: math.Inf(1)},
		{Time: "09:32:00", Price: -1, Avg: 20},
		{Time: "09:33:00", Price: 10.5, Avg: 20},
		{Time: "09:34:00", Price: 90, Avg: 20},
		{Time: "09:35:00", Price: 0, Avg: 0},
		{Time: "09:36:00", Price: 0, Avg: 100},
	}}
	riseFall := breadthChart("880005", day)
	limits := breadthChart("880006", proto.MACTickChartDay{Date: day.Date,
		Ticks: []proto.MACTickChartItem{{Time: "09:36:00", Price: 5, Avg: 2}}})
	points, err := projectBreadthTimeline(riseFall, limits, breadthTime("2026-10-15 18:00:00"), 100)
	if err != nil || len(points) != 1 || points[0].RiseCount != 0 || points[0].LimitUpCount != -1 || points[0].LimitDownCount != 2 {
		t.Fatalf("invalid data must be excluded while valid zero counts survive: %+v %v", points, err)
	}
	for _, invalid := range []*proto.MACTickChartsReply{nil, breadthChart("880001", day), breadthChart("880005")} {
		if _, err := projectBreadthTimeline(invalid, nil, breadthTime("2026-10-15 18:00:00"), 100); err == nil {
			t.Fatal("missing or wrong-instrument history must not become a fabricated trend")
		}
	}
}
