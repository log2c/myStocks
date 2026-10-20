package main

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"io"
	"net"
	"net/http"
	"strconv"
	"testing"
	"time"
)

func proxyAddress(t *testing.T, listener net.Listener, kind string) proxyConfig {
	t.Helper()
	h, p, _ := net.SplitHostPort(listener.Addr().String())
	n, _ := strconv.Atoi(p)
	return proxyConfig{Type: kind, Host: h, Port: n, User: "proxy-user", Password: "proxy-secret"}
}
func TestHTTPConnectPreservesBufferedPayload(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	errors := make(chan error, 1)
	go func() {
		c, err := listener.Accept()
		if err != nil {
			errors <- err
			return
		}
		defer c.Close()
		req, err := http.ReadRequest(bufio.NewReader(c))
		if err != nil {
			errors <- err
			return
		}
		if req.Method != "CONNECT" || req.Host != "tdx.example:7709" || req.Header.Get("Proxy-Authorization") != "Basic "+base64.StdEncoding.EncodeToString([]byte("proxy-user:proxy-secret")) {
			errors <- io.ErrUnexpectedEOF
			return
		}
		_, err = io.WriteString(c, "HTTP/1.1 200 Connection established\r\n\r\nTDX")
		errors <- err
	}()
	conn, err := dialProxy(proxyAddress(t, listener, "http"), "tdx.example:7709", time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	buf := make([]byte, 3)
	if _, err := io.ReadFull(conn, buf); err != nil || string(buf) != "TDX" {
		t.Fatalf("lost CONNECT payload: %q %v", buf, err)
	}
	if err := <-errors; err != nil {
		t.Fatal(err)
	}
}
func TestSocks5AuthenticationAndRemoteDNS(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	errors := make(chan error, 1)
	go func() {
		c, err := listener.Accept()
		if err != nil {
			errors <- err
			return
		}
		defer c.Close()
		c.SetDeadline(time.Now().Add(2 * time.Second))
		hello := make([]byte, 3)
		if _, err := io.ReadFull(c, hello); err != nil {
			errors <- err
			return
		}
		c.Write([]byte{5, 2})
		auth := make([]byte, 2)
		io.ReadFull(c, auth)
		user := make([]byte, int(auth[1]))
		io.ReadFull(c, user)
		size := []byte{0}
		io.ReadFull(c, size)
		pass := make([]byte, int(size[0]))
		io.ReadFull(c, pass)
		if string(user) != "proxy-user" || string(pass) != "proxy-secret" {
			errors <- io.ErrUnexpectedEOF
			return
		}
		c.Write([]byte{1, 0})
		header := make([]byte, 5)
		io.ReadFull(c, header)
		if header[3] != 3 {
			errors <- io.ErrUnexpectedEOF
			return
		}
		host := make([]byte, int(header[4]))
		io.ReadFull(c, host)
		port := make([]byte, 2)
		io.ReadFull(c, port)
		if string(host) != "unresolved.example" || binary.BigEndian.Uint16(port) != 7709 {
			errors <- io.ErrUnexpectedEOF
			return
		}
		c.Write([]byte{5, 0, 0, 1, 127, 0, 0, 1, 0, 0})
		_, err = io.WriteString(c, "TDX")
		errors <- err
	}()
	conn, err := dialProxy(proxyAddress(t, listener, "socks5"), "unresolved.example:7709", time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	buf := make([]byte, 3)
	if _, err := io.ReadFull(conn, buf); err != nil || string(buf) != "TDX" {
		t.Fatal(err)
	}
	if err := <-errors; err != nil {
		t.Fatal(err)
	}
}
func TestConfiguredProxyMustNotFallBackToDirect(t *testing.T) {
	target, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer target.Close()
	p := proxyConfig{Type: "http", Host: "127.0.0.1", Port: 1}
	if conn, err := dialProxy(p, target.Addr().String(), 100*time.Millisecond); err == nil {
		conn.Close()
		t.Fatal("a configured failed proxy was bypassed")
	}
	if err := (proxyConfig{Type: "socks5"}).validate(); err == nil {
		t.Fatal("incomplete enabled proxy must fail")
	}
}
