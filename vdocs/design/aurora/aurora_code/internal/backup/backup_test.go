package backup

import (
	"context"
	"testing"
	"time"
)

func TestCreateSnapshot(t *testing.T) {
	config := Config{
		NodeID:   "backup-1",
		GRPCPort: 9030,
		S3Bucket: "test-bucket",
		DataDir:  "/tmp/backup-test",
	}
	
	server, err := NewServer(config)
	if err != nil {
		t.Fatalf("create server: %v", err)
	}
	
	ctx := context.Background()
	
	// Create snapshot
	snap, err := server.CreateSnapshot(ctx, "cluster-001", "daily-backup")
	if err != nil {
		t.Fatalf("create snapshot: %v", err)
	}
	
	if snap.SnapshotName != "daily-backup" {
		t.Errorf("expected name 'daily-backup', got '%s'", snap.SnapshotName)
	}
	if snap.State != SnapshotStateCreating {
		t.Errorf("expected state Creating, got %d", snap.State)
	}
	
	// Wait for completion
	time.Sleep(200 * time.Millisecond)
	
	// Get snapshot
	updated, err := server.GetSnapshot(ctx, snap.SnapshotID)
	if err != nil {
		t.Fatalf("get snapshot: %v", err)
	}
	
	if updated.State != SnapshotStateAvailable {
		t.Errorf("expected state Available, got %d", updated.State)
	}
}

func TestListSnapshots(t *testing.T) {
	config := Config{NodeID: "backup-1", GRPCPort: 9030}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Create multiple snapshots
	server.CreateSnapshot(ctx, "cluster-001", "snap-1")
	server.CreateSnapshot(ctx, "cluster-001", "snap-2")
	server.CreateSnapshot(ctx, "cluster-002", "snap-3")
	
	time.Sleep(300 * time.Millisecond)
	
	// List snapshots for cluster-001
	snapshots, err := server.ListSnapshots(ctx, "cluster-001")
	if err != nil {
		t.Fatalf("list snapshots: %v", err)
	}
	
	if len(snapshots) != 2 {
		t.Errorf("expected 2 snapshots, got %d", len(snapshots))
	}
}

func TestDeleteSnapshot(t *testing.T) {
	config := Config{NodeID: "backup-1", GRPCPort: 9030}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Create and delete snapshot
	snap, _ := server.CreateSnapshot(ctx, "cluster-001", "to-delete")
	time.Sleep(200 * time.Millisecond)
	
	if err := server.DeleteSnapshot(ctx, snap.SnapshotID); err != nil {
		t.Fatalf("delete snapshot: %v", err)
	}
	
	time.Sleep(100 * time.Millisecond)
	
	// Verify deleted
	_, err := server.GetSnapshot(ctx, snap.SnapshotID)
	if err != ErrSnapshotNotFound {
		t.Errorf("expected ErrSnapshotNotFound, got %v", err)
	}
}

func TestRestoreToPointInTime(t *testing.T) {
	config := Config{NodeID: "backup-1", GRPCPort: 9030}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Create a snapshot first
	snap, _ := server.CreateSnapshot(ctx, "cluster-001", "base-snapshot")
	time.Sleep(200 * time.Millisecond)
	
	// Get snapshot to know its LSN
	snapInfo, _ := server.GetSnapshot(ctx, snap.SnapshotID)
	
	// Start restore
	restore, err := server.RestoreToPointInTime(ctx, "cluster-001", "restored-cluster", snapInfo.LSN+100, 0, "")
	if err != nil {
		t.Fatalf("restore: %v", err)
	}
	
	if restore.State != RestoreStatePending {
		t.Errorf("expected state Pending, got %d", restore.State)
	}
	
	// Wait for completion
	time.Sleep(700 * time.Millisecond)
	
	// Check status
	status, err := server.GetRestoreStatus(ctx, restore.RestoreID)
	if err != nil {
		t.Fatalf("get restore status: %v", err)
	}
	
	if status.State != RestoreStateCompleted {
		t.Errorf("expected state Completed, got %d", status.State)
	}
	if status.ProgressPercent != 100 {
		t.Errorf("expected progress 100, got %d", status.ProgressPercent)
	}
}

func TestBackupPolicy(t *testing.T) {
	config := Config{NodeID: "backup-1", GRPCPort: 9030}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	// Set policy
	policy := &BackupPolicy{
		Enabled:            true,
		Schedule:           "0 2 * * *",
		RetentionDays:      7,
		RedoRetentionHours: 24,
	}
	
	if err := server.SetBackupPolicy(ctx, "cluster-001", policy); err != nil {
		t.Fatalf("set policy: %v", err)
	}
	
	// Get policy
	retrieved, err := server.GetBackupPolicy(ctx, "cluster-001")
	if err != nil {
		t.Fatalf("get policy: %v", err)
	}
	
	if !retrieved.Enabled {
		t.Error("policy should be enabled")
	}
	if retrieved.RetentionDays != 7 {
		t.Errorf("expected retention 7 days, got %d", retrieved.RetentionDays)
	}
}

func TestLocalS3Client(t *testing.T) {
	client := NewLocalS3Client("/tmp/s3-test")
	ctx := context.Background()
	
	// Upload
	testData := "test data content"
	reader := &stringReader{data: testData}
	
	if err := client.Upload(ctx, "test-bucket", "test-key", reader); err != nil {
		t.Fatalf("upload: %v", err)
	}
	
	// Download
	var writer stringWriter
	if err := client.Download(ctx, "test-bucket", "test-key", &writer); err != nil {
		t.Fatalf("download: %v", err)
	}
	
	if writer.data != testData {
		t.Errorf("data mismatch: expected '%s', got '%s'", testData, writer.data)
	}
	
	// Delete
	if err := client.Delete(ctx, "test-bucket", "test-key"); err != nil {
		t.Fatalf("delete: %v", err)
	}
}

type stringReader struct {
	data string
	pos  int
}

func (r *stringReader) Read(p []byte) (n int, err error) {
	if r.pos >= len(r.data) {
		return 0, nil
	}
	n = copy(p, r.data[r.pos:])
	r.pos += n
	return n, nil
}

type stringWriter struct {
	data string
}

func (w *stringWriter) Write(p []byte) (n int, err error) {
	w.data += string(p)
	return len(p), nil
}
