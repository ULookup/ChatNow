package client

import (
	"encoding/json"
	"os"
	"regexp"
	"strconv"
	"time"

	common "chatnow-tests/proto/chatnow/common"
	"google.golang.org/protobuf/proto"
)

var rpcErrorCodes = regexp.MustCompile(`\[E([0-9]{1,5})\]`)

// recordResponseFailure preserves the four header fields as safe diagnostics.
// Free-form server text and request IDs are untrusted and never copied to logs.
func recordResponseFailure(response proto.Message, generatedTrace string) {
	provider, ok := response.(interface{ GetHeader() *common.ResponseHeader })
	if !ok {
		return
	}
	header := provider.GetHeader()
	if header != nil && header.GetSuccess() {
		return
	}
	classification := "application"
	codes := []int{}
	text := header.GetErrorMessage()
	if len(text) > 16384 {
		text = text[:16384]
	}
	for _, match := range rpcErrorCodes.FindAllStringSubmatch(text, 8) {
		value, _ := strconv.Atoi(match[1])
		codes = append(codes, value)
	}
	if header == nil {
		classification = "missing_header"
	} else if text == "no backend available" {
		classification = "no_backend"
	} else if len(codes) > 0 {
		classification = "rpc_transport"
	}
	event := struct {
		Event            string `json:"event"`
		Time             string `json:"time"`
		Trace            string `json:"trace_id,omitempty"`
		HeaderPresent    bool   `json:"header_present"`
		RequestIDPresent bool   `json:"request_id_present"`
		Success          bool   `json:"success"`
		ErrorCode        int32  `json:"error_code"`
		ErrorMessage     string `json:"error_message"`
		Classification   string `json:"classification"`
		RPCCodes         []int  `json:"rpc_codes"`
	}{"protobuf_response_failed", time.Now().UTC().Format(time.RFC3339Nano), generatedTrace,
		header != nil, header.GetRequestId() != "", false, header.GetErrorCode(), "omitted",
		classification, codes}
	// A failed diagnostic write cannot change the request result or trigger a retry.
	_ = json.NewEncoder(os.Stderr).Encode(event)
}
