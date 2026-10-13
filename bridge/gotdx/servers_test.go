package main

import (
	"bufio"
	"encoding/binary"
	"io"
	"math"
	"net"
	"net/http"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bensema/gotdx/proto"
)

func isolateServers(t *testing.T) {
	t.Helper()
	oldServers, oldProxy := activeServers, activeProxy
	dropClient(false, false)
	dropClient(true, false)
	mainRoute, exRoute = serverRoute{}, serverRoute{}
	activeServers, activeProxy = serverConfig{}, proxyConfig{}
	t.Cleanup(func() {
		dropClient(false, false)
		dropClient(true, false)
		mainRoute, exRoute = serverRoute{}, serverRoute{}
		activeServers, activeProxy = oldServers, oldProxy
	})
}

// A local MAC quotation peer. Probe connections close before sending a request;
// only real quotation requests increment the counter.
func quotePeer(t *testing.T, reject bool) (string, *atomic.Int32) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	count := &atomic.Int32{}
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				conn.SetDeadline(time.Now().Add(5 * time.Second))
				for {
					header := make([]byte, 12)
					if _, err := io.ReadFull(conn, header); err != nil {
						return
					}
					length := int(binary.LittleEndian.Uint16(header[8:10])) - 2
					if length < 22 || length > 4000 {
						return
					}
					payload := make([]byte, length)
					if _, err := io.ReadFull(conn, payload); err != nil {
						return
					}
					count.Add(1)
					if reject {
						return
					}
					n := int(binary.LittleEndian.Uint16(payload[20:22]))
					if len(payload) != 22+24*n {
						return
					}
					body := make([]byte, 26+80*n)
					copy(body[:20], payload[:20])
					binary.LittleEndian.PutUint32(body[20:24], uint32(n))
					binary.LittleEndian.PutUint16(body[24:26], uint16(n))
					for i := 0; i < n; i++ {
						row := body[26+i*80 : 26+(i+1)*80]
						copy(row[:24], payload[22+i*24:22+(i+1)*24])
						copy(row[24:68], "Fixture")
						binary.LittleEndian.PutUint32(row[68:72], math.Float32bits(10))
						binary.LittleEndian.PutUint32(row[72:76], math.Float32bits(11))
						binary.LittleEndian.PutUint32(row[76:80], math.Float32bits(0.5))
					}
					response := make([]byte, proto.MessageHeaderBytes)
					copy(response[5:9], header[1:5])
					copy(response[10:12], header[10:12])
					binary.LittleEndian.PutUint16(response[12:14], uint16(len(body)))
					binary.LittleEndian.PutUint16(response[14:16], uint16(len(body)))
					if _, err := conn.Write(append(response, body...)); err != nil {
						return
					}
				}
			}()
		}
	}()
	return listener.Addr().String(), count
}

func TestServerDefaultsAndValidation(t *testing.T) {
	main, err := normalizeGroup(serverGroup{}, false)
	if err != nil || main.Mode != "auto" || len(main.Addresses) == 0 {
		t.Fatalf("defaults: %+v %v", main, err)
	}
	ex, err := normalizeGroup(serverGroup{}, true)
	if err != nil || len(ex.Addresses) == 0 || reflect.DeepEqual(main.Addresses, ex.Addresses) {
		t.Fatal("main and ex must have independent default pools")
	}
	for _, input := range []string{"", "host", "host:0", "host:65536", "a/b:7709", "host:7709\nCONNECT", ":7709"} {
		if _, err := normalizeAddress(input); err == nil {
			t.Fatalf("invalid address accepted: %q", input)
		}
	}
	g, err := normalizeGroup(serverGroup{Addresses: []string{" Example.COM:07709 ", "example.com:7709"}}, false)
	if err != nil || len(g.Addresses) != 1 || g.Addresses[0] != "example.com:7709" {
		t.Fatalf("normalization: %+v %v", g, err)
	}
	if _, err := normalizeGroup(serverGroup{Mode: "manual", Address: "127.0.0.1:1", Addresses: []string{"127.0.0.1:2"}}, false); err == nil {
		t.Fatal("manual server outside pool accepted")
	}
}

func TestProbeAvailabilityAndSettingsIsolation(t *testing.T) {
	isolateServers(t)
	good, _ := quotePeer(t, false)
	closed, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	bad := closed.Addr().String()
	closed.Close()
	if err := configureServers(serverConfig{Main: serverGroup{Addresses: []string{good}}}); err != nil {
		t.Fatal(err)
	}
	client(false)
	beforeClient, beforeConfig := mainClient, activeServers
	result, err := processRequest(request{Operation: "server_probe", Kind: "main", Servers: serverConfig{Main: serverGroup{Addresses: []string{bad, good}}}})
	if err != nil {
		t.Fatal(err)
	}
	probes := result.(map[string]any)["results"].([]serverProbe)
	if len(probes) != 2 || probes[0].Address != good || !probes[0].Reachable || probes[1].Reachable || probes[1].LatencyMS != nil {
		t.Fatalf("unexpected probe results: %+v", probes)
	}
	if mainClient != beforeClient || !reflect.DeepEqual(activeServers, beforeConfig) {
		t.Fatal("settings preview changed the live connection")
	}
}

func TestAutoFailoverRetriesOnlyMissingQuotes(t *testing.T) {
	isolateServers(t)
	bad, badCount := quotePeer(t, true)
	good, goodCount := quotePeer(t, false)
	ex, exCount := quotePeer(t, false)
	cfg := serverConfig{Main: serverGroup{Addresses: []string{bad, good}}, Ex: serverGroup{Addresses: []string{ex}}}
	if err := configureServers(cfg); err != nil {
		t.Fatal(err)
	}
	mainRoute.ordered, mainRoute.expires = []string{bad, good}, time.Now().Add(time.Minute)
	req := request{Operation: "quotes", Stocks: []stock{{Code: "1.600519"}, {Code: "tdx:ex:12:A_IXIC"}}, Servers: cfg}
	result, err := processRequest(req)
	if err != nil {
		t.Fatal(err)
	}
	data := result.(map[string]any)
	if len(data["quotes"].([]quote)) != 2 || len(data["errors"].([]string)) != 0 {
		t.Fatalf("failover lost quotes: %+v", data)
	}
	if badCount.Load() != 1 || goodCount.Load() != 1 || exCount.Load() != 1 {
		t.Fatalf("duplicate or missing requests: bad=%d good=%d ex=%d", badCount.Load(), goodCount.Load(), exCount.Load())
	}
	if currentServer(false) != good || currentServer(true) != ex || !time.Now().Before(mainRoute.failed[bad]) {
		t.Fatal("failed node was not rotated and cooled down")
	}
	for _, address := range serverOrder(false) {
		if address == bad {
			t.Fatal("failed node was immediately reselected")
		}
	}
	mainRoute.failed[bad], mainRoute.expires = time.Now().Add(-time.Second), time.Time{}
	if len(serverOrder(false)) != 2 {
		t.Fatal("recovered node did not return after cooldown")
	}
}

func TestManualSelectionNeverFallsBack(t *testing.T) {
	isolateServers(t)
	bad, badCount := quotePeer(t, true)
	good, goodCount := quotePeer(t, false)
	cfg := serverConfig{Main: serverGroup{Mode: "manual", Address: bad, Addresses: []string{bad, good}}}
	result, err := processRequest(request{Operation: "quotes", Stocks: []stock{{Code: "1.600519"}}, Servers: cfg})
	if err != nil {
		t.Fatal(err)
	}
	data := result.(map[string]any)
	if len(data["quotes"].([]quote)) != 0 || badCount.Load() != 1 || goodCount.Load() != 0 {
		t.Fatal("manual selection silently used another server")
	}
}

func TestProxyProbeAndCustomPoolUseRemoteAddress(t *testing.T) {
	isolateServers(t)
	peer, count := quotePeer(t, false)
	proxy, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { proxy.Close() })
	var connects atomic.Int32
	go func() {
		for {
			conn, err := proxy.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				reader := bufio.NewReader(conn)
				req, err := http.ReadRequest(reader)
				if err != nil {
					return
				}
				if req.Method != "CONNECT" || req.Host != "custom.example:7709" {
					return
				}
				connects.Add(1)
				remote, err := net.DialTimeout("tcp", peer, time.Second)
				if err != nil {
					return
				}
				defer remote.Close()
				io.WriteString(conn, "HTTP/1.1 200 Connection established\r\n\r\n")
				done := make(chan struct{})
				go func() { io.Copy(remote, reader); remote.Close(); close(done) }()
				io.Copy(conn, remote)
				conn.Close()
				<-done
			}()
		}
	}()
	p := proxyAddress(t, proxy, "http")
	cfg := serverConfig{Main: serverGroup{Addresses: []string{"custom.example:7709"}}}
	probes, err := processRequest(request{Operation: "server_probe", Kind: "main", Proxy: p, Servers: cfg})
	if err != nil || !probes.(map[string]any)["results"].([]serverProbe)[0].Reachable {
		t.Fatalf("proxy probe failed: %v", err)
	}
	result, err := processRequest(request{Operation: "quotes", Stocks: []stock{{Code: "1.600519"}}, Proxy: p, Servers: cfg})
	if err != nil || len(result.(map[string]any)["quotes"].([]quote)) != 1 {
		t.Fatalf("proxy quote failed: %+v %v", result, err)
	}
	if currentServer(false) != "custom.example:7709" || count.Load() != 1 || connects.Load() < 3 {
		t.Fatal("custom node or proxy was bypassed")
	}
	result, err = processRequest(request{Operation: "server_list"})
	if err != nil || !strings.Contains(result.(map[string]any)["main"].(map[string]any)["current"].(string), "custom.example") {
		t.Fatal("current server exposed a local tunnel address")
	}
}

func TestUnavailableCustomPoolDoesNotUseSDKDefaults(t *testing.T) {
	isolateServers(t)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	listener.Close()
	cfg := serverConfig{Main: serverGroup{Addresses: []string{address}}}
	result, err := processRequest(request{Operation: "quotes", Stocks: []stock{{Code: "1.600519"}}, Servers: cfg})
	if err != nil {
		t.Fatal(err)
	}
	data := result.(map[string]any)
	if len(data["quotes"].([]quote)) != 0 || len(data["errors"].([]string)) == 0 || currentServer(false) != "" {
		t.Fatal("unavailable custom pool silently fell back to SDK defaults")
	}
}
