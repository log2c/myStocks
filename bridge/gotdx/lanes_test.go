package main

import (
	"errors"
	"io"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bensema/gotdx"
)

// A blocked bulk exchange must not delay a quote on the hot lane. This is the
// regression that made the UI stall: one serialized connection meant a
// directory scan froze every poll for as long as it ran.
func TestHotLaneIsNotBlockedByBulkLane(t *testing.T) {
	isolateServers(t)
	address, count := quotePeer(t, false)
	cfg := serverConfig{Main: serverGroup{Mode: "manual", Address: address, Addresses: []string{address}}}
	if err := configureServers(cfg); err != nil {
		t.Fatal(err)
	}

	release := make(chan struct{})
	started := make(chan struct{})
	bulkDone := make(chan error, 1)
	go func() {
		bulkDone <- withBulk(false, func(*gotdx.Client) error {
			close(started)
			<-release
			return nil
		})
	}()
	<-started

	quoted := make(chan error, 1)
	go func() {
		quoted <- withHot(false, func(c *gotdx.Client) error {
			_, err := c.MACSymbolQuotes([]uint8{1}, []string{"600519"},
				gotdx.MACFieldBitmap(gotdx.MACFieldClose))
			return err
		})
	}()
	select {
	case err := <-quoted:
		if err != nil {
			t.Fatalf("hot lane quote failed while bulk lane was busy: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("hot lane quote waited for the busy bulk lane")
	}
	close(release)
	if err := <-bulkDone; err != nil {
		t.Fatal(err)
	}
	if count.Load() < 1 {
		t.Fatal("quote never reached the peer")
	}
	if mainHot.connection() == nil || mainBulk.connection() == nil {
		t.Fatal("both lanes must hold their own connection")
	}
	if mainHot.connection() == mainBulk.connection() {
		t.Fatal("lanes must not share one client")
	}
}

// Requests on the same lane are still serialized, because one TDX connection
// cannot interleave two exchanges.
func TestLaneSerializesExchanges(t *testing.T) {
	isolateServers(t)
	address, _ := quotePeer(t, false)
	if err := configureServers(serverConfig{Main: serverGroup{Mode: "manual", Address: address, Addresses: []string{address}}}); err != nil {
		t.Fatal(err)
	}
	var inFlight, peak atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_ = withHot(false, func(*gotdx.Client) error {
				n := inFlight.Add(1)
				for {
					old := peak.Load()
					if n <= old || peak.CompareAndSwap(old, n) {
						break
					}
				}
				time.Sleep(20 * time.Millisecond)
				inFlight.Add(-1)
				return nil
			})
		}()
	}
	wg.Wait()
	if peak.Load() != 1 {
		t.Fatalf("lane allowed %d concurrent exchanges", peak.Load())
	}
}

func TestIdleLaneProbesBeforeReuse(t *testing.T) {
	isolateServers(t)
	address, _ := quotePeer(t, false)
	if err := configureServers(serverConfig{Main: serverGroup{Mode: "manual", Address: address, Addresses: []string{address}}}); err != nil {
		t.Fatal(err)
	}
	var probes atomic.Int32
	original := laneHeartbeat
	laneHeartbeat = func(*gotdx.Client) error {
		probes.Add(1)
		return nil
	}
	t.Cleanup(func() { laneHeartbeat = original })

	if err := withHot(false, func(*gotdx.Client) error { return nil }); err != nil {
		t.Fatal(err)
	}
	if probes.Load() != 0 {
		t.Fatal("a fresh lane must not probe")
	}
	// Simulate a socket that sat idle long enough for a NAT mapping to expire.
	mainHot.mu.Lock()
	mainHot.lastUsed = time.Now().Add(-2 * laneIdleProbeAfter)
	mainHot.mu.Unlock()
	if err := withHot(false, func(*gotdx.Client) error { return nil }); err != nil {
		t.Fatal(err)
	}
	if probes.Load() != 1 {
		t.Fatalf("idle lane reused without probing, probes=%d", probes.Load())
	}
}

func TestScanLaneIsIndependentOfBulkLane(t *testing.T) {
	isolateServers(t)
	address, _ := quotePeer(t, false)
	if err := configureServers(serverConfig{Main: serverGroup{Mode: "manual", Address: address, Addresses: []string{address}}}); err != nil {
		t.Fatal(err)
	}

	release := make(chan struct{})
	started := make(chan struct{})
	scanDone := make(chan error, 1)
	go func() {
		scanDone <- withScan(false, func(*gotdx.Client) error {
			close(started)
			<-release
			return nil
		})
	}()
	<-started

	// Board lists and breadth snapshots share the bulk lane; a directory scan
	// must not delay them any more than it delays quotes.
	bulkDone := make(chan error, 1)
	go func() {
		bulkDone <- withBulk(false, func(c *gotdx.Client) error {
			_, err := c.MACSymbolQuotes([]uint8{1}, []string{"880001"},
				gotdx.MACFieldBitmap(gotdx.MACFieldClose))
			return err
		})
	}()
	select {
	case err := <-bulkDone:
		if err != nil {
			t.Fatalf("bulk request failed while scan lane was busy: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("bulk lane waited for the scan lane")
	}
	close(release)
	if err := <-scanDone; err != nil {
		t.Fatal(err)
	}
	if mainScan.connection() == mainBulk.connection() {
		t.Fatal("scan and bulk lanes must not share one client")
	}
}

func TestTransportErrorRotatesNodeAndDataErrorDoesNot(t *testing.T) {
	isolateServers(t)
	address, _ := quotePeer(t, false)
	if err := configureServers(serverConfig{Main: serverGroup{Addresses: []string{address}}}); err != nil {
		t.Fatal(err)
	}
	if err := withHot(false, func(*gotdx.Client) error { return nil }); err != nil {
		t.Fatal(err)
	}
	before := routeGeneration(false)
	// A parse or empty-data failure is not a transport problem: keep the node.
	if err := withHot(false, func(*gotdx.Client) error { return errors.New("TDX returned no quote") }); err == nil {
		t.Fatal("data error was swallowed")
	}
	if routeGeneration(false) != before {
		t.Fatal("data error rotated the node")
	}
	if err := withHot(false, func(*gotdx.Client) error { return io.EOF }); err == nil {
		t.Fatal("transport error was swallowed")
	}
	if routeGeneration(false) == before {
		t.Fatal("transport error did not mark the node failed")
	}
	if mainHot.connection() != nil {
		t.Fatal("transport failure must detach the socket")
	}
}
