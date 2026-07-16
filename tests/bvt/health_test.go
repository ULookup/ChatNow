//go:build bvt

package bvt_test

import (
	"encoding/json"
	"net/http"
	"testing"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/cleanup"
)

// BVT-001 | P0 | 基础设施健康 | gateway HTTP 端口存活
func TestBVT_GatewayHTTP_Reachable(t *testing.T) {
	resp, err := http.Get("http://" + Cfg.Target.GatewayAddr + "/health")
	require.NoError(t, err)
	defer resp.Body.Close()
	assert.Equal(t, 200, resp.StatusCode)
}

// BVT-002 | P0 | 基础设施健康 | gateway WS 端口可连接
func TestBVT_GatewayWS_Reachable(t *testing.T) {
	url := "ws://" + Cfg.Target.WebsocketAddr + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	require.NoError(t, err)
	defer conn.Close()
	assert.True(t, conn != nil)
}

// BVT-003 | P0 | 基础设施健康 | etcd 中 8 个服务均有注册实例
func TestBVT_ServicesRegistered(t *testing.T) {
	etcdURL := "http://127.0.0.1:2379"
	count, err := cleanup.EtcdServiceCount(etcdURL)
	require.NoError(t, err)
	// 至少 8 个业务服务注册（gateway/push/identity/media/transmite/message/relationship/conversation/presence）
	assert.GreaterOrEqual(t, count, 8, "etcd 注册服务数应 >= 8，实际 %d", count)
	// 打印原始数据便于调试
	t.Logf("etcd /service/ 下注册了 %d 个 key", count)
}

// 辅助：解码 etcd REST API 响应（BVT-003 验证用）
func decodeEtcdKeys(body []byte) ([]string, error) {
	var result struct {
		Kvs []struct {
			Key string `json:"key"`
		} `json:"kvs"`
	}
	if err := json.Unmarshal(body, &result); err != nil {
		return nil, err
	}
	keys := make([]string, 0, len(result.Kvs))
	for _, kv := range result.Kvs {
		keys = append(keys, kv.Key)
	}
	return keys, nil
}
