package sink

import (
	"bufio"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	"github.com/buswhisperer/gateway/internal/pb"
)

func batch(seq uint64) *pb.Batch {
	return &pb.Batch{Sequence: seq, Signals: []pb.Signal{{Name: "engine_speed", Unit: "rpm", Value: 800}}}
}

func TestFileSink(t *testing.T) {
	p := filepath.Join(t.TempDir(), "out.pb")
	s, err := NewFile(p)
	if err != nil {
		t.Fatal(err)
	}
	for i := uint64(1); i <= 2; i++ {
		if err := s.Send(context.Background(), batch(i)); err != nil {
			t.Fatal(err)
		}
	}
	s.Close()
	f, _ := os.Open(p)
	defer f.Close()
	br := bufio.NewReader(f)
	for i := uint64(1); i <= 2; i++ {
		b, err := pb.ReadDelimited(br)
		if err != nil || b.Sequence != i {
			t.Fatalf("%v %+v", err, b)
		}
	}
	if _, err := pb.ReadDelimited(br); err != io.EOF {
		t.Fatal("want EOF")
	}
	if _, err := NewFile("/nonexistent-dir/x.pb"); err == nil {
		t.Fatal("expected error")
	}
}

func TestHTTPSinkSuccessAndRetry(t *testing.T) {
	var calls int32
	var got *pb.Batch
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if atomic.AddInt32(&calls, 1) < 3 {
			http.Error(w, "busy", http.StatusServiceUnavailable)
			return
		}
		if r.Header.Get("Content-Type") != "application/x-protobuf" {
			t.Errorf("content type %q", r.Header.Get("Content-Type"))
		}
		body, _ := io.ReadAll(r.Body)
		got, _ = pb.Unmarshal(body)
		w.WriteHeader(http.StatusNoContent)
	}))
	defer srv.Close()
	s := NewHTTP(srv.URL, 3, time.Millisecond)
	if err := s.Send(context.Background(), batch(9)); err != nil {
		t.Fatal(err)
	}
	if calls != 3 || got == nil || got.Sequence != 9 {
		t.Fatalf("calls=%d got=%+v", calls, got)
	}
}

func TestHTTPSinkFailures(t *testing.T) {
	var calls int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&calls, 1)
		http.Error(w, "bad", http.StatusBadRequest)
	}))
	s := NewHTTP(srv.URL, 3, time.Millisecond)
	if err := s.Send(context.Background(), batch(1)); err == nil || calls != 1 {
		t.Fatalf("4xx must fail without retry: err=%v calls=%d", err, calls)
	}
	srv.Close()
	s = NewHTTP(srv.URL, 2, time.Millisecond)
	if err := s.Send(context.Background(), batch(1)); err == nil {
		t.Fatal("expected connection error")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	s = NewHTTP(srv.URL, 5, time.Hour)
	if err := s.Send(ctx, batch(1)); err == nil {
		t.Fatal("expected cancelled error")
	}
}
