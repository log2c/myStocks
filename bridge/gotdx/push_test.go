package main

import (
	"encoding/hex"
	"fmt"
	"math"
	"testing"
	"time"
)

// capturedRefreshBody is one real 0x0547 response for 600519 as the server sent
// it. The values it carries were confirmed against a MACSymbolQuotes snapshot of
// the same session: 1392.05 against a 1354.00 previous close.
const capturedRefreshBody = "929392a5a3a3a6a2aaf682166c834ea84abe179150ae39c691931b8f043b9421b4408565dc19" +
	"46911e40979317356b80d1939192d69290927a9290919255919799984891969292c594db21939386bb67d4566c" +
	"83566c839393566c83566c839393566c83566c839393566c83566c839393566c83566c839393566c83566c8393" +
	"93566c83566c839393"

// capturedTwoRefreshBody holds the same instrument next to 000001, which is what
// makes the tag search and the record boundaries matter.
const capturedTwoRefreshBody = "919392a5a3a3a6a2aaf682166c834ea84abe179150ae39c691931b8f043b9421b4408565dc1946911e40979317356b80" +
	"d1939192d69290927a9290919255919799984891969292c594db21939386bb67d4566c83566c839393566c83566c8393" +
	"93566c83566c839393566c83566c839393566c83566c839393566c83566c83939393a3a3a3a3a3a2e0811f81d9d890cb" +
	"49c591933b3c95265b70923f1a977ed993dc1614f12352129293090431929392108a08a6d29101f51ab2d19039c1331f" +
	"91d09727ac057891d79616e723019245923a879193939393935f815f8193935f815f8193935f815f8193935f815f8193" +
	"935f815f8193935f815f819393"

func mustHex(t *testing.T, value string) []byte {
	t.Helper()
	decoded, err := hex.DecodeString(value)
	if err != nil {
		t.Fatalf("decode fixture: %v", err)
	}
	return decoded
}

func TestPushVarintMatchesSignedPacking(t *testing.T) {
	// Positive values keep six magnitude bits in the first byte, negatives set
	// the sign bit, and any value that does not fit adds seven bits per byte.
	cases := []struct {
		encoded []byte
		want    int64
	}{
		{[]byte{0x00}, 0},
		{[]byte{0x3f}, 63},
		{[]byte{0x80, 0x01}, 64},
		{[]byte{0x40}, 0},  // sign bit only: negative zero
		{[]byte{0x41}, -1}, // magnitude 1 with the sign bit set
		{[]byte{0x7f}, -63},
	}
	for _, test := range cases {
		got, next, err := pushVarint(test.encoded, 0)
		if err != nil {
			t.Fatalf("% x: %v", test.encoded, err)
		}
		if got != test.want || next != len(test.encoded) {
			t.Fatalf("% x: got %d at %d, want %d at %d", test.encoded, got, next, test.want, len(test.encoded))
		}
	}
}

func TestPushDecodeSingleRecordMatchesSnapshot(t *testing.T) {
	targets := []pushTarget{{display: "1.600519", market: 1, code: "600519", divisor: 1}}
	quotes, err := pushDecode(mustHex(t, capturedRefreshBody), pushIndex(targets))
	if err != nil {
		t.Fatal(err)
	}
	if len(quotes) != 1 {
		t.Fatalf("decoded %d quotes, want 1", len(quotes))
	}
	quote := quotes[0]
	if quote.Code != "1.600519" {
		t.Fatalf("code %q was not the requested watch code", quote.Code)
	}
	// The wire carries hundredths of a tick; two decimals need no extra divisor.
	if math.Abs(quote.Price-1392.05) > 0.0001 {
		t.Fatalf("price %v, want 1392.05", quote.Price)
	}
	if math.Abs(quote.PreClose-1354.00) > 0.0001 {
		t.Fatalf("preClose %v, want 1354.00", quote.PreClose)
	}
	wantPct := (1392.05 - 1354.0) / 1354.0 * 100
	if math.Abs(quote.Pct-wantPct) > 1e-9 {
		t.Fatalf("pct %v, want %v", quote.Pct, wantPct)
	}
}

func TestPushDecodeSplitsRecordsBySymbolTag(t *testing.T) {
	targets := []pushTarget{
		{display: "1.600519", market: 1, code: "600519", divisor: 1},
		{display: "0.000001", market: 0, code: "000001", divisor: 1},
	}
	quotes, err := pushDecode(mustHex(t, capturedTwoRefreshBody), pushIndex(targets))
	if err != nil {
		t.Fatal(err)
	}
	if len(quotes) != 2 {
		t.Fatalf("decoded %d quotes, want 2", len(quotes))
	}
	byCode := map[string]pushQuote{}
	for _, quote := range quotes {
		byCode[quote.Code] = quote
	}
	if got := byCode["1.600519"].Price; math.Abs(got-1392.05) > 0.0001 {
		t.Fatalf("600519 price %v, want 1392.05", got)
	}
	// The same symbol in market 0 is 平安银行, so a market-blind tag search would
	// hand the second record to the first subscription.
	if got := byCode["0.000001"].Price; math.Abs(got-11.64) > 0.0001 {
		t.Fatalf("000001 price %v, want 11.64", got)
	}
}

func TestPushDivisorFollowsQuotePrecision(t *testing.T) {
	cases := []struct {
		decimals *float64
		want     int64
	}{
		{nil, 1},
		{number(0), 1},
		{number(2), 1},
		{number(3), 10},
		{number(4), 100},
		{number(5), 1000},
		{number(9), 1}, // nonsense precision falls back to the mainland default
	}
	for _, test := range cases {
		if got := pushDivisor(test.decimals); got != test.want {
			t.Fatalf("decimals %v: divisor %d, want %d", test.decimals, got, test.want)
		}
	}
}

func TestPushRequestLaysOutCodesAndCursors(t *testing.T) {
	payload := pushRequest([]pushTarget{
		{market: 1, code: "600519"},
		{market: 0, code: "000001"},
	})
	if len(payload) != 2+11*2 {
		t.Fatalf("payload is %d bytes, want %d", len(payload), 2+11*2)
	}
	if payload[0] != 2 || payload[1] != 0 {
		t.Fatalf("count prefix is % x, want 02 00", payload[:2])
	}
	first := payload[2:13]
	if first[0] != 1 || string(first[1:7]) != "600519" {
		t.Fatalf("first record is % x", first)
	}
	if cursor := first[7:11]; cursor[0] != 0 || cursor[1] != 0 || cursor[2] != 0 || cursor[3] != 0 {
		t.Fatalf("cursor is % x, want four zero bytes", cursor)
	}
	second := payload[13:24]
	if second[0] != 0 || string(second[1:7]) != "000001" {
		t.Fatalf("second record is % x", second)
	}
}

func TestPushDecodeRejectsUnrequestedSymbols(t *testing.T) {
	// A record for an instrument nobody subscribed must never reach the cache,
	// otherwise a stale subscription could overwrite a row it does not own.
	targets := []pushTarget{{display: "1.600000", market: 1, code: "600000", divisor: 1}}
	quotes, err := pushDecode(mustHex(t, capturedRefreshBody), pushIndex(targets))
	if err != nil {
		t.Fatal(err)
	}
	if len(quotes) != 0 {
		t.Fatalf("decoded %d quotes for an unrequested symbol", len(quotes))
	}
}

func pushIndex(targets []pushTarget) map[string]*pushTarget {
	index := make(map[string]*pushTarget, len(targets))
	for i := range targets {
		target := &targets[i]
		index[pushKey(target.market, target.code)] = target
	}
	return index
}

func TestPushSelectTargetsCapsAtTheLimit(t *testing.T) {
	// A watchlist longer than the cap must subscribe the first pushMaxCodes
	// instruments and report the rest, not silently drop them.
	stocks := make([]stock, 0, pushMaxCodes+8)
	for i := 0; i < pushMaxCodes+8; i++ {
		stocks = append(stocks, stock{Code: fmt.Sprintf("1.%06d", 600000+i), Name: "x"})
	}
	targets, unsupported, overLimit := pushSelectTargets(stocks)
	if len(targets) != pushMaxCodes {
		t.Fatalf("subscribed %d instruments, want the cap of %d", len(targets), pushMaxCodes)
	}
	if overLimit != 8 {
		t.Fatalf("overLimit %d, want 8", overLimit)
	}
	if unsupported != 0 {
		t.Fatalf("unsupported %d, want 0", unsupported)
	}
	if targets[0].display != "1.600000" {
		t.Fatalf("first target %q, want the first watch code", targets[0].display)
	}
}

func TestPushSelectTargetsSeparatesUnsupportedInstruments(t *testing.T) {
	// Hong Kong and futures cannot ride 0x0547, and an unknown EastMoney code has
	// no TDX mapping at all; both are counted as unsupported, never as overLimit.
	stocks := []stock{
		{Code: "1.600519", Name: "贵州茅台"},
		{Code: "116.00700", Name: "腾讯控股"},
		{Code: "113.RB0", Name: "螺纹钢"},
		{Code: "90.BK0475", Name: "东财板块"},
		{Code: "0.000001", Name: "平安银行"},
	}
	targets, unsupported, overLimit := pushSelectTargets(stocks)
	if len(targets) != 2 || unsupported != 3 || overLimit != 0 {
		t.Fatalf("got targets=%d unsupported=%d overLimit=%d, want 2/3/0",
			len(targets), unsupported, overLimit)
	}
	if targets[0].market != 1 || targets[0].code != "600519" {
		t.Fatalf("first target is %+v", targets[0])
	}
	if targets[1].market != 0 || targets[1].code != "000001" {
		t.Fatalf("second target is %+v", targets[1])
	}
}

func TestPushSelectTargetsDropsDuplicates(t *testing.T) {
	// A code that appears twice must occupy one slot, otherwise it would consume
	// two of the thirty and the cap would stop short of the real list.
	stocks := []stock{
		{Code: "1.600519", Name: "贵州茅台"},
		{Code: "1.600519", Name: "贵州茅台"},
		{Code: "0.000001", Name: "平安银行"},
	}
	targets, unsupported, overLimit := pushSelectTargets(stocks)
	if len(targets) != 2 || unsupported != 0 || overLimit != 0 {
		t.Fatalf("got targets=%d unsupported=%d overLimit=%d, want 2/0/0",
			len(targets), unsupported, overLimit)
	}
}

func TestPushIntervalClampsTheAppSetting(t *testing.T) {
	// The app's polling setting drives the refresh cadence, but a bad value must
	// neither hammer a server nor let the cache go stale for minutes.
	cases := []struct {
		milliseconds int
		want         time.Duration
	}{
		{0, pushDefaultRefresh},
		{-1000, pushDefaultRefresh},
		{100, pushMinRefresh},
		{500, 500 * time.Millisecond},
		{1000, time.Second},
		{3000, 3 * time.Second},
		{60000, pushMaxRefresh},
	}
	for _, test := range cases {
		if got := pushInterval(test.milliseconds); got != test.want {
			t.Fatalf("%d ms: got %v, want %v", test.milliseconds, got, test.want)
		}
	}
}
