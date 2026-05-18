.PHONY: proto test-func test-scenario test-perf clean deps

# Generate Go protobuf from proto/ definitions
proto:
	@echo "Generating protobuf..."
	PROTO_BASE=../proto OUT_BASE=./proto; \
	for dir in common identity relationship conversation message transmite media presence; do \
		mkdir -p "$$OUT_BASE/chatnow/$$dir"; \
		for f in "$$PROTO_BASE/$$dir"/*.proto; do \
			[ -f "$$f" ] || continue; \
			protoc --proto_path="$$PROTO_BASE" --go_out="$$OUT_BASE" --go_opt=module=chatnow-tests/proto "$$f"; \
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
