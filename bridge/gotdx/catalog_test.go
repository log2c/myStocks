package main

import (
	"errors"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/bensema/gotdx/proto"
)

func isolateCatalog(t *testing.T) {
	t.Helper()
	old, expiry, warning := catalog, catalogExpires, catalogWarning
	catalog, catalogExpires, catalogWarning = nil, time.Time{}, ""
	t.Cleanup(func() { catalog, catalogExpires, catalogWarning = old, expiry, warning })
}

func TestExtendedCatalogAdvertisedTotalExceedsDirectory(t *testing.T) {
	var starts []uint32
	items, err := extendedCatalog(2500, func(start uint32, count uint16) ([]proto.ExListItem, error) {
		starts = append(starts, start)
		if count != 1000 {
			t.Fatalf("unexpected request count: %d", count)
		}
		// The server caps each page below the requested size and ends early.
		switch start {
		case 0:
			return []proto.ExListItem{{Category: 31, Code: "00700", Name: "腾讯控股"}}, nil
		case 1:
			return []proto.ExListItem{{Category: 30, Code: "AUL8", Name: "沪金主连"}}, nil
		default:
			return nil, nil
		}
	})
	if err != nil || len(items) != 2 || !reflect.DeepEqual(starts, []uint32{0, 1, 2}) {
		t.Fatalf("items=%+v starts=%v err=%v", items, starts, err)
	}
	if items[0].Code != "tdx:ex:31:00700" || items[1].Code != "tdx:ex:30:AUL8" {
		t.Fatalf("extended identifiers lost: %+v", items)
	}
}

func TestExtendedCatalogPageFailureAndBounds(t *testing.T) {
	calls := 0
	page := func(uint32, uint16) ([]proto.ExListItem, error) {
		calls++
		if calls == 1 {
			return []proto.ExListItem{{Category: 31, Code: "00700"}}, nil
		}
		return nil, errors.New("page failed")
	}
	if items, err := extendedCatalog(2, page); err == nil || items != nil {
		t.Fatalf("failed pages must not become a complete directory: %+v %v", items, err)
	}
	calls = 0
	if _, err := extendedCatalog(200001, page); err == nil || calls != 0 {
		t.Fatal("oversized advertised totals must fail before fetching pages")
	}
	if items, err := extendedCatalog(0, page); err != nil || len(items) != 0 || calls != 0 {
		t.Fatal("an empty directory must not fetch pages")
	}
}

func TestCatalogKeepsSuccessfulSourcesWhenOthersFail(t *testing.T) {
	isolateCatalog(t)
	loader := catalogLoader{
		stocks: func(market uint8) ([]proto.Security, error) {
			if market == 1 {
				return []proto.Security{{Code: "600519", Name: "贵州茅台"}}, nil
			}
			return nil, errors.New("market unavailable")
		},
		boards:   func() ([]proto.MACBoardListItem, error) { return nil, errors.New("boards unavailable") },
		extended: func() ([]catalogItem, error) { return nil, errors.New("extended unavailable") },
	}
	if err := loadCatalogWith(loader); err != nil {
		t.Fatal(err)
	}
	response, err := search(request{Keyword: "600519", Count: 20})
	if err != nil {
		t.Fatal(err)
	}
	result := response.(map[string]any)
	items := result["items"].([]catalogItem)
	if len(items) != 1 || items[0].Code != "1.600519" || !strings.Contains(result["warning"].(string), "extended unavailable") {
		t.Fatalf("available results or warning missing: %+v", result)
	}
	if time.Until(catalogExpires) > time.Minute {
		t.Fatal("unavailable sources must be retried before the normal cache expiry")
	}
	// Retry after a transient failure, then restore the full cache lifetime.
	catalogExpires = time.Time{}
	loader.stocks = func(uint8) ([]proto.Security, error) { return nil, nil }
	loader.boards = func() ([]proto.MACBoardListItem, error) { return nil, nil }
	loader.extended = func() ([]catalogItem, error) {
		return []catalogItem{{Code: "tdx:ex:31:00700", Name: "腾讯控股", Kind: "扩展市场"}}, nil
	}
	if err := loadCatalogWith(loader); err != nil || catalogWarning != "" || time.Until(catalogExpires) < 29*time.Minute {
		t.Fatalf("recovered catalog should have no warning: %v %s", err, catalogWarning)
	}
	if len(catalog) != 1 || catalog[0].Code != "tdx:ex:31:00700" {
		t.Fatalf("recovered source missing: %+v", catalog)
	}
}

func TestCatalogAllSourcesFail(t *testing.T) {
	isolateCatalog(t)
	loader := catalogLoader{
		stocks:   func(uint8) ([]proto.Security, error) { return nil, errors.New("stocks failed") },
		boards:   func() ([]proto.MACBoardListItem, error) { return nil, errors.New("boards failed") },
		extended: func() ([]catalogItem, error) { return nil, errors.New("extended failed") },
	}
	if err := loadCatalogWith(loader); err == nil || !strings.Contains(err.Error(), "stocks failed") || !strings.Contains(err.Error(), "extended failed") {
		t.Fatalf("missing directory failures: %v", err)
	}
	if len(catalog) != 0 || !catalogExpires.IsZero() {
		t.Fatal("failed catalog must not be cached")
	}
}
