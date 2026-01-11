package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/percona/aurora/internal/backup"
)

func main() {
	nodeID := flag.String("node-id", "backup-1", "Node ID")
	grpcPort := flag.Int("grpc-port", 9030, "gRPC server port")
	s3Endpoint := flag.String("s3-endpoint", "", "S3 endpoint")
	s3Bucket := flag.String("s3-bucket", "aurora-backups", "S3 bucket")
	dataDir := flag.String("data-dir", "/data/aurora/backup", "Data directory")

	flag.Parse()

	config := backup.Config{
		NodeID:     *nodeID,
		GRPCPort:   *grpcPort,
		S3Endpoint: *s3Endpoint,
		S3Bucket:   *s3Bucket,
		DataDir:    *dataDir,
	}

	server, err := backup.NewServer(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create server: %v\n", err)
		os.Exit(1)
	}

	if err := server.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Backup service %s started on port %d\n", *nodeID, *grpcPort)

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down...")
	server.Stop()
}
