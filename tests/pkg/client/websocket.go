package client

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"time"

	"google.golang.org/protobuf/encoding/protowire"
)

type WebSocket struct {
	conn net.Conn
}

func OpenWebSocket(cfg *Config) (*WebSocket, error) {
	addr := cfg.Target.WebsocketAddr
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	fail := func(err error) (*WebSocket, error) { _ = conn.Close(); return nil, err }
	if err := conn.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return fail(err)
	}
	keyBytes := make([]byte, 16)
	if _, err := rand.Read(keyBytes); err != nil {
		return fail(err)
	}
	req := &http.Request{
		Method: "GET", URL: &url.URL{Scheme: "http", Host: addr, Path: "/"}, Host: addr,
		Header: http.Header{"Connection": {"Upgrade"}, "Upgrade": {"websocket"},
			"Sec-Websocket-Key":     {base64.StdEncoding.EncodeToString(keyBytes)},
			"Sec-Websocket-Version": {"13"}},
	}
	if err := req.Write(conn); err != nil {
		return fail(err)
	}
	rsp, err := http.ReadResponse(bufio.NewReader(conn), req)
	if err != nil {
		return fail(err)
	}
	if rsp.StatusCode != http.StatusSwitchingProtocols {
		return fail(fmt.Errorf("websocket upgrade status %d", rsp.StatusCode))
	}
	if err := conn.SetDeadline(time.Time{}); err != nil {
		return fail(err)
	}
	return &WebSocket{conn: conn}, nil
}

func (ws *WebSocket) Close() error { return ws.conn.Close() }

func (ws *WebSocket) WriteBinary(payload []byte) error {
	frame := []byte{0x82}
	switch {
	case len(payload) < 126:
		frame = append(frame, 0x80|byte(len(payload)))
	case len(payload) <= 65535:
		frame = append(frame, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	default:
		frame = append(frame, 0x80|127)
		var size [8]byte
		binary.BigEndian.PutUint64(size[:], uint64(len(payload)))
		frame = append(frame, size[:]...)
	}
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	frame = append(frame, mask[:]...)
	for i, b := range payload {
		frame = append(frame, b^mask[i%len(mask)])
	}
	_, err := ws.conn.Write(frame)
	return err
}

func (ws *WebSocket) ReadFrame(timeout time.Duration) ([]byte, error) {
	if err := ws.conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return nil, err
	}
	var header [2]byte
	if _, err := io.ReadFull(ws.conn, header[:]); err != nil {
		return nil, err
	}
	length := uint64(header[1] & 0x7f)
	switch length {
	case 126:
		var size [2]byte
		if _, err := io.ReadFull(ws.conn, size[:]); err != nil {
			return nil, err
		}
		length = uint64(binary.BigEndian.Uint16(size[:]))
	case 127:
		var size [8]byte
		if _, err := io.ReadFull(ws.conn, size[:]); err != nil {
			return nil, err
		}
		length = binary.BigEndian.Uint64(size[:])
	}
	if length > 1<<20 {
		return nil, fmt.Errorf("websocket frame too large: %d", length)
	}
	var mask [4]byte
	if header[1]&0x80 != 0 {
		if _, err := io.ReadFull(ws.conn, mask[:]); err != nil {
			return nil, err
		}
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(ws.conn, payload); err != nil {
		return nil, err
	}
	if header[1]&0x80 != 0 {
		for i := range payload {
			payload[i] ^= mask[i%len(mask)]
		}
	}
	return payload, nil
}

func PushAuthNotify(accessToken, deviceID string) []byte {
	var auth []byte
	auth = appendProtoString(auth, 1, accessToken)
	auth = appendProtoString(auth, 2, deviceID)
	var notify []byte
	notify = protowire.AppendTag(notify, 2, protowire.VarintType)
	notify = protowire.AppendVarint(notify, 49)
	notify = protowire.AppendTag(notify, 10, protowire.BytesType)
	return protowire.AppendBytes(notify, auth)
}

func appendProtoString(dst []byte, field protowire.Number, value string) []byte {
	dst = protowire.AppendTag(dst, field, protowire.BytesType)
	return protowire.AppendString(dst, value)
}
