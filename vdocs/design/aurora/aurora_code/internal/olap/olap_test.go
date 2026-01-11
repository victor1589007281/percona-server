package olap

import (
	"context"
	"testing"
	"time"
)

func TestDataTypeSize(t *testing.T) {
	tests := []struct {
		dt   DataType
		want int
	}{
		{TypeInt8, 1},
		{TypeInt16, 2},
		{TypeInt32, 4},
		{TypeInt64, 8},
		{TypeFloat32, 4},
		{TypeFloat64, 8},
		{TypeTimestamp, 8},
		{TypeString, 0},
	}
	
	for _, tt := range tests {
		if got := tt.dt.Size(); got != tt.want {
			t.Errorf("DataType(%d).Size() = %d, want %d", tt.dt, got, tt.want)
		}
	}
}

func TestOLAPEngine(t *testing.T) {
	config := DefaultOLAPConfig()
	config.DataPath = t.TempDir()
	
	engine := NewOLAPEngine(config)
	
	if err := engine.Start(); err != nil {
		t.Fatalf("Failed to start engine: %v", err)
	}
	defer engine.Stop()
	
	// Create table
	table := &ColumnTable{
		TableID:   1,
		TableName: "test_table",
		Columns: []*ColumnMeta{
			{ColumnID: 1, ColumnName: "id", DataType: TypeInt64, Nullable: false},
			{ColumnID: 2, ColumnName: "name", DataType: TypeString, Nullable: true},
			{ColumnID: 3, ColumnName: "value", DataType: TypeFloat64, Nullable: true},
		},
		PrimaryKey: []uint32{1},
		SortKey:    []uint32{1},
	}
	
	if err := engine.CreateTable(table); err != nil {
		t.Fatalf("Failed to create table: %v", err)
	}
	
	// Try to create duplicate
	if err := engine.CreateTable(table); err != ErrTableExists {
		t.Errorf("Expected ErrTableExists, got %v", err)
	}
	
	// Drop table
	if err := engine.DropTable(1); err != nil {
		t.Fatalf("Failed to drop table: %v", err)
	}
	
	// Try to drop non-existent
	if err := engine.DropTable(1); err != ErrTableNotFound {
		t.Errorf("Expected ErrTableNotFound, got %v", err)
	}
}

func TestColumnBatch(t *testing.T) {
	batch := &ColumnBatch{
		TableID:   1,
		RowCount:  2,
		RowIDs:    []uint64{1, 2},
		Timestamp: time.Now(),
		Columns: []ColumnVector{
			{
				ColumnID: 1,
				DataType: TypeInt64,
				Data:     make([]byte, 16), // 2 int64s
				Length:   2,
			},
		},
	}
	
	if batch.RowCount != 2 {
		t.Errorf("RowCount = %d, want 2", batch.RowCount)
	}
}

func TestSyncService(t *testing.T) {
	config := DefaultOLAPConfig()
	config.DataPath = t.TempDir()
	
	engine := NewOLAPEngine(config)
	engine.Start()
	defer engine.Stop()
	
	// Create table first
	engine.CreateTable(&ColumnTable{
		TableID:   1,
		TableName: "test",
		Columns: []*ColumnMeta{
			{ColumnID: 1, ColumnName: "id", DataType: TypeInt64},
		},
	})
	
	syncConfig := DefaultSyncConfig()
	syncService := NewSyncService(syncConfig, engine)
	
	if err := syncService.Start(); err != nil {
		t.Fatalf("Failed to start sync service: %v", err)
	}
	defer syncService.Stop()
	
	// Send event
	event := &RedoEvent{
		LSN:       1000,
		TableID:   1,
		Operation: OpInsert,
		NewData:   map[string]interface{}{"id": int64(1)},
		Timestamp: time.Now(),
	}
	
	syncService.SendEvent(event)
	
	// Wait a bit for processing
	time.Sleep(100 * time.Millisecond)
	
	stats := syncService.GetStats()
	if stats.EventsReceived == 0 {
		t.Error("Expected at least one event received")
	}
}

func TestQueryRouter(t *testing.T) {
	engine := NewOLAPEngine(nil)
	router := NewQueryRouter(engine)
	
	// Large query should use OLAP
	if !router.ShouldUseOLAP("SELECT * FROM table", 100000) {
		t.Error("Large query should use OLAP")
	}
	
	// Small query should use OLTP
	if router.ShouldUseOLAP("SELECT * FROM table WHERE id = 1", 1) {
		t.Error("Small query should use OLTP")
	}
}

func TestVectorizedExecutor(t *testing.T) {
	config := DefaultOLAPConfig()
	config.DataPath = t.TempDir()
	
	engine := NewOLAPEngine(config)
	engine.Start()
	defer engine.Stop()
	
	table := &ColumnTable{
		TableID:   1,
		TableName: "test",
		Columns: []*ColumnMeta{
			{ColumnID: 1, ColumnName: "id", DataType: TypeInt64},
			{ColumnID: 2, ColumnName: "value", DataType: TypeFloat64},
		},
	}
	engine.CreateTable(table)
	
	executor := NewVectorizedExecutor(engine, table)
	
	query := &OLAPQuery{
		TableID: 1,
		Columns: []uint32{1, 2},
		Aggregates: []Aggregate{
			{Function: AggCount, Column: 1},
		},
	}
	
	result, err := executor.Execute(context.Background(), query)
	if err != nil {
		t.Fatalf("Execute failed: %v", err)
	}
	
	if result == nil {
		t.Error("Result should not be nil")
	}
}
