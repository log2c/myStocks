package main

import (
	"bufio"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strconv"
	"testing"
	"time"
)

func loopbackInterface(t *testing.T) string {
	t.Helper()
	interfaces, err := net.Interfaces()
	if err != nil {
		t.Fatal(err)
	}
	for _, iface := range interfaces {
		if iface.Flags&net.FlagLoopback != 0 && iface.Flags&net.FlagUp != 0 {
			return iface.Name
		}
	}
	t.Fatal("No loopback interface for isolated binding tests")
	return ""
}

func TestInterfaceDialerBindsLocalInterface(t *testing.T) {
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	conn, err := dialProxy(proxyConfig{Interface: loopbackInterface(t)}, listener.Addr().String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	conn.Close()
	if _, err := dialProxy(proxyConfig{Interface: "mystocks-nonexistent-interface"}, listener.Addr().String(), time.Second); err == nil {
		t.Fatal("Missing selected interface must never fall back to system routing")
	}
}

func TestPrepareRouteAndQuotesUseSelectedInterface(t *testing.T) {
	isolateServers(t)
	address, count := quotePeer(t, false)
	p := proxyConfig{Interface: loopbackInterface(t)}
	cfg := serverConfig{Main: serverGroup{Mode: "manual", Address: address, Addresses: []string{address}}}
	if _, err := processRequest(request{Operation: "prepare_servers", Kind: "main", Proxy: p, Servers: cfg}); err != nil {
		t.Fatal(err)
	}
	result, err := processRequest(request{Operation: "quotes", Proxy: p, Servers: cfg,
		Stocks: []stock{{Code: "1.600519"}, {Code: "1.510300"}}})
	if err != nil || len(result.(map[string]any)["quotes"].([]quote)) != 2 || count.Load() < 2 {
		t.Fatalf("Bound route must support protocol preparation and compatible legacy symbols: %+v %v", result, err)
	}
	if currentServer(false) != address || len(mainRoute.tunnels) != 1 {
		t.Fatal("SDK traffic must go through the interface-bound tunnel")
	}
	p.Interface = "mystocks-nonexistent-interface"
	if _, err := processRequest(request{Operation: "prepare_servers", Kind: "main", Proxy: p, Servers: cfg}); err == nil {
		t.Fatal("Route preparation succeeded through another interface")
	}
}

func startRelayForTest(t *testing.T, p proxyConfig) proxyConfig {
	t.Helper()
	data, err := json.Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	result, err := networkProxyRequest(data)
	if err != nil {
		t.Fatal(err)
	}
	row := result.(map[string]any)
	t.Cleanup(func() {
		relayMu.Lock()
		if relay := relays[p]; relay != nil {
			relay.listener.Close()
			delete(relays, p)
		}
		relayMu.Unlock()
	})
	return proxyConfig{Type: "socks5", Host: row["host"].(string), Port: row["port"].(int),
		User: row["user"].(string), Password: row["password"].(string)}
}

func echoServer(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() { defer conn.Close(); io.Copy(conn, conn) }()
		}
	}()
	return listener.Addr().String()
}

func assertEcho(t *testing.T, p proxyConfig, target string) {
	t.Helper()
	conn, err := dialProxy(p, target, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err := conn.Write([]byte("bound-request")); err != nil {
		t.Fatal(err)
	}
	data := make([]byte, len("bound-request"))
	if _, err := io.ReadFull(conn, data); err != nil || string(data) != "bound-request" {
		t.Fatalf("relay stream failed: %q %v", data, err)
	}
}

func TestNetworkRelayAuthenticationAndMissingInterface(t *testing.T) {
	target := echoServer(t)
	relay := startRelayForTest(t, proxyConfig{Interface: loopbackInterface(t)})
	assertEcho(t, relay, target)
	relay.Password = "wrong-password"
	if _, err := dialProxy(relay, target, time.Second); err == nil {
		t.Fatal("Loopback relay must reject unauthorized callers")
	}
	missing := startRelayForTest(t, proxyConfig{Interface: "mystocks-nonexistent-interface"})
	if _, err := dialProxy(missing, target, time.Second); err == nil {
		t.Fatal("Loopback relay must not route around an unavailable selected interface")
	}
	// Starting a relay must stay independent of lengthy serialized quotation I/O.
	loopback := loopbackInterface(t)
	mu.Lock()
	done := make(chan struct{})
	go func() {
		start, _ := json.Marshal(proxyConfig{Interface: loopback})
		networkProxyRequest(start)
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(time.Second):
		mu.Unlock()
		t.Fatal("Relay initialization blocked behind quotation requests")
	}
	mu.Unlock()
}

func TestNetworkRelayUsesBoundUpstreamHTTPProxy(t *testing.T) {
	target := echoServer(t)
	upstream, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer upstream.Close()
	seen := make(chan string, 1)
	go func() {
		conn, err := upstream.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		reader := bufio.NewReader(conn)
		req, err := http.ReadRequest(reader)
		if err != nil || req.Method != "CONNECT" || req.Host != "remote.invalid:7709" {
			return
		}
		seen <- req.Header.Get("Proxy-Authorization")
		remote, err := net.Dial("tcp", target)
		if err != nil {
			return
		}
		defer remote.Close()
		io.WriteString(conn, "HTTP/1.1 200 Connection established\r\n\r\n")
		go io.Copy(remote, reader)
		io.Copy(conn, remote)
	}()
	host, portText, _ := net.SplitHostPort(upstream.Addr().String())
	port, _ := strconv.Atoi(portText)
	relay := startRelayForTest(t, proxyConfig{Interface: loopbackInterface(t), Type: "http", Host: host, Port: port,
		User: "proxy-user", Password: "proxy-password"})
	assertEcho(t, relay, "remote.invalid:7709")
	select {
	case auth := <-seen:
		if auth == "" {
			t.Fatal("Configured upstream authentication was dropped")
		}
	case <-time.After(time.Second):
		t.Fatal("Selected interface relay bypassed upstream proxy")
	}
}
