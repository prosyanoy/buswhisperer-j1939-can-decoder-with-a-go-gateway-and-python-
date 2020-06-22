// Package batch groups decoded signals into protobuf batches.
package batch

import (
	"context"
	"fmt"
	"time"

	"github.com/buswhisperer/gateway/internal/pb"
	"github.com/buswhisperer/gateway/internal/sink"
)

// Result summarises a batching run.
type Result struct {
	Signals       int
	Batches       int
	FailedBatches int
	FirstError    error
}

// Run reads signals from `in` until it is closed, emitting a batch whenever
// `size` signals have accumulated or `interval` has elapsed with data pending.
// A failed send is counted and the run continues, so one bad delivery does not
// lose the rest of the log; the caller decides how to treat Result.FailedBatches.
func Run(ctx context.Context, in <-chan pb.Signal, out sink.Sink, size int, interval time.Duration) Result {
	if size < 1 {
		size = 1
	}
	var res Result
	var seq uint64
	pending := make([]pb.Signal, 0, size)

	flush := func() {
		if len(pending) == 0 {
			return
		}
		seq++
		b := &pb.Batch{Sequence: seq, Signals: pending}
		if err := out.Send(ctx, b); err != nil {
			res.FailedBatches++
			if res.FirstError == nil {
				res.FirstError = fmt.Errorf("batch %d: %w", seq, err)
			}
		} else {
			res.Batches++
		}
		pending = make([]pb.Signal, 0, size)
	}

	var tick <-chan time.Time
	if interval > 0 {
		t := time.NewTicker(interval)
		defer t.Stop()
		tick = t.C
	}
	for {
		select {
		case s, ok := <-in:
			if !ok {
				flush()
				return res
			}
			res.Signals++
			pending = append(pending, s)
			if len(pending) >= size {
				flush()
			}
		case <-tick:
			flush()
		}
	}
}
