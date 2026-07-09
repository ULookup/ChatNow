.PHONY: proto test-func test-scenario test-perf clean deps

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

# Run all functional tests (L2)
test-func:
	go test -tags=func ./func/... -v -count=1

# Run scenario tests only (L3)
test-scenario:
	go test -tags=func ./func/... -run TestScenario -v -count=1

# Run performance benchmarks (L4)
test-perf:
	go test -tags=perf ./perf/... -bench=. -benchmem -count=3 -benchtime=10s

# Download dependencies
deps:
	go mod download
	go mod tidy

# Clean generated proto
clean:
	rm -rf proto/chatnow
