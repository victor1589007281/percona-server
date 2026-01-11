// OLAP Node Service - Column-store engine for analytical queries
package main

import (
	"context"
	"flag"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"

	"google.golang.org/grpc"
	
	"github.com/percona/aurora/internal/olap"
)

var (
	nodeID      = flag.String("node-id", "", "OLAP node ID")
	grpcPort    = flag.String("grpc-port", "9040", "gRPC server port")
	dataDir     = flag.String("data-dir", "/var/lib/aurora/olap", "Data directory")
	syncEnabled = flag.Bool("sync-enabled", true, "Enable OLTP->OLAP sync")
)

func main() {
	flag.Parse()
	
	if *nodeID == "" {
		log.Fatal("node-id is required")
	}
	
	log.Printf("Starting OLAP Node: %s", *nodeID)
	
	// Create OLAP config
	config := olap.DefaultOLAPConfig()
	config.DataPath = *dataDir
	
	// Create OLAP engine
	engine := olap.NewOLAPEngine(config)
	if err := engine.Start(); err != nil {
		log.Fatalf("Failed to start OLAP engine: %v", err)
	}
	
	// Create sync service if enabled
	var syncService *olap.SyncService
	if *syncEnabled {
		syncConfig := olap.DefaultSyncConfig()
		syncService = olap.NewSyncService(syncConfig, engine)
		if err := syncService.Start(); err != nil {
			log.Fatalf("Failed to start sync service: %v", err)
		}
	}
	
	// Create gRPC server
	listener, err := net.Listen("tcp", ":"+*grpcPort)
	if err != nil {
		log.Fatalf("Failed to listen: %v", err)
	}
	
	grpcServer := grpc.NewServer()
	
	// Register OLAP service
	// pb.RegisterOLAPServiceServer(grpcServer, NewOLAPServer(engine, syncService))
	
	log.Printf("OLAP Node listening on :%s", *grpcPort)
	
	// Handle shutdown
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	
	go func() {
		if err := grpcServer.Serve(listener); err != nil {
			log.Fatalf("Failed to serve: %v", err)
		}
	}()
	
	<-sigCh
	log.Println("Shutting down...")
	
	grpcServer.GracefulStop()
	
	if syncService != nil {
		syncService.Stop()
	}
	engine.Stop()
	
	log.Println("OLAP Node stopped")
}

// OLAPServer implements the OLAP gRPC service
type OLAPServer struct {
	engine      *olap.OLAPEngine
	syncService *olap.SyncService
}

// NewOLAPServer creates a new OLAP server
func NewOLAPServer(engine *olap.OLAPEngine, syncService *olap.SyncService) *OLAPServer {
	return &OLAPServer{
		engine:      engine,
		syncService: syncService,
	}
}

// Query handles OLAP queries
func (s *OLAPServer) Query(ctx context.Context, query *olap.OLAPQuery) (*olap.QueryResult, error) {
	return s.engine.Query(ctx, query)
}
