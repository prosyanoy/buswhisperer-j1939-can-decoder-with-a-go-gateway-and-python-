// Command gateway replays a candump log through the C decoder, batches the
// decoded signals as protobuf and writes them to a file or an HTTP endpoint.
//
// Configuration is by environment variable (see .env.example).
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"time"

	"github.com/buswhisperer/gateway/internal/batch"
	"github.com/buswhisperer/gateway/internal/decoder"
	"github.com/buswhisperer/gateway/internal/pb"
	"github.com/buswhisperer/gateway/internal/sink"
)

type config struct {
	Decoder       string
	Log           string
	Sink          string
	Out           string
	URL           string
	BatchSize     int
	FlushInterval time.Duration
	HTTPRetries   int
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func getint(key string, def int) (int, error) {
	v := os.Getenv(key)
	if v == "" {
		return def, nil
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < 0 {
		return 0, fmt.Errorf("%s must be a non-negative integer, got %q", key, v)
	}
	return n, nil
}

func defaultDecoder() string {
	if exe, err := os.Executable(); err == nil {
		p := filepath.Join(filepath.Dir(exe), "bw_decode")
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return "bw_decode"
}

func loadConfig() (config, error) {
	var c config
	var err error
	c.Decoder = getenv("BW_DECODER", defaultDecoder())
	c.Log = getenv("BW_LOG", "data/sample.log")
	c.Sink = getenv("BW_SINK", "file")
	c.Out = getenv("BW_OUT", "signals.pb")
	c.URL = getenv("BW_HTTP_URL", "http://localhost:8080/ingest")
	if c.BatchSize, err = getint("BW_BATCH_SIZE", 100); err != nil {
		return c, err
	}
	if c.BatchSize == 0 {
		return c, fmt.Errorf("BW_BATCH_SIZE must be at least 1")
	}
	ms, err := getint("BW_FLUSH_INTERVAL_MS", 1000)
	if err != nil {
		return c, err
	}
	c.FlushInterval = time.Duration(ms) * time.Millisecond
	if c.HTTPRetries, err = getint("BW_HTTP_RETRIES", 3); err != nil {
		return c, err
	}
	if c.Sink != "file" && c.Sink != "http" {
		return c, fmt.Errorf("BW_SINK must be 'file' or 'http', got %q", c.Sink)
	}
	return c, nil
}

func run() error {
	cfg, err := loadConfig()
	if err != nil {
		return err
	}
	if _, err := exec.LookPath(cfg.Decoder); err != nil {
		return fmt.Errorf("decoder not found (set BW_DECODER): %w", err)
	}
	var in io.Reader = os.Stdin
	if cfg.Log != "-" {
		f, err := os.Open(cfg.Log)
		if err != nil {
			return err
		}
		defer f.Close()
		in = f
	}
	var out sink.Sink
	if cfg.Sink == "http" {
		out = sink.NewHTTP(cfg.URL, cfg.HTTPRetries, 200*time.Millisecond)
	} else if out, err = sink.NewFile(cfg.Out); err != nil {
		return err
	}
	defer out.Close()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	signals := make(chan pb.Signal, 256)
	type decRes struct {
		stats decoder.Stats
		err   error
	}
	dch := make(chan decRes, 1)
	go func() {
		stats, err := decoder.Run(ctx, cfg.Decoder, in, signals)
		close(signals)
		dch <- decRes{stats, err}
	}()
	res := batch.Run(ctx, signals, out, cfg.BatchSize, cfg.FlushInterval)
	d := <-dch

	summary, _ := json.Marshal(map[string]any{
		"signals": res.Signals, "batches": res.Batches, "failed_batches": res.FailedBatches,
		"sink": cfg.Sink, "decoder": d.stats,
	})
	fmt.Fprintf(os.Stderr, "summary %s\n", summary)
	if d.err != nil {
		return fmt.Errorf("decoder: %w", d.err)
	}
	if res.FailedBatches > 0 {
		return fmt.Errorf("%d batch(es) failed to deliver: %w", res.FailedBatches, res.FirstError)
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "gateway:", err)
		os.Exit(1)
	}
}
