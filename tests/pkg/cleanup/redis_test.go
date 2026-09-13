package cleanup

import (
	"io"
	"net"
	"testing"

	"github.com/stretchr/testify/require"
)

func TestFlushRedisNodeHandlesClusterReplicas(t *testing.T) {
	for _, tc := range []struct {
		name, response string
		wantError      bool
	}{
		{"primary", "+OK\r\n", false},
		{"replica", "-READONLY You can't write against a read only replica.\r\n", false},
		{"unauthorized", "-NOAUTH Authentication required.\r\n", true},
		{"unavailable", "-LOADING Redis is loading the dataset in memory\r\n", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			require.NoError(t, err)
			t.Cleanup(func() { listener.Close() })
			done := make(chan struct{})
			go func() {
				defer close(done)
				conn, err := listener.Accept()
				if err != nil {
					return
				}
				defer conn.Close()
				request := make([]byte, len("*1\r\n$8\r\nFLUSHALL\r\n"))
				if _, err := io.ReadFull(conn, request); err != nil {
					return
				}
				_, _ = io.WriteString(conn, tc.response)
			}()
			err = flushRedisNode(listener.Addr().String())
			if tc.wantError {
				require.Error(t, err)
			} else {
				require.NoError(t, err)
			}
			<-done
		})
	}
}
