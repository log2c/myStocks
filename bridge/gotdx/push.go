package main

// The push channel keeps one socket to a mainland server for 0x0547 refresh
// frames only. It is deliberately separate from the lane clients: gotdx's
// exchange() is strictly one write and one read, so a frame a server sends on
// its own would be mistaken for the next response. On its own socket an
// unexpected frame can only ever be a quote.
//
// A server either streams frames while its quotes change or waits to be asked
// again; this channel handles both by refreshing after the configured interval
// without data, so the cache stays warm outside the session as well.

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/bensema/gotdx"
	"github.com/bensema/gotdx/proto"
)

const (
	pushHelloMethod   = 0x000d
	pushRefreshMethod = 0x0547
	// The channel is capped well below the 100 records one refresh allows. Every
	// subscribed instrument is re-sent each second, so a long list would spend
	// bandwidth to duplicate what the snapshot poll already delivers; the
	// remainder stays on that poll.
	pushMaxCodes = 30
	// Default and bounds for the refresh interval the app sends; the app's
	// polling setting drives it, clamped so a bad value cannot hammer a server or
	// freeze the stream.
	pushDefaultRefresh = time.Second
	pushMinRefresh     = 500 * time.Millisecond
	pushMaxRefresh     = 10 * time.Second
	pushDialTimeout    = 4 * time.Second
	pushMaxBackoff     = 15 * time.Second
	pushMaxFrame       = 1 << 15
)

// pushTarget is one subscribed instrument. divisor turns the wire price, which
// is an integer in hundredths of a tick, into a display price.
type pushTarget struct {
	display string // the watch code the UI sent, echoed back unchanged
	market  uint8
	code    string // six-character TDX symbol
	divisor int64  // 1 for two decimals, 10 for three, 100 for four
}

// pushKey identifies one instrument on the wire cache.
func pushKey(market uint8, code string) string {
	return fmt.Sprintf("%d.%s", market, strings.ToUpper(code))
}

type pushQuote struct {
	Code      string  `json:"code"`
	Price     float64 `json:"price"`
	PreClose  float64 `json:"preClose"`
	Change    float64 `json:"change"`
	Pct       float64 `json:"pct"`
	UpdatedMs int64   `json:"updatedMs"`
}

type pushChannel struct {
	mu         sync.Mutex
	generation uint64
	quotes     map[string]pushQuote
	updatedAt  time.Time
	connected  bool
	lastError  string
	frames     uint64
	refreshes  uint64
	stop       chan struct{}
	running    bool
}

var push = &pushChannel{quotes: map[string]pushQuote{}}

// pushStart replaces the subscription. Everything the push channel cannot serve
// stays on the MAC snapshot path, because 0x0547 exists only on the mainland
// quotation service.
func pushStart(req request) (any, error) {
	targets, unsupported, overLimit := pushSelectTargets(req.Stocks)
	pushDivisors(targets)
	interval := pushInterval(req.PushIntervalMs)
	push.start(targets, interval)
	return map[string]any{
		"started": true, "count": len(targets),
		"skipped": unsupported, "overLimit": overLimit, "limit": pushMaxCodes,
		"intervalMs": interval.Milliseconds(),
	}, nil
}

// pushInterval converts the app's polling setting into the wait between refresh
// requests, falling back to the default when the caller sends nothing.
func pushInterval(milliseconds int) time.Duration {
	if milliseconds <= 0 {
		return pushDefaultRefresh
	}
	interval := time.Duration(milliseconds) * time.Millisecond
	if interval < pushMinRefresh {
		return pushMinRefresh
	}
	if interval > pushMaxRefresh {
		return pushMaxRefresh
	}
	return interval
}

// pushSelectTargets splits the watchlist into what the channel will carry and
// what stays on the snapshot poll. It is separate from pushStart so the split is
// testable without a socket.
func pushSelectTargets(stocks []stock) (targets []pushTarget, unsupported, overLimit int) {
	targets = make([]pushTarget, 0, min(len(stocks), pushMaxCodes))
	seen := make(map[string]bool, len(stocks))
	// Two separate counts: an instrument the channel cannot serve at all, and one
	// it could serve but that fell past the cap. Reporting them apart keeps a long
	// watchlist from looking like a resolution failure.
	for _, item := range stocks {
		s, err := resolveNamed(item.Code, item.Name)
		if err != nil || s.extended || !mainlandPattern.MatchString(s.code) {
			unsupported++
			continue
		}
		key := pushKey(s.market, s.code)
		if seen[key] {
			continue
		}
		seen[key] = true
		if len(targets) == pushMaxCodes {
			overLimit++
			continue
		}
		targets = append(targets, pushTarget{display: item.Code, market: s.market, code: s.code, divisor: 1})
	}
	return targets, unsupported, overLimit
}

func pushStop() (any, error) {
	push.start(nil, pushDefaultRefresh)
	return map[string]any{"started": false, "count": 0}, nil
}

func pushPoll() (any, error) {
	return push.poll(), nil
}

// pushDivisors asks the market for the quote precision of each target. A
// failure is not fatal: two decimals is the mainland default and the next MAC
// snapshot still corrects the row.
func pushDivisors(targets []pushTarget) {
	for offset := 0; offset < len(targets); offset += 80 {
		end := min(offset+80, len(targets))
		markets := make([]uint8, 0, end-offset)
		codes := make([]string, 0, end-offset)
		for _, t := range targets[offset:end] {
			markets = append(markets, t.market)
			codes = append(codes, t.code)
		}
		var reply *proto.MACSymbolQuotesReply
		if err := withHot(false, func(c *gotdx.Client) error {
			var err error
			reply, err = c.MACSymbolQuotes(markets, codes, gotdx.MACFieldBitmap(gotdx.MACFieldDecimalPoint))
			return err
		}); err != nil {
			continue
		}
		for _, item := range reply.Stocks {
			key := pushKey(uint8(item.Market), item.Symbol)
			for i := range targets[offset:end] {
				t := &targets[offset+i]
				if pushKey(t.market, t.code) != key {
					continue
				}
				t.divisor = pushDivisor(field(item.Values, "decimal_point"))
			}
		}
	}
}

// pushDivisor mirrors the wire precision: the server scales a price by ten, so
// two decimals need one factor of a hundred and every extra decimal one more
// factor of ten.
func pushDivisor(decimals *float64) int64 {
	if decimals == nil || *decimals <= 2 || *decimals > 8 {
		return 1
	}
	divisor := int64(1)
	for i := 2; i < int(*decimals); i++ {
		divisor *= 10
	}
	return divisor
}

func (p *pushChannel) start(targets []pushTarget, interval time.Duration) {
	p.mu.Lock()
	p.generation++
	generation := p.generation
	if p.stop != nil {
		close(p.stop)
		p.stop = nil
	}
	stop := make(chan struct{})
	p.stop = stop
	p.quotes = make(map[string]pushQuote, len(targets))
	p.updatedAt = time.Time{}
	p.connected = false
	p.lastError = ""
	p.frames = 0
	p.refreshes = 0
	p.running = len(targets) > 0
	p.mu.Unlock()
	if len(targets) > 0 {
		go p.loop(generation, targets, interval, stop)
	}
}

func (p *pushChannel) poll() map[string]any {
	p.mu.Lock()
	quotes := make([]pushQuote, 0, len(p.quotes))
	for _, q := range p.quotes {
		quotes = append(quotes, q)
	}
	ageMs := int64(-1)
	if !p.updatedAt.IsZero() {
		ageMs = time.Since(p.updatedAt).Milliseconds()
	}
	sort.Slice(quotes, func(i, j int) bool { return quotes[i].Code < quotes[j].Code })
	status := map[string]any{
		"quotes":    quotes,
		"ageMs":     ageMs,
		"connected": p.connected,
		"running":   p.running,
		"frames":    p.frames,
		"refreshes": p.refreshes,
		"lastError": p.lastError,
	}
	p.mu.Unlock()
	return status
}

func (p *pushChannel) loop(generation uint64, targets []pushTarget, interval time.Duration, stop <-chan struct{}) {
	backoff := time.Second
	for {
		select {
		case <-stop:
			return
		default:
		}
		connected := p.session(generation, targets, interval, stop)
		if connected {
			// Reachable again, so a later drop starts retrying quickly.
			backoff = time.Second
		} else {
			backoff *= 2
			if backoff > pushMaxBackoff {
				backoff = pushMaxBackoff
			}
		}
		// Always pause: a node that accepts the handshake and drops the socket
		// immediately must not turn this into a reconnect spin.
		select {
		case <-stop:
			return
		case <-time.After(backoff):
		}
	}
}

// session runs one connection until it fails. It reports whether the handshake
// ever succeeded, which separates a bad node from a node that dropped us.
func (p *pushChannel) session(generation uint64, targets []pushTarget, interval time.Duration, stop <-chan struct{}) bool {
	conn := p.dial()
	if conn == nil {
		return false
	}
	defer conn.Close()
	p.setConnected(generation, true)
	defer p.setConnected(generation, false)

	byKey := make(map[string]*pushTarget, len(targets))
	for i := range targets {
		t := &targets[i]
		byKey[pushKey(t.market, t.code)] = t
	}
	payload := pushRequest(targets)

	// nextSend is the moment we give up waiting for frames and ask again.
	nextSend := time.Now()
	pending := false
	var sequence uint32
	for {
		select {
		case <-stop:
			return true
		default:
		}

		now := time.Now()
		if pending && !now.Before(nextSend) {
			// The refresh went unanswered; do not block the stream on it.
			pending = false
		}
		if !pending && !now.Before(nextSend) {
			sequence++
			if err := pushWrite(conn, pushRefreshMethod, sequence, payload); err != nil {
				// A dropped refresh says nothing about the node: it may only be
				// closing an idle stream, so the route stays trusted.
				p.setError(generation, err)
				return true
			}
			pending = true
			p.countRefresh(generation)
			nextSend = now.Add(interval)
		}

		wait := time.Until(nextSend)
		if wait < 10*time.Millisecond {
			wait = 10 * time.Millisecond
		}
		conn.SetReadDeadline(time.Now().Add(wait))
		frame, err := pushRead(conn)
		if err != nil {
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}
			p.setError(generation, err)
			return true
		}
		if frame.method != pushRefreshMethod {
			continue
		}
		if pending && frame.seq == sequence {
			pending = false
		}
		// Frames extend the window, so a streaming server never sees a poll.
		nextSend = time.Now().Add(interval)
		p.apply(generation, frame.body, byKey)
	}
}

// dial tries the configured route and hands back the first socket that answers
// the handshake. The proxy and interface settings apply exactly as they do to
// the lane clients.
func (p *pushChannel) dial() net.Conn {
	proxy := proxySnapshot()
	var lastErr error
	for _, address := range serverOrder(false) {
		conn, err := dialProxy(proxy, address, pushDialTimeout)
		if err != nil {
			lastErr = err
			continue
		}
		conn.SetDeadline(time.Now().Add(pushDialTimeout))
		if err := pushWrite(conn, pushHelloMethod, 0, []byte{0x01}); err != nil {
			conn.Close()
			lastErr = err
			markRouteFailure(false, address)
			continue
		}
		if _, err := pushRead(conn); err != nil {
			conn.Close()
			lastErr = err
			markRouteFailure(false, address)
			continue
		}
		conn.SetDeadline(time.Time{})
		return conn
	}
	if lastErr == nil {
		lastErr = errors.New("no TDX server accepted the push connection")
	}
	p.mu.Lock()
	p.lastError = lastErr.Error()
	p.mu.Unlock()
	return nil
}

func (p *pushChannel) apply(generation uint64, body []byte, byKey map[string]*pushTarget) {
	quotes, err := pushDecode(body, byKey)
	if err != nil {
		p.setError(generation, err)
		return
	}
	if len(quotes) == 0 {
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.generation != generation {
		return
	}
	for _, q := range quotes {
		p.quotes[q.Code] = q
	}
	p.updatedAt = time.Now()
	p.frames++
}

func (p *pushChannel) setConnected(generation uint64, connected bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.generation != generation {
		return
	}
	p.connected = connected
	if connected {
		p.lastError = ""
	}
}

func (p *pushChannel) setError(generation uint64, err error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.generation != generation {
		return
	}
	p.lastError = err.Error()
}

func (p *pushChannel) countRefresh(generation uint64) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.generation != generation {
		return
	}
	p.refreshes++
}

// pushRequest builds the 0x0547 payload: a code count, then market, symbol and a
// refresh cursor per instrument. The cursor is always zero because live servers
// return the current record for every requested code no matter what it holds.
func pushRequest(targets []pushTarget) []byte {
	payload := make([]byte, 0, 2+11*len(targets))
	payload = append(payload, byte(len(targets)), byte(len(targets)>>8))
	for _, t := range targets {
		payload = append(payload, t.market)
		code := make([]byte, 6)
		copy(code, t.code)
		payload = append(payload, code...)
		payload = append(payload, 0, 0, 0, 0)
	}
	return payload
}

func pushWrite(conn net.Conn, method uint16, sequence uint32, data []byte) error {
	length := uint16(len(data) + 2)
	frame := make([]byte, 12+len(data))
	frame[0] = 0x0c
	binary.LittleEndian.PutUint32(frame[1:5], sequence)
	frame[5] = 0x01
	binary.LittleEndian.PutUint16(frame[6:8], length)
	binary.LittleEndian.PutUint16(frame[8:10], length)
	binary.LittleEndian.PutUint16(frame[10:12], method)
	copy(frame[12:], data)
	_, err := conn.Write(frame)
	return err
}

type pushFrame struct {
	seq    uint32
	method uint16
	body   []byte
}

func pushRead(conn net.Conn) (*pushFrame, error) {
	header := make([]byte, proto.MessageHeaderBytes)
	if _, err := io.ReadFull(conn, header); err != nil {
		return nil, err
	}
	frame := &pushFrame{
		seq:    binary.LittleEndian.Uint32(header[5:9]),
		method: binary.LittleEndian.Uint16(header[10:12]),
	}
	zipSize := binary.LittleEndian.Uint16(header[12:14])
	unzipSize := binary.LittleEndian.Uint16(header[14:16])
	if zipSize > pushMaxFrame {
		return nil, errors.New("push frame exceeds the maximum message size")
	}
	body := make([]byte, zipSize)
	if _, err := io.ReadFull(conn, body); err != nil {
		return nil, err
	}
	if zipSize != unzipSize {
		reader, err := zlib.NewReader(bytes.NewReader(body))
		if err != nil {
			return nil, err
		}
		defer reader.Close()
		inflated, err := io.ReadAll(io.LimitReader(reader, 1<<20))
		if err != nil {
			return nil, err
		}
		body = inflated
	}
	frame.body = body
	return frame, nil
}

// pushDecode turns one 0x0547 body into quotes. The body is XOR 0x93 masked and
// holds a record count followed by variable-length records whose only delimiter
// is the requested symbol at the start of each record.
func pushDecode(body []byte, byKey map[string]*pushTarget) ([]pushQuote, error) {
	masked := make([]byte, len(body))
	for i, b := range body {
		masked[i] = b ^ 0x93
	}
	if len(masked) < 2 {
		return nil, errors.New("push payload is too short")
	}
	count := int(binary.LittleEndian.Uint16(masked))
	if count == 0 {
		return nil, nil
	}
	records, err := pushSplit(masked[2:], byKey, count)
	if err != nil {
		return nil, err
	}
	quotes := make([]pushQuote, 0, len(records))
	now := time.Now().UnixMilli()
	for _, record := range records {
		if quote, ok := pushRecord(record, byKey, now); ok {
			quotes = append(quotes, quote)
		}
	}
	return quotes, nil
}

func pushSplit(data []byte, byKey map[string]*pushTarget, count int) ([][]byte, error) {
	if count == 1 {
		return [][]byte{data}, nil
	}
	markers := make([][]byte, 0, len(byKey))
	for _, target := range byKey {
		marker := make([]byte, 7)
		marker[0] = target.market
		copy(marker[1:], target.code)
		markers = append(markers, marker)
	}
	starts := []int{0}
	searchFrom := 7
	for len(starts) < count {
		next := -1
		for _, marker := range markers {
			index := bytes.Index(data[searchFrom:], marker)
			if index < 0 {
				continue
			}
			if position := searchFrom + index; next < 0 || position < next {
				next = position
			}
		}
		if next < 0 {
			return nil, errors.New("push record marker not found")
		}
		starts = append(starts, next)
		searchFrom = next + 7
	}
	records := make([][]byte, 0, len(starts))
	for i, start := range starts {
		end := len(data)
		if i+1 < len(starts) {
			end = starts[i+1]
		}
		records = append(records, data[start:end])
	}
	return records, nil
}

// pushRecord reads the fields the quote list needs. Prices are deltas in
// hundredths of a tick against the current price, so one varint chain yields
// last, previous close, open, high and low together.
func pushRecord(record []byte, byKey map[string]*pushTarget, now int64) (pushQuote, bool) {
	if len(record) < 9 {
		return pushQuote{}, false
	}
	code := strings.TrimRight(string(record[1:7]), "\x00")
	target, ok := byKey[pushKey(record[0], code)]
	if !ok {
		return pushQuote{}, false
	}
	var deltas [5]int64
	offset := 9
	for i := range deltas {
		value, next, err := pushVarint(record, offset)
		if err != nil {
			return pushQuote{}, false
		}
		deltas[i] = value
		offset = next
	}
	// The server sends a price scaled by ten; milli() finishes the conversion to
	// the decimal place the instrument is quoted in.
	priceAt := func(delta int64) float64 {
		return float64((deltas[0]+delta)*10/target.divisor) / 1000.0
	}
	price := priceAt(0)
	if price <= 0 {
		// Unopened instruments stay on the snapshot path, which knows whether the
		// zero means a holiday, a call auction or a real reset.
		return pushQuote{}, false
	}
	preClose := priceAt(deltas[1])
	quote := pushQuote{Code: target.display, Price: price, PreClose: preClose, UpdatedMs: now}
	if preClose > 0 {
		quote.Change = price - preClose
		quote.Pct = quote.Change / preClose * 100
	}
	return quote, true
}

// pushVarint decodes the server's packed integer: the first byte carries six
// magnitude bits and a sign bit, later bytes carry seven bits each.
func pushVarint(payload []byte, offset int) (int64, int, error) {
	if offset >= len(payload) {
		return 0, offset, errors.New("push varint is truncated")
	}
	var magnitude uint64
	position := offset
	shift := uint(0)
	width := 0
	for {
		if position >= len(payload) {
			return 0, position, errors.New("push varint is unterminated")
		}
		value := payload[position]
		part := uint64(value & 0x3f)
		if width > 0 {
			part = uint64(value&0x7f) << shift
		}
		magnitude += part
		position++
		width++
		if value&0x80 == 0 {
			break
		}
		if width == 1 {
			shift = 6
		} else {
			shift += 7
		}
		if width > 10 {
			return 0, position, errors.New("push varint is too long")
		}
	}
	result := int64(magnitude)
	if payload[offset]&0x40 != 0 {
		result = -result
	}
	return result, position, nil
}
