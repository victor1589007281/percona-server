package quorum

import (
	"testing"
	"time"
)

func TestTrackerVDLCalculation(t *testing.T) {
	tracker := NewTracker(DefaultConfig())

	// Add LSNs for 6 nodes
	tracker.UpdateNodeLSN("node1", 1000)
	tracker.UpdateNodeLSN("node2", 995)
	tracker.UpdateNodeLSN("node3", 1000)
	tracker.UpdateNodeLSN("node4", 998)
	tracker.UpdateNodeLSN("node5", 990)
	tracker.UpdateNodeLSN("node6", 1000)

	// VDL should be the 4th highest = 998
	vdl := tracker.CalculateVDL()
	if vdl != 998 {
		t.Errorf("expected VDL 998, got %d", vdl)
	}
}

func TestTrackerVDLWithFewerNodes(t *testing.T) {
	tracker := NewTracker(DefaultConfig())

	// Only 3 nodes (less than Vw=4)
	tracker.UpdateNodeLSN("node1", 1000)
	tracker.UpdateNodeLSN("node2", 995)
	tracker.UpdateNodeLSN("node3", 990)

	// VDL should be 0 since we don't have enough nodes
	vdl := tracker.CalculateVDL()
	if vdl != 0 {
		t.Errorf("expected VDL 0, got %d", vdl)
	}
}

func TestTrackerMinMaxLSN(t *testing.T) {
	tracker := NewTracker(DefaultConfig())

	tracker.UpdateNodeLSN("node1", 1000)
	tracker.UpdateNodeLSN("node2", 500)
	tracker.UpdateNodeLSN("node3", 1500)

	min := tracker.GetMinLSN()
	max := tracker.GetMaxLSN()

	if min != 500 {
		t.Errorf("expected min 500, got %d", min)
	}
	if max != 1500 {
		t.Errorf("expected max 1500, got %d", max)
	}
}

func TestTrackerUpdateOnlyHigher(t *testing.T) {
	tracker := NewTracker(DefaultConfig())

	tracker.UpdateNodeLSN("node1", 1000)
	tracker.UpdateNodeLSN("node1", 500) // Lower, should be ignored
	tracker.UpdateNodeLSN("node1", 1500) // Higher, should update

	lsn, _ := tracker.GetNodeLSN("node1")
	if lsn != 1500 {
		t.Errorf("expected LSN 1500, got %d", lsn)
	}
}

func TestWriteAcker(t *testing.T) {
	config := DefaultConfig()
	acker := NewWriteAcker(config, 1000)

	// Send ACKs from 3 nodes (not enough for quorum)
	acker.Ack("node1", 1000)
	acker.Ack("node2", 1000)
	acker.Ack("node3", 1000)

	if acker.IsComplete() {
		t.Error("should not be complete with 3 ACKs")
	}
	if acker.AckCount() != 3 {
		t.Errorf("expected 3 ACKs, got %d", acker.AckCount())
	}

	// Send 4th ACK (quorum reached)
	done := acker.Ack("node4", 1000)
	if !done {
		t.Error("should return true when quorum is reached")
	}
	if !acker.IsComplete() {
		t.Error("should be complete with 4 ACKs")
	}
}

func TestWriteAckerDoneChannel(t *testing.T) {
	config := DefaultConfig()
	acker := NewWriteAcker(config, 1000)

	// Start a goroutine to wait for done
	done := make(chan bool)
	go func() {
		select {
		case <-acker.Done():
			done <- true
		case <-time.After(time.Second):
			done <- false
		}
	}()

	// Send enough ACKs
	acker.Ack("node1", 1000)
	acker.Ack("node2", 1000)
	acker.Ack("node3", 1000)
	acker.Ack("node4", 1000)

	select {
	case result := <-done:
		if !result {
			t.Error("done channel should have been closed")
		}
	case <-time.After(time.Second):
		t.Error("timeout waiting for done")
	}
}

func TestWriteAckerLowLSNIgnored(t *testing.T) {
	config := DefaultConfig()
	acker := NewWriteAcker(config, 1000)

	// ACK with lower LSN should be ignored
	acker.Ack("node1", 999)
	if acker.AckCount() != 0 {
		t.Errorf("expected 0 ACKs, got %d", acker.AckCount())
	}

	// ACK with exact LSN should count
	acker.Ack("node1", 1000)
	if acker.AckCount() != 1 {
		t.Errorf("expected 1 ACK, got %d", acker.AckCount())
	}

	// ACK with higher LSN should count
	acker.Ack("node2", 1001)
	if acker.AckCount() != 2 {
		t.Errorf("expected 2 ACKs, got %d", acker.AckCount())
	}
}

func TestGetAckedNodes(t *testing.T) {
	config := DefaultConfig()
	acker := NewWriteAcker(config, 1000)

	acker.Ack("node1", 1000)
	acker.Ack("node3", 1000)

	nodes := acker.GetAckedNodes()
	if len(nodes) != 2 {
		t.Errorf("expected 2 nodes, got %d", len(nodes))
	}

	// Check nodes are correct (order may vary)
	nodeSet := make(map[string]bool)
	for _, n := range nodes {
		nodeSet[n] = true
	}
	if !nodeSet["node1"] || !nodeSet["node3"] {
		t.Errorf("unexpected nodes: %v", nodes)
	}
}
