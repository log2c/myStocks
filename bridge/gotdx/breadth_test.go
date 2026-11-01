package main

import (
	"testing"
	"time"
)

// The summary row is fed from the quote batch, so these four counts and the flat
// remainder are the whole contract the floating window depends on.
func TestBreadthCountsDerivesFlatFromTheUniverse(t *testing.T) {
	// Prime the cached universe so the test never opens a socket.
	breadthUniverseMu.Lock()
	previousValue, previousAt := breadthUniverseValue, breadthUniverseAt
	breadthUniverseValue, breadthUniverseAt = 5580, time.Now()
	breadthUniverseMu.Unlock()
	defer func() {
		breadthUniverseMu.Lock()
		breadthUniverseValue, breadthUniverseAt = previousValue, previousAt
		breadthUniverseMu.Unlock()
	}()

	values := map[string]any{
		"up_count":           float64(893),
		"down_count":         float64(4602),
		"limit_up_count":     float64(49),
		"limit_down_count":   float64(86),
		"server_update_date": float64(20261030),
		"server_update_time": float64(150002),
	}
	snapshot, err := breadthCounts(values)
	if err != nil {
		t.Fatal(err)
	}
	if snapshot["upCount"] != 893 || snapshot["downCount"] != 4602 {
		t.Fatalf("counts were not passed through: %+v", snapshot)
	}
	// 5580 - 893 - 4602 = 85
	if snapshot["flatCount"] != 85 {
		t.Fatalf("flatCount %v, want 85", snapshot["flatCount"])
	}
	if snapshot["limitUpCount"] != 49 || snapshot["limitDownCount"] != 86 {
		t.Fatalf("limit counts were not passed through: %+v", snapshot)
	}
	if snapshot["total"] != 5580 {
		t.Fatalf("total %v, want the cached universe", snapshot["total"])
	}
	if snapshot["lastUpdatedAtMs"].(int64) == 0 {
		t.Fatal("the server stamp was not converted")
	}
}

func TestBreadthCountsRejectsAMissingField(t *testing.T) {
	values := map[string]any{"up_count": float64(1), "down_count": float64(2)}
	if _, err := breadthCounts(values); err == nil {
		t.Fatal("a summary without limit counts must be rejected")
	}
}

func TestBreadthCountsRejectsCountsAboveTheUniverse(t *testing.T) {
	breadthUniverseMu.Lock()
	previousValue, previousAt := breadthUniverseValue, breadthUniverseAt
	breadthUniverseValue, breadthUniverseAt = 10, time.Now()
	breadthUniverseMu.Unlock()
	defer func() {
		breadthUniverseMu.Lock()
		breadthUniverseValue, breadthUniverseAt = previousValue, previousAt
		breadthUniverseMu.Unlock()
	}()

	values := map[string]any{
		"up_count": float64(8), "down_count": float64(8),
		"limit_up_count": float64(0), "limit_down_count": float64(0),
	}
	if _, err := breadthCounts(values); err == nil {
		t.Fatal("counts exceeding the universe must be rejected, not reported as a negative flat")
	}
}
