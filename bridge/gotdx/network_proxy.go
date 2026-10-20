package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"strconv"
	"sync"
	"time"
)

// Qt has no general API to bind QNetworkAccessManager to an interface. Its
// HTTP(S) traffic uses an authenticated loopback SOCKS5 relay; TLS stays in Qt,
// and every remote connection (including an upstream proxy) uses dialProxy.
// Relay creation uses its own lock, independent of serialized TDX requests.
type networkRelay struct {
	listener net.Listener
	password string
	proxy    proxyConfig
}

var relayMu sync.Mutex
var relays = make(map[proxyConfig]*networkRelay)

func networkProxyRequest(input []byte) (any, error) {
	var p proxyConfig
	if err := json.Unmarshal(input, &p); err != nil {
		return nil, fmt.Errorf("invalid network proxy settings")
	}
	if err := p.validate(); err != nil {
		return nil, err
	}
	relayMu.Lock()
	defer relayMu.Unlock()
	relay := relays[p]
	if relay == nil {
		listener, err := net.Listen("tcp4", "127.0.0.1:0")
		if err != nil {
			return nil, fmt.Errorf("cannot start interface relay")
		}
		secret := make([]byte, 24)
		if _, err := rand.Read(secret); err != nil {
			listener.Close()
			return nil, err
		}
		relay = &networkRelay{listener: listener, password: hex.EncodeToString(secret), proxy: p}
		relays[p] = relay
		go relay.serve()
	}
	return map[string]any{"host": "127.0.0.1", "port": relay.listener.Addr().(*net.TCPAddr).Port,
		"user": "mystocks", "password": relay.password}, nil
}

func (relay *networkRelay) serve() {
	for {
		conn, err := relay.listener.Accept()
		if err != nil {
			return
		}
		go relay.handle(conn)
	}
}

func (relay *networkRelay) handle(conn net.Conn) {
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(20 * time.Second))
	var header [2]byte
	if _, err := io.ReadFull(conn, header[:]); err != nil || header[0] != 5 || header[1] == 0 {
		return
	}
	methods := make([]byte, int(header[1]))
	if _, err := io.ReadFull(conn, methods); err != nil {
		return
	}
	authentication := false
	for _, method := range methods {
		if method == 2 {
			authentication = true
		}
	}
	if !authentication {
		conn.Write([]byte{5, 255})
		return
	}
	if _, err := conn.Write([]byte{5, 2}); err != nil {
		return
	}
	if _, err := io.ReadFull(conn, header[:]); err != nil || header[0] != 1 {
		return
	}
	user := make([]byte, int(header[1]))
	if _, err := io.ReadFull(conn, user); err != nil {
		return
	}
	var length [1]byte
	if _, err := io.ReadFull(conn, length[:]); err != nil {
		return
	}
	password := make([]byte, int(length[0]))
	if _, err := io.ReadFull(conn, password); err != nil {
		return
	}
	if string(user) != "mystocks" || subtle.ConstantTimeCompare(password, []byte(relay.password)) != 1 {
		conn.Write([]byte{1, 1})
		return
	}
	if _, err := conn.Write([]byte{1, 0}); err != nil {
		return
	}
	var request [4]byte
	if _, err := io.ReadFull(conn, request[:]); err != nil || request[0] != 5 || request[1] != 1 || request[2] != 0 {
		return
	}
	var host string
	switch request[3] {
	case 1:
		address := make([]byte, 4)
		if _, err := io.ReadFull(conn, address); err != nil {
			return
		}
		host = net.IP(address).String()
	case 4:
		address := make([]byte, 16)
		if _, err := io.ReadFull(conn, address); err != nil {
			return
		}
		host = net.IP(address).String()
	case 3:
		if _, err := io.ReadFull(conn, length[:]); err != nil || length[0] == 0 {
			return
		}
		address := make([]byte, int(length[0]))
		if _, err := io.ReadFull(conn, address); err != nil {
			return
		}
		host = string(address)
	default:
		return
	}
	if _, err := io.ReadFull(conn, header[:]); err != nil {
		return
	}
	port := int(header[0])<<8 | int(header[1])
	remote, err := dialProxy(relay.proxy, net.JoinHostPort(host, strconv.Itoa(port)), 15*time.Second)
	if err != nil {
		conn.Write([]byte{5, 1, 0, 1, 0, 0, 0, 0, 0, 0})
		return
	}
	defer remote.Close()
	if _, err := conn.Write([]byte{5, 0, 0, 1, 0, 0, 0, 0, 0, 0}); err != nil {
		return
	}
	conn.SetDeadline(time.Time{})
	done := make(chan struct{})
	go func() { io.Copy(remote, conn); remote.Close(); close(done) }()
	io.Copy(conn, remote)
	conn.Close()
	<-done
}
