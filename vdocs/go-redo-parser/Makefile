.PHONY: test bench clean lint fmt

# Run all tests
test:
	go test -v -race -cover ./pkg/logindex/...

# Run tests with coverage report
test-coverage:
	go test -v -race -coverprofile=coverage.out ./pkg/logindex/...
	go tool cover -html=coverage.out -o coverage.html
	@echo "Coverage report generated: coverage.html"

# Run benchmarks
bench:
	go test -bench=. -benchmem ./pkg/logindex/...

# Run specific benchmark
bench-insert:
	go test -bench=Insert -benchmem ./pkg/logindex/...

bench-query:
	go test -bench=Query -benchmem ./pkg/logindex/...

# Format code
fmt:
	go fmt ./...

# Run linter
lint:
	golangci-lint run ./...

# Clean generated files
clean:
	rm -f coverage.out coverage.html
	go clean -testcache

# Install dependencies
deps:
	go mod download
	go mod tidy

# Build example
build:
	go build -o bin/logindex-example ./examples/...

# Run all checks (fmt, lint, test)
ci: fmt lint test

# Show help
help:
	@echo "Available targets:"
	@echo "  test            - Run all tests"
	@echo "  test-coverage   - Run tests with coverage report"
	@echo "  bench           - Run all benchmarks"
	@echo "  bench-insert    - Run insert benchmarks"
	@echo "  bench-query     - Run query benchmarks"
	@echo "  fmt             - Format code"
	@echo "  lint            - Run linter"
	@echo "  clean           - Clean generated files"
	@echo "  deps            - Download and tidy dependencies"
	@echo "  ci              - Run all checks"
	@echo "  help            - Show this help message"

