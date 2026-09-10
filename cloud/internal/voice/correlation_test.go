package voice

import (
	"bytes"
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
	"github.com/digital-dream-labs/vector-cloud/internal/voice/stream"
)

func TestStreamIdentityRejectsRetiredWorkers(t *testing.T) {
	current, prior := &stream.Streamer{}, &stream.Streamer{}
	for _, id := range []uint32{0, 42} {
		if !isCurrentStream(&strmReceiver{stream: current, streamID: id}, current, id) {
			t.Fatal("current worker rejected")
		}
		if isCurrentStream(&strmReceiver{stream: prior, streamID: id}, current, id) ||
			isCurrentStream(&strmReceiver{stream: current, streamID: id + 1}, current, id) ||
			isCurrentStream(&strmReceiver{}, nil, id) {
			t.Fatal("retired or incorrectly tagged worker accepted")
		}
	}
}

func TestCorrelatedCloudProtocolRoundTrip(t *testing.T) {
	const id = uint32(0xfedcba98)
	messages := []*cloud.Message{
		cloud.NewMessageWithHotword(&cloud.Hotword{StreamId: id}),
		cloud.NewMessageWithAudio(&cloud.AudioData{Data: []int16{1, -1}, StreamId: id}),
		cloud.NewMessageWithAudioDone(&cloud.StreamIdentifier{StreamId: id}),
		cloud.NewMessageWithCancelStream(&cloud.StreamIdentifier{StreamId: id}),
		cloud.NewMessageWithStopSignal(&cloud.StreamIdentifier{StreamId: id}),
		cloud.NewMessageWithStreamClosed(&cloud.StreamIdentifier{StreamId: id}),
		cloud.NewMessageWithStreamOpen(&cloud.StreamOpen{Session: "session", StreamId: id}),
		cloud.NewMessageWithResult(&cloud.IntentResult{Intent: "silence", StreamId: id}),
		cloud.NewMessageWithError(&cloud.IntentError{Error: cloud.ErrorType_Timeout, StreamId: id}),
		cloud.NewMessageWithResponseAudioStart(&cloud.ResponseAudioStart{StreamId: id}),
		cloud.NewMessageWithResponseAudioChunk(&cloud.ResponseAudioChunk{Data: []byte{1, 2}, StreamId: id}),
		cloud.NewMessageWithResponseAudioEnd(&cloud.ResponseAudioEnd{StreamId: id}),
		cloud.NewMessageWithResponseAudioError(&cloud.ResponseAudioError{StreamId: id}),
	}
	for _, msg := range messages {
		var packed, repacked bytes.Buffer
		if err := msg.Pack(&packed); err != nil {
			t.Fatal(err)
		}
		original := append([]byte(nil), packed.Bytes()...)
		var decoded cloud.Message
		if err := decoded.Unpack(&packed); err != nil {
			t.Fatal(err)
		}
		if err := decoded.Pack(&repacked); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(original, repacked.Bytes()) {
			t.Fatalf("identity roundtrip changed %v", msg.Tag())
		}
	}
}

func TestCloudAudioCorrelatesEveryLifecycleMessage(t *testing.T) {
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.beginAudioOwner(37)
	owner := p.getAudioOwner(nil)
	p.writeCloudAudio(cloud.NewMessageWithResponseAudioStart(&cloud.ResponseAudioStart{}), owner)
	p.streamCloudAudio("response", bytes.NewReader([]byte{1, 2}), owner)
	p.writeCloudAudio(cloud.NewMessageWithResponseAudioEnd(&cloud.ResponseAudioEnd{}), owner)
	p.sendCloudAudioError("response", errors.New("failure"), owner)
	msgs := collector.snapshot()
	if len(msgs) != 4 || msgs[0].GetResponseAudioStart().StreamId != 37 ||
		msgs[1].GetResponseAudioChunk().StreamId != 37 ||
		msgs[2].GetResponseAudioEnd().StreamId != 37 ||
		msgs[3].GetResponseAudioError().StreamId != 37 {
		t.Fatal("response audio lost its stream identity")
	}
	p.cancelAudioOwner(36)
	p.sendCloudAudioError("response", errors.New("still owned"), owner)
	if len(collector.snapshot()) != 5 {
		t.Fatal("stale cancellation stopped newer audio")
	}
	p.cancelAudioOwner(37)
	p.sendCloudAudioError("response", errors.New("late"), owner)
	if len(collector.snapshot()) != 5 {
		t.Fatal("cancelled worker emitted late audio error")
	}
}

func TestLateLegacyHTTPWorkerCannotWriteToReplacement(t *testing.T) {
	started, release, finished := make(chan struct{}), make(chan struct{}), make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		close(started)
		<-release
		w.Write([]byte{1, 2})
	}))
	defer srv.Close()
	collector := &collectSender{}
	p := &Process{intents: []MsgSender{collector}}
	p.beginAudioOwner(0)
	owner := p.getAudioOwner(nil)
	go func() {
		p.sendCloudAudio(srv.URL, "old-response", "audio", "", owner)
		close(finished)
	}()
	<-started
	p.beginAudioOwner(0)
	close(release)
	<-finished
	if len(collector.snapshot()) != 0 {
		t.Fatal("retired legacy worker emitted audio into replacement stream")
	}
}

func TestDelayedCloudAudioWorkerRetainsCancelledContext(t *testing.T) {
	requests := make(chan struct{}, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests <- struct{}{}
		w.Write([]byte{1, 2})
	}))
	defer srv.Close()
	for _, id := range []uint32{0, 42} {
		collector := &collectSender{}
		p := &Process{intents: []MsgSender{collector}}
		if id != 0 {
			p.beginAudioOwner(id)
		}
		owner := p.getAudioOwner(nil)
		p.beginAudioOwner(id)
		p.sendCloudAudio(srv.URL, "delayed", "audio", "", owner)
		if owner.ctx.Err() != context.Canceled {
			t.Fatal("retired snapshot did not retain its cancelled context")
		}
		select {
		case <-requests:
			t.Fatal("delayed worker started HTTP using the replacement owner's context")
		default:
		}
		p.writeCloudAudio(cloud.NewMessageWithResponseAudioStart(&cloud.ResponseAudioStart{}), owner)
		p.streamCloudAudio("late", bytes.NewReader([]byte{1, 2}), owner)
		p.writeCloudAudio(cloud.NewMessageWithResponseAudioEnd(&cloud.ResponseAudioEnd{}), owner)
		p.sendCloudAudioError("late", errors.New("late failure"), owner)
		if len(collector.snapshot()) != 0 {
			t.Fatal("retired worker emitted late audio lifecycle messages")
		}
		p.cancelAudioOwner(id)
	}
}

func TestCancelStreamStopsHTTPWithoutActiveASR(t *testing.T) {
	request := make(chan context.Context, 1)
	release := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		request <- r.Context()
		select {
		case <-r.Context().Done():
		case <-release:
		}
	}))
	defer srv.Close()
	defer close(release)
	collector := &collectSender{sent: make(chan *cloud.Message, 8)}
	p := &Process{msg: make(chan messageEvent), intents: []MsgSender{collector}}
	p.beginAudioOwner(42)
	owner := p.getAudioOwner(nil)
	finished := make(chan struct{})
	go func() {
		p.sendCloudAudio(srv.URL, "answer", "audio", "", owner)
		close(finished)
	}()
	var requestCtx context.Context
	select {
	case requestCtx = <-request:
	case <-time.After(2 * time.Second):
		t.Fatal("HTTP request did not start")
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		p.Run(ctx) // No microphone/ASR stream remains, but answer audio is active.
		close(done)
	}()
	defer func() {
		cancel()
		awaitCloudAudio(t, done)
	}()
	sendCancel := func(id uint32) {
		t.Helper()
		select {
		case p.msg <- messageEvent{msg: cloud.NewMessageWithCancelStream(&cloud.StreamIdentifier{StreamId: id})}:
		case <-time.After(2 * time.Second):
			t.Fatal("process did not receive CancelStream")
		}
		select {
		case msg := <-collector.sent:
			if msg.GetStreamClosed() == nil || msg.GetStreamClosed().StreamId != id {
				t.Fatal("CancelStream did not complete the matching stream")
			}
		case <-time.After(2 * time.Second):
			t.Fatal("process did not handle CancelStream")
		}
	}
	sendCancel(41)
	if owner.ctx.Err() != nil {
		t.Fatal("stale stream cancellation stopped the newer HTTP request")
	}
	select {
	case <-requestCtx.Done():
		t.Fatal("stale stream cancellation reached the newer HTTP server")
	default:
	}
	sendCancel(42)
	awaitCloudAudio(t, requestCtx.Done())
	awaitCloudAudio(t, finished)
	if len(collector.snapshot()) != 2 {
		t.Fatal("cancelled HTTP worker emitted late lifecycle messages")
	}
}

func TestInvalidStartReturnsCorrelatedTerminalSignals(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	mic, engine := &collectSender{}, &collectSender{}
	p := &Process{
		msg:       make(chan messageEvent),
		receivers: []*Receiver{{writer: mic}},
		intents:   []MsgSender{engine},
	}
	done := make(chan struct{})
	go func() {
		p.Run(ctx)
		close(done)
	}()
	p.msg <- messageEvent{msg: cloud.NewMessageWithHotword(&cloud.Hotword{
		Mode: cloud.StreamType(255), StreamId: 73,
	})}
	deadline := time.Now().Add(time.Second)
	for len(engine.snapshot()) < 2 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	cancel()
	<-done
	micMsgs, engineMsgs := mic.snapshot(), engine.snapshot()
	if len(micMsgs) != 1 || micMsgs[0].GetStopSignal().StreamId != 73 ||
		len(engineMsgs) != 2 || engineMsgs[0].GetError().StreamId != 73 ||
		engineMsgs[1].GetStreamClosed().StreamId != 73 {
		t.Fatal("rejected start must stop and complete the same stream")
	}
}

func TestRetiredReceiverCallbacksDoNotBlockOrPanic(t *testing.T) {
	r := &strmReceiver{
		intent: make(chan cloudIntent), err: make(chan cloudError),
		open: make(chan cloudOpen), connection: make(chan cloudConnCheck),
		done: make(chan struct{}),
	}
	r.Close()
	finished := make(chan struct{})
	go func() {
		r.OnIntent(&cloud.IntentResult{})
		r.OnError(cloud.ErrorType_Timeout, errors.New("late"))
		r.OnStreamOpen("late")
		r.OnConnectionResult(&cloud.ConnectionResult{})
		close(finished)
	}()
	select {
	case <-finished:
	case <-time.After(time.Second):
		t.Fatal("retired callback blocked after process shutdown")
	}
}
