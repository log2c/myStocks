package main

import (
	"context"
	"fmt"
	"net"
	"syscall"
	"time"
)

// The interface is resolved for every connection so unplugging/reconnecting an
// adapter cannot silently route traffic over a different default interface.
func interfaceDialer(name string, timeout time.Duration) (*net.Dialer, error) {
	dialer := &net.Dialer{Timeout: timeout}
	if name == "" {
		return dialer, nil
	}
	iface, err := net.InterfaceByName(name)
	if err != nil {
		return nil, fmt.Errorf("selected network interface %q is unavailable", name)
	}
	if iface.Flags&net.FlagUp == 0 {
		return nil, fmt.Errorf("selected network interface %q is down", name)
	}
	control := func(network, address string, socket syscall.RawConn) error {
		return bindInterface(socket, network, iface)
	}
	dialer.Control = control
	// DNS must use the same interface as TCP. A SOCKS5/CONNECT destination name
	// is still resolved by the upstream proxy; only its own hostname is local.
	dialer.Resolver = &net.Resolver{PreferGo: true,
		Dial: func(ctx context.Context, network, address string) (net.Conn, error) {
			dns := net.Dialer{Timeout: timeout, Control: control}
			return dns.DialContext(ctx, network, address)
		}}
	return dialer, nil
}
