// Package pb implements the protobuf wire format for proto/buswhisperer.proto.
//
// The schema is tiny and stable, so the gateway carries a small hand-written
// codec instead of generated code: no protoc toolchain, no module downloads.
// The Python harness decodes the output with an independent implementation.
package pb

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
)

// Signal mirrors buswhisperer.v1.Signal.
type Signal struct {
	TimestampUs   uint64
	SourceAddress uint32
	PGN           uint32
	Name          string
	Unit          string
	Value         float64
	Text          string
}

// Batch mirrors buswhisperer.v1.SignalBatch.
type Batch struct {
	Sequence uint64
	Signals  []Signal
}

const (
	wireVarint  = 0
	wireFixed64 = 1
	wireBytes   = 2
	wireFixed32 = 5
)

// MaxMessageSize bounds what the decoder accepts for a single message.
const MaxMessageSize = 64 << 20

func appendTag(b []byte, field, wire int) []byte {
	return binary.AppendUvarint(b, uint64(field)<<3|uint64(wire))
}

func appendString(b []byte, field int, s string) []byte {
	if s == "" {
		return b
	}
	b = appendTag(b, field, wireBytes)
	b = binary.AppendUvarint(b, uint64(len(s)))
	return append(b, s...)
}

func appendUint(b []byte, field int, v uint64) []byte {
	if v == 0 {
		return b
	}
	b = appendTag(b, field, wireVarint)
	return binary.AppendUvarint(b, v)
}

// AppendMarshal appends the encoding of s to b.
func (s *Signal) AppendMarshal(b []byte) []byte {
	b = appendUint(b, 1, s.TimestampUs)
	b = appendUint(b, 2, uint64(s.SourceAddress))
	b = appendUint(b, 3, uint64(s.PGN))
	b = appendString(b, 4, s.Name)
	b = appendString(b, 5, s.Unit)
	if s.Value != 0 || math.Signbit(s.Value) {
		b = appendTag(b, 6, wireFixed64)
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(s.Value))
	}
	return appendString(b, 7, s.Text)
}

// Marshal encodes the batch.
func (m *Batch) Marshal() []byte {
	var b []byte
	b = appendUint(b, 1, m.Sequence)
	var tmp []byte
	for i := range m.Signals {
		tmp = m.Signals[i].AppendMarshal(tmp[:0])
		b = appendTag(b, 2, wireBytes)
		b = binary.AppendUvarint(b, uint64(len(tmp)))
		b = append(b, tmp...)
	}
	return b
}

// MarshalDelimited encodes the batch with a varint length prefix.
func (m *Batch) MarshalDelimited() []byte {
	body := m.Marshal()
	out := binary.AppendUvarint(make([]byte, 0, len(body)+binary.MaxVarintLen64), uint64(len(body)))
	return append(out, body...)
}

var errTruncated = errors.New("pb: truncated message")

type reader struct {
	b []byte
}

func (r *reader) varint() (uint64, error) {
	v, n := binary.Uvarint(r.b)
	if n <= 0 {
		return 0, errTruncated
	}
	r.b = r.b[n:]
	return v, nil
}

func (r *reader) bytes() ([]byte, error) {
	n, err := r.varint()
	if err != nil {
		return nil, err
	}
	if n > uint64(len(r.b)) {
		return nil, errTruncated
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v, nil
}

func (r *reader) fixed(n int) ([]byte, error) {
	if len(r.b) < n {
		return nil, errTruncated
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v, nil
}

// skip discards a field of the given wire type (forward compatibility).
func (r *reader) skip(wire uint64) error {
	var err error
	switch wire {
	case wireVarint:
		_, err = r.varint()
	case wireFixed64:
		_, err = r.fixed(8)
	case wireBytes:
		_, err = r.bytes()
	case wireFixed32:
		_, err = r.fixed(4)
	default:
		err = fmt.Errorf("pb: unsupported wire type %d", wire)
	}
	return err
}

func unmarshalSignal(data []byte) (Signal, error) {
	var s Signal
	r := reader{data}
	for len(r.b) > 0 {
		tag, err := r.varint()
		if err != nil {
			return s, err
		}
		field, wire := tag>>3, tag&7
		switch {
		case field == 1 && wire == wireVarint:
			s.TimestampUs, err = r.varint()
		case field == 2 && wire == wireVarint:
			var v uint64
			v, err = r.varint()
			s.SourceAddress = uint32(v)
		case field == 3 && wire == wireVarint:
			var v uint64
			v, err = r.varint()
			s.PGN = uint32(v)
		case field == 4 && wire == wireBytes:
			var v []byte
			v, err = r.bytes()
			s.Name = string(v)
		case field == 5 && wire == wireBytes:
			var v []byte
			v, err = r.bytes()
			s.Unit = string(v)
		case field == 6 && wire == wireFixed64:
			var v []byte
			v, err = r.fixed(8)
			if err == nil {
				s.Value = math.Float64frombits(binary.LittleEndian.Uint64(v))
			}
		case field == 7 && wire == wireBytes:
			var v []byte
			v, err = r.bytes()
			s.Text = string(v)
		default:
			err = r.skip(wire)
		}
		if err != nil {
			return s, err
		}
	}
	return s, nil
}

// Unmarshal decodes a SignalBatch.
func Unmarshal(data []byte) (*Batch, error) {
	m := &Batch{}
	r := reader{data}
	for len(r.b) > 0 {
		tag, err := r.varint()
		if err != nil {
			return nil, err
		}
		field, wire := tag>>3, tag&7
		switch {
		case field == 1 && wire == wireVarint:
			m.Sequence, err = r.varint()
		case field == 2 && wire == wireBytes:
			var v []byte
			if v, err = r.bytes(); err == nil {
				var s Signal
				if s, err = unmarshalSignal(v); err == nil {
					m.Signals = append(m.Signals, s)
				}
			}
		default:
			err = r.skip(wire)
		}
		if err != nil {
			return nil, err
		}
	}
	return m, nil
}

// ReadDelimited reads the next length-delimited batch. It returns io.EOF at a
// clean end of stream.
func ReadDelimited(br *bufio.Reader) (*Batch, error) {
	n, err := binary.ReadUvarint(br)
	if err != nil {
		return nil, err // io.EOF when no bytes were read
	}
	if n > MaxMessageSize {
		return nil, fmt.Errorf("pb: message of %d bytes exceeds limit", n)
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(br, buf); err != nil {
		return nil, errTruncated
	}
	return Unmarshal(buf)
}
