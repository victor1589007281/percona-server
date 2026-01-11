package control

import (
	"context"
	"testing"
	"time"
)

func TestCreateCluster(t *testing.T) {
	config := Config{
		NodeID:   "control-1",
		GRPCPort: 9000,
	}

	server, err := NewServer(config)
	if err != nil {
		t.Fatalf("create server: %v", err)
	}

	ctx := context.Background()

	// Create cluster
	cluster, err := server.CreateCluster(ctx, "test-cluster", "db.m5.large", 2, []string{"az-a", "az-b", "az-c"}, 100*1024*1024*1024)
	if err != nil {
		t.Fatalf("create cluster: %v", err)
	}

	if cluster.ClusterName != "test-cluster" {
		t.Errorf("expected name 'test-cluster', got '%s'", cluster.ClusterName)
	}
	if cluster.Writer == nil {
		t.Error("writer should not be nil")
	}
	if len(cluster.Readers) != 2 {
		t.Errorf("expected 2 readers, got %d", len(cluster.Readers))
	}
	if cluster.State != ClusterStateAvailable {
		t.Errorf("expected state Available, got %d", cluster.State)
	}
}

func TestGetCluster(t *testing.T) {
	config := Config{NodeID: "control-1", GRPCPort: 9000}
	server, _ := NewServer(config)
	ctx := context.Background()

	// Create cluster
	created, _ := server.CreateCluster(ctx, "test-cluster", "db.m5.large", 1, nil, 0)

	// Get cluster
	cluster, err := server.GetCluster(ctx, created.ClusterID)
	if err != nil {
		t.Fatalf("get cluster: %v", err)
	}

	if cluster.ClusterID != created.ClusterID {
		t.Errorf("cluster ID mismatch")
	}
}

func TestDeleteCluster(t *testing.T) {
	config := Config{NodeID: "control-1", GRPCPort: 9000}
	server, _ := NewServer(config)
	ctx := context.Background()

	// Create and delete cluster
	created, _ := server.CreateCluster(ctx, "test-cluster", "db.m5.large", 1, nil, 0)
	
	if err := server.DeleteCluster(ctx, created.ClusterID, false); err != nil {
		t.Fatalf("delete cluster: %v", err)
	}

	// Verify cluster is gone
	_, err := server.GetCluster(ctx, created.ClusterID)
	if err != ErrClusterNotFound {
		t.Errorf("expected ErrClusterNotFound, got %v", err)
	}
}

func TestAddRemoveReader(t *testing.T) {
	config := Config{NodeID: "control-1", GRPCPort: 9000}
	server, _ := NewServer(config)
	ctx := context.Background()

	// Create cluster
	cluster, _ := server.CreateCluster(ctx, "test-cluster", "db.m5.large", 0, nil, 0)

	// Add reader
	reader, err := server.AddReader(ctx, cluster.ClusterID, "db.m5.large", "az-b")
	if err != nil {
		t.Fatalf("add reader: %v", err)
	}
	if reader.Role != InstanceRoleReader {
		t.Error("new instance should be a reader")
	}

	// Verify reader was added
	updated, _ := server.GetCluster(ctx, cluster.ClusterID)
	if len(updated.Readers) != 1 {
		t.Errorf("expected 1 reader, got %d", len(updated.Readers))
	}

	// Remove reader
	if err := server.RemoveReader(ctx, cluster.ClusterID, reader.InstanceID); err != nil {
		t.Fatalf("remove reader: %v", err)
	}

	// Verify reader was removed
	updated, _ = server.GetCluster(ctx, cluster.ClusterID)
	if len(updated.Readers) != 0 {
		t.Errorf("expected 0 readers, got %d", len(updated.Readers))
	}
}

func TestTriggerFailover(t *testing.T) {
	config := Config{NodeID: "control-1", GRPCPort: 9000}
	server, _ := NewServer(config)
	ctx := context.Background()

	// Create cluster with readers
	cluster, _ := server.CreateCluster(ctx, "test-cluster", "db.m5.large", 2, nil, 0)
	originalWriterID := cluster.Writer.InstanceID

	// Set reader LSN
	cluster.Readers[0].CurrentLSN = 1000
	cluster.Readers[1].CurrentLSN = 2000

	// Trigger failover
	fo, err := server.TriggerFailover(ctx, cluster.ClusterID, cluster.Readers[1].InstanceID)
	if err != nil {
		t.Fatalf("trigger failover: %v", err)
	}

	if fo.SourceInstanceID != originalWriterID {
		t.Errorf("expected source %s, got %s", originalWriterID, fo.SourceInstanceID)
	}

	// Wait for failover to complete
	time.Sleep(100 * time.Millisecond)

	// Check failover status
	status, err := server.GetFailoverStatus(ctx, fo.FailoverID)
	if err != nil {
		t.Fatalf("get failover status: %v", err)
	}

	if status.State != FailoverStateCompleted {
		t.Errorf("expected state Completed, got %d", status.State)
	}
}

func TestGetClusterStatus(t *testing.T) {
	config := Config{NodeID: "control-1", GRPCPort: 9000}
	server, _ := NewServer(config)
	ctx := context.Background()

	// Create cluster
	cluster, _ := server.CreateCluster(ctx, "test-cluster", "db.m5.large", 2, nil, 0)

	// Get status
	status, err := server.GetClusterStatus(ctx, cluster.ClusterID)
	if err != nil {
		t.Fatalf("get status: %v", err)
	}

	if status.ClusterID != cluster.ClusterID {
		t.Error("cluster ID mismatch")
	}
	if status.Writer == nil {
		t.Error("writer status should not be nil")
	}
	if len(status.Readers) != 2 {
		t.Errorf("expected 2 reader statuses, got %d", len(status.Readers))
	}
}

func TestMonitorService(t *testing.T) {
	config := Config{NodeID: "control-1", GRPCPort: 9000}
	server, _ := NewServer(config)

	monitor := NewMonitorService(server)

	// Get health for non-existent instance
	_, ok := monitor.GetHealth("non-existent")
	if ok {
		t.Error("should not find health for non-existent instance")
	}

	// Get all health (should be empty initially)
	all := monitor.GetAllHealth()
	if len(all) != 0 {
		t.Errorf("expected 0 health entries, got %d", len(all))
	}
}
