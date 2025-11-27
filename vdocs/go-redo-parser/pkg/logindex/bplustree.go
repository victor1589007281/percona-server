package logindex

import (
	"sort"
	"sync"
)

// B+Tree configuration
const (
	// BPlusTreeDegree defines the maximum number of children per node
	// Using 16 to fit in a cache line (64 bytes)
	BPlusTreeDegree = 16
	MaxKeys         = BPlusTreeDegree - 1
	MinKeys         = BPlusTreeDegree / 2
)

// LSNBPlusTree is a B+Tree that stores LSNs for a specific page
type LSNBPlusTree struct {
	mu     sync.RWMutex
	root   *bplusNode
	minLSN uint64
	maxLSN uint64
	count  int
}

// bplusNode represents a node in the B+Tree
type bplusNode struct {
	isLeaf   bool
	keys     []uint64      // LSN values
	children []*bplusNode  // Child nodes (for internal nodes)
	next     *bplusNode    // Next leaf node (for leaf nodes only)
}

// NewLSNBPlusTree creates a new B+Tree for storing LSNs
func NewLSNBPlusTree() *LSNBPlusTree {
	return &LSNBPlusTree{
		root: &bplusNode{
			isLeaf: true,
			keys:   make([]uint64, 0, MaxKeys),
		},
		minLSN: ^uint64(0), // Max uint64
		maxLSN: 0,
		count:  0,
	}
}

// Insert adds a new LSN to the tree
func (t *LSNBPlusTree) Insert(lsn uint64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	
	// Update min/max
	if lsn < t.minLSN {
		t.minLSN = lsn
	}
	if lsn > t.maxLSN {
		t.maxLSN = lsn
	}
	
	// Check if LSN already exists
	if t.containsNoLock(lsn) {
		return
	}
	
	// Insert into tree
	newRoot := t.insertInternal(t.root, lsn)
	if newRoot != nil {
		t.root = newRoot
	}
	t.count++
}

// Contains checks if an LSN exists in the tree
func (t *LSNBPlusTree) Contains(lsn uint64) bool {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.containsNoLock(lsn)
}

func (t *LSNBPlusTree) containsNoLock(lsn uint64) bool {
	node := t.root
	
	for !node.isLeaf {
		idx := sort.Search(len(node.keys), func(i int) bool {
			return node.keys[i] > lsn
		})
		node = node.children[idx]
	}
	
	idx := sort.SearchUint64s(node.keys, lsn)
	return idx < len(node.keys) && node.keys[idx] == lsn
}

// RangeQuery returns all LSNs in the range [startLSN, endLSN]
func (t *LSNBPlusTree) RangeQuery(startLSN, endLSN uint64) []uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()
	
	if startLSN > endLSN {
		return nil
	}
	
	// Quick check using min/max
	if endLSN < t.minLSN || startLSN > t.maxLSN {
		return nil
	}
	
	result := make([]uint64, 0, 32)
	
	// Find the starting leaf node
	node := t.findLeafNode(startLSN)
	
	// Traverse leaf nodes and collect LSNs in range
	for node != nil {
		for _, lsn := range node.keys {
			if lsn > endLSN {
				return result
			}
			if lsn >= startLSN {
				result = append(result, lsn)
			}
		}
		node = node.next
	}
	
	return result
}

// PurgeBefore removes all LSNs less than the given threshold
func (t *LSNBPlusTree) PurgeBefore(threshold uint64) int {
	t.mu.Lock()
	defer t.mu.Unlock()
	
	if threshold <= t.minLSN {
		return 0 // Nothing to purge
	}
	
	if threshold > t.maxLSN {
		// Remove all LSNs
		count := t.count
		t.root = &bplusNode{
			isLeaf: true,
			keys:   make([]uint64, 0, MaxKeys),
		}
		t.minLSN = ^uint64(0)
		t.maxLSN = 0
		t.count = 0
		return count
	}
	
	// Rebuild the tree with only LSNs >= threshold
	return t.rebuildAfterPurge(threshold)
}

// GetMinLSN returns the minimum LSN in the tree
func (t *LSNBPlusTree) GetMinLSN() uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.minLSN
}

// GetMaxLSN returns the maximum LSN in the tree
func (t *LSNBPlusTree) GetMaxLSN() uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.maxLSN
}

// Count returns the number of LSNs in the tree
func (t *LSNBPlusTree) Count() int {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.count
}

// GetAll returns all LSNs in sorted order
func (t *LSNBPlusTree) GetAll() []uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()
	
	result := make([]uint64, 0, t.count)
	node := t.findLeafNode(t.minLSN)
	
	for node != nil {
		result = append(result, node.keys...)
		node = node.next
	}
	
	return result
}

// insertInternal inserts an LSN into the tree and handles node splitting
func (t *LSNBPlusTree) insertInternal(node *bplusNode, lsn uint64) *bplusNode {
	if node.isLeaf {
		// Insert into leaf node
		return t.insertIntoLeaf(node, lsn)
	}
	
	// Find the child to insert into
	idx := sort.Search(len(node.keys), func(i int) bool {
		return node.keys[i] > lsn
	})
	
	child := node.children[idx]
	newChild := t.insertInternal(child, lsn)
	
	if newChild != nil {
		// Child was split, insert the new child
		return t.insertIntoInternal(node, idx, newChild)
	}
	
	return nil
}

// insertIntoLeaf inserts an LSN into a leaf node
func (t *LSNBPlusTree) insertIntoLeaf(node *bplusNode, lsn uint64) *bplusNode {
	// Find insertion position
	idx := sort.Search(len(node.keys), func(i int) bool {
		return node.keys[i] >= lsn
	})
	
	// Insert the LSN
	node.keys = append(node.keys, 0)
	copy(node.keys[idx+1:], node.keys[idx:])
	node.keys[idx] = lsn
	
	// Check if split is needed
	if len(node.keys) >= BPlusTreeDegree {
		return t.splitLeaf(node)
	}
	
	return nil
}

// splitLeaf splits a full leaf node
func (t *LSNBPlusTree) splitLeaf(node *bplusNode) *bplusNode {
	mid := len(node.keys) / 2
	
	// Create new right node
	right := &bplusNode{
		isLeaf: true,
		keys:   make([]uint64, len(node.keys)-mid),
		next:   node.next,
	}
	copy(right.keys, node.keys[mid:])
	
	// Update left node
	node.keys = node.keys[:mid:mid]
	node.next = right
	
	// Create new parent
	parent := &bplusNode{
		isLeaf:   false,
		keys:     []uint64{right.keys[0]},
		children: []*bplusNode{node, right},
	}
	
	return parent
}

// insertIntoInternal inserts a new child into an internal node
func (t *LSNBPlusTree) insertIntoInternal(node *bplusNode, childIdx int, newChild *bplusNode) *bplusNode {
	// Extract the key from the new child
	key := newChild.keys[0]
	
	// Insert the key and children
	node.keys = append(node.keys, 0)
	copy(node.keys[childIdx+1:], node.keys[childIdx:])
	node.keys[childIdx] = key
	
	node.children = append(node.children, nil)
	copy(node.children[childIdx+2:], node.children[childIdx+1:])
	node.children[childIdx] = newChild.children[0]
	node.children[childIdx+1] = newChild.children[1]
	
	// Check if split is needed
	if len(node.keys) >= BPlusTreeDegree {
		return t.splitInternal(node)
	}
	
	return nil
}

// splitInternal splits a full internal node
func (t *LSNBPlusTree) splitInternal(node *bplusNode) *bplusNode {
	mid := len(node.keys) / 2
	
	// Create new right node
	right := &bplusNode{
		isLeaf:   false,
		keys:     make([]uint64, len(node.keys)-mid-1),
		children: make([]*bplusNode, len(node.children)-mid-1),
	}
	copy(right.keys, node.keys[mid+1:])
	copy(right.children, node.children[mid+1:])
	
	// Update left node
	midKey := node.keys[mid]
	node.keys = node.keys[:mid:mid]
	node.children = node.children[:mid+1:mid+1]
	
	// Create new parent
	parent := &bplusNode{
		isLeaf:   false,
		keys:     []uint64{midKey},
		children: []*bplusNode{node, right},
	}
	
	return parent
}

// findLeafNode finds the leaf node that could contain the given LSN
func (t *LSNBPlusTree) findLeafNode(lsn uint64) *bplusNode {
	node := t.root
	
	for !node.isLeaf {
		idx := sort.Search(len(node.keys), func(i int) bool {
			return node.keys[i] > lsn
		})
		node = node.children[idx]
	}
	
	return node
}

// rebuildAfterPurge rebuilds the tree after purging LSNs before threshold
func (t *LSNBPlusTree) rebuildAfterPurge(threshold uint64) int {
	// Collect all LSNs >= threshold
	remaining := make([]uint64, 0, t.count)
	node := t.findLeafNode(t.minLSN)
	
	for node != nil {
		for _, lsn := range node.keys {
			if lsn >= threshold {
				remaining = append(remaining, lsn)
			}
		}
		node = node.next
	}
	
	purgedCount := t.count - len(remaining)
	
	// Rebuild tree
	t.root = &bplusNode{
		isLeaf: true,
		keys:   make([]uint64, 0, MaxKeys),
	}
	
	if len(remaining) > 0 {
		t.minLSN = remaining[0]
		t.maxLSN = remaining[len(remaining)-1]
		t.count = len(remaining)
		
		for _, lsn := range remaining {
			newRoot := t.insertInternal(t.root, lsn)
			if newRoot != nil {
				t.root = newRoot
			}
		}
	} else {
		t.minLSN = ^uint64(0)
		t.maxLSN = 0
		t.count = 0
	}
	
	return purgedCount
}


// LSNBPlusTree type alias for compatibility
type LSNBPlusTree = BPlusTree

// NewLSNBPlusTree creates a new B+Tree for LSNs
func NewLSNBPlusTree() *LSNBPlusTree {
	return NewBPlusTree()
}

// GetAll returns all LSNs in sorted order
func (tree *BPlusTree) GetAll() []uint64 {
	lsns := make([]uint64, 0, tree.Count())
	if tree.root == nil {
		return lsns
	}
	
	// Find leftmost leaf
	node := tree.root
	for !node.isLeaf {
		inode := node.internal
		if len(inode.children) > 0 {
			node = inode.children[0]
		} else {
			break
		}
	}
	
	// Collect all LSNs from linked leaves
	for node != nil && node.isLeaf {
		lnode := node.leaf
		lsns = append(lsns, lnode.lsns...)
		node = lnode.next
	}
	
	return lsns
}
