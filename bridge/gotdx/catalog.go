package main

import (
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/bensema/gotdx"
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

// catalogMu guards the directory caches. It is never held across an exchange.
var catalogMu sync.Mutex

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
	catalogMu.Lock()
	if time.Now().After(boardsExpires) {
		boardLists = map[uint16][]proto.MACBoardListItem{}
		boardsExpires = time.Now().Add(30 * time.Minute)
	}
	if list, ok := boardLists[kind]; ok {
		catalogMu.Unlock()
		return list, nil
	}
	catalogMu.Unlock()
	// Board lists are a directory scan: keep them off the quotation lane.
	var list []proto.MACBoardListItem
	if err := withScan(false, func(c *gotdx.Client) error {
		var err error
		list, err = c.MACBoardList(kind, 10000)
		return err
	}); err != nil {
		return nil, err
	}
	catalogMu.Lock()
	boardLists[kind] = list
	catalogMu.Unlock()
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
	// partial publishes a usable directory before the extended market scan
	// finishes, and progressive lets the loader return at that point. The
	// extended directory is ~110k rows in ~1021-row pages, so waiting for it
	// turned a cold search into a six second freeze.
	partial     func([]catalogItem)
	progressive bool
}

func loadCatalog() error {
	return loadCatalogWith(catalogLoader{
		stocks: func(market uint8) ([]proto.Security, error) {
			var list []proto.Security
			err := withScan(false, func(c *gotdx.Client) error {
				var err error
				list, err = c.StockAll(market)
				return err
			})
			return list, err
		},
		boards: func() ([]proto.MACBoardListItem, error) { return boards(types.BoardTypeAll) },
		extended: func() ([]catalogItem, error) {
			var list []catalogItem
			// The extended directory is the expensive one: ~110k rows in
			// 1021-row pages, so it gets the lane that may be slow.
			err := withScan(true, func(c *gotdx.Client) error {
				total, err := c.ExCount()
				if err != nil {
					return err
				}
				list, err = extendedCatalog(total, c.ExList)
				return err
			})
			return list, err
		},
		partial: func(items []catalogItem) {
			catalogMu.Lock()
			defer catalogMu.Unlock()
			catalog = items
			// The extended scan replaces this soon. The short window also means a
			// scan that dies without publishing is retried by the next search.
			catalogExpires = time.Now().Add(2 * time.Minute)
			catalogWarning = errCatalogScanning.Error()
		},
		progressive: true,
	})
}

var errCatalogScanning = errors.New("扩展市场目录仍在加载")

// A scan publishes the mainland and board directory first, then finishes the
// extended market in the background. catalogScanMu makes the start exclusive and
// catalogFirstResult is closed on the first publication, so a search that
// arrives mid-scan waits for data instead of reporting "no matches".
var (
	catalogScanMu      sync.Mutex
	catalogScanRunning bool
	catalogFirstResult chan struct{}
)

func finishScan() {
	catalogScanMu.Lock()
	catalogScanRunning = false
	catalogFirstResult = nil
	catalogScanMu.Unlock()
}

func loadCatalogWith(loader catalogLoader) error {
	// A bounded loop: wait out a scan already in progress, and retry locally if
	// that scan failed without publishing anything.
	for attempt := 0; attempt < 3; attempt++ {
		if catalogReady() {
			return nil
		}
		catalogScanMu.Lock()
		if catalogScanRunning {
			wait := catalogFirstResult
			catalogScanMu.Unlock()
			<-wait
			continue
		}
		first := make(chan struct{})
		catalogScanRunning, catalogFirstResult = true, first
		catalogScanMu.Unlock()
		return runCatalogScan(loader, first)
	}
	return nil
}

func runCatalogScan(loader catalogLoader, first chan struct{}) error {
	// The three sources are independent connections; fetch them together so a
	// first search costs the slowest source instead of the sum of all three.
	var (
		mainland  [3][]proto.Security
		mainErr   [3]error
		boardList []proto.MACBoardListItem
		boardErr  error
		extended  []catalogItem
		extErr    error
	)
	var wg sync.WaitGroup
	var mainDone sync.WaitGroup
	mainDone.Add(4)
	for i, market := range []uint8{0, 1, 2} {
		go func(i int, market uint8) {
			defer mainDone.Done()
			mainland[i], mainErr[i] = loader.stocks(market)
		}(i, market)
	}
	go func() { defer mainDone.Done(); boardList, boardErr = loader.boards() }()
	wg.Add(1)
	go func() { defer wg.Done(); extended, extErr = loader.extended() }()

	// Publish mainland and board results as soon as they are ready. Searching a
	// mainland code or a sector should not wait for Hong Kong and futures.
	mainDone.Wait()
	var partial []catalogItem
	if loader.partial != nil {
		var failures []error
		partial, failures = composeCatalog(mainland, mainErr, boardList, boardErr, nil)
		if len(partial) == 0 || len(failures) != 0 {
			partial = nil
		} else {
			loader.partial(partial)
			close(first)
		}
	}
	if loader.progressive && partial != nil {
		// A stock search is answered from what is already published; the
		// extended directory lands later and replaces this snapshot.
		go func() {
			wg.Wait()
			defer finishScan()
			_ = publishCatalog(loader, mainland, mainErr, boardList, boardErr, extended, extErr)
		}()
		return nil
	}
	wg.Wait()
	defer finishScan()
	if partial == nil {
		// Nothing was published early, so this result is the first data a waiter
		// can use; a failure still has to release them with an error.
		defer close(first)
	}
	return publishCatalog(loader, mainland, mainErr, boardList, boardErr, extended, extErr)
}

func publishCatalog(
	loader catalogLoader,
	mainland [3][]proto.Security,
	mainErr [3]error,
	boardList []proto.MACBoardListItem,
	boardErr error,
	extended []catalogItem,
	extErr error,
) error {
	result, failures := composeCatalog(mainland, mainErr, boardList, boardErr, &extended)
	if extErr != nil {
		// Reported separately: the extended directory is optional for a search
		// that already matched a mainland or board code.
		failures = append(failures, fmt.Errorf("TDX extended market: %w", extErr))
	}
	if len(result) == 0 && len(failures) > 0 {
		return errors.Join(failures...)
	}
	expires := time.Now().Add(30 * time.Minute)
	warning := ""
	if len(failures) > 0 {
		warning = errors.Join(failures...).Error()
		// Retry unavailable sources soon rather than caching a partial directory for 30 minutes.
		expires = time.Now().Add(time.Minute)
	}
	catalogMu.Lock()
	catalog, catalogWarning, catalogExpires = result, warning, expires
	catalogMu.Unlock()
	return nil
}

// composeCatalog merges the fetched sources, keeping every source that worked.
func composeCatalog(
	mainland [3][]proto.Security,
	mainErr [3]error,
	boardList []proto.MACBoardListItem,
	boardErr error,
	extended *[]catalogItem,
) ([]catalogItem, []error) {
	result := []catalogItem{}
	var failures []error
	for i, market := range []uint8{0, 1, 2} {
		if err := mainErr[i]; err != nil {
			failures = append(failures, fmt.Errorf("TDX market %d: %w", market, err))
			continue
		}
		for _, item := range mainland[i] {
			code, kind := fmt.Sprintf("%d.%s", market, item.Code), "证券"
			if market == 1 && strings.HasPrefix(item.Code, "88") {
				code = canonical(uint16(market), item.Code, false)
				kind = "板块"
			}
			result = append(result, catalogItem{code, item.Name, kind})
		}
	}
	if boardErr != nil {
		failures = append(failures, fmt.Errorf("TDX boards: %w", boardErr))
	} else {
		for _, item := range boardList {
			result = append(result, catalogItem{canonical(item.Market, item.Code, false), item.Name, "板块"})
		}
	}
	if extended != nil {
		result = append(result, *extended...)
	}
	unique := make(map[string]bool)
	dedup := make([]catalogItem, 0, len(result))
	for _, item := range result {
		if !unique[item.Code] {
			unique[item.Code] = true
			dedup = append(dedup, item)
		}
	}
	return dedup, failures
}

func catalogReady() bool {
	catalogMu.Lock()
	defer catalogMu.Unlock()
	return len(catalog) > 0 && time.Now().Before(catalogExpires)
}

func catalogSnapshot() []catalogItem {
	catalogMu.Lock()
	defer catalogMu.Unlock()
	return catalog
}

func catalogExpiry() time.Time {
	catalogMu.Lock()
	defer catalogMu.Unlock()
	return catalogExpires
}

func resetCatalogExpiry() {
	catalogMu.Lock()
	catalogExpires = time.Time{}
	catalogMu.Unlock()
}

func catalogDiagnostic() string {
	catalogMu.Lock()
	defer catalogMu.Unlock()
	return catalogWarning
}

// warmCatalog loads the directory in the background so the first stock search
// does not pay for a full scan. A failure is not reported here: the search that
// needs the data will retry and surface the error.
func warmCatalog() {
	go func() {
		_ = loadCatalog()
	}()
}

func search(req request) (any, error) {
	key := strings.ToUpper(strings.TrimSpace(req.Keyword))
	if key == "" {
		return map[string]any{"items": []catalogItem{}}, nil
	}
	if err := loadCatalog(); err != nil {
		return nil, err
	}
	items := catalogSnapshot()
	result := []catalogItem{}
	for _, item := range items {
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
	return map[string]any{"items": result, "warning": catalogDiagnostic()}, nil
}
