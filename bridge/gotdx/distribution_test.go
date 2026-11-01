package main

import (
	"strings"
	"testing"
)

// capturedDistribution is one real response, recorded on 2026-10-30 after the
// close. Note the first band's key: the source labels it ">10%" even though it
// holds the falling tail, which is why labels come from position.
const capturedDistribution = `{"status_code":0,"data":{"suspend":6,"last_update_time":"2026-10-30 15:34:01","limit_down":86,"limit_up":49,"flat":77,"up":892,"down":4602,"table":[{"value":123,"key":">10%"},{"value":513,"key":"10~7"},{"value":839,"key":"7~5"},{"value":1401,"key":"5~3"},{"value":1726,"key":"3~0"},{"value":77,"key":"0"},{"value":652,"key":"0~3"},{"value":117,"key":"3~5"},{"value":48,"key":"5~7"},{"value":37,"key":"7~10"},{"value":38,"key":">10%"}]},"status_msg":"success"}`

func TestParseTHSDistributionUsesPositionalLabels(t *testing.T) {
	bands, err := parseTHSDistribution([]byte(capturedDistribution))
	if err != nil {
		t.Fatal(err)
	}
	if len(bands) != thsDistributionBuckets {
		t.Fatalf("parsed %d bands, want %d", len(bands), thsDistributionBuckets)
	}
	// The first band is the falling tail, not ">10%" as the payload claims.
	if bands[0]["bucket"] != "<-10%" || bands[0]["value"] != 123 {
		t.Fatalf("first band is %+v, want <-10%% = 123", bands[0])
	}
	if bands[5]["bucket"] != "0%" || bands[5]["value"] != 77 {
		t.Fatalf("flat band is %+v, want 0%% = 77", bands[5])
	}
	if bands[10]["bucket"] != ">10%" || bands[10]["value"] != 38 {
		t.Fatalf("last band is %+v, want >10%% = 38", bands[10])
	}
}

func TestParseTHSDistributionRejectsBadPayloads(t *testing.T) {
	cases := []struct {
		name string
		body string
	}{
		{"not json", `{"status_code":0,"data":`},
		{"status", strings.Replace(capturedDistribution, `"status_code":0`, `"status_code":1`, 1)},
		{"no data", `{"status_code":0}`},
		{"truncated bands", strings.Replace(capturedDistribution, `,{"value":38,"key":">10%"}]`, `]`, 1)},
		{"reordered bands", strings.Replace(capturedDistribution, `{"value":77,"key":"0"}`, `{"value":77,"key":"0~3"}`, 1)},
		{"counts disagree", strings.Replace(capturedDistribution, `"up":892`, `"up":9999`, 1)},
		{"negative band", strings.Replace(capturedDistribution, `"value":123`, `"value":-1`, 1)},
	}
	for _, test := range cases {
		if _, err := parseTHSDistribution([]byte(test.body)); err == nil {
			t.Fatalf("%s: expected a rejection so the TDX fallback takes over", test.name)
		}
	}
}

func TestDistributionLabelsMatchTheBandsBothSourcesProduce(t *testing.T) {
	// The TDX fallback bins with the same edges as the published aggregate, so a
	// source switch must not relabel the chart.
	if len(distributionLabels) != thsDistributionBuckets {
		t.Fatalf("%d labels for %d bands", len(distributionLabels), thsDistributionBuckets)
	}
	for pct, want := range map[float64]string{
		-12: "<-10%", -10: "<-10%", -8: "-10~-7%", -4: "-5~-3%", -0.5: "-3~0%",
		0: "0%", 1: "0~3%", 4: "3~5%", 6: "5~7%", 8: "7~10%", 12: ">10%",
	} {
		if got := distributionLabels[distributionBin(pct)]; got != want {
			t.Fatalf("%v%% lands in %q, want %q", pct, got, want)
		}
	}
}
