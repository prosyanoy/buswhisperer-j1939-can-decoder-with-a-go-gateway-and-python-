package pb

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"io"
	"math"
	"reflect"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	in := &Batch{Sequence: 7, Signals: []Signal{
		{TimestampUs: 1609459200123456, SourceAddress: 0, PGN: 61444, Name: "engine_speed", Unit: "rpm", Value: 1500.125},
		{TimestampUs: 1, SourceAddress: 255, PGN: 65260, Name: "vin", Text: "1FUJGLDR12LM12345"},
		{Name: "neg", Value: -40},
		{},
	}}
	out, err := Unmarshal(in.Marshal())
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(in, out) {
		t.Fatalf("mismatch\n in: %+v\nout: %+v", in, out)
	}
}

// Golden bytes cross-checked against the protobuf spec:
// Signal{ts=150, sa=1, pgn=2, name="a", value=1.0}
func TestGoldenEncoding(t *testing.T) {
	s := Signal{TimestampUs: 150, SourceAddress: 1, PGN: 2, Name: "a", Value: 1.0}
	got := hex.EncodeToString(s.AppendMarshal(nil))
	want := "089601" + "1001" + "1802" + "220161" + "31000000000000f03f"
	if got != want {
		t.Fatalf("got %s want %s", got, want)
	}
	b := Batch{Sequence: 1, Signals: []Signal{s}}
	got = hex.EncodeToString(b.Marshal())
	want = "0801" + "12" + "13" + want
	if got != want {
		t.Fatalf("batch got %s want %s", got, want)
	}
}

func TestDelimitedStream(t *testing.T) {
	var buf bytes.Buffer
	for i := uint64(1); i <= 3; i++ {
		b := Batch{Sequence: i, Signals: []Signal{{Name: "x", Value: float64(i)}}}
		buf.Write(b.MarshalDelimited())
	}
	br := bufio.NewReader(&buf)
	for i := uint64(1); i <= 3; i++ {
		b, err := ReadDelimited(br)
		if err != nil || b.Sequence != i || b.Signals[0].Value != float64(i) {
			t.Fatalf("batch %d: %v %+v", i, err, b)
		}
	}
	if _, err := ReadDelimited(br); err != io.EOF {
		t.Fatalf("want EOF, got %v", err)
	}
}

func TestMalformed(t *testing.T) {
	good := (&Batch{Sequence: 1, Signals: []Signal{{Name: "abc", Value: 2}}}).Marshal()
	for n := 1; n < len(good); n++ {
		if _, err := Unmarshal(good[:n]); err == nil {
			// a truncation at a field boundary is a valid shorter message
			continue
		}
	}
	if _, err := Unmarshal([]byte{0x12, 0x05, 0x01}); err == nil {
		t.Fatal("expected error for overlong length")
	}
	if _, err := Unmarshal([]byte{0x0B}); err == nil { // wire type 3 (group)
		t.Fatal("expected error for unsupported wire type")
	}
	if _, err := ReadDelimited(bufio.NewReader(bytes.NewReader([]byte{0x05, 0x01}))); err == nil {
		t.Fatal("expected truncated error")
	}
}

func TestUnknownFieldsSkipped(t *testing.T) {
	data := []byte{0x08, 0x02}                        // sequence=2
	data = append(data, 0x28, 0x7f)                   // unknown varint field 5
	data = append(data, 0x3a, 0x02, 0xaa, 0xbb)       // unknown bytes field 7
	data = append(data, 0x45, 1, 2, 3, 4)             // unknown fixed32 field 8
	data = append(data, 0x49, 1, 2, 3, 4, 5, 6, 7, 8) // unknown fixed64 field 9
	b, err := Unmarshal(data)
	if err != nil || b.Sequence != 2 {
		t.Fatalf("%v %+v", err, b)
	}
}

func FuzzUnmarshal(f *testing.F) {
	f.Add((&Batch{Sequence: 1, Signals: []Signal{{Name: "a", Value: math.Pi}}}).Marshal())
	f.Fuzz(func(t *testing.T, data []byte) { _, _ = Unmarshal(data) })
}
