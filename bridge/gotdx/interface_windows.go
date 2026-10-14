package main

import (
	"math/bits"
	"net"
	"strings"
	"syscall"
)

func bindInterface(socket syscall.RawConn, network string, iface *net.Interface) error {
	var bindErr error
	err := socket.Control(func(fd uintptr) {
		// IP_UNICAST_IF uses a network-order IPv4 interface index;
		// IPV6_UNICAST_IF uses host order. Both option values are 31.
		level, index := syscall.IPPROTO_IP, int(bits.ReverseBytes32(uint32(iface.Index)))
		if strings.HasSuffix(network, "6") {
			level, index = syscall.IPPROTO_IPV6, iface.Index
		}
		bindErr = syscall.SetsockoptInt(syscall.Handle(fd), level, 31, index)
	})
	if err != nil {
		return err
	}
	return bindErr
}
