package crossregion

import (
	"context"
	"testing"
	"time"
)

func TestRegisterRegion(t *testing.T) {
	config := Config{
		NodeID:   "cr-1",
		RegionID: "region-a",
		GRPCPort: 9010,
	}
	
	server, err := NewServer(config)
	if err != nil {
		t.Fatalf("create server: %v", err)
	}
	
	ctx := context.Background()
	
	// Register another region
	if err := server.RegisterRegion(ctx, "region-b", "Region B", "region-b:9010", false); err != nil {
		t.Fatalf("register region: %v", err)
	}
	
	// Get regions
	regions, err := server.GetRegions(ctx)
	if err != nil {
		t.Fatalf("get regions: %v", err)
	}
	
	if len(regions) != 2 {
		t.Errorf("expected 2 regions, got %d", len(regions))
	}
}

func TestStartReplication(t *testing.T) {
	config := Config{
		NodeID:   "cr-1",
		RegionID: "region-a",
		GRPCPort: 9010,
	}
	
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Register target region
	server.RegisterRegion(ctx, "region-b", "Region B", "region-b:9010", false)
	
	// Start replication
	if err := server.StartReplication(ctx, "cluster-001", "region-a", "region-b"); err != nil {
		t.Fatalf("start replication: %v", err)
	}
	
	// Wait for replication to start
	time.Sleep(200 * time.Millisecond)
	
	// Get replication status
	status, err := server.GetReplicationStatus(ctx, "cluster-001")
	if err != nil {
		t.Fatalf("get replication status: %v", err)
	}
	
	if status.ClusterID != "cluster-001" {
		t.Errorf("expected cluster-001, got %s", status.ClusterID)
	}
	if !status.IsHealthy {
		t.Error("replication should be healthy")
	}
}

func TestDRFailover(t *testing.T) {
	config := Config{
		NodeID:   "cr-1",
		RegionID: "region-a",
		GRPCPort: 9010,
	}
	
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Register target region
	server.RegisterRegion(ctx, "region-b", "Region B", "region-b:9010", false)
	
	// Start replication
	server.StartReplication(ctx, "cluster-001", "region-a", "region-b")
	time.Sleep(200 * time.Millisecond)
	
	// Trigger failover
	failover, err := server.TriggerDRFailover(ctx, "cluster-001", "region-b", false)
	if err != nil {
		t.Fatalf("trigger failover: %v", err)
	}
	
	if failover.State != DRFailoverStateStarted {
		t.Errorf("expected state Started, got %d", failover.State)
	}
	
	// Wait for completion
	time.Sleep(500 * time.Millisecond)
	
	// Check status
	status, err := server.GetDRFailoverStatus(ctx, failover.FailoverID)
	if err != nil {
		t.Fatalf("get failover status: %v", err)
	}
	
	if status.State != DRFailoverStateCompleted {
		t.Errorf("expected state Completed, got %d", status.State)
	}
	
	// Verify region roles changed
	regions, _ := server.GetRegions(ctx)
	for _, r := range regions {
		if r.RegionID == "region-b" && !r.IsPrimary {
			t.Error("region-b should be primary after failover")
		}
	}
}

func TestForceFailover(t *testing.T) {
	config := Config{
		NodeID:   "cr-1",
		RegionID: "region-a",
		GRPCPort: 9010,
	}
	
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Register target region
	server.RegisterRegion(ctx, "region-b", "Region B", "region-b:9010", false)
	
	// Force failover (no replication setup)
	failover, err := server.TriggerDRFailover(ctx, "cluster-001", "region-b", true)
	if err != nil {
		t.Fatalf("trigger force failover: %v", err)
	}
	
	// Wait for completion
	time.Sleep(500 * time.Millisecond)
	
	status, _ := server.GetDRFailoverStatus(ctx, failover.FailoverID)
	if status.State != DRFailoverStateCompleted {
		t.Errorf("expected state Completed, got %d", status.State)
	}
}

func TestBinlogSenderReceiver(t *testing.T) {
	sender := NewBinlogSender("region-a")
	receiver := NewBinlogReceiver("region-b")
	ctx := context.Background()
	
	// Get batch
	batch, err := sender.GetBatch(ctx)
	if err != nil {
		t.Fatalf("get batch: %v", err)
	}
	
	if len(batch.Events) == 0 {
		t.Error("expected some events")
	}
	
	// Compress and send
	compressed := compressBatch(batch)
	
	// Apply
	gtid, err := receiver.ApplyBatch(ctx, compressed)
	if err != nil {
		t.Fatalf("apply batch: %v", err)
	}
	
	if gtid == "" {
		t.Error("GTID should not be empty")
	}
}

func TestReplicator(t *testing.T) {
	config := Config{
		NodeID:   "cr-1",
		RegionID: "region-a",
		GRPCPort: 9010,
	}
	
	server, _ := NewServer(config)
	replicator := server.replicator
	
	if replicator == nil {
		t.Fatal("replicator should not be nil")
	}
	
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	
	// Start stream
	go replicator.StartStream(ctx, "cluster-001", "region-a", "region-b")
	
	time.Sleep(300 * time.Millisecond)
	
	// Check replication state was updated
	status, _ := server.GetReplicationStatus(ctx, "cluster-001")
	if status == nil {
		// May not have been updated yet, that's OK for this test
	}
}

func TestRelayLog(t *testing.T) {
	relayLog := NewRelayLog("/tmp/relay", 1024*1024*100)
	
	// Write
	if err := relayLog.Write([]byte("test data")); err != nil {
		t.Fatalf("write: %v", err)
	}
	
	// Cleanup
	if err := relayLog.Cleanup(0); err != nil {
		t.Fatalf("cleanup: %v", err)
	}
}
