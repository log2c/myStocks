//go:build !darwin && !windows && !linux

package main

import (
	"fmt"
	"net"
	"syscall"
)

func bindInterface(socket syscall.RawConn, network string, iface *net.Interface) error {
	return fmt.Errorf("network interface binding is unsupported on this platform")
}
