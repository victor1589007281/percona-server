package pool

import (
	"sync"
)

type Task func() error

type WorkerPool struct {
	workers int
	tasks   chan Task
	wg      sync.WaitGroup
	errors  chan error
}

func NewWorkerPool(workers int) *WorkerPool {
	return &WorkerPool{
		workers: workers,
		tasks:   make(chan Task, workers*2),
		errors:  make(chan error, workers),
	}
}

func (p *WorkerPool) Start() {
	for i := 0; i < p.workers; i++ {
		p.wg.Add(1)
		go p.worker()
	}
}

func (p *WorkerPool) worker() {
	defer p.wg.Done()
	for task := range p.tasks {
		if err := task(); err != nil {
			p.errors <- err
		}
	}
}

func (p *WorkerPool) Submit(task Task) {
	p.tasks <- task
}

func (p *WorkerPool) Wait() {
	close(p.tasks)
	p.wg.Wait()
	close(p.errors)
}

func (p *WorkerPool) Errors() <-chan error {
	return p.errors
}
