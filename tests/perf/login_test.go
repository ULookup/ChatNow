//go:build perf

package perf_test

import (
	"math/rand"
	"sort"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

func BenchmarkLogin(b *testing.B) {
	const preRegCount = 100
	type cred struct{ user, pass string }
	creds := make([]cred, preRegCount)
	for i := 0; i < preRegCount; i++ {
		authed, user, pass := fixture.RegisterAndLogin(b, HTTP)
		creds[i] = cred{user, pass}
		_ = authed
	}

	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			c := creds[rand.Intn(preRegCount)]
			req := &identity.LoginReq{
				RequestId: client.NewRequestID(),
				Credential: &identity.LoginReq_UsernamePwd{
					UsernamePwd: &identity.UsernamePassword{Username: c.user, Password: c.pass},
				},
				DeviceId: client.NewDeviceID(), DeviceName: "perf-test",
			}
			rsp := &identity.LoginRsp{}
			if err := HTTP.DoNoAuth("/service/identity/login", req, rsp); err != nil {
				b.Fatal(err)
			}
			if !rsp.Header.Success {
				b.Fatalf("login failed: %s", rsp.Header.ErrorMessage)
			}
		}
	})
	b.Logf("pre-registered %d users for benchmark", preRegCount)
}

func BenchmarkLoginLatency(b *testing.B) {
	authed, user, pass := fixture.RegisterAndLogin(b, HTTP)
	_ = authed

	latencies := make([]int64, b.N)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		start := time.Now()
		req := &identity.LoginReq{
			RequestId: client.NewRequestID(),
			Credential: &identity.LoginReq_UsernamePwd{
				UsernamePwd: &identity.UsernamePassword{Username: user, Password: pass},
			},
			DeviceId: client.NewDeviceID(), DeviceName: "perf",
		}
		rsp := &identity.LoginRsp{}
		if err := HTTP.DoNoAuth("/service/identity/login", req, rsp); err != nil {
			b.Fatal(err)
		}
		latencies[i] = time.Since(start).Nanoseconds()
	}
	sort.Slice(latencies, func(i, j int) bool { return latencies[i] < latencies[j] })
	p50 := float64(latencies[b.N/2]) / 1e6
	p90 := float64(latencies[b.N*9/10]) / 1e6
	p99 := float64(latencies[b.N*99/100]) / 1e6
	b.ReportMetric(p50, "p50-ms")
	b.ReportMetric(p90, "p90-ms")
	b.ReportMetric(p99, "p99-ms")
}
