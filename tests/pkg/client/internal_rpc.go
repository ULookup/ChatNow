package client

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/url"
	"time"

	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
)

// DoInternalRPC makes one uncompressed baidu_std call on a disposable stack.
// It forwards fixture metadata across the existing trusted internal boundary;
// it does not authenticate callers or implement a public client protocol.
// Wire reference: apache/brpc 041cec5, src/brpc/policy/baidu_rpc_meta.proto
// and baidu_rpc_protocol.cpp. One connection owns one correlation ID.
func (c *HTTPClient) DoInternalRPC(endpoint, service, method string, req, resp proto.Message, attachment []byte) error {
	u, err := url.Parse(endpoint)
	if err != nil || u.Scheme != "http" || u.Host == "" || u.Port() == "" || u.User != nil ||
		(u.Path != "" && u.Path != "/") || u.RawQuery != "" || u.Fragment != "" {
		return fmt.Errorf("internal RPC requires an explicit test endpoint host and port")
	}
	if service == "" || method == "" {
		return fmt.Errorf("internal RPC service and method required")
	}
	payload, err := proto.Marshal(req)
	if err != nil {
		return fmt.Errorf("encode RPC request: %w", err)
	}
	requestMeta := rpcBytes(nil, 1, []byte(service))
	requestMeta = rpcBytes(requestMeta, 2, []byte(method))
	meta := rpcBytes(nil, 1, requestMeta)
	meta = rpcUint(meta, 4, 1)
	meta = rpcUint(meta, 5, uint64(len(attachment)))
	const maxFrame = 1 << 20
	if len(meta)+len(payload)+len(attachment) > maxFrame {
		return fmt.Errorf("RPC request frame size exceeds limit")
	}
	header := make([]byte, 12)
	copy(header, "PRPC")
	binary.BigEndian.PutUint32(header[4:8], uint32(len(meta)+len(payload)+len(attachment)))
	binary.BigEndian.PutUint32(header[8:12], uint32(len(meta)))
	frame := append(append(append(header, meta...), payload...), attachment...)
	timeout := time.Duration(c.cfg.Timeout.HTTPRequestSec) * time.Second
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	deadline := time.Now().Add(timeout)
	conn, err := net.DialTimeout("tcp", u.Host, timeout)
	if err != nil {
		return fmt.Errorf("connect internal RPC: %w", err)
	}
	defer conn.Close()
	if err := conn.SetDeadline(deadline); err != nil {
		return err
	}
	if _, err := io.Copy(conn, bytes.NewReader(frame)); err != nil {
		return fmt.Errorf("write RPC request: %w", err)
	}
	if _, err := io.ReadFull(conn, header); err != nil {
		return fmt.Errorf("read RPC response header: %w", err)
	}
	size, metaSize := binary.BigEndian.Uint32(header[4:8]), binary.BigEndian.Uint32(header[8:12])
	if string(header[:4]) != "PRPC" || size > maxFrame || metaSize > size {
		return fmt.Errorf("invalid RPC response frame size or magic")
	}
	body := make([]byte, size)
	if _, err := io.ReadFull(conn, body); err != nil {
		return fmt.Errorf("read RPC response body: %w", err)
	}
	fields, err := rpcFields(body[:metaSize])
	if err != nil {
		return err
	}
	if fields[4].kind != protowire.VarintType || fields[4].number != 1 {
		return fmt.Errorf("RPC response correlation mismatch")
	}
	for _, number := range []protowire.Number{3, 5} {
		if field, exists := fields[number]; exists && field.kind != protowire.VarintType {
			return fmt.Errorf("invalid RPC metadata type")
		}
	}
	if fields[3].number != 0 {
		return fmt.Errorf("compressed RPC response unsupported")
	}
	if fields[2].kind != protowire.BytesType {
		return fmt.Errorf("RPC response metadata missing")
	}
	status, err := rpcFields(fields[2].data)
	if err != nil {
		return err
	}
	if field, exists := status[1]; exists && field.kind != protowire.VarintType {
		return fmt.Errorf("invalid RPC metadata type")
	}
	if status[1].number != 0 {
		return fmt.Errorf("RPC error %d", status[1].number)
	}
	attached := fields[5].number
	if attached > uint64(size-metaSize) {
		return fmt.Errorf("invalid RPC response attachment size")
	}
	if err := proto.Unmarshal(body[metaSize:uint64(size)-attached], resp); err != nil {
		return fmt.Errorf("decode RPC response: %w", err)
	}
	return nil
}

func rpcBytes(dst []byte, field protowire.Number, value []byte) []byte {
	return protowire.AppendBytes(protowire.AppendTag(dst, field, protowire.BytesType), value)
}

func rpcUint(dst []byte, field protowire.Number, value uint64) []byte {
	return protowire.AppendVarint(protowire.AppendTag(dst, field, protowire.VarintType), value)
}

type rpcField struct {
	kind   protowire.Type
	number uint64
	data   []byte
}

func rpcFields(data []byte) (map[protowire.Number]rpcField, error) {
	fields := make(map[protowire.Number]rpcField)
	for len(data) > 0 {
		num, kind, n := protowire.ConsumeTag(data)
		if n < 0 {
			return nil, fmt.Errorf("invalid RPC metadata tag")
		}
		data = data[n:]
		field := rpcField{kind: kind}
		switch kind {
		case protowire.VarintType:
			field.number, n = protowire.ConsumeVarint(data)
		case protowire.BytesType:
			field.data, n = protowire.ConsumeBytes(data)
		default:
			n = protowire.ConsumeFieldValue(num, kind, data)
		}
		if n < 0 {
			return nil, fmt.Errorf("invalid RPC metadata value")
		}
		fields[num], data = field, data[n:]
	}
	return fields, nil
}
