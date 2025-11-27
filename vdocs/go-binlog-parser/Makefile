.PHONY: test bench build clean

test:
	go test -v ./...

test-coverage:
	go test -v -coverprofile=coverage.out ./...
	go tool cover -html=coverage.out

bench:
	go test -bench=. -benchmem ./...

build:
	go build -o bin/binlog-parser cmd/parser/main.go

clean:
	rm -rf bin/ coverage.out
