package stream

import (
	"context"
	"encoding/json"
	"net"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/digital-dream-labs/api-clients/chipper"
	pb "github.com/digital-dream-labs/api/go/chipperpb"
	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
	"github.com/gwatts/rootcerts"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/test/bufconn"
)

type routeServer struct {
	pb.UnimplementedChipperGrpcServer
	requests chan string
}

func (s *routeServer) StreamingKnowledgeGraph(stream pb.ChipperGrpc_StreamingKnowledgeGraphServer) error {
	request, err := stream.Recv()
	if err != nil {
		return err
	}
	s.requests <- "KG:" + request.Session
	return stream.Send(&pb.KnowledgeGraphResponse{QueryText: "question", SpokenText: "answer"})
}

func (s *routeServer) StreamingIntentGraph(stream pb.ChipperGrpc_StreamingIntentGraphServer) error {
	request, err := stream.Recv()
	if err != nil {
		return err
	}
	s.requests <- "IG:" + request.Session
	return stream.Send(&pb.IntentGraphResponse{IsFinal: true})
}

func TestFollowUpActualChipperRPCSelection(t *testing.T) {
	// Chipper's request metadata requires TLS even on an in-memory connection.
	// Trust a test certificate only for this serial test, then restore the pool.
	tlsServer := httptest.NewTLSServer(nil)
	tlsConfig := tlsServer.TLS.Clone()
	certificate := tlsServer.Certificate()
	tlsServer.Close()
	pool := rootcerts.ServerCertPool()
	previousPool := pool.Clone()
	pool.AddCert(certificate)
	defer func() { *pool = *previousPool }()
	listener := bufconn.Listen(1024 * 1024)
	server := grpc.NewServer(grpc.Creds(credentials.NewTLS(tlsConfig)))
	service := &routeServer{requests: make(chan string, 2)}
	pb.RegisterChipperGrpcServer(server, service)
	go server.Serve(listener)
	defer server.Stop()
	defer listener.Close()
	previous := platformOpts
	platformOpts = []chipper.ConnOpt{
		chipper.WithGrpcOptions(grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) {
			return listener.Dial()
		})),
	}
	defer func() { platformOpts = previous }()
	for _, tc := range []struct {
		name string
		opt  Option
	}{
		{"IG", WithIntentGraphOptions(chipper.IntentGraphOpts{}, cloud.StreamType_Normal)},
		{"KG", WithKnowledgeGraphOptions(chipper.KGOpts{})},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			strm := &Streamer{}
			tc.opt(&strm.opts)
			strm.opts.url = "127.0.0.1:443"
			conn, request, err := strm.openChipperStream(ctx, nil, tc.name+"-fresh-session")
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			defer request.Close()
			if err := request.SendAudio([]byte{0, 0}); err != nil {
				t.Fatal(err)
			}
			select {
			case got := <-service.requests:
				if got != tc.name+":"+tc.name+"-fresh-session" {
					t.Fatalf("wrong RPC/session: %s", got)
				}
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			if _, err := request.WaitForResponse(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestFollowUpKGAdapterPreservesTranscriptAndEmptyResults(t *testing.T) {
	t.Setenv("VECTOR_KG_TTS_URL", "http://127.0.0.1/unused")
	for _, tc := range []struct{ query, answer string }{
		{"What is its size?", "It is large."},
		{"", "Spurious answer"},
		{" \n", ""},
		{"stop", "An answer that must not be played"},
		{"question about stop", "Stop!"},
		{"question", ""},
	} {
		receiver := &kgReceiver{}
		sendKGResponse(&chipper.KnowledgeGraphResponse{
			QueryText: tc.query, SpokenText: tc.answer, AudioId: "published-audio",
		}, receiver, false)
		if receiver.intent == nil || receiver.intent.Intent != "intent_knowledge_response_extend" {
			t.Fatalf("KG adapter unexpectedly dispatched a bypass: %+v", receiver.intent)
		}
		var params map[string]interface{}
		if err := json.Unmarshal([]byte(receiver.intent.Parameters), &params); err != nil {
			t.Fatal(err)
		}
		if params["query_text"] != tc.query || params["answer"] != tc.answer ||
			params["audio_id"] != "published-audio" || params["response_id"] == "" {
			t.Fatalf("lost transcript or answer correlation: %+v", params)
		}
	}
}
