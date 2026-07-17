package verify

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

// ESVerifier 直查 Elasticsearch 验证消息索引一致性。
// 使用标准 net/http，无额外 ES SDK 依赖。
type ESVerifier struct {
	client *http.Client
	esURL  string
}

// NewESVerifier 创建 ES 直查验证器。
func NewESVerifier(url string) *ESVerifier {
	return &ESVerifier{
		client: &http.Client{Timeout: 10 * time.Second},
		esURL:  strings.TrimRight(url, "/"),
	}
}

// MessageIndexed 验证消息已索引到 ES（按 message_id 查）。
func (v *ESVerifier) MessageIndexed(t testing.TB, messageID int64, contentKeyword string) {
	// 轮询等待 ES 异步索引（最多 5 秒）
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if v.checkMessageIndexed(messageID, contentKeyword) {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("ES 未索引消息 %d (keyword=%s)，5s 内未出现", messageID, contentKeyword)
}

func (v *ESVerifier) checkMessageIndexed(messageID int64, contentKeyword string) bool {
	body := fmt.Sprintf(`{
		"query": {
			"bool": {
				"must": [
					{"term": {"message_id": %d}},
					{"match": {"content": "%s"}}
				]
			}
		}
	}`, messageID, contentKeyword)

	resp, err := v.client.Post(v.esURL+"/message/_search", "application/json", strings.NewReader(body))
	if err != nil {
		return false
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return false
	}

	var result struct {
		Hits struct {
			Total struct {
				Value int `json:"value"`
			} `json:"total"`
		} `json:"hits"`
	}
	if err := json.Unmarshal(data, &result); err != nil {
		return false
	}
	return result.Hits.Total.Value >= 1
}

// SearchHitCount 验证 ES 搜索命中数。
func (v *ESVerifier) SearchHitCount(t testing.TB, conversationID, keyword string, expected int) {
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		actual := v.searchHitCount(conversationID, keyword)
		if actual == expected {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("ES 搜索 conv=%s keyword=%s 期望 %d 命中，5s 内未达到", conversationID, keyword, expected)
}

func (v *ESVerifier) searchHitCount(conversationID, keyword string) int {
	body := fmt.Sprintf(`{
		"query": {
			"bool": {
				"must": [
					{"term": {"chat_session_id.keyword": "%s"}},
					{"match": {"content": "%s"}}
				],
				"filter": [{"term": {"status": 0}}]
			}
		}
	}`, conversationID, keyword)

	resp, err := v.client.Post(v.esURL+"/message/_search", "application/json", strings.NewReader(body))
	if err != nil {
		return -1
	}
	defer resp.Body.Close()

	data, _ := io.ReadAll(resp.Body)
	var result struct {
		Hits struct {
			Total struct {
				Value int `json:"value"`
			} `json:"total"`
		} `json:"hits"`
	}
	json.Unmarshal(data, &result)
	return result.Hits.Total.Value
}

// IndexExists 验证 ES 索引是否存在。
func (v *ESVerifier) IndexExists(t testing.TB, indexName string) {
	resp, err := v.client.Head(v.esURL + "/" + indexName)
	if err != nil {
		t.Fatalf("ES HEAD index %s: %v", indexName, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("ES 索引 %s 不存在 (status=%d)", indexName, resp.StatusCode)
	}
}
