package main

// The full-market up/down distribution used to be built by pulling every
// constituent over 0x122C: ~2.8 s and 1.4 MB for eleven numbers. The same
// aggregate is published as a small JSON document, so it is read there first and
// the full pull is kept only as a fallback.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"time"
)

const (
	thsDistributionURL     = "https://dq.10jqka.com.cn/fuyao/up_down_distribution/distribution/v2/realtime"
	thsDistributionTimeout = 3 * time.Second
	// The source always answers with the same eleven bands.
	thsDistributionBuckets = 11
	// The flat band sits in the middle; the two end keys both read ">10%".
	thsDistributionFlatIndex = 5
)

// distributionLabels are positional. The source lists the most negative band
// first and its own key for that entry is wrong (it repeats ">10%"), so the label
// comes from the position instead of the payload.
var distributionLabels = []string{
	"<-10%", "-10~-7%", "-7~-5%", "-5~-3%", "-3~0%", "0%",
	"0~3%", "3~5%", "5~7%", "7~10%", ">10%",
}

// The distribution is an HTTP fetch, but it must honour the same proxy and
// network-interface settings as quotation traffic, so it dials through the
// existing proxy path rather than the default dialer.
var thsClient = &http.Client{
	Timeout: thsDistributionTimeout,
	Transport: &http.Transport{
		DialContext: func(_ context.Context, _, address string) (net.Conn, error) {
			return dialProxy(proxySnapshot(), address, thsDistributionTimeout)
		},
		MaxIdleConns:        2,
		IdleConnTimeout:     90 * time.Second,
		TLSHandshakeTimeout: thsDistributionTimeout,
	},
}

func thsDistribution() ([]map[string]any, error) {
	request, err := http.NewRequest(http.MethodGet, thsDistributionURL, nil)
	if err != nil {
		return nil, err
	}
	response, err := thsClient.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("up/down distribution returned HTTP %d", response.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, 1<<16))
	if err != nil {
		return nil, err
	}
	return parseTHSDistribution(body)
}

// parseTHSDistribution validates the document before trusting it. A truncated or
// reordered payload must fall back to the TDX pull rather than draw a wrong
// histogram, so the band count, the flat-band position and the total all have to
// agree with the published counts.
func parseTHSDistribution(body []byte) ([]map[string]any, error) {
	var payload struct {
		StatusCode int `json:"status_code"`
		Data       *struct {
			Up      int `json:"up"`
			Down    int `json:"down"`
			Flat    int `json:"flat"`
			Suspend int `json:"suspend"`
			Table   []struct {
				Key   string `json:"key"`
				Value int    `json:"value"`
			} `json:"table"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &payload); err != nil {
		return nil, fmt.Errorf("up/down distribution is not valid JSON: %w", err)
	}
	if payload.StatusCode != 0 {
		return nil, fmt.Errorf("up/down distribution reported status %d", payload.StatusCode)
	}
	if payload.Data == nil {
		return nil, errors.New("up/down distribution carries no data")
	}
	table := payload.Data.Table
	if len(table) != thsDistributionBuckets {
		return nil, fmt.Errorf("up/down distribution has %d bands, want %d", len(table), thsDistributionBuckets)
	}
	if table[thsDistributionFlatIndex].Key != "0" {
		return nil, errors.New("up/down distribution band order changed")
	}
	sum := 0
	for _, band := range table {
		if band.Value < 0 {
			return nil, errors.New("up/down distribution carries a negative band")
		}
		sum += band.Value
	}
	if total := payload.Data.Up + payload.Data.Flat + payload.Data.Down; total > 0 && sum != total {
		return nil, fmt.Errorf("up/down distribution sums to %d but the counts total %d", sum, total)
	}
	result := make([]map[string]any, 0, len(table))
	for i, band := range table {
		result = append(result, map[string]any{"bucket": distributionLabels[i], "value": band.Value})
	}
	return result, nil
}
