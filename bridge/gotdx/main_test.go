package main

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
)

func TestResolveMarketMappings(t *testing.T) {
	cases := []struct {
		input    string
		market   uint8
		code     string
		extended bool
	}{
		{"1.600519", 1, "600519", false},
		{"0.000001", 0, "000001", false},
		{"0.920002", 2, "920002", false},
		{"116.00700", 31, "00700", true},
		{"100.HSI", 27, "HSI", true},
		{"124.HSTECH", 27, "HZ5017", true},
		{"113.M0", 29, "ML8", true},
		{"114.AU0", 30, "AUL8", true},
		{"114.AU2610", 30, "AU2610", true},
		{"115.MA0", 28, "MAL8", true},
		{"8.IF0", 47, "IFL8", true},
		{"133.USDCNH", 10, "USDCNH", true},
	}
	for _, c := range cases {
		t.Run(c.input, func(t *testing.T) {
			got, err := resolve(c.input)
			if err != nil || got.market != c.market || got.code != c.code || got.extended != c.extended {
				t.Fatalf("resolve(%q)=%+v,%v", c.input, got, err)
			}
		})
	}
	for _, code := range []string{"", "1", "1.", "1.123", "1.ABCDEF", "90.BK0420", "100.SPX", "133.EURUSD"} {
		if _, err := resolve(code); err == nil {
			t.Fatalf("unverified %q accepted", code)
		}
	}
}
func TestMissingAndNonFiniteNumbers(t *testing.T) {
	values := map[string]any{"zero": float32(0), "speed": float64(-1.25), "nan": math.NaN(), "inf": math.Inf(1), "text": "0"}
	for _, key := range []string{"missing", "nan", "inf", "text"} {
		if field(values, key) != nil {
			t.Fatalf("%s must remain unavailable", key)
		}
	}
	if p := field(values, "zero"); p == nil || *p != 0 {
		t.Fatal("zero must not become missing")
	}
	if p := field(values, "speed"); p == nil || *p != -1.25 {
		t.Fatal("speed must stay in percent units")
	}
}
func TestCapabilitiesDoNotClaimAuthentication(t *testing.T) {
	result, err := dispatch(request{Operation: "capabilities", User: json.RawMessage(`{"userId":"test","token":"secret"}`)})
	if err != nil {
		t.Fatal(err)
	}
	data, _ := json.Marshal(result)
	if strings.Contains(string(data), "secret") || result.(map[string]any)["authentication"] != false {
		t.Fatal("unimplemented authentication or secret disclosed")
	}
	if _, err := dispatch(request{Operation: "login"}); err == nil {
		t.Fatal("login must not pretend to succeed")
	}
}
func TestUnknownCodesProduceExplicitPartialErrors(t *testing.T) {
	result := quotes(request{Stocks: []stock{{Code: "90.BK0420"}, {Code: "100.SPX"}}})
	if len(result["quotes"].([]quote)) != 0 || len(result["errors"].([]string)) != 2 {
		t.Fatalf("unexpected result: %#v", result)
	}
}
