# Testing Guide

This document describes how to test the LogIndex implementation.

## Prerequisites

- Go 1.21 or later
- Make (optional, for convenient commands)

## Running Tests

### Basic Test Run

```bash
go test -v ./pkg/logindex/...
```

### With Race Detection

```bash
go test -v -race ./pkg/logindex/...
```

### With Coverage Report

```bash
go test -v -coverprofile=coverage.out ./pkg/logindex/...
go tool cover -html=coverage.out -o coverage.html
```

Or using Make:

```bash
make test-coverage
```

## Running Benchmarks

### All Benchmarks

```bash
go test -bench=. -benchmem ./pkg/logindex/...
```

### Specific Benchmarks

```bash
# Swiss Table benchmarks
go test -bench=SwissTable -benchmem ./pkg/logindex/

# B+Tree benchmarks
go test -bench=BPlusTree -benchmem ./pkg/logindex/

# LogIndex benchmarks
go test -bench=LogIndex -benchmem ./pkg/logindex/
```

Or using Make:

```bash
make bench
make bench-insert
make bench-query
```

## Test Coverage

The test suite includes comprehensive tests for:

### Swiss Table Tests (`swiss_table_test.go`)

- ✅ Basic operations (Get, Set, Delete)
- ✅ Update existing entries
- ✅ Multiple entries handling
- ✅ Automatic resizing
- ✅ ForEach iteration
- ✅ Concurrent access
- ✅ Benchmarks for Insert and Get

### B+Tree Tests (`bplustree_test.go`)

- ✅ Insert and Contains
- ✅ Duplicate insert handling
- ✅ GetAll (sorted output)
- ✅ Range queries
- ✅ Purge operations
- ✅ Large dataset handling
- ✅ Random insert order
- ✅ Multiple purges
- ✅ Benchmarks for Insert, Contains, and RangeQuery

### LogIndex Tests (`logindex_test.go`)

- ✅ Initialization
- ✅ Insert operations
- ✅ Contains checks
- ✅ Query operations (single and range)
- ✅ GetPageLSNRange
- ✅ PurgeBefore operations
- ✅ BatchInsert
- ✅ Statistics
- ✅ Clear operation
- ✅ ForEachPage iteration
- ✅ Concurrent access
- ✅ Real-world scenario simulation
- ✅ Memory usage estimation
- ✅ Benchmarks for Insert, QueryRange, Contains, and PurgeBefore

## Expected Test Results

All tests should pass with:

```
PASS
ok      github.com/percona/go-redo-parser/pkg/logindex    X.XXXs
```

## Benchmark Results

Typical benchmark results on modern hardware:

```
BenchmarkSwissTable_Insert-8           5000000    250 ns/op    120 B/op    2 allocs/op
BenchmarkSwissTable_Get-8             10000000    150 ns/op      0 B/op    0 allocs/op
BenchmarkBPlusTree_Insert-8            3000000    400 ns/op    200 B/op    3 allocs/op
BenchmarkBPlusTree_Contains-8          5000000    300 ns/op      0 B/op    0 allocs/op
BenchmarkBPlusTree_RangeQuery-8        1000000   1200 ns/op    512 B/op    5 allocs/op
BenchmarkLogIndex_Insert-8             2000000    600 ns/op    300 B/op    5 allocs/op
BenchmarkLogIndex_QueryRange-8          500000   2500 ns/op   1024 B/op    8 allocs/op
```

## Continuous Integration

Use the CI target to run all checks:

```bash
make ci
```

This will:
1. Format the code
2. Run the linter
3. Run all tests with race detection

## Test Structure

```
pkg/logindex/
├── swiss_table.go          # Swiss Table implementation
├── swiss_table_test.go     # Swiss Table tests
├── bplustree.go            # B+Tree implementation
├── bplustree_test.go       # B+Tree tests
├── logindex.go             # LogIndex implementation
└── logindex_test.go        # LogIndex tests
```

## Adding New Tests

When adding new functionality:

1. Add the implementation to the appropriate file
2. Add corresponding tests in the `*_test.go` file
3. Run `make test` to verify
4. Run `make bench` to check performance impact
5. Update this document if needed

## Troubleshooting

### Tests Fail with Race Detector

If tests fail with `-race`:
- Check for concurrent access without proper locking
- Review mutex usage in Swiss Table and LogIndex

### Benchmarks Show Performance Regression

If benchmarks show degradation:
- Compare with baseline results
- Check for added allocations
- Profile with `go test -bench=. -cpuprofile=cpu.prof`
- Analyze with `go tool pprof cpu.prof`

### Memory Usage Issues

To profile memory usage:

```bash
go test -bench=LogIndex_Insert -memprofile=mem.prof
go tool pprof mem.prof
```

## Performance Goals

Target performance metrics:

| Operation | Target | Notes |
|-----------|--------|-------|
| Swiss Table Insert | < 300 ns/op | O(1) average case |
| Swiss Table Get | < 200 ns/op | O(1) average case |
| B+Tree Insert | < 500 ns/op | O(log N) |
| B+Tree Contains | < 400 ns/op | O(log N) |
| B+Tree RangeQuery | < 2 μs/op | O(log N + K) where K = result size |
| LogIndex Insert | < 800 ns/op | Combined overhead |
| LogIndex QueryRange | < 3 μs/op | Combined overhead |

## Code Coverage Goals

Target coverage: **> 90%**

Check current coverage:

```bash
go test -coverprofile=coverage.out ./pkg/logindex/...
go tool cover -func=coverage.out
```

