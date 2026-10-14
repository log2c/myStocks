package main

import (
	"net"
	"syscall"
)

func bindInterface(socket syscall.RawConn, network string, iface *net.Interface) error {
	var bindErr error
	err := socket.Control(func(fd uintptr) {
		bindErr = syscall.SetsockoptString(int(fd), syscall.SOL_SOCKET, syscall.SO_BINDTODEVICE, iface.Name)
	})
	if err != nil {
		return err
	}
	return bindErr
}
