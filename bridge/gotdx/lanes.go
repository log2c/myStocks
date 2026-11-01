package main

import (
	"errors"
	"io"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/bensema/gotdx"
)

// One TDX connection answers one request at a time: a directory scan or a
// breadth aggregation can occupy the socket for seconds. Quotation traffic must
// never queue behind that, so each market family keeps three lanes. The hot lane
// serves quotes, timelines and K-lines; the bulk lane serves board lists and
// breadth snapshots; the scan lane walks directories, which is the only work
// that can hold a socket for the better part of ten seconds.
type lane struct {
	mu       sync.Mutex
	client   *gotdx.Client
	tunnels  []*proxyTunnel
	remote   map[string]string
	epoch    uint64
	lastUsed time.Time
}

// laneKind separates work by how long it can hold a socket.
type laneKind int

const (
	laneHot laneKind = iota
	laneBulk
	laneScan
)

var (
	mainHot, mainBulk, mainScan lane
	exHot, exBulk, exScan       lane

	// Epochs are per market family: changing the extended-server pool must not
	// tear down a healthy main-market connection.
	mainEpoch, exEpoch atomic.Uint64
	keepAliveOnce      sync.Once
)

func familyEpoch(extended bool) *atomic.Uint64 {
	if extended {
		return &exEpoch
	}
	return &mainEpoch
}

func epochOf(extended bool) uint64 { return familyEpoch(extended).Load() }

const (
	// A socket that sat idle across a lunch break or overnight is usually gone:
	// the NAT mapping expires long before the server notices. Probing first
	// turns an 8 s read timeout into one round trip, or a cheap reconnect.
	laneIdleProbeAfter    = 20 * time.Second
	laneKeepAliveInterval = 20 * time.Second
)

// laneHeartbeat is replaceable so tests can observe the idle probe.
var laneHeartbeat = func(c *gotdx.Client) error {
	_, err := c.GetServerHeartbeat()
	return err
}

func laneFor(extended bool, kind laneKind) *lane {
	if extended {
		switch kind {
		case laneBulk:
			return &exBulk
		case laneScan:
			return &exScan
		default:
			return &exHot
		}
	}
	switch kind {
	case laneBulk:
		return &mainBulk
	case laneScan:
		return &mainScan
	default:
		return &mainHot
	}
}

func familyLanes(extended bool) []*lane {
	if extended {
		return []*lane{&exHot, &exBulk, &exScan}
	}
	return []*lane{&mainHot, &mainBulk, &mainScan}
}

// withClient serializes one request on its lane. The lane lock is the only
// serialization point: options are built before it, and failure bookkeeping
// happens after the socket has already been dropped by the SDK.
func withClient(extended bool, kind laneKind, fn func(*gotdx.Client) error) error {
	startLaneKeepAlive()
	l := laneFor(extended, kind)
	l.mu.Lock()
	defer l.mu.Unlock()
	defer l.retireIfStale(extended)

	c, err := l.ensure(extended)
	if err != nil {
		return err
	}
	l.probeIfIdle(c)
	if err := fn(c); err != nil {
		if isTransportError(err) {
			// The SDK closes the socket on any failed exchange, so the next
			// request reconnects anyway. Record the node failure here so an
			// auto-selecting lane can move to another address.
			l.closeLocked()
			markRouteFailure(extended, l.remoteAddress(c.CurrentAddress()))
		}
		return err
	}
	l.lastUsed = time.Now()
	return nil
}

func (l *lane) ensure(extended bool) (*gotdx.Client, error) {
	epoch := epochOf(extended)
	if l.client != nil && l.epoch == epoch {
		return l.client, nil
	}
	l.closeLocked()
	opts, tunnels, remote := buildClientOptions(extended)
	var c *gotdx.Client
	if extended {
		c = gotdx.NewMACEx(opts...)
	} else {
		c = gotdx.NewMAC(opts...)
	}
	l.client, l.tunnels, l.remote, l.epoch = c, tunnels, remote, epoch
	return c, nil
}

func (l *lane) probeIfIdle(c *gotdx.Client) {
	if l.lastUsed.IsZero() || time.Since(l.lastUsed) < laneIdleProbeAfter {
		return
	}
	// A failure here is expected; the request that follows reconnects and
	// reports the real error, if any.
	_ = laneHeartbeat(c)
}

func (l *lane) retireIfStale(extended bool) {
	if l.epoch == 0 || l.epoch == epochOf(extended) {
		return
	}
	l.closeLocked()
	l.epoch = 0
}

func (l *lane) closeLocked() {
	if l.client != nil {
		_ = l.client.Disconnect()
		l.client = nil
	}
	for _, t := range l.tunnels {
		t.close()
	}
	l.tunnels, l.remote = nil, nil
	l.epoch = 0
}

func (l *lane) address() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.client == nil {
		return ""
	}
	a := l.client.CurrentAddress()
	if remote := l.remote[a]; remote != "" {
		return remote
	}
	return a
}

// connection reports the current client identity; tests use it to prove that a
// settings preview did not touch the live socket.
func (l *lane) connection() *gotdx.Client {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.client
}

func (l *lane) tunnelCount() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.tunnels)
}

func (l *lane) remoteAddress(address string) string {
	if remote := l.remote[address]; remote != "" {
		return remote
	}
	return address
}

// installLaneClient reuses a connection that was already protocol-verified, so
// startup does not pay for a second handshake on the first quote.
func installLaneClient(extended bool, kind laneKind, c *gotdx.Client, tunnels []*proxyTunnel, remote map[string]string) {
	l := laneFor(extended, kind)
	l.mu.Lock()
	defer l.mu.Unlock()
	l.closeLocked()
	l.client, l.tunnels, l.remote = c, tunnels, remote
	l.epoch = epochOf(extended)
	l.lastUsed = time.Now()
}

// closeFamilyLanes invalidates the family's sockets. A lane that is mid-request
// keeps its connection until that request returns, then retires it, so a config
// change never cuts an exchange in half.
func closeFamilyLanes(extended bool) {
	for _, l := range familyLanes(extended) {
		if l.mu.TryLock() {
			l.closeLocked()
			l.mu.Unlock()
			continue
		}
		// Busy: the owner retires it in retireIfStale when it releases.
	}
}

// startLaneKeepAlive refreshes idle TDX sockets and discovers dead ones while
// nobody is waiting, which is what keeps the first poll after a pause fast.
func startLaneKeepAlive() {
	keepAliveOnce.Do(func() {
		go func() {
			ticker := time.NewTicker(laneKeepAliveInterval)
			defer ticker.Stop()
			for range ticker.C {
				for _, extended := range []bool{false, true} {
					for _, kind := range []laneKind{laneHot, laneBulk, laneScan} {
						keepAlive(laneFor(extended, kind))
					}
				}
			}
		}()
	})
}

func keepAlive(l *lane) {
	// A busy lane is already proving the socket works; never queue behind it.
	if !l.mu.TryLock() {
		return
	}
	defer l.mu.Unlock()
	if l.client == nil || l.lastUsed.IsZero() || time.Since(l.lastUsed) < laneKeepAliveInterval {
		return
	}
	if err := laneHeartbeat(l.client); err == nil {
		l.lastUsed = time.Now()
		return
	}
	// Detach only. The next real request reconnects and reports anything that
	// is still wrong, with the error the user actually needs to see.
	l.closeLocked()
}

func isTransportError(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) ||
		errors.Is(err, os.ErrDeadlineExceeded) || errors.Is(err, net.ErrClosed) {
		return true
	}
	var netErr net.Error
	if errors.As(err, &netErr) {
		return true
	}
	var opErr *net.OpError
	return errors.As(err, &opErr)
}
