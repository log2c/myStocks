package main

import (
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
func loadCatalog() error {
	if len(catalog) > 0 && time.Now().Before(catalogExpires) {
		return nil
	}
	result := []catalogItem{}
	// Build into a fresh slice so a failed page cannot mark an incomplete list cached.
	for _, market := range []uint8{0, 1, 2} {
		list, err := client(false).StockAll(market)
		if err != nil {
			reset(false)
			return err
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
	list, err := boards(types.BoardTypeAll)
	if err != nil {
		return err
	}
	for _, item := range list {
		result = append(result, catalogItem{canonical(item.Market, item.Code, false), item.Name, "板块"})
	}
	total, err := client(true).ExCount()
	if err != nil {
		reset(true)
		return err
	}
	if total > 200000 {
		return fmt.Errorf("TDX extended catalog is unexpectedly large")
	}
	for start := uint32(0); start < total; start += 1000 {
		list, err := client(true).ExList(start, 1000)
		if err != nil {
			reset(true)
			return err
		}
		if len(list) == 0 {
			return fmt.Errorf("TDX extended catalog ended before declared total")
		}
		for _, item := range list {
			result = append(result, catalogItem{canonical(uint16(item.Category), item.Code, true), item.Name, "扩展市场"})
		}
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
	catalogExpires = time.Now().Add(30 * time.Minute)
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
	return map[string]any{"items": result}, nil
}
