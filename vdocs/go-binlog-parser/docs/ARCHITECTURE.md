# Architecture

## Overview

The Go Binlog Parser is designed for high-performance concurrent parsing of MySQL binlog files.

## Components

### 1. Parser (pkg/binlog/parser.go)

Sequential binlog parser that reads events one by one.

### 2. ConcurrentParser (pkg/binlog/concurrent_parser.go)

Concurrent parser using worker pool for parallel event processing.

### 3. Worker Pool (pkg/pool/worker_pool.go)

Generic worker pool implementation for concurrent task execution.

### 4. Types (pkg/types/event.go)

Binlog event type definitions following MySQL 8.4 specification.

## Design Decisions

### Concurrency Model

- Sequential reading (binlog must be read sequentially)
- Parallel processing of events
- Worker pool pattern for resource management

### Memory Efficiency

- Streaming parser (no full file load)
- Buffered I/O for performance
- Event-driven architecture

### Extensibility

- Event type interface allows custom event handlers
- Pluggable worker pool
- Configuration-driven behavior
