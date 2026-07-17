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
	conn   net.Conn
	reader *bufio.Reader
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
	reader := bufio.NewReader(conn)
	rsp, err := http.ReadResponse(reader, req)
	if err != nil {
		return fail(err)
	}
	if rsp.StatusCode != http.StatusSwitchingProtocols {
		return fail(fmt.Errorf("websocket upgrade status %d", rsp.StatusCode))
	}
	if err := conn.SetDeadline(time.Time{}); err != nil {
		return fail(err)
	}
	return &WebSocket{conn: conn, reader: reader}, nil
}

func (ws *WebSocket) Close() error { return ws.conn.Close() }

func (ws *WebSocket) WriteBinary(payload []byte) error {
	return ws.writeFrame(0x2, payload)
}

func (ws *WebSocket) writeFrame(opcode byte, payload []byte) error {
	frame := []byte{0x80 | opcode}
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
	var message []byte
	continuing := false
	for {
		fin, opcode, payload, err := ws.readRawFrame()
		if err != nil {
			if continuing {
				_ = ws.Close()
			}
			return nil, err
		}
		switch opcode {
		case 0x8:
			_ = ws.Close()
			return nil, io.EOF
		case 0x9:
			if err := ws.writeFrame(0xA, payload); err != nil {
				return nil, err
			}
			continue
		case 0xA:
			continue
		case 0x1, 0x2:
			if continuing {
				_ = ws.Close()
				return nil, fmt.Errorf("new data frame during continuation")
			}
			message = append(message, payload...)
			if fin {
				return message, nil
			}
			continuing = true
		case 0x0:
			if !continuing {
				_ = ws.Close()
				return nil, fmt.Errorf("unexpected continuation frame")
			}
			message = append(message, payload...)
			if fin {
				return message, nil
			}
		default:
			_ = ws.Close()
			return nil, fmt.Errorf("unsupported websocket opcode %d", opcode)
		}
	}
}

func (ws *WebSocket) readRawFrame() (bool, byte, []byte, error) {
	var header [2]byte
	n, err := io.ReadFull(ws.reader, header[:])
	if err != nil {
		if n != 0 {
			_ = ws.Close()
		}
		return false, 0, nil, err
	}
	fin, opcode := header[0]&0x80 != 0, header[0]&0x0f
	masked := header[1]&0x80 != 0
	if masked {
		_ = ws.Close()
		return false, 0, nil, fmt.Errorf("masked websocket server frame")
	}
	lengthCode := header[1] & 0x7f
	if opcode&0x8 != 0 && (!fin || lengthCode > 125) {
		_ = ws.Close()
		return false, 0, nil, fmt.Errorf("invalid websocket control frame")
	}
	length := uint64(lengthCode)
	switch length {
	case 126:
		var size [2]byte
		if _, err := io.ReadFull(ws.reader, size[:]); err != nil {
			_ = ws.Close()
			return false, 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(size[:]))
	case 127:
		var size [8]byte
		if _, err := io.ReadFull(ws.reader, size[:]); err != nil {
			_ = ws.Close()
			return false, 0, nil, err
		}
		length = binary.BigEndian.Uint64(size[:])
	}
	if length > 1<<20 {
		_ = ws.Close()
		return false, 0, nil, fmt.Errorf("websocket frame too large: %d", length)
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(ws.reader, payload); err != nil {
		_ = ws.Close()
		return false, 0, nil, err
	}
	return fin, opcode, payload, nil
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
