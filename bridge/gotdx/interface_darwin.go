package main

import (
	"net"
	"strings"
	"syscall"
)

func bindInterface(socket syscall.RawConn, network string, iface *net.Interface) error {
	var bindErr error
	err := socket.Control(func(fd uintptr) {
		level, option := syscall.IPPROTO_IP, syscall.IP_BOUND_IF
		if strings.HasSuffix(network, "6") {
			level, option = syscall.IPPROTO_IPV6, syscall.IPV6_BOUND_IF
		}
		bindErr = syscall.SetsockoptInt(int(fd), level, option, iface.Index)
	})
	if err != nil {
		return err
	}
	return bindErr
}
