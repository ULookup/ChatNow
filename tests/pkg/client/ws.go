package client

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"

	push "chatnow-tests/proto/chatnow/push"
)

// WSClient 封装 WebSocket 连接，用于接收服务端推送通知。
type WSClient struct {
	conn        *websocket.Conn
	accessToken string
	userID      string
	deviceID    string

	mu       sync.Mutex
	notifies []*push.NotifyMessage
	notifyCh chan *push.NotifyMessage
	closed   bool
}

// NewWSClient 连接 gateway WS，发送 CLIENT_AUTH 鉴权帧，启动 readLoop。
func NewWSClient(cfg *Config, accessToken, userID, deviceID string) (*WSClient, error) {
	url := "ws://" + cfg.Target.WebsocketAddr + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		return nil, fmt.Errorf("ws dial: %w", err)
	}

	w := &WSClient{
		conn:        conn,
		accessToken: accessToken,
		userID:      userID,
		deviceID:    deviceID,
		notifyCh:    make(chan *push.NotifyMessage, 100),
	}

	// 发送 CLIENT_AUTH 鉴权帧
	authNotify := &push.NotifyMessage{
		NotifyType: push.NotifyType_CLIENT_AUTH,
		NotifyRemarks: &push.NotifyMessage_ClientAuth{
			ClientAuth: &push.NotifyClientAuth{
				AccessToken: accessToken,
				DeviceId:    deviceID,
			},
		},
	}
	authBytes, err := proto.Marshal(authNotify)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("marshal auth: %w", err)
	}
	if err := conn.WriteMessage(websocket.BinaryMessage, authBytes); err != nil {
		conn.Close()
		return nil, fmt.Errorf("write auth: %w", err)
	}

	go w.readLoop()
	return w, nil
}

// WaitForNotify 阻塞等待指定 notify_type 的通知，超时返回 ctx.Err()。
func (w *WSClient) WaitForNotify(ctx context.Context, notifyType int32) (*push.NotifyMessage, error) {
	// 先检查已缓存的
	w.mu.Lock()
	for i, n := range w.notifies {
		if n.NotifyType == push.NotifyType(notifyType) {
			w.notifies = append(w.notifies[:i], w.notifies[i+1:]...)
			w.mu.Unlock()
			return n, nil
		}
	}
	w.mu.Unlock()

	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case n := <-w.notifyCh:
			if n.NotifyType == push.NotifyType(notifyType) {
				return n, nil
			}
			// 缓存非匹配通知
			w.mu.Lock()
			w.notifies = append(w.notifies, n)
			w.mu.Unlock()
		}
	}
}

// WaitForNotifyCount 等待指定 type 的 n 条通知。
func (w *WSClient) WaitForNotifyCount(ctx context.Context, notifyType int32, n int) ([]*push.NotifyMessage, error) {
	results := make([]*push.NotifyMessage, 0, n)
	// 先检查缓存
	w.mu.Lock()
	remaining := make([]*push.NotifyMessage, 0)
	for _, msg := range w.notifies {
		if msg.NotifyType == push.NotifyType(notifyType) && len(results) < n {
			results = append(results, msg)
		} else {
			remaining = append(remaining, msg)
		}
	}
	w.notifies = remaining
	w.mu.Unlock()

	for len(results) < n {
		select {
		case <-ctx.Done():
			return results, ctx.Err()
		case msg := <-w.notifyCh:
			if msg.NotifyType == push.NotifyType(notifyType) {
				results = append(results, msg)
			} else {
				w.mu.Lock()
				w.notifies = append(w.notifies, msg)
				w.mu.Unlock()
			}
		}
	}
	return results, nil
}

// Close 关闭 WS 连接。
func (w *WSClient) Close() error {
	w.mu.Lock()
	if w.closed {
		w.mu.Unlock()
		return nil
	}
	w.closed = true
	w.mu.Unlock()
	return w.conn.Close()
}

func (w *WSClient) readLoop() {
	for {
		_, data, err := w.conn.ReadMessage()
		if err != nil {
			return
		}
		notify := &push.NotifyMessage{}
		if err := proto.Unmarshal(data, notify); err != nil {
			continue
		}
		w.mu.Lock()
		if w.closed {
			w.mu.Unlock()
			return
		}
		w.mu.Unlock()
		select {
		case w.notifyCh <- notify:
		default:
			// channel 满了，丢弃
		}
	}
}

// WaitForNotifyWithTimeout 是带超时的便捷方法。
func (w *WSClient) WaitForNotifyWithTimeout(notifyType int32, timeout time.Duration) (*push.NotifyMessage, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	return w.WaitForNotify(ctx, notifyType)
}
