package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/percona/aurora/internal/dts"
)

func main() {
	nodeID := flag.String("node-id", "dts-1", "Node ID")
	grpcPort := flag.Int("grpc-port", 9020, "gRPC server port")

	flag.Parse()

	config := dts.Config{
		NodeID:   *nodeID,
		GRPCPort: *grpcPort,
	}

	server, err := dts.NewServer(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create server: %v\n", err)
		os.Exit(1)
	}

	if err := server.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("DTS service %s started on port %d\n", *nodeID, *grpcPort)

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down...")
	server.Stop()
}
