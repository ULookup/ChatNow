package client

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"

	common "chatnow-tests/proto/chatnow/common"
	msg "chatnow-tests/proto/chatnow/message"
)

// Client boundary | P0 | Failed responses retain classification without secrets.
func TestHTTPFailureDiagnostics(t *testing.T) {
	t.Setenv("CHATNOW_TEST_DIAGNOSTICS", "1")
	const secret = "synthetic-private-value"
	for _, tc := range []struct {
		name    string
		code    int32
		message string
		class   string
	}{
		{"transport", 9002, "[E112]not connected to endpoint: " + secret, "rpc_transport"},
		{"discovery", 9002, "no backend available", "no_backend"},
		{"application", 4001, "mid not found", "application"},
		{"untrusted", 9001, secret + "\nBearer " + secret, "application"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			calls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls++
				response, err := proto.Marshal(&msg.RecallMessageRsp{Header: &common.ResponseHeader{
					RequestId: secret, ErrorCode: tc.code, ErrorMessage: tc.message,
				}})
				require.NoError(t, err)
				_, _ = w.Write(response)
			}))
			t.Cleanup(server.Close)
			output, err := os.CreateTemp(t.TempDir(), "diagnostic")
			require.NoError(t, err)
			original := os.Stderr
			os.Stderr = output
			t.Cleanup(func() { os.Stderr = original; output.Close() })
			c := NewHTTPClient(&Config{})
			c.baseURL = server.URL
			response := &msg.RecallMessageRsp{}
			err = c.Do("/service/message/recall", &msg.RecallMessageReq{RequestId: NewRequestID()}, response, secret)
			os.Stderr = original
			require.NoError(t, err, "diagnostics must preserve application responses")
			require.Equal(t, tc.code, response.GetHeader().GetErrorCode())
			require.Equal(t, tc.message, response.GetHeader().GetErrorMessage())
			require.Equal(t, 1, calls, "diagnostics must never retry a failed request")
			_, err = output.Seek(0, 0)
			require.NoError(t, err)
			data, err := io.ReadAll(output)
			require.NoError(t, err)
			text := string(data)
			require.Contains(t, text, `"event":"protobuf_response_failed"`)
			require.Contains(t, text, `"classification":"`+tc.class+`"`)
			require.Contains(t, text, `"success":false`)
			require.Contains(t, text, `"error_code":`)
			require.NotContains(t, text, secret)
			require.NotContains(t, text, "Bearer")
			require.Equal(t, 1, strings.Count(text, "\n"), "one bounded JSON event per failed response")
			if tc.name == "transport" {
				require.Contains(t, text, `"rpc_codes":[112]`)
			}
		})
	}
}
