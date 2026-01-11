.PHONY: all build test clean proto lint fmt

# Go parameters
GOCMD=go
GOBUILD=$(GOCMD) build
GOTEST=$(GOCMD) test
GOGET=$(GOCMD) get
GOMOD=$(GOCMD) mod
GOFMT=gofmt
GOLINT=golangci-lint

# Build targets
STORAGE_NODE=bin/storage-node
METADATA_SERVICE=bin/metadata-service
CONTROL_PLANE=bin/control-plane
BACKUP_SERVICE=bin/backup-service
DTS_SERVICE=bin/dts-service
CROSS_REGION=bin/cross-region

all: build

# Build all services
build: build-storage build-metadata build-control

build-storage:
	$(GOBUILD) -o $(STORAGE_NODE) ./cmd/storage-node

build-metadata:
	$(GOBUILD) -o $(METADATA_SERVICE) ./cmd/metadata-service

build-control:
	$(GOBUILD) -o $(CONTROL_PLANE) ./cmd/control-plane

build-backup:
	$(GOBUILD) -o $(BACKUP_SERVICE) ./cmd/backup-service

build-dts:
	$(GOBUILD) -o $(DTS_SERVICE) ./cmd/dts-service

build-crossregion:
	$(GOBUILD) -o $(CROSS_REGION) ./cmd/cross-region

# Test
test:
	$(GOTEST) -v -race -cover ./...

test-coverage:
	$(GOTEST) -v -race -coverprofile=coverage.out ./...
	$(GOCMD) tool cover -html=coverage.out -o coverage.html

# Lint
lint:
	$(GOLINT) run ./...

# Format
fmt:
	$(GOFMT) -s -w .

# Clean
clean:
	rm -rf bin/
	rm -f coverage.out coverage.html

# Dependencies
deps:
	$(GOMOD) download
	$(GOMOD) tidy

# Proto generation (requires protoc and protoc-gen-go)
proto:
	protoc --go_out=. --go_opt=paths=source_relative \
		--go-grpc_out=. --go-grpc_opt=paths=source_relative \
		pkg/proto/*.proto

# Docker builds
docker-storage:
	docker build -t aurora-storage-node:latest -f docker/Dockerfile.storage .

docker-metadata:
	docker build -t aurora-metadata-service:latest -f docker/Dockerfile.metadata .

docker-control:
	docker build -t aurora-control-plane:latest -f docker/Dockerfile.control .

# Help
help:
	@echo "Available targets:"
	@echo "  build          - Build all services"
	@echo "  build-storage  - Build storage node"
	@echo "  build-metadata - Build metadata service"
	@echo "  build-control  - Build control plane"
	@echo "  test           - Run all tests"
	@echo "  test-coverage  - Run tests with coverage report"
	@echo "  lint           - Run linter"
	@echo "  fmt            - Format code"
	@echo "  clean          - Clean build artifacts"
	@echo "  deps           - Download dependencies"
	@echo "  proto          - Generate protobuf code"
