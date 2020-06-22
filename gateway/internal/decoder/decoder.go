// Package decoder runs the C decoder (bw_decode) as a subprocess and turns its
// JSON-lines output into pb.Signal values.
package decoder

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os/exec"

	"github.com/buswhisperer/gateway/internal/pb"
)

// Stats is the decoder's end-of-run counter summary (see bw_decode --stats).
type Stats map[string]uint64

type record struct {
	TsUs  uint64   `json:"ts_us"`
	SA    uint32   `json:"sa"`
	PGN   uint32   `json:"pgn"`
	Name  string   `json:"name"`
	Unit  string   `json:"unit"`
	Value *float64 `json:"value"`
	Text  *string  `json:"text"`
}

// ParseLine converts one JSON line from bw_decode into a Signal.
func ParseLine(line []byte) (pb.Signal, error) {
	var r record
	if err := json.Unmarshal(line, &r); err != nil {
		return pb.Signal{}, err
	}
	if r.Name == "" {
		return pb.Signal{}, fmt.Errorf("decoder record without name: %q", line)
	}
	s := pb.Signal{TimestampUs: r.TsUs, SourceAddress: r.SA, PGN: r.PGN, Name: r.Name, Unit: r.Unit}
	if r.Value != nil {
		s.Value = *r.Value
	}
	if r.Text != nil {
		s.Text = *r.Text
	}
	return s, nil
}

// Run feeds `input` (a candump log) to the decoder binary and sends every
// decoded signal on `out`. It closes neither `input` nor `out`.
func Run(ctx context.Context, binary string, input io.Reader, out chan<- pb.Signal) (Stats, error) {
	cmd := exec.CommandContext(ctx, binary, "--stats")
	cmd.Stdin = input
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start decoder %q: %w", binary, err)
	}

	sc := bufio.NewScanner(stdout)
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	var parseErr error
	for sc.Scan() {
		sig, err := ParseLine(sc.Bytes())
		if err != nil {
			parseErr = err
			break
		}
		select {
		case out <- sig:
		case <-ctx.Done():
			parseErr = ctx.Err()
		}
		if parseErr != nil {
			break
		}
	}
	if parseErr != nil || sc.Err() != nil {
		_, _ = io.Copy(io.Discard, stdout) // let the process finish
	}
	waitErr := cmd.Wait()

	var stats Stats
	if lines := bytes.Split(bytes.TrimSpace(stderr.Bytes()), []byte("\n")); len(lines) > 0 {
		_ = json.Unmarshal(lines[len(lines)-1], &stats)
	}
	switch {
	case parseErr != nil:
		return stats, parseErr
	case sc.Err() != nil:
		return stats, sc.Err()
	case waitErr != nil:
		return stats, fmt.Errorf("decoder exited abnormally: %w (stderr: %s)", waitErr, bytes.TrimSpace(stderr.Bytes()))
	}
	return stats, nil
}
