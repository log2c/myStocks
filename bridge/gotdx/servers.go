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

var (
	// configMu guards the settings snapshot (proxy, servers, calendar). It is
	// only ever held for short reads and writes, never across an exchange, so
	// a lane may take it while holding its own lock.
	configMu      sync.Mutex
	routeMu       sync.Mutex
	activeServers serverConfig
	mainRoute     serverRoute
	exRoute       serverRoute
)

func route(extended bool) *serverRoute {
	if extended {
		return &exRoute
	}
	return &mainRoute
}
func group(extended bool) serverGroup {
	configMu.Lock()
	defer configMu.Unlock()
	if extended {
		return activeServers.Ex
	}
	return activeServers.Main
}
func proxySnapshot() proxyConfig {
	configMu.Lock()
	defer configMu.Unlock()
	return activeProxy
}
func proxyActive() bool {
	p := proxySnapshot()
	return p.enabled() || p.Interface != ""
}
func calendarPathSnapshot() string {
	configMu.Lock()
	defer configMu.Unlock()
	return calendarPath
}
func routeGeneration(extended bool) uint64 {
	routeMu.Lock()
	defer routeMu.Unlock()
	return route(extended).generation
}
func markRouteFailure(extended bool, address string) {
	routeMu.Lock()
	defer routeMu.Unlock()
	r := route(extended)
	if address != "" {
		if r.failed == nil {
			r.failed = make(map[string]time.Time)
		}
		r.failed[address] = time.Now().Add(time.Minute)
	}
	r.expires = time.Time{}
	r.generation++
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
func normalizeServers(config serverConfig) (serverConfig, error) {
	var err error
	config.Main, err = normalizeGroup(config.Main, false)
	if err != nil {
		return config, err
	}
	config.Ex, err = normalizeGroup(config.Ex, true)
	if err != nil {
		return config, err
	}
	return config, nil
}

func configureServers(config serverConfig) error {
	configMu.Lock()
	config, err := normalizeServers(config)
	if err != nil {
		configMu.Unlock()
		return err
	}
	mainChanged := !reflect.DeepEqual(activeServers.Main, config.Main)
	exChanged := !reflect.DeepEqual(activeServers.Ex, config.Ex)
	activeServers = config
	configMu.Unlock()
	// Lanes rebuild lazily; closing them here only reclaims sockets that are
	// already idle, and the epoch bump also covers the bulk lane.
	if mainChanged {
		resetRoute(false)
		dropClient(false)
	}
	if exChanged {
		resetRoute(true)
		dropClient(true)
	}
	return nil
}

func resetRoute(extended bool) {
	routeMu.Lock()
	defer routeMu.Unlock()
	r := route(extended)
	r.ordered, r.failed = nil, nil
	r.expires = time.Time{}
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
	routeMu.Lock()
	if now.Before(r.expires) && len(r.ordered) != 0 {
		ordered := append([]string(nil), r.ordered...)
		routeMu.Unlock()
		return ordered
	}
	candidates := make([]string, 0, len(g.Addresses))
	for _, a := range g.Addresses {
		if !now.Before(r.failed[a]) {
			candidates = append(candidates, a)
		}
	}
	routeMu.Unlock()
	// Retry the pool when every entry is cooling down, so recovered nodes return.
	if len(candidates) == 0 {
		candidates = g.Addresses
	}
	// Probing is network work and must not run under the route lock.
	reachable := make([]string, 0, len(candidates))
	for _, p := range probeServers(candidates, proxySnapshot()) {
		if p.Reachable {
			reachable = append(reachable, p.Address)
		}
	}
	routeMu.Lock()
	r.ordered = reachable
	r.expires = now.Add(5 * time.Minute)
	routeMu.Unlock()
	return append([]string(nil), reachable...)
}

// serverOptions resolves addresses and, when a proxy or interface is active,
// wraps each of them in a loopback tunnel. The returned tunnels belong to the
// lane that called this, which closes them with its client; the route only keeps
// a diagnostic record so a reconnect does not accumulate dead listeners.
func serverOptions(extended bool) ([]gotdx.Option, []*proxyTunnel, map[string]string) {
	addresses := serverOrder(extended)
	p := proxySnapshot()
	r := route(extended)
	remote := make(map[string]string)
	tunnels := make([]*proxyTunnel, 0)
	local := make([]string, 0, len(addresses))
	for _, address := range addresses {
		if p.enabled() || p.Interface != "" {
			t, err := newTunnel(p, []string{address})
			if err != nil {
				continue
			}
			tunnels = append(tunnels, t)
			a := t.listener.Addr().String()
			remote[a] = address
			local = append(local, a)
		} else {
			local = append(local, address)
		}
	}
	routeMu.Lock()
	r.tunnels = tunnels
	routeMu.Unlock()
	// Clear the SDK defaults even if a local tunnel could not be created.
	address := ""
	if len(local) > 0 {
		address = local[0]
		local = local[1:]
	}
	if extended {
		return []gotdx.Option{gotdx.WithMacExTCPAddress(address), gotdx.WithMacExTCPAddressPool(local...), gotdx.WithAutoSelectFastest(false)}, tunnels, remote
	}
	return []gotdx.Option{gotdx.WithMacTCPAddress(address), gotdx.WithMacTCPAddressPool(local...), gotdx.WithAutoSelectFastest(false)}, tunnels, remote
}
func currentServer(extended bool) string {
	return laneFor(extended, laneHot).address()
}
func dropClient(extended bool) {
	// Lanes rebuild lazily; bumping the family epoch covers the hot, bulk and
	// scan sockets without cutting an exchange that is already in flight. Each
	// lane closes its own tunnels with its client, so the route record is only
	// dropped here rather than closed.
	familyEpoch(extended).Add(1)
	closeFamilyLanes(extended)
	routeMu.Lock()
	r := route(extended)
	r.tunnels, r.remote = nil, nil
	routeMu.Unlock()
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
	mainGeneration, exGeneration := routeGeneration(false), routeGeneration(true)
	result, err := dispatch(req)
	failed := (mainGeneration != routeGeneration(false) && group(false).Mode == "auto") ||
		(exGeneration != routeGeneration(true) && group(true).Mode == "auto")
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
	if req.CalendarPath != calendarPathSnapshot() {
		dropClient(false)
		dropClient(true)
		configMu.Lock()
		calendarPath = req.CalendarPath
		configMu.Unlock()
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
	dropClient(extended)
	setActiveGroup(extended, g)
	routeMu.Lock()
	r := route(extended)
	r.failed = make(map[string]time.Time)
	routeMu.Unlock()
	for _, candidate := range candidates {
		// Limit each protocol validation to this one node. SDK fallback must not
		// silently accept a different address than the one being measured.
		routeMu.Lock()
		r.ordered, r.expires = []string{candidate.Address}, time.Now().Add(5*time.Minute)
		routeMu.Unlock()
		opts, tunnels, remote := serverOptions(extended)
		opts = append(opts, gotdx.WithTimeoutSec(3), gotdx.WithFuturesCalendarPath(calendarPathSnapshot()))
		var c *gotdx.Client
		market, code := uint8(1), "000001"
		if extended {
			c = gotdx.NewMACEx(opts...)
			market, code = 27, "HSI"
		} else {
			c = gotdx.NewMAC(opts...)
		}
		_, err := c.MACSymbolQuotes([]uint8{market}, []string{code}, gotdx.MACFieldBitmap(gotdx.MACFieldClose))
		if err != nil {
			for _, t := range tunnels {
				t.close()
			}
			routeMu.Lock()
			r.failed[candidate.Address] = time.Now().Add(time.Minute)
			routeMu.Unlock()
			dropClient(extended)
			continue
		}
		g.Mode, g.Address = mode, candidate.Address
		setActiveGroup(extended, g)
		// Hand the verified connection to the hot lane so the first quote does
		// not pay for a second handshake. The bulk lane reconnects on demand.
		installLaneClient(extended, laneHot, c, tunnels, remote)
		routeMu.Lock()
		r.ordered = []string{candidate.Address}
		for _, other := range candidates {
			if other.Address != candidate.Address && r.failed[other.Address].IsZero() {
				r.ordered = append(r.ordered, other.Address)
			}
		}
		r.expires = time.Now().Add(5 * time.Minute)
		routeMu.Unlock()
		return map[string]any{
			"address": candidate.Address, "latencyMs": candidate.LatencyMS,
			"previous": previous, "switched": mode == "manual" && previous != candidate.Address,
			"protocolVerified": true,
		}, nil
	}
	routeMu.Lock()
	r.ordered, r.expires = nil, time.Time{}
	routeMu.Unlock()
	return nil, fmt.Errorf("no usable %s TDX route; check the address pool or proxy", req.Kind)
}

func setActiveGroup(extended bool, g serverGroup) {
	configMu.Lock()
	defer configMu.Unlock()
	if extended {
		activeServers.Ex = g
	} else {
		activeServers.Main = g
	}
}
