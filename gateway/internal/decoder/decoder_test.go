package decoder

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/buswhisperer/gateway/internal/pb"
)

func TestParseLine(t *testing.T) {
	s, err := ParseLine([]byte(`{"ts_us":5,"sa":3,"pgn":61444,"name":"engine_speed","unit":"rpm","value":1500.125}`))
	if err != nil || s.Value != 1500.125 || s.PGN != 61444 || s.SourceAddress != 3 || s.TimestampUs != 5 {
		t.Fatalf("%v %+v", err, s)
	}
	s, err = ParseLine([]byte(`{"ts_us":5,"sa":3,"pgn":65260,"name":"vin","unit":"","text":"ABC"}`))
	if err != nil || s.Text != "ABC" {
		t.Fatalf("%v %+v", err, s)
	}
	for _, bad := range []string{``, `{`, `{"pgn":1}`, `[]`} {
		if _, err := ParseLine([]byte(bad)); err == nil {
			t.Errorf("expected error for %q", bad)
		}
	}
}

// fakeDecoder writes a shell script that mimics bw_decode.
func fakeDecoder(t *testing.T, body string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "fake")
	if err := os.WriteFile(p, []byte("#!/bin/sh\n"+body), 0o755); err != nil {
		t.Fatal(err)
	}
	return p
}

func collect(t *testing.T, bin string) ([]pb.Signal, Stats, error) {
	t.Helper()
	ch := make(chan pb.Signal, 100)
	stats, err := Run(context.Background(), bin, strings.NewReader("ignored\n"), ch)
	close(ch)
	var got []pb.Signal
	for s := range ch {
		got = append(got, s)
	}
	return got, stats, err
}

func TestRun(t *testing.T) {
	bin := fakeDecoder(t, `cat >/dev/null
echo '{"ts_us":1,"sa":0,"pgn":65262,"name":"coolant_temp","unit":"degC","value":90}'
echo '{"ts_us":2,"sa":0,"pgn":65262,"name":"coolant_temp","unit":"degC","value":91}'
echo '{"frames":2,"tp_timeouts":1}' >&2
`)
	got, stats, err := collect(t, bin)
	if err != nil || len(got) != 2 || got[1].Value != 91 {
		t.Fatalf("%v %+v", err, got)
	}
	if stats["frames"] != 2 || stats["tp_timeouts"] != 1 {
		t.Fatalf("stats %v", stats)
	}
}

func TestRunFailures(t *testing.T) {
	if _, _, err := collect(t, "/nonexistent/bw_decode"); err == nil {
		t.Error("expected start error")
	}
	if _, _, err := collect(t, fakeDecoder(t, "cat >/dev/null\nexit 3\n")); err == nil {
		t.Error("expected exit error")
	}
	if _, _, err := collect(t, fakeDecoder(t, "cat >/dev/null\necho garbage\n")); err == nil {
		t.Error("expected parse error")
	}
}
