package client

import (
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/types/known/wrapperspb"
)

// The peer consumes the actual TCP frame. These fixtures are independent wire
// bytes for brpc's pinned baidu_std protocol, not another implementation.
func TestInternalRPCTransport(t *testing.T) {
	for _, tc := range []struct {
		name      string
		response  []byte
		wantError string
	}{
		{"success", []byte{0x12, 2, 8, 0, 0x20, 1, 0x0a, 2, 'o', 'k'}, ""},
		{"server_error", []byte{0x12, 2, 8, 5, 0x20, 1}, "RPC error 5"},
		{"wrong_correlation", []byte{0x12, 2, 8, 0, 0x20, 2}, "correlation"},
		{"invalid_status_type", []byte{0x12, 2, 0x0a, 0, 0x20, 1}, "metadata type"},
		{"oversized_body", nil, "frame size"},
		{"truncated_body", []byte{0x12}, "response body"},
		{"stalled_response", nil, "timeout"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			require.NoError(t, err)
			peerDone := make(chan struct{})
			t.Cleanup(func() { _ = listener.Close(); <-peerDone })
			observed := make(chan []byte, 1)
			go func() {
				defer close(peerDone)
				conn, err := listener.Accept()
				if err != nil {
					observed <- nil
					return
				}
				defer conn.Close()
				_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
				header := make([]byte, 12)
				if _, err := io.ReadFull(conn, header); err != nil {
					observed <- nil
					return
				}
				size := binary.BigEndian.Uint32(header[4:8])
				if size > 1024 {
					observed <- nil
					return
				}
				body := make([]byte, size)
				_, _ = io.ReadFull(conn, body)
				observed <- append(header, body...)
				if tc.name == "stalled_response" {
					time.Sleep(1500 * time.Millisecond)
					return
				}
				copy(header, "PRPC")
				binary.BigEndian.PutUint32(header[4:8], uint32(len(tc.response)))
				binary.BigEndian.PutUint32(header[8:12], 6)
				if tc.name == "oversized_body" {
					binary.BigEndian.PutUint32(header[4:8], 2<<20)
				}
				if tc.name == "truncated_body" {
					binary.BigEndian.PutUint32(header[4:8], 10)
				}
				_, _ = conn.Write(append(header, tc.response...))
			}()
			cfg := &Config{Timeout: TimeoutConfig{HTTPRequestSec: 1}}
			c := NewHTTPClient(cfg)
			resp := &wrapperspb.StringValue{}
			err = c.DoInternalRPC("http://"+listener.Addr().String(), "S", "M", wrapperspb.String("in"), resp, []byte{7, 8})
			if tc.wantError == "" {
				require.NoError(t, err)
				require.Equal(t, "ok", resp.Value)
			} else {
				require.ErrorContains(t, err, tc.wantError)
			}
			select {
			case frame := <-observed:
				require.Equal(t, []byte{'P', 'R', 'P', 'C', 0, 0, 0, 18, 0, 0, 0, 12,
					0x0a, 6, 0x0a, 1, 'S', 0x12, 1, 'M', 0x20, 1, 0x28, 2, 0x0a, 2, 'i', 'n', 7, 8}, frame)
			case <-time.After(2 * time.Second):
				t.Fatal("peer did not receive request")
			}
		})
	}
}
