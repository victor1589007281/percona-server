package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/percona/aurora/internal/crossregion"
)

func main() {
	nodeID := flag.String("node-id", "crossregion-1", "Node ID")
	regionID := flag.String("region-id", "region-a", "Region ID")
	grpcPort := flag.Int("grpc-port", 9010, "gRPC server port")

	flag.Parse()

	config := crossregion.Config{
		NodeID:   *nodeID,
		RegionID: *regionID,
		GRPCPort: *grpcPort,
	}

	server, err := crossregion.NewServer(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create server: %v\n", err)
		os.Exit(1)
	}

	if err := server.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Cross-region service %s (region: %s) started on port %d\n", *nodeID, *regionID, *grpcPort)

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down...")
	server.Stop()
}
