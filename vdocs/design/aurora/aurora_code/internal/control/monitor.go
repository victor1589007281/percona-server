package control

import (
	"context"
	"sync"
	"time"
)

// MonitorService monitors cluster health
type MonitorService struct {
	server       *Server
	checkInterval time.Duration
	failThreshold int
	
	mu           sync.RWMutex
	healthStatus map[string]*InstanceHealth
}

// InstanceHealth represents health status of an instance
type InstanceHealth struct {
	InstanceID       string
	ClusterID        string
	IsHealthy        bool
	ConsecutiveFails int
	LastCheck        time.Time
	CurrentLSN       int64
	Role             InstanceRole
}

// NewMonitorService creates a new monitor service
func NewMonitorService(server *Server) *MonitorService {
	return &MonitorService{
		server:        server,
		checkInterval: 5 * time.Second,
		failThreshold: 3,
		healthStatus:  make(map[string]*InstanceHealth),
	}
}

// Start starts the monitor service
func (m *MonitorService) Start(ctx context.Context) {
	ticker := time.NewTicker(m.checkInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			m.checkAll(ctx)
		}
	}
}

// checkAll checks all instances
func (m *MonitorService) checkAll(ctx context.Context) {
	clusters, _ := m.server.ListClusters(ctx)

	for _, cluster := range clusters {
		// Check writer
		if cluster.Writer != nil {
			m.checkInstance(ctx, cluster.ClusterID, cluster.Writer)
		}

		// Check readers
		for _, reader := range cluster.Readers {
			m.checkInstance(ctx, cluster.ClusterID, reader)
		}
	}
}

// checkInstance checks a single instance
func (m *MonitorService) checkInstance(ctx context.Context, clusterID string, inst *InstanceInfo) {
	m.mu.Lock()
	defer m.mu.Unlock()

	health, ok := m.healthStatus[inst.InstanceID]
	if !ok {
		health = &InstanceHealth{
			InstanceID: inst.InstanceID,
			ClusterID:  clusterID,
			IsHealthy:  true,
			Role:       inst.Role,
		}
		m.healthStatus[inst.InstanceID] = health
	}

	// In a real implementation, this would:
	// 1. Send gRPC health check to the instance
	// 2. Update health status based on response

	// Simulate health check
	isHealthy := inst.State == InstanceStateAvailable

	if !isHealthy {
		health.ConsecutiveFails++
		if health.ConsecutiveFails >= m.failThreshold && health.IsHealthy {
			health.IsHealthy = false
			
			// Trigger failover if writer failed
			if health.Role == InstanceRoleWriter {
				go m.triggerFailover(ctx, clusterID, inst.InstanceID)
			}
		}
	} else {
		health.ConsecutiveFails = 0
		health.IsHealthy = true
		health.CurrentLSN = inst.CurrentLSN
	}

	health.LastCheck = time.Now()
}

// triggerFailover triggers a failover for a failed writer
func (m *MonitorService) triggerFailover(ctx context.Context, clusterID, failedWriterID string) {
	_, err := m.server.TriggerFailover(ctx, clusterID, "")
	if err != nil {
		// Log error
		_ = err
	}
}

// GetHealth returns health status for an instance
func (m *MonitorService) GetHealth(instanceID string) (*InstanceHealth, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	h, ok := m.healthStatus[instanceID]
	return h, ok
}

// GetAllHealth returns all health statuses
func (m *MonitorService) GetAllHealth() map[string]*InstanceHealth {
	m.mu.RLock()
	defer m.mu.RUnlock()

	result := make(map[string]*InstanceHealth)
	for k, v := range m.healthStatus {
		copied := *v
		result[k] = &copied
	}
	return result
}
