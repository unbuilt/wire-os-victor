package voice

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
	"github.com/digital-dream-labs/vector-cloud/internal/config"
	"github.com/digital-dream-labs/vector-cloud/internal/log"
)

const (
	// cloudAudioSampleRate is the PCM sample rate the robot's streaming player
	// expects (see sdkAudioComponent, valid 8000-16025 Hz).
	cloudAudioSampleRate = 16000
	// cloudAudioChannels is mono.
	cloudAudioChannels = 1
	// cloudAudioChunkBytes is the maximum payload per ResponseAudioChunk. The
	// robot player accepts up to 1024-byte chunks.
	cloudAudioChunkBytes = 1024
	// cloudAudioMaxBytes caps a single response to guard against runaway
	// synthesis (~60s of 16kHz mono s16le audio).
	cloudAudioMaxBytes = 16000 * 2 * 60
	// cloudAudioTimeout bounds the whole answer, not just the first byte. The
	// backend synthesizes sentence by sentence and keeps the body open while
	// later sentences are still being produced.
	cloudAudioTimeout = 60 * time.Second
)

// kgResponseParams is the subset of the Knowledge Graph intent parameters that
// the cloud-audio path cares about.
type kgResponseParams struct {
	Answer              string `json:"answer"`
	ResponseID          string `json:"response_id"`
	AudioID             string `json:"audio_id"`
	CloudAudioAvailable bool   `json:"cloud_audio_available"`
}

// maybeSendCloudAudio inspects a just-delivered intent result and, if it is a
// Knowledge Graph answer that advertised cloud audio, asynchronously fetches the
// answer audio and streams it to the engine as ResponseAudio* messages.
// Anything unrelated (or with the feature disabled) is a no-op.
func (p *Process) maybeSendCloudAudio(result *cloud.IntentResult) {
	if result == nil || result.Parameters == "" {
		return
	}
	endpoint := config.KGCloudAudioURL()
	if endpoint == "" {
		return
	}
	var params kgResponseParams
	if err := json.Unmarshal([]byte(result.Parameters), &params); err != nil {
		logVerbose("Cloud audio: could not parse intent params:", err)
		return
	}
	if !params.CloudAudioAvailable || params.ResponseID == "" {
		return
	}
	if params.AudioID == "" && params.Answer == "" {
		return
	}
	go p.sendCloudAudio(endpoint, params.ResponseID, params.AudioID, params.Answer)
}

// sendCloudAudio fetches the answer audio and streams it to the engine as it
// arrives, so playback can start on the first sentence while the backend is
// still synthesizing the rest. On any failure it emits a ResponseAudioError so
// the robot falls back to local TTS promptly.
func (p *Process) sendCloudAudio(endpoint, responseID, audioID, text string) {
	log.Printf("Cloud audio: starting fetch for %s (audio_id_present=%t)\n", responseID, audioID != "")
	body, err := openCloudAudio(endpoint, audioID, text)
	if err != nil {
		log.Println("Cloud audio: fetch failed:", err)
		p.sendCloudAudioError(responseID, err)
		return
	}
	defer body.Close()

	// The answer length is not known up front: the backend keeps the body open
	// while later sentences are synthesized.
	p.writeResponse(cloud.NewMessageWithResponseAudioStart(&cloud.ResponseAudioStart{
		ResponseId:       responseID,
		Encoding:         cloud.ResponseAudioEncoding_Pcm16Le,
		SampleRateHz:     cloudAudioSampleRate,
		Channels:         cloudAudioChannels,
		TotalFrames:      0,
		TotalFramesKnown: false,
	}))

	seq, total, err := p.streamCloudAudio(responseID, body)
	if err != nil {
		log.Println("Cloud audio: stream failed:", err)
		p.sendCloudAudioError(responseID, err)
		return
	}
	// seq is 0 when nothing was emitted, which also covers a body too short to hold
	// a single 16-bit sample.
	if total == 0 || seq == 0 {
		p.sendCloudAudioError(responseID, errors.New("no audio returned"))
		return
	}

	p.writeResponse(cloud.NewMessageWithResponseAudioEnd(&cloud.ResponseAudioEnd{
		ResponseId:          responseID,
		FinalSequenceNumber: seq,
	}))
	log.Printf("Cloud audio: streamed %d bytes in %d chunks for %s\n", total, seq, responseID)
}

// streamCloudAudio forwards the response body to the engine in player-sized
// chunks as the bytes arrive. It returns the number of chunks sent and the
// total byte count read.
func (p *Process) streamCloudAudio(responseID string, body io.Reader) (uint32, int, error) {
	var (
		seq     uint32
		total   int
		pending []byte
		buf     = make([]byte, cloudAudioChunkBytes)
	)

	emit := func(data []byte) {
		chunk := make([]byte, len(data))
		copy(chunk, data)
		p.writeResponse(cloud.NewMessageWithResponseAudioChunk(&cloud.ResponseAudioChunk{
			ResponseId:     responseID,
			SequenceNumber: seq,
			Data:           chunk,
		}))
		seq++
	}

	for {
		n, readErr := body.Read(buf)
		if n > 0 {
			total += n
			if total > cloudAudioMaxBytes {
				return seq, total, errors.New("answer audio exceeds maximum size")
			}
			pending = append(pending, buf[:n]...)
			// Emit only whole chunks of whole 16-bit frames; a partial frame
			// waits for the next read so the player never sees a split sample.
			sent := 0
			for len(pending)-sent >= cloudAudioChunkBytes {
				emit(pending[sent : sent+cloudAudioChunkBytes])
				sent += cloudAudioChunkBytes
			}
			pending = pending[sent:]
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return seq, total, readErr
		}
	}

	// Flush whatever is left, dropping a trailing odd byte.
	if tail := len(pending) - len(pending)%2; tail > 0 {
		emit(pending[:tail])
	}
	return seq, total, nil
}

func (p *Process) sendCloudAudioError(responseID string, err error) {
	p.writeResponse(cloud.NewMessageWithResponseAudioError(&cloud.ResponseAudioError{
		ResponseId: responseID,
		Error:      cloud.ResponseAudioErrorType_Provider,
		Details:    err.Error(),
	}))
}

// openCloudAudio starts the answer-audio request and returns the response body
// for incremental reading.
//
// When the Knowledge service handed back an audio id, the answer audio already
// exists on the server and is fetched directly by id - this is the escape-pod
// style path and needs no text-to-speech configuration on the robot. Otherwise
// the answer text is POSTed to a generic synthesis endpoint.
//
// Either way the body is raw 16 kHz mono signed 16-bit little-endian PCM,
// delivered incrementally.
func openCloudAudio(endpoint, audioID, text string) (io.ReadCloser, error) {
	client := &http.Client{Timeout: cloudAudioTimeout}

	var req *http.Request
	var err error
	if audioID != "" {
		req, err = http.NewRequest(http.MethodGet, joinAudioURL(endpoint, audioID), nil)
		if err != nil {
			return nil, err
		}
	} else {
		reqBody, jsonErr := json.Marshal(map[string]interface{}{
			"text":        text,
			"sample_rate": cloudAudioSampleRate,
			"channels":    cloudAudioChannels,
			"encoding":    "pcm_s16le",
		})
		if jsonErr != nil {
			return nil, jsonErr
		}
		req, err = http.NewRequest(http.MethodPost, endpoint, bytes.NewReader(reqBody))
		if err != nil {
			return nil, err
		}
		req.Header.Set("Content-Type", "application/json")
	}
	req.Header.Set("Accept", "application/octet-stream")

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, fmt.Errorf("answer audio endpoint returned status %d", resp.StatusCode)
	}
	return resp.Body, nil
}

// joinAudioURL appends the audio id to the configured base endpoint.
func joinAudioURL(endpoint, audioID string) string {
	return strings.TrimSuffix(endpoint, "/") + "/" + url.PathEscape(audioID)
}
