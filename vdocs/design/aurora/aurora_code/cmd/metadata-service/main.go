package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/percona/aurora/internal/metadata"
)

func main() {
	nodeID := flag.String("node-id", "metadata-1", "Node ID")
	raftAddr := flag.String("raft-addr", "localhost:9004", "Raft bind address")
	grpcPort := flag.Int("grpc-port", 9003, "gRPC server port")
	dataDir := flag.String("data-dir", "/data/aurora/metadata", "Data directory")

	flag.Parse()

	config := metadata.Config{
		NodeID:   *nodeID,
		RaftAddr: *raftAddr,
		GRPCPort: *grpcPort,
		DataDir:  *dataDir,
	}

	server, err := metadata.NewServer(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create server: %v\n", err)
		os.Exit(1)
	}

	if err := server.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Metadata service %s started on port %d\n", *nodeID, *grpcPort)

	// Wait for shutdown signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down...")
	server.Stop()
}
