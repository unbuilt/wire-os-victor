package stream

import (
	"encoding/json"
	"testing"

	"github.com/digital-dream-labs/api-clients/chipper"
	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
)

type kgReceiver struct {
	intent *cloud.IntentResult
}

func (r *kgReceiver) OnError(cloud.ErrorType, error)             {}
func (r *kgReceiver) OnStreamOpen(string)                        {}
func (r *kgReceiver) OnConnectionResult(*cloud.ConnectionResult) {}
func (r *kgReceiver) OnIntent(intent *cloud.IntentResult)        { r.intent = intent }

func TestCloudAudioKGResponseMetadata(t *testing.T) {
	t.Setenv("VECTOR_KG_TTS_URL", "http://lycopod.local:8080/v1/kg-audio")
	for _, bypass := range []bool{false, true} {
		receiver := &kgReceiver{}
		sendKGResponse(&chipper.KnowledgeGraphResponse{
			SpokenText: "The Sun is about 150 million kilometers away.",
			AudioId:    "answer-audio",
		}, receiver, bypass)
		if receiver.intent == nil {
			t.Fatal("KG answer was not delivered")
		}
		var params struct {
			Answer     string `json:"answer"`
			AudioID    string `json:"audio_id"`
			ResponseID string `json:"response_id"`
			CloudAudio bool   `json:"cloud_audio_available"`
		}
		if err := json.Unmarshal([]byte(receiver.intent.Parameters), &params); err != nil {
			t.Fatal(err)
		}
		if !params.CloudAudio || params.AudioID != "answer-audio" || params.ResponseID == "" || params.Answer == "" {
			t.Fatalf("incomplete cloud audio metadata: %+v", params)
		}
	}
}
