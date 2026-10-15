package main

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/bensema/gotdx/proto"
)

type proxyConfig struct {
	Interface string `json:"interface"`
	Type      string `json:"type"`
	Host      string `json:"host"`
	Port      int    `json:"port"`
	User      string `json:"user"`
	Password  string `json:"password"`
}

func (p proxyConfig) enabled() bool { return p.Type != "" && p.Type != "none" }
func (p proxyConfig) validate() error {
	if !p.enabled() {
		return nil
	}
	if p.Type != "http" && p.Type != "socks5" {
		return errors.New("unsupported TDX proxy type")
	}
	if strings.TrimSpace(p.Host) == "" || p.Port < 1 || p.Port > 65535 {
		return errors.New("TDX proxy requires a host and port")
	}
	if len(p.User) > 255 || len(p.Password) > 255 {
		return errors.New("proxy credentials are too long")
	}
	return nil
}

type bufferedConn struct {
	net.Conn
	reader *bufio.Reader
}

func (c *bufferedConn) Read(p []byte) (int, error) { return c.reader.Read(p) }

func dialProxy(p proxyConfig, target string, timeout time.Duration) (net.Conn, error) {
	if err := p.validate(); err != nil {
		return nil, err
	}
	dialer, err := interfaceDialer(p.Interface, timeout)
	if err != nil {
		return nil, err
	}
	if !p.enabled() {
		return dialer.Dial("tcp", target)
	}
	conn, err := dialer.Dial("tcp", net.JoinHostPort(p.Host, strconv.Itoa(p.Port)))
	if err != nil {
		return nil, errors.New("cannot connect to configured proxy")
	}
	success := false
	defer func() {
		if !success {
			conn.Close()
		}
	}()
	conn.SetDeadline(time.Now().Add(timeout))
	if p.Type == "http" {
		req := &http.Request{Method: "CONNECT", URL: nil, Host: target, Header: make(http.Header)}
		// CONNECT uses an authority-form request target, supplied via opaque URL.
		auth := ""
		if p.User != "" || p.Password != "" {
			auth = "Proxy-Authorization: Basic " + base64.StdEncoding.EncodeToString([]byte(p.User+":"+p.Password)) + "\r\n"
		}
		if _, err = io.WriteString(conn, "CONNECT "+target+" HTTP/1.1\r\nHost: "+target+"\r\n"+auth+"\r\n"); err != nil {
			return nil, errors.New("proxy CONNECT write failed")
		}
		reader := bufio.NewReader(conn)
		resp, e := http.ReadResponse(reader, req)
		if e != nil {
			return nil, errors.New("invalid proxy CONNECT response")
		}
		if resp.StatusCode != 200 {
			return nil, fmt.Errorf("proxy CONNECT rejected (%d)", resp.StatusCode)
		}
		conn = &bufferedConn{conn, reader}
	} else {
		methods := []byte{5, 1, 0}
		if p.User != "" || p.Password != "" {
			methods = []byte{5, 1, 2}
		}
		if _, err = conn.Write(methods); err != nil {
			return nil, errors.New("SOCKS5 negotiation failed")
		}
		var reply [2]byte
		if _, err = io.ReadFull(conn, reply[:]); err != nil || reply[0] != 5 {
			return nil, errors.New("invalid SOCKS5 response")
		}
		if reply[1] == 2 {
			auth := append([]byte{1, byte(len(p.User))}, []byte(p.User)...)
			auth = append(auth, byte(len(p.Password)))
			auth = append(auth, []byte(p.Password)...)
			if _, err = conn.Write(auth); err != nil {
				return nil, errors.New("SOCKS5 authentication failed")
			}
			if _, err = io.ReadFull(conn, reply[:]); err != nil || reply[0] != 1 || reply[1] != 0 {
				return nil, errors.New("SOCKS5 authentication rejected")
			}
		} else if reply[1] != 0 || len(methods) == 3 && methods[2] != 0 {
			return nil, errors.New("SOCKS5 proxy does not support requested authentication")
		}
		host, portText, e := net.SplitHostPort(target)
		if e != nil {
			return nil, e
		}
		port, e := strconv.Atoi(portText)
		if e != nil || port < 1 || port > 65535 {
			return nil, errors.New("invalid proxy destination")
		}
		request := []byte{5, 1, 0}
		if ip := net.ParseIP(host); ip != nil {
			if v4 := ip.To4(); v4 != nil {
				request = append(request, 1)
				request = append(request, v4...)
			} else {
				request = append(request, 4)
				request = append(request, ip.To16()...)
			}
		} else {
			if len(host) > 255 {
				return nil, errors.New("proxy destination too long")
			}
			request = append(request, 3, byte(len(host)))
			request = append(request, []byte(host)...)
		}
		request = binary.BigEndian.AppendUint16(request, uint16(port))
		if _, err = conn.Write(request); err != nil {
			return nil, errors.New("SOCKS5 CONNECT failed")
		}
		var header [4]byte
		if _, err = io.ReadFull(conn, header[:]); err != nil || header[0] != 5 || header[1] != 0 || header[2] != 0 {
			return nil, errors.New("SOCKS5 CONNECT rejected")
		}
		length := 0
		switch header[3] {
		case 1:
			length = 4
		case 4:
			length = 16
		case 3:
			var b [1]byte
			if _, err = io.ReadFull(conn, b[:]); err != nil {
				return nil, err
			}
			length = int(b[0])
		default:
			return nil, errors.New("invalid SOCKS5 address")
		}
		if _, err = io.CopyN(io.Discard, conn, int64(length+2)); err != nil {
			return nil, err
		}
	}
	conn.SetDeadline(time.Time{})
	success = true
	return conn, nil
}

// gotdx has no custom dialer. Loopback tunnels keep the unmodified SDK's
// protocol implementation and ensure all remote traffic crosses the proxy.
type proxyTunnel struct {
	listener    net.Listener
	mu          sync.Mutex
	connections map[net.Conn]bool
	closed      bool
}

func newTunnel(p proxyConfig, hosts []string) (*proxyTunnel, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	t := &proxyTunnel{listener: listener, connections: make(map[net.Conn]bool)}
	go func() {
		for {
			local, err := listener.Accept()
			if err != nil {
				return
			}
			if !t.track(local) {
				local.Close()
				continue
			}
			go func(local net.Conn) {
				var err error
				defer t.remove(local)
				defer local.Close()
				var remote net.Conn
				for i, host := range hosts {
					if i >= 6 {
						break
					}
					remote, err = dialProxy(p, host, 3*time.Second)
					if err == nil {
						break
					}
				}
				if remote == nil {
					return
				}
				if !t.track(remote) {
					remote.Close()
					return
				}
				defer t.remove(remote)
				defer remote.Close()
				done := make(chan struct{})
				go func() { io.Copy(remote, local); remote.Close(); close(done) }()
				io.Copy(local, remote)
				local.Close()
				<-done
			}(local)
		}
	}()
	return t, nil
}
func (t *proxyTunnel) track(c net.Conn) bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.closed {
		return false
	}
	t.connections[c] = true
	return true
}
func (t *proxyTunnel) remove(c net.Conn) { t.mu.Lock(); delete(t.connections, c); t.mu.Unlock() }
func (t *proxyTunnel) close() {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.closed = true
	t.listener.Close()
	for c := range t.connections {
		c.Close()
	}
}

var activeProxy proxyConfig

func configureProxy(p proxyConfig) error {
	if err := p.validate(); err != nil {
		return err
	}
	if p == activeProxy {
		return nil
	}
	dropClient(false, false)
	dropClient(true, false)
	mainRoute.ordered, mainRoute.failed, mainRoute.expires = nil, nil, time.Time{}
	exRoute.ordered, exRoute.failed, exRoute.expires = nil, nil, time.Time{}
	activeProxy = p
	catalog = nil
	catalogWarning = ""
	boardLists = make(map[uint16][]proto.MACBoardListItem)
	breadthCache = nil
	breadthHistory = nil
	return nil
}
