package batch

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/buswhisperer/gateway/internal/pb"
)

type memSink struct {
	batches []*pb.Batch
	failOn  uint64
}

func (m *memSink) Send(_ context.Context, b *pb.Batch) error {
	if b.Sequence == m.failOn {
		return errors.New("boom")
	}
	m.batches = append(m.batches, b)
	return nil
}
func (m *memSink) Close() error { return nil }

func feed(n int) chan pb.Signal {
	ch := make(chan pb.Signal, n)
	for i := 0; i < n; i++ {
		ch <- pb.Signal{Name: "x", Value: float64(i)}
	}
	close(ch)
	return ch
}

func TestBatchBySize(t *testing.T) {
	m := &memSink{}
	res := Run(context.Background(), feed(25), m, 10, 0)
	if res.Signals != 25 || res.Batches != 3 || res.FailedBatches != 0 {
		t.Fatalf("%+v", res)
	}
	sizes := []int{len(m.batches[0].Signals), len(m.batches[1].Signals), len(m.batches[2].Signals)}
	if sizes[0] != 10 || sizes[1] != 10 || sizes[2] != 5 {
		t.Fatalf("sizes %v", sizes)
	}
	for i, b := range m.batches {
		if b.Sequence != uint64(i+1) {
			t.Fatalf("sequence %d at %d", b.Sequence, i)
		}
	}
	if m.batches[2].Signals[4].Value != 24 {
		t.Fatal("order not preserved")
	}
}

func TestEmptyInput(t *testing.T) {
	m := &memSink{}
	res := Run(context.Background(), feed(0), m, 10, 0)
	if res.Batches != 0 || len(m.batches) != 0 {
		t.Fatalf("%+v", res)
	}
}

func TestFailureIsCountedAndRunContinues(t *testing.T) {
	m := &memSink{failOn: 2}
	res := Run(context.Background(), feed(30), m, 10, 0)
	if res.Batches != 2 || res.FailedBatches != 1 || res.FirstError == nil {
		t.Fatalf("%+v", res)
	}
}

func TestFlushOnInterval(t *testing.T) {
	m := &memSink{}
	in := make(chan pb.Signal)
	done := make(chan Result)
	go func() { done <- Run(context.Background(), in, m, 1000, 20*time.Millisecond) }()
	in <- pb.Signal{Name: "a"}
	time.Sleep(150 * time.Millisecond) // interval flush happens well before size
	in <- pb.Signal{Name: "b"}
	close(in)
	res := <-done
	if res.Batches != 2 || len(m.batches[0].Signals) != 1 {
		t.Fatalf("%+v", res)
	}
}
