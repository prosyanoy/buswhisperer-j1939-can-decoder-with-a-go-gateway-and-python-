// Package sink delivers protobuf batches to a file or an HTTP endpoint.
package sink

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"
	"os"
	"time"

	"github.com/buswhisperer/gateway/internal/pb"
)

// Sink receives finished batches.
type Sink interface {
	Send(ctx context.Context, b *pb.Batch) error
	Close() error
}

// File appends length-delimited SignalBatch messages to a file.
type File struct {
	f *os.File
}

// NewFile creates (truncating) the output file.
func NewFile(path string) (*File, error) {
	f, err := os.Create(path)
	if err != nil {
		return nil, err
	}
	return &File{f: f}, nil
}

func (s *File) Send(_ context.Context, b *pb.Batch) error {
	_, err := s.f.Write(b.MarshalDelimited())
	return err
}

func (s *File) Close() error { return s.f.Close() }

// HTTP POSTs each batch as application/x-protobuf. Network errors and 5xx
// responses are retried with exponential backoff; other 4xx are permanent.
type HTTP struct {
	URL     string
	Retries int           // additional attempts after the first
	Backoff time.Duration // first retry delay, doubled each time
	Client  *http.Client
}

// NewHTTP returns an HTTP sink with a 5 s request timeout.
func NewHTTP(url string, retries int, backoff time.Duration) *HTTP {
	return &HTTP{URL: url, Retries: retries, Backoff: backoff, Client: &http.Client{Timeout: 5 * time.Second}}
}

type permanentError struct{ error }

func (s *HTTP) post(ctx context.Context, body []byte) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.URL, bytes.NewReader(body))
	if err != nil {
		return permanentError{err}
	}
	req.Header.Set("Content-Type", "application/x-protobuf")
	resp, err := s.Client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<20))
	switch {
	case resp.StatusCode/100 == 2:
		return nil
	case resp.StatusCode >= 500:
		return fmt.Errorf("server returned %s", resp.Status)
	default:
		return permanentError{fmt.Errorf("server rejected batch: %s", resp.Status)}
	}
}

func (s *HTTP) Send(ctx context.Context, b *pb.Batch) error {
	body := b.Marshal()
	delay := s.Backoff
	var err error
	for attempt := 0; attempt <= s.Retries; attempt++ {
		if err = s.post(ctx, body); err == nil {
			return nil
		}
		if _, perm := err.(permanentError); perm || attempt == s.Retries {
			break
		}
		select {
		case <-time.After(delay):
			delay *= 2
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	return err
}

func (s *HTTP) Close() error { return nil }
