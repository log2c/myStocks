package main

import (
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/bensema/gotdx/proto"
	"github.com/bensema/gotdx/types"
)

type catalogItem struct {
	Code string `json:"code"`
	Name string `json:"name"`
	Kind string `json:"kind"`
}

var catalog []catalogItem
var catalogExpires time.Time
var catalogWarning string
var boardLists = map[uint16][]proto.MACBoardListItem{}
var boardsExpires time.Time

func canonical(market uint16, code string, extended bool) string {
	family := "main"
	if extended {
		family = "ex"
	}
	return fmt.Sprintf("tdx:%s:%d:%s", family, market, strings.ToUpper(code))
}
func parseCanonical(raw string) (symbol, error) {
	p := strings.Split(strings.TrimSpace(raw), ":")
	if len(p) != 4 || strings.ToLower(p[0]) != "tdx" || (strings.ToLower(p[1]) != "main" && strings.ToLower(p[1]) != "ex") {
		return symbol{}, fmt.Errorf("invalid TDX code: %s", raw)
	}
	market, err := strconv.ParseUint(p[2], 10, 8)
	if err != nil {
		return symbol{}, fmt.Errorf("invalid TDX market")
	}
	code := strings.ToUpper(p[3])
	if code == "" || len(code) > 22 {
		return symbol{}, fmt.Errorf("invalid TDX symbol")
	}
	for _, r := range code {
		if !(r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '_' || r == '-' || r == '.') {
			return symbol{}, fmt.Errorf("invalid TDX symbol")
		}
	}
	return symbol{uint8(market), code, strings.ToLower(p[1]) == "ex"}, nil
}
func boards(kind uint16) ([]proto.MACBoardListItem, error) {
	if time.Now().After(boardsExpires) {
		boardLists = map[uint16][]proto.MACBoardListItem{}
		boardsExpires = time.Now().Add(30 * time.Minute)
	}
	if list, ok := boardLists[kind]; ok {
		return list, nil
	}
	list, err := client(false).MACBoardList(kind, 10000)
	if err != nil {
		reset(false)
		return nil, err
	}
	boardLists[kind] = list
	return list, nil
}
func resolveNamed(code, name string) (symbol, error) {
	s, err := resolve(code)
	if err == nil {
		return s, nil
	}
	if !strings.HasPrefix(strings.ToUpper(code), "90.BK") {
		return s, err
	}
	if strings.TrimSpace(name) == "" {
		return s, fmt.Errorf("legacy EastMoney board %s requires a name; search it again using TDX", code)
	}
	list, e := boards(types.BoardTypeAll)
	if e != nil {
		return s, e
	}
	matches := []proto.MACBoardListItem{}
	for _, b := range list {
		if b.Name == strings.TrimSpace(name) {
			matches = append(matches, b)
		}
	}
	if len(matches) != 1 {
		return s, fmt.Errorf("cannot uniquely match EastMoney board %s (%s); search it again using TDX", code, name)
	}
	return symbol{uint8(matches[0].Market), matches[0].Code, false}, nil
}

// ExCount is an advertised bound: live servers can end the directory before it.
// Advance by the actual page size, including when the server caps a request.
func extendedCatalog(total uint32, page func(uint32, uint16) ([]proto.ExListItem, error)) ([]catalogItem, error) {
	if total > 200000 {
		return nil, fmt.Errorf("TDX extended catalog is unexpectedly large")
	}
	result := []catalogItem{}
	for start := uint32(0); start < total; {
		count := min(uint32(1000), total-start)
		list, err := page(start, uint16(count))
		if err != nil {
			return nil, err
		}
		if len(list) == 0 {
			break
		}
		for _, item := range list {
			result = append(result, catalogItem{canonical(uint16(item.Category), item.Code, true), item.Name, "扩展市场"})
		}
		start += uint32(len(list))
	}
	return result, nil
}

type catalogLoader struct {
	stocks   func(uint8) ([]proto.Security, error)
	boards   func() ([]proto.MACBoardListItem, error)
	extended func() ([]catalogItem, error)
}

func loadCatalog() error {
	return loadCatalogWith(catalogLoader{
		stocks: func(market uint8) ([]proto.Security, error) {
			list, err := client(false).StockAll(market)
			if err != nil {
				reset(false)
			}
			return list, err
		},
		boards: func() ([]proto.MACBoardListItem, error) { return boards(types.BoardTypeAll) },
		extended: func() ([]catalogItem, error) {
			total, err := client(true).ExCount()
			if err == nil {
				var list []catalogItem
				list, err = extendedCatalog(total, client(true).ExList)
				if err == nil {
					return list, nil
				}
			}
			reset(true)
			return nil, err
		},
	})
}

func loadCatalogWith(loader catalogLoader) error {
	if len(catalog) > 0 && time.Now().Before(catalogExpires) {
		return nil
	}
	result := []catalogItem{}
	var failures []error
	// A failed source must not discard successful directories from other markets.
	for _, market := range []uint8{0, 1, 2} {
		list, err := loader.stocks(market)
		if err != nil {
			failures = append(failures, fmt.Errorf("TDX market %d: %w", market, err))
			continue
		}
		for _, item := range list {
			code, kind := fmt.Sprintf("%d.%s", market, item.Code), "证券"
			if market == 1 && strings.HasPrefix(item.Code, "88") {
				code = canonical(uint16(market), item.Code, false)
				kind = "板块"
			}
			result = append(result, catalogItem{code, item.Name, kind})
		}
	}
	list, err := loader.boards()
	if err != nil {
		failures = append(failures, fmt.Errorf("TDX boards: %w", err))
	} else {
		for _, item := range list {
			result = append(result, catalogItem{canonical(item.Market, item.Code, false), item.Name, "板块"})
		}
	}
	extended, err := loader.extended()
	if err != nil {
		failures = append(failures, fmt.Errorf("TDX extended market: %w", err))
	} else {
		result = append(result, extended...)
	}
	if len(result) == 0 && len(failures) > 0 {
		return errors.Join(failures...)
	}
	unique := make(map[string]bool)
	dedup := []catalogItem{}
	for _, item := range result {
		if !unique[item.Code] {
			unique[item.Code] = true
			dedup = append(dedup, item)
		}
	}
	catalog = dedup
	catalogWarning = ""
	catalogExpires = time.Now().Add(30 * time.Minute)
	if len(failures) > 0 {
		catalogWarning = errors.Join(failures...).Error()
		// Retry unavailable sources soon rather than caching a partial directory for 30 minutes.
		catalogExpires = time.Now().Add(time.Minute)
	}
	return nil
}
func search(req request) (any, error) {
	key := strings.ToUpper(strings.TrimSpace(req.Keyword))
	if key == "" {
		return map[string]any{"items": []catalogItem{}}, nil
	}
	if err := loadCatalog(); err != nil {
		return nil, err
	}
	result := []catalogItem{}
	for _, item := range catalog {
		if strings.Contains(strings.ToUpper(item.Name), key) || strings.Contains(strings.ToUpper(item.Code), key) {
			result = append(result, item)
		}
	}
	score := func(item catalogItem) int {
		symbol := item.Code
		if i := strings.LastIndexAny(symbol, ":."); i >= 0 {
			symbol = symbol[i+1:]
		}
		if symbol == key || strings.ToUpper(item.Name) == key {
			return 0
		}
		if strings.HasPrefix(symbol, key) || strings.HasPrefix(strings.ToUpper(item.Name), key) {
			return 1
		}
		return 2
	}
	sort.SliceStable(result, func(i, j int) bool {
		a, b := score(result[i]), score(result[j])
		if a != b {
			return a < b
		}
		return result[i].Code < result[j].Code
	})
	limit := int(req.Count)
	if limit < 1 || limit > 100 {
		limit = 30
	}
	if len(result) > limit {
		result = result[:limit]
	}
	return map[string]any{"items": result, "warning": catalogWarning}, nil
}
