package dts

import (
	"context"
	"testing"
	"time"
)

func TestCreateMigrationTask(t *testing.T) {
	config := Config{NodeID: "dts-1", GRPCPort: 9020}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	source := &SourceEndpoint{
		Host:     "mysql.source.com",
		Port:     3306,
		Username: "root",
		Password: "password",
	}
	
	target := &TargetEndpoint{
		ClusterID: "cluster-001",
		Database:  "mydb",
	}
	
	task, err := server.CreateMigrationTask(ctx, "test-migration", source, target, MigrationModeFullAndIncremental, []string{"db1", "db2"})
	if err != nil {
		t.Fatalf("create task: %v", err)
	}
	
	if task.TaskName != "test-migration" {
		t.Errorf("expected name 'test-migration', got '%s'", task.TaskName)
	}
	if task.State != MigrationStateCreated {
		t.Errorf("expected state Created, got %d", task.State)
	}
	if len(task.Databases) != 2 {
		t.Errorf("expected 2 databases, got %d", len(task.Databases))
	}
}

func TestStartMigrationTask(t *testing.T) {
	config := Config{NodeID: "dts-1", GRPCPort: 9020}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	source := &SourceEndpoint{Host: "localhost", Port: 3306}
	target := &TargetEndpoint{ClusterID: "cluster-001"}
	
	task, _ := server.CreateMigrationTask(ctx, "test", source, target, MigrationModeStructureOnly, []string{"db1"})
	
	if err := server.StartMigrationTask(ctx, task.TaskID); err != nil {
		t.Fatalf("start task: %v", err)
	}
	
	// Check state is running
	updated, _ := server.GetMigrationTask(ctx, task.TaskID)
	if updated.State != MigrationStateRunning {
		t.Errorf("expected state Running, got %d", updated.State)
	}
	
	// Wait for completion
	time.Sleep(200 * time.Millisecond)
	
	// Should be completed for structure-only mode
	final, _ := server.GetMigrationTask(ctx, task.TaskID)
	if final.State != MigrationStateCompleted {
		t.Errorf("expected state Completed, got %d", final.State)
	}
}

func TestMigrationProgress(t *testing.T) {
	config := Config{NodeID: "dts-1", GRPCPort: 9020}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	source := &SourceEndpoint{Host: "localhost", Port: 3306}
	target := &TargetEndpoint{ClusterID: "cluster-001"}
	
	task, _ := server.CreateMigrationTask(ctx, "test", source, target, MigrationModeFull, []string{"db1"})
	server.StartMigrationTask(ctx, task.TaskID)
	
	// Check progress updates
	time.Sleep(100 * time.Millisecond)
	
	updated, _ := server.GetMigrationTask(ctx, task.TaskID)
	if updated.Progress == nil {
		t.Error("progress should not be nil")
	}
}

func TestListMigrationTasks(t *testing.T) {
	config := Config{NodeID: "dts-1", GRPCPort: 9020}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	source := &SourceEndpoint{Host: "localhost", Port: 3306}
	target := &TargetEndpoint{ClusterID: "cluster-001"}
	
	server.CreateMigrationTask(ctx, "task1", source, target, MigrationModeFull, nil)
	server.CreateMigrationTask(ctx, "task2", source, target, MigrationModeFull, nil)
	
	tasks, err := server.ListMigrationTasks(ctx)
	if err != nil {
		t.Fatalf("list tasks: %v", err)
	}
	
	if len(tasks) != 2 {
		t.Errorf("expected 2 tasks, got %d", len(tasks))
	}
}

func TestCreateSyncTask(t *testing.T) {
	config := Config{NodeID: "dts-1", GRPCPort: 9020}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	source := &SourceEndpoint{Host: "localhost", Port: 3306}
	target := &TargetEndpoint{ClusterID: "cluster-001"}
	
	task, err := server.CreateSyncTask(ctx, "test-sync", source, target, []string{"db1"})
	if err != nil {
		t.Fatalf("create sync task: %v", err)
	}
	
	if task.State != SyncStateCreated {
		t.Errorf("expected state Created, got %d", task.State)
	}
}

func TestStartStopSyncTask(t *testing.T) {
	config := Config{NodeID: "dts-1", GRPCPort: 9020}
	server, _ := NewServer(config)
	ctx := context.Background()
	
	source := &SourceEndpoint{Host: "localhost", Port: 3306}
	target := &TargetEndpoint{ClusterID: "cluster-001"}
	
	task, _ := server.CreateSyncTask(ctx, "test-sync", source, target, nil)
	
	// Start
	if err := server.StartSyncTask(ctx, task.TaskID); err != nil {
		t.Fatalf("start sync task: %v", err)
	}
	
	time.Sleep(200 * time.Millisecond)
	
	// Check running
	running, _ := server.GetSyncTask(ctx, task.TaskID)
	if running.State != SyncStateRunning {
		t.Errorf("expected state Running, got %d", running.State)
	}
	if running.CurrentGTID == "" {
		t.Error("GTID should be set")
	}
	
	// Stop
	if err := server.StopSyncTask(ctx, task.TaskID); err != nil {
		t.Fatalf("stop sync task: %v", err)
	}
	
	stopped, _ := server.GetSyncTask(ctx, task.TaskID)
	if stopped.State != SyncStateStopped {
		t.Errorf("expected state Stopped, got %d", stopped.State)
	}
}

func TestBinlogParser(t *testing.T) {
	parser := &BinlogParser{
		source: &SourceEndpoint{Host: "localhost", Port: 3306},
	}
	ctx := context.Background()
	
	events, gtid, err := parser.GetNextBatch(ctx)
	if err != nil {
		t.Fatalf("get batch: %v", err)
	}
	
	if len(events) == 0 {
		t.Error("expected some events")
	}
	if gtid == "" {
		t.Error("GTID should not be empty")
	}
	
	delay := parser.GetDelay()
	if delay < 0 {
		t.Error("delay should be non-negative")
	}
}
