package cloud

import (
	"bytes"
	"testing"
)

// packUnpack round-trips a Message through the wire format and returns the
// decoded copy, failing the test on any pack/unpack error.
func packUnpack(t *testing.T, msg *Message) *Message {
	t.Helper()
	var buf bytes.Buffer
	if err := msg.Pack(&buf); err != nil {
		t.Fatalf("Pack failed: %v", err)
	}
	out := &Message{}
	if err := out.Unpack(&buf); err != nil {
		t.Fatalf("Unpack failed: %v", err)
	}
	if buf.Len() != 0 {
		t.Errorf("Unpack left %d trailing bytes", buf.Len())
	}
	if out.Tag() != msg.Tag() {
		t.Fatalf("tag mismatch: got %d want %d", out.Tag(), msg.Tag())
	}
	return out
}

func TestResponseAudioStartRoundTrip(t *testing.T) {
	in := &ResponseAudioStart{
		ResponseId:       "resp-abc-123",
		Encoding:         ResponseAudioEncoding_Pcm16Le,
		SampleRateHz:     16000,
		Channels:         1,
		TotalFrames:      48000,
		TotalFramesKnown: true,
	}
	out := packUnpack(t, NewMessageWithResponseAudioStart(in)).GetResponseAudioStart()
	if out == nil {
		t.Fatal("GetResponseAudioStart returned nil")
	}
	if *out != *in {
		t.Errorf("round-trip mismatch:\n got %+v\nwant %+v", *out, *in)
	}
}

func TestResponseAudioChunkRoundTrip(t *testing.T) {
	payload := make([]uint8, 1024) // max chunk payload per the audio contract
	for i := range payload {
		payload[i] = uint8(i)
	}
	in := &ResponseAudioChunk{
		ResponseId:     "resp-abc-123",
		SequenceNumber: 7,
		Data:           payload,
	}
	msg := NewMessageWithResponseAudioChunk(in)

	// Packed size must stay below the 4096-byte engine receive buffer.
	if sz := msg.Size(); sz >= 4096 {
		t.Fatalf("packed chunk size %d must stay below the 4096-byte engine buffer", sz)
	}

	out := packUnpack(t, msg).GetResponseAudioChunk()
	if out == nil {
		t.Fatal("GetResponseAudioChunk returned nil")
	}
	if out.ResponseId != in.ResponseId || out.SequenceNumber != in.SequenceNumber {
		t.Errorf("header mismatch: got %+v want %+v", out, in)
	}
	if !bytes.Equal(out.Data, in.Data) {
		t.Errorf("payload mismatch: got %d bytes want %d bytes", len(out.Data), len(in.Data))
	}
}

func TestResponseAudioEndRoundTrip(t *testing.T) {
	in := &ResponseAudioEnd{ResponseId: "resp-abc-123", FinalSequenceNumber: 42}
	out := packUnpack(t, NewMessageWithResponseAudioEnd(in)).GetResponseAudioEnd()
	if out == nil {
		t.Fatal("GetResponseAudioEnd returned nil")
	}
	if *out != *in {
		t.Errorf("round-trip mismatch: got %+v want %+v", *out, *in)
	}
}

func TestResponseAudioErrorRoundTrip(t *testing.T) {
	in := &ResponseAudioError{
		ResponseId: "resp-abc-123",
		Error:      ResponseAudioErrorType_Timeout,
		Details:    "provider timed out",
	}
	out := packUnpack(t, NewMessageWithResponseAudioError(in)).GetResponseAudioError()
	if out == nil {
		t.Fatal("GetResponseAudioError returned nil")
	}
	if *out != *in {
		t.Errorf("round-trip mismatch: got %+v want %+v", *out, *in)
	}
}
