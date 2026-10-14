package main

import (
	"fmt"
	"net"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/bensema/gotdx"
)

type serverGroup struct {
	Mode      string   `json:"mode"`
	Address   string   `json:"address"`
	Addresses []string `json:"addresses"`
}
type serverConfig struct {
	Main serverGroup `json:"main"`
	Ex   serverGroup `json:"ex"`
}
type serverProbe struct {
	Address   string   `json:"address"`
	Reachable bool     `json:"reachable"`
	LatencyMS *float64 `json:"latencyMs"`
	Error     string   `json:"error,omitempty"`
}
type serverRoute struct {
	ordered    []string
	expires    time.Time
	failed     map[string]time.Time
	tunnels    []*proxyTunnel
	remote     map[string]string
	generation uint64
}

var activeServers serverConfig
var mainRoute, exRoute serverRoute

func route(extended bool) *serverRoute {
	if extended {
		return &exRoute
	}
	return &mainRoute
}
func group(extended bool) serverGroup {
	if extended {
		return activeServers.Ex
	}
	return activeServers.Main
}
func defaultHosts(extended bool) []gotdx.HostInfo {
	if extended {
		return gotdx.MACExHosts()
	}
	return gotdx.MACHosts()
}
func normalizeAddress(address string) (string, error) {
	address = strings.TrimSpace(address)
	host, portText, err := net.SplitHostPort(address)
	port, portErr := strconv.Atoi(portText)
	if err != nil || portErr != nil || port < 1 || port > 65535 || host == "" || strings.ContainsAny(host, " \t\r\n/@?#") {
		return "", fmt.Errorf("invalid server address %q; use host:port", address)
	}
	return net.JoinHostPort(strings.ToLower(host), strconv.Itoa(port)), nil
}
func normalizeGroup(g serverGroup, extended bool) (serverGroup, error) {
	if g.Mode == "" {
		g.Mode = "auto"
	}
	if g.Mode != "auto" && g.Mode != "manual" {
		return g, fmt.Errorf("invalid server selection mode")
	}
	if len(g.Addresses) == 0 {
		for _, h := range defaultHosts(extended) {
			g.Addresses = append(g.Addresses, h.Address())
		}
	}
	if len(g.Addresses) > 64 {
		return g, fmt.Errorf("server pool exceeds 64 addresses")
	}
	seen := make(map[string]bool)
	pool := make([]string, 0, len(g.Addresses))
	for _, a := range g.Addresses {
		a, err := normalizeAddress(a)
		if err != nil {
			return g, err
		}
		if !seen[a] {
			pool = append(pool, a)
			seen[a] = true
		}
	}
	g.Addresses = pool
	if g.Address != "" {
		var err error
		g.Address, err = normalizeAddress(g.Address)
		if err != nil {
			return g, err
		}
	}
	if g.Mode == "manual" && !seen[g.Address] {
		return g, fmt.Errorf("select a server from the address pool")
	}
	return g, nil
}
func configureServers(config serverConfig) error {
	var err error
	config.Main, err = normalizeGroup(config.Main, false)
	if err != nil {
		return err
	}
	config.Ex, err = normalizeGroup(config.Ex, true)
	if err != nil {
		return err
	}
	for _, extended := range []bool{false, true} {
		next := config.Main
		if extended {
			next = config.Ex
		}
		if !reflect.DeepEqual(group(extended), next) {
			dropClient(extended, false)
			r := route(extended)
			r.ordered, r.failed = nil, nil
			r.expires = time.Time{}
		}
	}
	activeServers = config
	return nil
}

// Probe through the same proxy as quotation traffic; TCP timing is not ICMP ping.
func probeServers(addresses []string, proxy proxyConfig) []serverProbe {
	results := make([]serverProbe, len(addresses))
	var wg sync.WaitGroup
	semaphore := make(chan struct{}, 8)
	for i, address := range addresses {
		wg.Add(1)
		go func(i int, address string) {
			defer wg.Done()
			semaphore <- struct{}{}
			defer func() { <-semaphore }()
			start := time.Now()
			conn, err := dialProxy(proxy, address, 3*time.Second)
			result := serverProbe{Address: address}
			if err == nil {
				latency := float64(time.Since(start).Microseconds()) / 1000
				result.Reachable, result.LatencyMS = true, &latency
				conn.Close()
			} else {
				result.Error = err.Error()
			}
			results[i] = result
		}(i, address)
	}
	wg.Wait()
	sort.SliceStable(results, func(i, j int) bool {
		if results[i].Reachable != results[j].Reachable {
			return results[i].Reachable
		}
		if !results[i].Reachable {
			return false
		}
		return *results[i].LatencyMS < *results[j].LatencyMS
	})
	return results
}
func serverOrder(extended bool) []string {
	g, r := group(extended), route(extended)
	if g.Mode == "manual" {
		return []string{g.Address}
	}
	now := time.Now()
	if now.Before(r.expires) && len(r.ordered) != 0 {
		return r.ordered
	}
	candidates := make([]string, 0, len(g.Addresses))
	for _, a := range g.Addresses {
		if !now.Before(r.failed[a]) {
			candidates = append(candidates, a)
		}
	}
	// Retry the pool when every entry is cooling down, so recovered nodes return.
	if len(candidates) == 0 {
		candidates = g.Addresses
	}
	r.ordered = nil
	for _, p := range probeServers(candidates, activeProxy) {
		if p.Reachable {
			r.ordered = append(r.ordered, p.Address)
		}
	}
	r.expires = now.Add(5 * time.Minute)
	return r.ordered
}
func serverOptions(extended bool) []gotdx.Option {
	addresses := serverOrder(extended)
	r := route(extended)
	r.remote = make(map[string]string)
	local := make([]string, 0, len(addresses))
	for _, address := range addresses {
		if activeProxy.enabled() || activeProxy.Interface != "" {
			t, err := newTunnel(activeProxy, []string{address})
			if err != nil {
				continue
			}
			r.tunnels = append(r.tunnels, t)
			a := t.listener.Addr().String()
			r.remote[a] = address
			local = append(local, a)
		} else {
			local = append(local, address)
		}
	}
	// Clear the SDK defaults even if a local tunnel could not be created.
	address := ""
	if len(local) > 0 {
		address = local[0]
		local = local[1:]
	}
	if extended {
		return []gotdx.Option{gotdx.WithMacExTCPAddress(address), gotdx.WithMacExTCPAddressPool(local...), gotdx.WithAutoSelectFastest(false)}
	}
	return []gotdx.Option{gotdx.WithMacTCPAddress(address), gotdx.WithMacTCPAddressPool(local...), gotdx.WithAutoSelectFastest(false)}
}
func currentServer(extended bool) string {
	c := mainClient
	if extended {
		c = exClient
	}
	if c == nil {
		return ""
	}
	a := c.CurrentAddress()
	if remote := route(extended).remote[a]; remote != "" {
		return remote
	}
	return a
}
func dropClient(extended, failed bool) {
	r := route(extended)
	if failed {
		if address := currentServer(extended); address != "" {
			if r.failed == nil {
				r.failed = make(map[string]time.Time)
			}
			r.failed[address] = time.Now().Add(time.Minute)
			r.generation++
		}
		r.expires = time.Time{}
	}
	slot := &mainClient
	if extended {
		slot = &exClient
	}
	if *slot != nil {
		(*slot).Disconnect()
		*slot = nil
	}
	for _, t := range r.tunnels {
		t.close()
	}
	r.tunnels, r.remote = nil, nil
}
func listServers() any {
	groups := make(map[string]any)
	for _, extended := range []bool{false, true} {
		key := "main"
		if extended {
			key = "ex"
		}
		hosts := make([]map[string]string, 0)
		for _, host := range defaultHosts(extended) {
			hosts = append(hosts, map[string]string{"name": host.Name, "address": host.Address()})
		}
		groups[key] = map[string]any{"hosts": hosts, "current": currentServer(extended)}
	}
	return groups
}
func testServers(req request) (any, error) {
	if err := req.Proxy.validate(); err != nil {
		return nil, err
	}
	if req.Kind != "main" && req.Kind != "ex" {
		return nil, fmt.Errorf("select main or ex server group")
	}
	g := req.Servers.Main
	if req.Kind == "ex" {
		g = req.Servers.Ex
	}
	g.Mode = "auto" // Testing does not require a manually selected node.
	g, err := normalizeGroup(g, req.Kind == "ex")
	if err != nil {
		return nil, err
	}
	return map[string]any{"results": probeServers(g.Addresses, req.Proxy), "current": currentServer(req.Kind == "ex")}, nil
}

// Retry once only after a transport reset. Invalid symbols and empty data do not
// trigger node rotation; successful parts of a quote batch remain intact.
func dispatchWithFailover(req request) (any, error) {
	mainGeneration, exGeneration := mainRoute.generation, exRoute.generation
	result, err := dispatch(req)
	failed := (mainGeneration != mainRoute.generation && group(false).Mode == "auto") ||
		(exGeneration != exRoute.generation && group(true).Mode == "auto")
	if !failed {
		return result, err
	}
	if err != nil {
		return dispatch(req)
	}
	if req.Operation != "quotes" {
		return result, err
	}
	first := result.(map[string]any)
	collected := first["quotes"].([]quote)
	seen := make(map[string]bool)
	for _, q := range collected {
		seen[q.Code] = true
	}
	missing := make([]stock, 0)
	for _, s := range req.Stocks {
		if !seen[s.Code] {
			missing = append(missing, s)
		}
	}
	if len(missing) == 0 {
		return result, err
	}
	req.Stocks = missing
	second := quotes(req)
	second["quotes"] = append(collected, second["quotes"].([]quote)...)
	return second, nil
}

// Startup/reload preparation probes the configured pool, then verifies the MAC
// quotation protocol before committing a route. A successful TCP connection
// alone can be a proxy or an unrelated service, so it is not considered ready.
func prepareServers(req request) (any, error) {
	if req.Kind != "main" && req.Kind != "ex" {
		return nil, fmt.Errorf("select main or ex server group")
	}
	if err := configureProxy(req.Proxy); err != nil {
		return nil, err
	}
	if req.CalendarPath != calendarPath {
		dropClient(false, false)
		dropClient(true, false)
		calendarPath = req.CalendarPath
	}
	if _, err := interfaceDialer(req.Proxy.Interface, 3*time.Second); err != nil {
		return nil, err
	}
	extended := req.Kind == "ex"
	configured := req.Servers.Main
	if extended {
		configured = req.Servers.Ex
	}
	mode, previous := configured.Mode, strings.TrimSpace(configured.Address)
	if mode == "" {
		mode = "auto"
	}
	if mode != "auto" && mode != "manual" {
		return nil, fmt.Errorf("invalid server selection mode")
	}
	// A stale manual selection must not prevent testing the rest of the pool.
	configured.Mode, configured.Address = "auto", ""
	g, err := normalizeGroup(configured, extended)
	if err != nil {
		return nil, err
	}
	if normalized, err := normalizeAddress(previous); err == nil {
		previous = normalized
	}
	probes := probeServers(g.Addresses, req.Proxy)
	candidates := make([]serverProbe, 0, len(probes))
	for _, probe := range probes {
		if probe.Reachable {
			candidates = append(candidates, probe)
		}
	}
	// Respect a working manual selection even when another node is faster.
	if mode == "manual" {
		for i, candidate := range candidates {
			if candidate.Address == previous {
				candidates = append([]serverProbe{candidate}, append(candidates[:i], candidates[i+1:]...)...)
				break
			}
		}
	}
	dropClient(extended, false)
	if extended {
		activeServers.Ex = g
	} else {
		activeServers.Main = g
	}
	r := route(extended)
	r.failed = make(map[string]time.Time)
	for _, candidate := range candidates {
		// Limit each protocol validation to this one node. SDK fallback must not
		// silently accept a different address than the one being measured.
		r.ordered, r.expires = []string{candidate.Address}, time.Now().Add(5*time.Minute)
		opts := append(serverOptions(extended), gotdx.WithTimeoutSec(3), gotdx.WithFuturesCalendarPath(calendarPath))
		var c *gotdx.Client
		market, code := uint8(1), "000001"
		if extended {
			c = gotdx.NewMACEx(opts...)
			market, code = 27, "HSI"
			exClient = c
		} else {
			c = gotdx.NewMAC(opts...)
			mainClient = c
		}
		_, err := c.MACSymbolQuotes([]uint8{market}, []string{code}, gotdx.MACFieldBitmap(gotdx.MACFieldClose))
		if err != nil {
			r.failed[candidate.Address] = time.Now().Add(time.Minute)
			dropClient(extended, false)
			continue
		}
		g.Mode, g.Address = mode, candidate.Address
		if extended {
			activeServers.Ex = g
		} else {
			activeServers.Main = g
		}
		r.ordered = []string{candidate.Address}
		for _, other := range candidates {
			if other.Address != candidate.Address && r.failed[other.Address].IsZero() {
				r.ordered = append(r.ordered, other.Address)
			}
		}
		r.expires = time.Now().Add(5 * time.Minute)
		return map[string]any{
			"address": candidate.Address, "latencyMs": candidate.LatencyMS,
			"previous": previous, "switched": mode == "manual" && previous != candidate.Address,
			"protocolVerified": true,
		}, nil
	}
	r.ordered, r.expires = nil, time.Time{}
	return nil, fmt.Errorf("no usable %s TDX route; check the address pool or proxy", req.Kind)
}
