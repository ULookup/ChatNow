package client

import (
	"bufio"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func serverFrame(fin bool, opcode byte, payload string) []byte {
	first := opcode
	if fin {
		first |= 0x80
	}
	return append([]byte{first, byte(len(payload))}, []byte(payload)...)
}

func TestWebSocketPreservesFrameBufferedWithUpgrade(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hijacker := w.(http.Hijacker)
		conn, rw, err := hijacker.Hijack()
		if err != nil {
			return
		}
		defer conn.Close()
		_, _ = rw.WriteString("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n")
		_, _ = rw.Write(serverFrame(true, 2, "buffered"))
		_ = rw.Flush()
		time.Sleep(100 * time.Millisecond)
	}))
	defer server.Close()
	u, _ := url.Parse(server.URL)
	ws, err := OpenWebSocket(&Config{Target: TargetConfig{WebsocketAddr: u.Host}})
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()
	payload, err := ws.ReadFrame(time.Second)
	if err != nil || string(payload) != "buffered" {
		t.Fatalf("payload=%q err=%v", payload, err)
	}
}

func pipeWebSocket(t *testing.T, frames ...[]byte) *WebSocket {
	t.Helper()
	ws, serverConn := newPipeWebSocket(t)
	go func() {
		for _, frame := range frames {
			_, _ = serverConn.Write(frame)
			if len(frame) >= 2 && frame[0]&0x0f == 9 {
				pong := make([]byte, 7) // FIN+PONG, masked one-byte payload.
				_, _ = io.ReadFull(serverConn, pong)
			}
		}
	}()
	return ws
}

func newPipeWebSocket(t *testing.T) (*WebSocket, net.Conn) {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() { _ = clientConn.Close(); _ = serverConn.Close() })
	return &WebSocket{conn: clientConn, reader: bufio.NewReader(clientConn)}, serverConn
}

func requirePeerClosed(t *testing.T, peer net.Conn) {
	t.Helper()
	_ = peer.SetWriteDeadline(time.Now().Add(100 * time.Millisecond))
	_, err := peer.Write(serverFrame(true, 2, "probe"))
	if err == nil {
		t.Fatal("websocket peer remained writable")
	}
	if timeout, ok := err.(net.Error); ok && timeout.Timeout() {
		t.Fatalf("websocket peer was not closed: %v", err)
	}
}

func TestWebSocketSkipsPingBeforeData(t *testing.T) {
	ws := pipeWebSocket(t, serverFrame(true, 9, "p"), serverFrame(true, 2, "data"))
	payload, err := ws.ReadFrame(time.Second)
	if err != nil || string(payload) != "data" {
		t.Fatalf("payload=%q err=%v", payload, err)
	}
}

func TestWebSocketReassemblesFragmentsAcrossControlFrame(t *testing.T) {
	ws := pipeWebSocket(t,
		serverFrame(false, 2, "frag-"),
		serverFrame(true, 9, "p"),
		serverFrame(true, 0, "ment"),
	)
	payload, err := ws.ReadFrame(time.Second)
	if err != nil || string(payload) != "frag-ment" {
		t.Fatalf("payload=%q err=%v", payload, err)
	}
}

func TestWebSocketTimeoutBetweenFragmentsClosesConnection(t *testing.T) {
	ws, serverConn := newPipeWebSocket(t)
	go func() { _, _ = serverConn.Write(serverFrame(false, 2, "partial")) }()
	_, err := ws.ReadFrame(30 * time.Millisecond)
	if timeout, ok := err.(net.Error); !ok || !timeout.Timeout() {
		t.Fatalf("expected fragment timeout, got %v", err)
	}
	requirePeerClosed(t, serverConn)
}

func TestWebSocketRejectsMaskedServerFrame(t *testing.T) {
	ws, serverConn := newPipeWebSocket(t)
	masked := []byte{0x82, 0x81, 1, 2, 3, 4, 'x' ^ 1}
	go func() { _, _ = serverConn.Write(masked) }()
	_, err := ws.ReadFrame(time.Second)
	if err == nil || !strings.Contains(err.Error(), "masked") {
		t.Fatalf("expected masked-server error, got %v", err)
	}
	requirePeerClosed(t, serverConn)
}

func TestWebSocketRejectsInvalidControlFrames(t *testing.T) {
	tests := map[string][]byte{
		"fragmented": serverFrame(false, 9, "p"),
		"oversized":  {0x89, 126, 0, 126},
	}
	for name, frame := range tests {
		t.Run(name, func(t *testing.T) {
			ws, serverConn := newPipeWebSocket(t)
			go func() { _, _ = serverConn.Write(frame) }()
			_, err := ws.ReadFrame(time.Second)
			if err == nil || !strings.Contains(err.Error(), "control") {
				t.Fatalf("expected control-frame error, got %v", err)
			}
			requirePeerClosed(t, serverConn)
		})
	}
}
