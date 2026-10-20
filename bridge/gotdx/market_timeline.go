package main

import (
	"fmt"
	"math"
	"sort"
	"time"

	"github.com/bensema/gotdx/proto"
)

type breadthTimelinePoint struct {
	TimestampMs    int64 `json:"timestampMs"`
	RiseCount      int   `json:"riseCount"`
	FallCount      int   `json:"fallCount"`
	LimitUpCount   int   `json:"limitUpCount"`
	LimitDownCount int   `json:"limitDownCount"`
}

func marketBreadthTimeline(asOf time.Time, total int) ([]breadthTimelinePoint, error) {
	// For these two statistical instruments, Price/Avg contain counts rather
	// than security prices: 880005 = rise/fall, 880006 = limit up/down.
	riseFall, err := client(false).MACTickCharts(1, "880005", 0, 1)
	if err != nil {
		return nil, fmt.Errorf("TDX breadth timeline: %w", err)
	}
	limits, limitErr := client(false).MACTickCharts(1, "880006", 0, 1)
	points, err := projectBreadthTimeline(riseFall, limits, asOf, total)
	if err != nil {
		return nil, err
	}
	if limitErr != nil {
		return points, fmt.Errorf("TDX limit-count timeline: %w", limitErr)
	}
	return points, nil
}

func breadthCount(value float64, total int) int {
	if math.IsNaN(value) || math.IsInf(value, 0) || value < 0 || value > float64(total) || math.Trunc(value) != value {
		return -1
	}
	return int(value)
}

func breadthTradingTime(t time.Time) bool {
	minute := t.Hour()*60 + t.Minute()
	return (minute >= 570 && minute <= 690) || (minute >= 780 && minute <= 900)
}

func projectBreadthTimeline(riseFall, limits *proto.MACTickChartsReply, asOf time.Time, total int) ([]breadthTimelinePoint, error) {
	if riseFall == nil || riseFall.Market != 1 || riseFall.Code != "880005" || total <= 0 {
		return nil, fmt.Errorf("TDX returned invalid breadth timeline")
	}
	// The SDK may return more than one day even when Days=1. Pick the latest
	// valid trading day reported by the server, independent of the host timezone.
	var selected *proto.MACTickChartDay
	asOf = asOf.In(chinaZone())
	for i := range riseFall.Charts {
		day := &riseFall.Charts[i]
		date, err := time.ParseInLocation("2006-01-02", day.Date, chinaZone())
		if err != nil || date.Format("2006-01-02") > asOf.Format("2006-01-02") || len(day.Ticks) == 0 {
			continue
		}
		if selected == nil || day.Date > selected.Date {
			selected = day
		}
	}
	if selected == nil {
		return nil, fmt.Errorf("TDX returned no breadth timeline for the latest trading day")
	}

	limitByTime := make(map[string]proto.MACTickChartItem)
	if limits != nil && limits.Market == 1 && limits.Code == "880006" {
		for _, day := range limits.Charts {
			if day.Date == selected.Date {
				for _, tick := range day.Ticks {
					limitByTime[tick.Time] = tick
				}
			}
		}
	}
	byTime := make(map[int64]breadthTimelinePoint)
	for _, tick := range selected.Ticks {
		timestamp, err := time.ParseInLocation("2006-01-02 15:04:05", selected.Date+" "+tick.Time, chinaZone())
		if err != nil || !breadthTradingTime(timestamp) || timestamp.After(asOf) {
			continue
		}
		up, down := breadthCount(tick.Price, total), breadthCount(tick.Avg, total)
		if up < 0 || down < 0 || up+down == 0 || up+down > total {
			continue
		}
		point := breadthTimelinePoint{timestamp.UnixMilli(), up, down, -1, -1}
		if limit, ok := limitByTime[tick.Time]; ok {
			point.LimitUpCount = breadthCount(limit.Price, up)
			point.LimitDownCount = breadthCount(limit.Avg, down)
		}
		byTime[point.TimestampMs] = point
	}
	points := make([]breadthTimelinePoint, 0, len(byTime))
	for _, point := range byTime {
		points = append(points, point)
	}
	sort.Slice(points, func(i, j int) bool { return points[i].TimestampMs < points[j].TimestampMs })
	if len(points) == 0 {
		return nil, fmt.Errorf("TDX returned an empty breadth timeline")
	}
	return points, nil
}

func appendBreadthClose(points []breadthTimelinePoint, updated time.Time, up, down, limitUp, limitDown int) []breadthTimelinePoint {
	if len(points) == 0 || up < 0 || down < 0 || up+down == 0 {
		return points
	}
	updated = updated.In(chinaZone())
	closeTime := time.Date(updated.Year(), updated.Month(), updated.Day(), 15, 0, 0, 0, chinaZone())
	last := &points[len(points)-1]
	// MACTickCharts' final minute is 14:59. Use the actual closing snapshot
	// for 15:00, and never append a newer calendar date to an older day's chart.
	if updated.Before(closeTime) || time.UnixMilli(last.TimestampMs).In(chinaZone()).Format("2006-01-02") != updated.Format("2006-01-02") {
		return points
	}
	point := breadthTimelinePoint{closeTime.UnixMilli(), up, down, limitUp, limitDown}
	if last.TimestampMs == point.TimestampMs {
		*last = point
		return points
	}
	return append(points, point)
}
