package metadata

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/hashicorp/raft"
)

func TestFSMCreateVolume(t *testing.T) {
	fsm := NewFSM()

	// Create volume
	cmdData, _ := json.Marshal(CreateVolumeData{
		VolumeID:  "vol-001",
		ClusterID: "cluster-001",
		SizeBytes: 100 * 1024 * 1024 * 1024, // 100GB
		PGCount:   10,
		CreatedAt: time.Now().Unix(),
	})

	cmd := Command{Type: CmdCreateVolume, Data: cmdData}
	cmdBytes, _ := json.Marshal(cmd)

	result := fsm.Apply(&raft.Log{Data: cmdBytes})
	if result != nil {
		t.Fatalf("apply failed: %v", result)
	}

	// Verify volume exists
	vol, ok := fsm.GetVolume("vol-001")
	if !ok {
		t.Fatal("volume should exist")
	}
	if vol.ClusterID != "cluster-001" {
		t.Errorf("expected cluster-001, got %s", vol.ClusterID)
	}
	if vol.PGCount != 10 {
		t.Errorf("expected PGCount 10, got %d", vol.PGCount)
	}
}

func TestFSMUpdateVDL(t *testing.T) {
	fsm := NewFSM()

	// Create volume first
	createData, _ := json.Marshal(CreateVolumeData{
		VolumeID:  "vol-001",
		ClusterID: "cluster-001",
		CreatedAt: time.Now().Unix(),
	})
	createCmd := Command{Type: CmdCreateVolume, Data: createData}
	createBytes, _ := json.Marshal(createCmd)
	fsm.Apply(&raft.Log{Data: createBytes})

	// Update VDL
	updateData, _ := json.Marshal(UpdateVDLData{
		VolumeID:   "vol-001",
		NewVDL:     5000,
		InstanceID: "writer-001",
		UpdatedAt:  time.Now().Unix(),
	})
	updateCmd := Command{Type: CmdUpdateVDL, Data: updateData}
	updateBytes, _ := json.Marshal(updateCmd)

	result := fsm.Apply(&raft.Log{Data: updateBytes})
	if result != nil {
		t.Fatalf("apply failed: %v", result)
	}

	// Verify VDL
	vdl, ok := fsm.GetVDL("vol-001")
	if !ok {
		t.Fatal("volume should exist")
	}
	if vdl != 5000 {
		t.Errorf("expected VDL 5000, got %d", vdl)
	}
}

func TestFSMUpdateNodeLSN(t *testing.T) {
	fsm := NewFSM()

	// Create volume first
	createData, _ := json.Marshal(CreateVolumeData{
		VolumeID:  "vol-001",
		ClusterID: "cluster-001",
		CreatedAt: time.Now().Unix(),
	})
	createCmd := Command{Type: CmdCreateVolume, Data: createData}
	createBytes, _ := json.Marshal(createCmd)
	fsm.Apply(&raft.Log{Data: createBytes})

	// Update node LSNs
	for i, lsn := range []int64{1000, 995, 1000, 998, 990, 1000} {
		updateData, _ := json.Marshal(UpdateNodeLSNData{
			VolumeID:   "vol-001",
			NodeID:     string(rune('a' + i)),
			CurrentLSN: lsn,
		})
		updateCmd := Command{Type: CmdUpdateNodeLSN, Data: updateData}
		updateBytes, _ := json.Marshal(updateCmd)
		fsm.Apply(&raft.Log{Data: updateBytes})
	}

	// Verify calculated VDL (4th highest = 998)
	vdl := fsm.CalculateVDL("vol-001")
	if vdl != 998 {
		t.Errorf("expected VDL 998, got %d", vdl)
	}
}

func TestFSMRegisterInstance(t *testing.T) {
	fsm := NewFSM()

	// Register instance
	regData, _ := json.Marshal(RegisterInstanceData{
		InstanceID:   "writer-001",
		ClusterID:    "cluster-001",
		VolumeID:     "vol-001",
		Role:         "writer",
		Endpoint:     "10.0.0.1:3306",
		RegisteredAt: time.Now().Unix(),
	})
	regCmd := Command{Type: CmdRegisterInstance, Data: regData}
	regBytes, _ := json.Marshal(regCmd)

	result := fsm.Apply(&raft.Log{Data: regBytes})
	if result != nil {
		t.Fatalf("apply failed: %v", result)
	}

	// Verify instance
	inst, ok := fsm.GetInstance("writer-001")
	if !ok {
		t.Fatal("instance should exist")
	}
	if inst.Role != "writer" {
		t.Errorf("expected role writer, got %s", inst.Role)
	}
	if inst.Endpoint != "10.0.0.1:3306" {
		t.Errorf("expected endpoint 10.0.0.1:3306, got %s", inst.Endpoint)
	}
}

func TestFSMListInstances(t *testing.T) {
	fsm := NewFSM()

	// Register multiple instances
	instances := []RegisterInstanceData{
		{InstanceID: "writer-001", ClusterID: "cluster-001", Role: "writer"},
		{InstanceID: "reader-001", ClusterID: "cluster-001", Role: "reader"},
		{InstanceID: "reader-002", ClusterID: "cluster-001", Role: "reader"},
		{InstanceID: "writer-002", ClusterID: "cluster-002", Role: "writer"},
	}

	for _, inst := range instances {
		inst.RegisteredAt = time.Now().Unix()
		regData, _ := json.Marshal(inst)
		regCmd := Command{Type: CmdRegisterInstance, Data: regData}
		regBytes, _ := json.Marshal(regCmd)
		fsm.Apply(&raft.Log{Data: regBytes})
	}

	// List cluster-001 instances
	cluster1Instances := fsm.ListInstances("cluster-001")
	if len(cluster1Instances) != 3 {
		t.Errorf("expected 3 instances for cluster-001, got %d", len(cluster1Instances))
	}

	// List cluster-002 instances
	cluster2Instances := fsm.ListInstances("cluster-002")
	if len(cluster2Instances) != 1 {
		t.Errorf("expected 1 instance for cluster-002, got %d", len(cluster2Instances))
	}
}

func TestFSMUnregisterInstance(t *testing.T) {
	fsm := NewFSM()

	// Register instance
	regData, _ := json.Marshal(RegisterInstanceData{
		InstanceID:   "writer-001",
		ClusterID:    "cluster-001",
		RegisteredAt: time.Now().Unix(),
	})
	regCmd := Command{Type: CmdRegisterInstance, Data: regData}
	regBytes, _ := json.Marshal(regCmd)
	fsm.Apply(&raft.Log{Data: regBytes})

	// Unregister instance
	unregData, _ := json.Marshal(UnregisterInstanceData{InstanceID: "writer-001"})
	unregCmd := Command{Type: CmdUnregisterInstance, Data: unregData}
	unregBytes, _ := json.Marshal(unregCmd)

	result := fsm.Apply(&raft.Log{Data: unregBytes})
	if result != nil {
		t.Fatalf("apply failed: %v", result)
	}

	// Verify instance is gone
	_, ok := fsm.GetInstance("writer-001")
	if ok {
		t.Error("instance should not exist after unregistration")
	}
}

func TestFSMSnapshot(t *testing.T) {
	fsm := NewFSM()

	// Create some state
	createData, _ := json.Marshal(CreateVolumeData{
		VolumeID:  "vol-001",
		ClusterID: "cluster-001",
		PGCount:   10,
		CreatedAt: time.Now().Unix(),
	})
	createCmd := Command{Type: CmdCreateVolume, Data: createData}
	createBytes, _ := json.Marshal(createCmd)
	fsm.Apply(&raft.Log{Data: createBytes})

	// Take snapshot
	snapshot, err := fsm.Snapshot()
	if err != nil {
		t.Fatalf("snapshot failed: %v", err)
	}

	if snapshot == nil {
		t.Fatal("snapshot should not be nil")
	}
}
