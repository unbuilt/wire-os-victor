package voice

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"sync"
	"testing"
	"testing/iotest"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
	"github.com/digital-dream-labs/vector-cloud/internal/config"
)

func TestKnowledgeGraphRequestBudget(t *testing.T) {
	previous := config.Env
	t.Cleanup(func() { config.Env = previous })
	config.Env = config.URLs{Chipper: "lycopod.local:443"}
	t.Setenv("VECTOR_KG_TTS_URL", "")
	p := &Process{}
	if got := p.defaultChipperOptions(cloud.StreamType_KnowledgeGraph).Timeout; got != 60*time.Second {
		t.Fatalf("cloud KG timeout = %v", got)
	}
	if got := p.defaultChipperOptions(cloud.StreamType_Normal).Timeout; got != DefaultTimeout {
		t.Fatalf("normal intent timeout changed: %v", got)
	}
	t.Setenv("VECTOR_KG_TTS_URL", "off")
	if got := p.defaultChipperOptions(cloud.StreamType_KnowledgeGraph).Timeout; got != DefaultTimeout {
		t.Fatalf("disabled cloud KG timeout changed: %v", got)
	}
}

func TestCloudAudioAbortedHTTPBodyEmitsError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(makePCM(16000))
		w.(http.Flusher).Flush()
		// Like a producer failure after headers/PCM have already been sent:
		// close without the final chunk instead of returning a successful EOF.
		conn, _, err := w.(http.Hijacker).Hijack()
		if err != nil {
			t.Error(err)
			return
		}
		conn.Close()
	}))
	defer srv.Close()

	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.sendCloudAudio(srv.URL, "aborted", "audio-id", "answer")
	msgs := collector.snapshot()
	if len(msgs) < 3 || msgs[len(msgs)-1].GetResponseAudioError() == nil {
		t.Fatal("partial HTTP failure must end with ResponseAudioError")
	}
	for _, msg := range msgs {
		if msg.GetResponseAudioEnd() != nil {
			t.Fatal("partial HTTP failure must not report successful completion")
		}
	}
}

func TestMaybeSendCloudAudioFetchesPublishedHandle(t *testing.T) {
	requests := make(chan string, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests <- r.Method + " " + r.URL.Path
		w.Write(makePCM(100))
	}))
	defer srv.Close()
	t.Setenv("VECTOR_KG_TTS_URL", srv.URL+"/v1/kg-audio")
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.maybeSendCloudAudio(&cloud.IntentResult{
		Intent:     "intent_knowledge_response_extend",
		Parameters: `{"answer":"The Sun is about 150 million kilometers away.","audio_id":"sun-answer","response_id":"kg-response","cloud_audio_available":true}`,
	})
	select {
	case request := <-requests:
		if request != "GET /v1/kg-audio/sun-answer" {
			t.Fatalf("unexpected fetch: %s", request)
		}
	case <-time.After(time.Second):
		t.Fatal("accepted KG intent did not start an audio fetch")
	}
}

// collectSender is a MsgSender that records every message it receives.
type collectSender struct {
	mu   sync.Mutex
	msgs []*cloud.Message
}

func (c *collectSender) Send(msg *cloud.Message) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.msgs = append(c.msgs, msg)
	return nil
}

func (c *collectSender) snapshot() []*cloud.Message {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make([]*cloud.Message, len(c.msgs))
	copy(out, c.msgs)
	return out
}

func makePCM(frames int) []byte {
	buf := make([]byte, frames*2)
	for i := 0; i < frames; i++ {
		binary.LittleEndian.PutUint16(buf[i*2:], uint16(i))
	}
	return buf
}

func TestSendCloudAudioStreamsChunks(t *testing.T) {
	// 2500 frames = 5000 bytes -> ceil(5000/1024) = 5 chunks
	pcm := makePCM(2500)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.WriteHeader(http.StatusOK)
		w.Write(pcm)
	}))
	defer srv.Close()

	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}

	p.sendCloudAudio(srv.URL, "resp-1", "", "hello world")

	msgs := collector.snapshot()
	if len(msgs) < 3 {
		t.Fatalf("expected start+chunks+end, got %d messages", len(msgs))
	}

	// First message: Start with correct metadata.
	start := msgs[0].GetResponseAudioStart()
	if start == nil {
		t.Fatalf("first message is not ResponseAudioStart: %v", msgs[0].Tag())
	}
	if start.ResponseId != "resp-1" {
		t.Errorf("start responseId = %q, want resp-1", start.ResponseId)
	}
	if start.SampleRateHz != cloudAudioSampleRate || start.Channels != cloudAudioChannels {
		t.Errorf("unexpected format: %d Hz, %d ch", start.SampleRateHz, start.Channels)
	}
	if start.Encoding != cloud.ResponseAudioEncoding_Pcm16Le {
		t.Errorf("unexpected encoding %v", start.Encoding)
	}
	// The answer is produced incrementally, so its length is not known up front.
	if start.TotalFramesKnown {
		t.Errorf("totalFramesKnown = true, want false for a streamed answer")
	}

	// Last message: End with the correct final sequence number.
	end := msgs[len(msgs)-1].GetResponseAudioEnd()
	if end == nil {
		t.Fatalf("last message is not ResponseAudioEnd: %v", msgs[len(msgs)-1].Tag())
	}

	// Middle messages: chunks whose concatenated data reconstruct the PCM.
	var reassembled []byte
	var seq uint32
	for _, m := range msgs[1 : len(msgs)-1] {
		chunk := m.GetResponseAudioChunk()
		if chunk == nil {
			t.Fatalf("expected chunk, got %v", m.Tag())
		}
		if chunk.SequenceNumber != seq {
			t.Errorf("chunk seq = %d, want %d", chunk.SequenceNumber, seq)
		}
		if len(chunk.Data) > cloudAudioChunkBytes {
			t.Errorf("chunk too big: %d bytes", len(chunk.Data))
		}
		reassembled = append(reassembled, chunk.Data...)
		seq++
	}
	if end.FinalSequenceNumber != seq {
		t.Errorf("final seq = %d, want %d", end.FinalSequenceNumber, seq)
	}
	if string(reassembled) != string(pcm) {
		t.Errorf("reassembled PCM does not match original (%d vs %d bytes)", len(reassembled), len(pcm))
	}
}

func TestSendCloudAudioErrorOnBadStatus(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer srv.Close()

	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.sendCloudAudio(srv.URL, "resp-err", "", "boom")

	msgs := collector.snapshot()
	if len(msgs) != 1 {
		t.Fatalf("expected a single error message, got %d", len(msgs))
	}
	e := msgs[0].GetResponseAudioError()
	if e == nil {
		t.Fatalf("expected ResponseAudioError, got %v", msgs[0].Tag())
	}
	if e.ResponseId != "resp-err" {
		t.Errorf("error responseId = %q, want resp-err", e.ResponseId)
	}
}

func TestCloudAudioFetchFailuresAreNotQuietInterruptions(t *testing.T) {
	for _, status := range []int{401, 403, 404, 500, 0} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(status)
			}))
			defer srv.Close()
			if status == 0 {
				srv.Close() // Connection establishment failure, before any answer body.
			}
			collector := &collectSender{}
			p := &Process{intents: []MsgSender{collector}}
			p.sendCloudAudio(srv.URL, "answer", "audio", "answer")
			msgs := collector.snapshot()
			if len(msgs) != 1 || msgs[0].GetResponseAudioError() == nil ||
				msgs[0].GetResponseAudioError().Error != cloud.ResponseAudioErrorType_Provider {
				t.Fatal("ordinary fetch/authentication errors must retain fallback handling")
			}
		})
	}
}

func TestMaybeSendCloudAudioDisabledIsNoop(t *testing.T) {
	os.Unsetenv("VECTOR_KG_TTS_URL")
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.maybeSendCloudAudio(&cloud.IntentResult{
		Intent:     "intent_knowledge_response_extend",
		Parameters: `{"answer":"hi","response_id":"x","cloud_audio_available":true}`,
	})
	if got := len(collector.snapshot()); got != 0 {
		t.Fatalf("expected no messages when feature disabled, got %d", got)
	}
}

func TestMaybeSendCloudAudioIgnoresUnavailable(t *testing.T) {
	os.Setenv("VECTOR_KG_TTS_URL", "http://example.invalid/tts")
	defer os.Unsetenv("VECTOR_KG_TTS_URL")
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	// cloud_audio_available=false -> no synthesis attempted.
	p.maybeSendCloudAudio(&cloud.IntentResult{
		Intent:     "intent_knowledge_response_extend",
		Parameters: `{"answer":"hi","response_id":"x","cloud_audio_available":false}`,
	})
	if got := len(collector.snapshot()); got != 0 {
		t.Fatalf("expected no messages when cloud audio unavailable, got %d", got)
	}
}

func TestSendCloudAudioFetchesByAudioID(t *testing.T) {
	pcm := makePCM(100)
	var gotPath, gotMethod string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		gotMethod = r.Method
		w.WriteHeader(http.StatusOK)
		w.Write(pcm)
	}))
	defer srv.Close()

	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	// The answer text is ignored when the server already produced the audio.
	p.sendCloudAudio(srv.URL+"/v1/kg-audio", "resp-id", "abc123", "unused text")

	if gotMethod != http.MethodGet {
		t.Errorf("method = %s, want GET", gotMethod)
	}
	if gotPath != "/v1/kg-audio/abc123" {
		t.Errorf("path = %s, want /v1/kg-audio/abc123", gotPath)
	}
	msgs := collector.snapshot()
	if msgs[len(msgs)-1].GetResponseAudioEnd() == nil {
		t.Fatalf("expected the stream to complete, last message is %v", msgs[len(msgs)-1].Tag())
	}
}

// The backend produces the answer sentence by sentence, so chunks must reach
// the engine as they arrive rather than after the whole answer is synthesized.
func TestSendCloudAudioForwardsChunksBeforeTheAnswerIsComplete(t *testing.T) {
	firstSentence := makePCM(2048) // 4096 bytes -> 4 full chunks
	secondSentence := makePCM(2048)
	release := make(chan struct{})

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		flusher, ok := w.(http.Flusher)
		if !ok {
			t.Error("test server does not support flushing")
			return
		}
		w.WriteHeader(http.StatusOK)
		w.Write(firstSentence)
		flusher.Flush()
		<-release
		w.Write(secondSentence)
		flusher.Flush()
	}))
	defer srv.Close()

	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}

	done := make(chan struct{})
	go func() {
		p.sendCloudAudio(srv.URL, "resp-live", "id-less", "")
		close(done)
	}()

	// Chunks for the first sentence must appear while the server is still
	// holding the body open for the second one.
	deadline := time.After(5 * time.Second)
	for {
		chunks := 0
		for _, m := range collector.snapshot() {
			if m.GetResponseAudioChunk() != nil {
				chunks++
			}
		}
		if chunks >= 4 {
			break
		}
		select {
		case <-deadline:
			t.Fatalf("only %d chunks arrived before the answer completed", chunks)
		case <-time.After(10 * time.Millisecond):
		}
	}

	for _, m := range collector.snapshot() {
		if m.GetResponseAudioEnd() != nil {
			t.Fatal("stream ended before the answer was complete")
		}
	}

	close(release)
	<-done

	msgs := collector.snapshot()
	var reassembled []byte
	for _, m := range msgs {
		if chunk := m.GetResponseAudioChunk(); chunk != nil {
			reassembled = append(reassembled, chunk.Data...)
		}
	}
	want := append(append([]byte{}, firstSentence...), secondSentence...)
	if string(reassembled) != string(want) {
		t.Errorf("reassembled %d bytes, want %d", len(reassembled), len(want))
	}
	if msgs[len(msgs)-1].GetResponseAudioEnd() == nil {
		t.Errorf("expected ResponseAudioEnd last, got %v", msgs[len(msgs)-1].Tag())
	}
}

// A chunk boundary must never split a 16-bit sample, even when the backend
// flushes an odd number of bytes.
func TestStreamCloudAudioKeepsFramesIntact(t *testing.T) {
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}

	pcm := makePCM(1000)
	seq, total, err := p.streamCloudAudio("resp-frames", iotest.OneByteReader(bytes.NewReader(pcm)))
	if err != nil {
		t.Fatalf("streamCloudAudio: %v", err)
	}
	if total != len(pcm) {
		t.Errorf("total = %d, want %d", total, len(pcm))
	}

	var reassembled []byte
	for _, m := range collector.snapshot() {
		chunk := m.GetResponseAudioChunk()
		if chunk == nil {
			t.Fatalf("unexpected message %v", m.Tag())
		}
		if len(chunk.Data)%2 != 0 {
			t.Errorf("chunk %d has an odd length %d, splitting a sample",
				chunk.SequenceNumber, len(chunk.Data))
		}
		reassembled = append(reassembled, chunk.Data...)
	}
	if uint32(len(collector.snapshot())) != seq {
		t.Errorf("emitted %d chunks but reported seq %d", len(collector.snapshot()), seq)
	}
	if string(reassembled) != string(pcm) {
		t.Errorf("reassembled PCM does not match original")
	}
}

func TestStreamCloudAudioRejectsTrailingOddByte(t *testing.T) {
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}

	pcm := append(makePCM(10), 0xFF)
	if _, _, err := p.streamCloudAudio("resp-odd", bytes.NewReader(pcm)); err == nil {
		t.Fatal("incomplete sample must not be reported as successful audio")
	}
	var reassembled []byte
	for _, m := range collector.snapshot() {
		reassembled = append(reassembled, m.GetResponseAudioChunk().Data...)
	}
	if len(reassembled) != 0 {
		t.Errorf("forwarded incomplete final buffer of %d bytes", len(reassembled))
	}
}

func answerFrame(kind byte, payload []byte) []byte {
	header := make([]byte, 5)
	header[0] = kind
	binary.BigEndian.PutUint32(header[1:], uint32(len(payload)))
	return append(header, payload...)
}

func TestFramedCloudAudioTerminalOutcomes(t *testing.T) {
	for _, tc := range []struct {
		name    string
		tail    []byte
		kind    cloud.ResponseAudioErrorType
		success bool
	}{
		{"end", answerFrame(1, nil), 0, true},
		{"cancelled", answerFrame(2, nil), cloud.ResponseAudioErrorType_Cancelled, false},
		{"connection", answerFrame(3, nil), cloud.ResponseAudioErrorType_Transport, false},
		{"provider", answerFrame(4, nil), cloud.ResponseAudioErrorType_Provider, false},
		{"http-eof-without-end", nil, cloud.ResponseAudioErrorType_Transport, false},
		{"truncated-terminal", []byte{1, 0}, cloud.ResponseAudioErrorType_Transport, false},
		{"truncated-pcm", answerFrame(0, []byte{1, 2})[:6], cloud.ResponseAudioErrorType_Transport, false},
		{"invalid-terminal", answerFrame(2, []byte{1}), cloud.ResponseAudioErrorType_InvalidFormat, false},
		{"unknown-frame", answerFrame(99, nil), cloud.ResponseAudioErrorType_InvalidFormat, false},
		{"trailing-data", append(answerFrame(1, nil), 42), cloud.ResponseAudioErrorType_InvalidFormat, false},
		{"oversized-frame", answerFrame(0, make([]byte, 1025)), cloud.ResponseAudioErrorType_InvalidFormat, false},
	} {
		for _, partial := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/partial=%t", tc.name, partial), func(t *testing.T) {
				srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
					if r.Header.Get("Accept") != cloudAudioFramedType {
						t.Error("framing not negotiated")
					}
					w.Header().Set("Content-Type", cloudAudioFramedType)
					if partial {
						w.Write(answerFrame(0, makePCM(512)))
					}
					w.Write(tc.tail)
				}))
				defer srv.Close()
				collector := &collectSender{}
				p := &Process{intents: []MsgSender{collector}}
				p.beginAudioOwner(42)
				p.sendCloudAudio(srv.URL, "answer", "audio", "partial answer")
				msgs := collector.snapshot()
				last := msgs[len(msgs)-1]
				if tc.success && partial {
					if last.GetResponseAudioEnd() == nil {
						t.Fatal("complete answer did not end")
					}
				} else {
					err := last.GetResponseAudioError()
					want := tc.kind
					if tc.success {
						want = cloud.ResponseAudioErrorType_Provider
					}
					if err == nil || err.Error != want || err.StreamId != 42 {
						t.Fatalf("wrong terminal: %v, want %v", last, want)
					}
					for _, msg := range msgs {
						if msg.GetResponseAudioEnd() != nil {
							t.Fatal("failed answer reported success")
						}
					}
				}
			})
		}
	}
}

func TestFramedCloudAudioForwardsBeforeLateAbort(t *testing.T) {
	release := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", cloudAudioFramedType)
		w.Write(answerFrame(0, makePCM(512)))
		w.(http.Flusher).Flush()
		<-release
		w.Write(answerFrame(2, nil))
	}))
	defer srv.Close()
	defer close(release)
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	done := make(chan struct{})
	go func() { p.sendCloudAudio(srv.URL, "answer", "audio", "first sentence"); close(done) }()
	deadline := time.Now().Add(2 * time.Second)
	for len(collector.snapshot()) < 2 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}

	msgs := collector.snapshot()
	if len(msgs) != 2 || msgs[1].GetResponseAudioChunk() == nil {
		t.Fatal("first sentence buffered until termination")
	}
	// Unblock without closing twice in the deferred failure cleanup.
	release <- struct{}{}
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("late abort did not terminate")
	}
	msgs = collector.snapshot()
	if len(msgs) != 3 || msgs[2].GetResponseAudioError().Error != cloud.ResponseAudioErrorType_Cancelled {
		t.Fatal("late abort must send Cancelled, never End")
	}
}

func TestFramedEndStillRequiresCompleteHTTPBody(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", cloudAudioFramedType)
		w.Write(answerFrame(0, makePCM(512)))
		w.Write(answerFrame(1, nil))
		w.(http.Flusher).Flush()
		conn, _, err := w.(http.Hijacker).Hijack()
		if err != nil {
			t.Error(err)
			return
		}
		conn.Close()
	}))
	defer srv.Close()
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.sendCloudAudio(srv.URL, "answer", "audio", "answer")
	msgs := collector.snapshot()
	last := msgs[len(msgs)-1].GetResponseAudioError()
	if last == nil || last.Error != cloud.ResponseAudioErrorType_Transport {
		t.Fatal("missing HTTP terminator must remain a transport failure even after END")
	}
	for _, msg := range msgs {
		if msg.GetResponseAudioEnd() != nil {
			t.Fatal("truncated HTTP transfer emitted successful completion")
		}
	}
}

func TestMaybeSendCloudAudioIgnoresMissingHandles(t *testing.T) {
	os.Setenv("VECTOR_KG_TTS_URL", "http://example.invalid/tts")
	defer os.Unsetenv("VECTOR_KG_TTS_URL")
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	// Advertised, but neither an audio id nor text to synthesize.
	p.maybeSendCloudAudio(&cloud.IntentResult{
		Intent:     "intent_knowledge_response_extend",
		Parameters: `{"answer":"","audio_id":"","response_id":"x","cloud_audio_available":true}`,
	})
	if got := len(collector.snapshot()); got != 0 {
		t.Fatalf("expected no messages without an audio handle, got %d", got)
	}
}

func TestJoinAudioURL(t *testing.T) {
	cases := []struct{ base, id, want string }{
		{"http://h:8080/v1/kg-audio", "abc", "http://h:8080/v1/kg-audio/abc"},
		{"http://h:8080/v1/kg-audio/", "abc", "http://h:8080/v1/kg-audio/abc"},
		{"http://h:8080/v1/kg-audio", "a b", "http://h:8080/v1/kg-audio/a%20b"},
	}
	for _, c := range cases {
		if got := joinAudioURL(c.base, c.id); got != c.want {
			t.Errorf("joinAudioURL(%q, %q) = %q, want %q", c.base, c.id, got, c.want)
		}
	}
}

func TestSendCloudAudioErrorOnBodyTooShortForASample(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte{0x01})
	}))
	defer srv.Close()

	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.sendCloudAudio(srv.URL, "resp-short", "", "hi")

	msgs := collector.snapshot()
	for _, m := range msgs {
		if m.GetResponseAudioEnd() != nil {
			t.Fatalf("a body with no complete sample must not report a successful end")
		}
	}
	if len(msgs) == 0 || msgs[len(msgs)-1].GetResponseAudioError() == nil {
		t.Fatalf("expected a ResponseAudioError, got %v", msgs)
	}
}
