.PHONY: proto test-bvt test-func test-reliability test-scenario test-perf test-perf-cache test-perf-cache-gate clean deps

PF09_BASELINE_MSG_PER_SEC ?= 5000
PF09_EXPECTED_TRANSMITE_INSTANCES ?= 1

# Generate Go protobuf from proto/ definitions
# NOTE: protoc-gen-go >= v1.35 requires go_package option or M flags to derive
# the Go import path. The proto files here do not declare go_package, so we
# build M mappings dynamically from the directory layout:
#   <dir>/<file>.proto  ->  chatnow-tests/proto/chatnow/<dir>
# NOTE: notify.proto is NOT skipped. The original plan assumed notify.proto
# duplicates types from push_service.proto, but this is incorrect:
# notify.proto defines NotifyMessage/NotifyType/NotifyClientAuth etc.,
# push_service.proto defines PushToUserReq/PushBatchReq etc. Both are needed.
proto:
	@echo "Generating protobuf..."
	PROTO_BASE=../proto OUT_BASE=./proto; \
	GO_OPTS="module=chatnow-tests/proto"; \
	for p in $$PROTO_BASE/*/*.proto; do \
		[ -f "$$p" ] || continue; \
		rel=$${p#$$PROTO_BASE/}; \
		dir=$$(dirname $$rel); \
		GO_OPTS="$$GO_OPTS,M$$rel=chatnow-tests/proto/chatnow/$$dir"; \
	done; \
	for dir in common identity relationship conversation message transmite media presence push; do \
		mkdir -p "$$OUT_BASE/chatnow/$$dir"; \
		for f in "$$PROTO_BASE/$$dir"/*.proto; do \
			[ -f "$$f" ] || continue; \
			protoc --proto_path="$$PROTO_BASE" --go_out="$$OUT_BASE" --go_opt=$$GO_OPTS "$$f"; \
		done; \
	done

# Run BVT smoke tests (L1) - fastest, runs first as gate
test-bvt:
	go test -tags=bvt ./bvt/... -v -count=1 -timeout=300s

# Run all functional tests (L2)
test-func:
	go test -tags=func ./func/... -v -count=1

# Run reliability/chaos tests (requires the Docker Compose stack)
test-reliability:
	go test -tags=reliability ./reliability/... -v -count=1 -timeout=180s

# Run scenario tests only (L3)
test-scenario:
	go test -tags=func ./func/... -run TestScenario -v -count=1

# Run performance benchmarks (L4)
test-perf:
	go test -tags=perf ./perf/... -bench=. -benchmem -count=3 -benchtime=10s

# Compile/discover PF-09 locally. It skips unless the explicit full-stack gate is enabled.
test-perf-cache:
	go test -v -tags=perf ./perf/... -run '^$$' -bench '^BenchmarkPF09_UserInfoCache$$' -benchtime=1x

# Real Linux full-stack gate. The deployment must raise Transmite's user/session
# rate limits above the generated load; any rate_limited response fails PF-09.
# Multi-instance stacks must set PF09_TRANSMITE_VARS_URLS to every bvar endpoint,
# separated by commas, so the RPC-reduction snapshot cannot omit an instance.
test-perf-cache-gate:
	@test "$$(uname -s)" = Linux || (echo "PF-09 gate requires Linux" >&2; exit 1)
	PF09_RUN_FULLSTACK=1 PF09_BASELINE_MSG_PER_SEC=$(PF09_BASELINE_MSG_PER_SEC) \
		PF09_EXPECTED_TRANSMITE_INSTANCES=$(PF09_EXPECTED_TRANSMITE_INSTANCES) \
		go test -tags=perf ./perf/... -run '^$$' -bench '^BenchmarkPF09_UserInfoCache$$' \
		-benchmem -count=3 -benchtime=1x

# Download dependencies
deps:
	go mod download
	go mod tidy

# Clean generated proto
clean:
	rm -rf proto/chatnow
