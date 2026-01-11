package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/percona/aurora/internal/control"
)

func main() {
	nodeID := flag.String("node-id", "control-1", "Node ID")
	grpcPort := flag.Int("grpc-port", 9000, "gRPC server port")
	restPort := flag.Int("rest-port", 8080, "REST server port")

	flag.Parse()

	config := control.Config{
		NodeID:   *nodeID,
		GRPCPort: *grpcPort,
		RESTPort: *restPort,
	}

	server, err := control.NewServer(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create server: %v\n", err)
		os.Exit(1)
	}

	if err := server.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Control plane %s started on gRPC port %d, REST port %d\n", *nodeID, *grpcPort, *restPort)

	// Wait for shutdown signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down...")
	server.Stop()
}
