package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/percona/aurora/internal/storage"
)

func main() {
	nodeID := flag.String("node-id", "storage-node-1", "Node ID")
	grpcPort := flag.Int("grpc-port", 9002, "gRPC server port")
	dataDir := flag.String("data-dir", "/data/aurora/storage", "Data directory")
	walDir := flag.String("wal-dir", "/data/aurora/storage/wal", "WAL directory")
	pageCacheSize := flag.Int("page-cache-size", 10000, "Page cache size")

	flag.Parse()

	config := storage.Config{
		NodeID:        *nodeID,
		GRPCPort:      *grpcPort,
		DataDir:       *dataDir,
		WALDir:        *walDir,
		PageCacheSize: *pageCacheSize,
	}

	server, err := storage.NewServer(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create server: %v\n", err)
		os.Exit(1)
	}

	if err := server.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Storage node %s started on port %d\n", *nodeID, *grpcPort)

	// Wait for shutdown signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down...")
	server.Stop()
}
