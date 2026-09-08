package voice

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
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
	cloudAudioTimeout    = 60 * time.Second
	cloudAudioFramedType = "application/vnd.lycopod.answer-audio.v1"
)

type cloudAudioFailure struct {
	kind   cloud.ResponseAudioErrorType
	detail string
}

func (e *cloudAudioFailure) Error() string { return e.detail }

// A successful HTTP EOF is insufficient: the negotiated protocol must contain
// END. Abort and upstream transport failure stay distinct even after status 200.
type framedCloudAudio struct {
	io.ReadCloser
	remaining uint32
	done      bool
}

func (r *framedCloudAudio) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	if r.done {
		return 0, io.EOF
	}
	failure := func(kind cloud.ResponseAudioErrorType, detail string) (int, error) {
		return 0, &cloudAudioFailure{kind: kind, detail: detail}
	}
	if r.remaining == 0 {
		var header [5]byte
		if _, err := io.ReadFull(r.ReadCloser, header[:]); err != nil {
			return failure(cloud.ResponseAudioErrorType_Transport, "answer stream ended without terminal frame: "+err.Error())
		}
		r.remaining = binary.BigEndian.Uint32(header[1:])
		if header[0] == 0 {
			if r.remaining == 0 || r.remaining > cloudAudioChunkBytes {
				return failure(cloud.ResponseAudioErrorType_InvalidFormat, "invalid answer PCM frame length")
			}
		} else {
			if r.remaining != 0 {
				return failure(cloud.ResponseAudioErrorType_InvalidFormat, "answer terminal frame has payload")
			}
			switch header[0] {
			case 1:
				var tail [1]byte
				n, err := io.ReadFull(r.ReadCloser, tail[:])
				if n != 0 {
					return failure(cloud.ResponseAudioErrorType_InvalidFormat, "unexpected data after answer END")
				}
				if err != io.EOF {
					return failure(cloud.ResponseAudioErrorType_Transport, "answer stream did not finish cleanly after END")
				}
				r.done = true
				return 0, io.EOF
			case 2:
				return failure(cloud.ResponseAudioErrorType_Cancelled, "answer cancelled by upstream")
			case 3:
				return failure(cloud.ResponseAudioErrorType_Transport, "upstream answer connection interrupted")
			case 4:
				return failure(cloud.ResponseAudioErrorType_Provider, "answer producer failed")
			default:
				return failure(cloud.ResponseAudioErrorType_InvalidFormat, "unknown answer frame type")
			}
		}
	}
	if uint32(len(p)) > r.remaining {
		p = p[:r.remaining]
	}
	n, err := io.ReadFull(r.ReadCloser, p)
	r.remaining -= uint32(n)
	if err != nil {
		return n, &cloudAudioFailure{kind: cloud.ResponseAudioErrorType_Transport,
			detail: "truncated answer PCM frame: " + err.Error()}
	}
	return n, nil
}

// kgResponseParams is the subset of the Knowledge Graph intent parameters that
// the cloud-audio path cares about.
type kgResponseParams struct {
	Answer              string `json:"answer"`
	ResponseID          string `json:"response_id"`
	AudioID             string `json:"audio_id"`
	CloudAudioAvailable bool   `json:"cloud_audio_available"`
}

type cloudAudioOwner struct {
	streamID   uint32
	generation uint64
}

func (p *Process) beginAudioOwner(streamID uint32) {
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	p.audioOwner.generation++
	p.audioOwner.streamID = streamID
}

func (p *Process) cancelAudioOwner(streamID uint32) {
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	if p.audioOwner.streamID == streamID {
		p.audioOwner.generation++
	}
}

func (p *Process) getAudioOwner(owners []cloudAudioOwner) cloudAudioOwner {
	if len(owners) != 0 {
		return owners[0]
	}
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	return p.audioOwner
}

// Check generation and write under the same lock: even legacy stream ID zero
// cannot allow a previous HTTP worker to write into a replacement request.
func (p *Process) writeCloudAudio(response *cloud.Message, owner cloudAudioOwner) {
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	if owner != p.audioOwner {
		return
	}
	switch response.Tag() {
	case cloud.MessageTag_ResponseAudioStart:
		response.GetResponseAudioStart().StreamId = owner.streamID
	case cloud.MessageTag_ResponseAudioChunk:
		response.GetResponseAudioChunk().StreamId = owner.streamID
	case cloud.MessageTag_ResponseAudioEnd:
		response.GetResponseAudioEnd().StreamId = owner.streamID
	case cloud.MessageTag_ResponseAudioError:
		response.GetResponseAudioError().StreamId = owner.streamID
	}
	p.writeResponseLocked(response)
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
	owner := p.getAudioOwner(nil)
	go p.sendCloudAudio(endpoint, params.ResponseID, params.AudioID, params.Answer, owner)
}

// sendCloudAudio fetches the answer audio and streams it to the engine as it
// arrives, so playback can start on the first sentence while the backend is
// still synthesizing the rest. Interrupted answers emit typed errors so the
// engine stops quietly, rather than repeating partial text through local TTS.
func (p *Process) sendCloudAudio(endpoint, responseID, audioID, text string, owners ...cloudAudioOwner) {
	owner := p.getAudioOwner(owners)
	log.Printf("Cloud audio: starting fetch for %s (audio_id_present=%t)\n", responseID, audioID != "")
	body, err := openCloudAudio(endpoint, audioID, text)
	if err != nil {
		log.Println("Cloud audio: fetch failed:", err)
		p.sendCloudAudioError(responseID, err, owner)
		return
	}
	defer body.Close()

	// The answer length is not known up front: the backend keeps the body open
	// while later sentences are synthesized.
	p.writeCloudAudio(cloud.NewMessageWithResponseAudioStart(&cloud.ResponseAudioStart{
		ResponseId:       responseID,
		Encoding:         cloud.ResponseAudioEncoding_Pcm16Le,
		SampleRateHz:     cloudAudioSampleRate,
		Channels:         cloudAudioChannels,
		TotalFrames:      0,
		TotalFramesKnown: false,
	}), owner)

	seq, total, err := p.streamCloudAudio(responseID, body, owner)
	if err != nil {
		// Legacy raw servers cannot distinguish abort from connection loss.
		// Only an established partial audio answer qualifies for quiet stop.
		var typed *cloudAudioFailure
		if total > 0 && !errors.As(err, &typed) {
			err = &cloudAudioFailure{kind: cloud.ResponseAudioErrorType_Transport, detail: err.Error()}
		}
		log.Println("Cloud audio: stream failed:", err)
		p.sendCloudAudioError(responseID, err, owner)
		return
	}
	// seq is 0 when nothing was emitted, which also covers a body too short to hold
	// a single 16-bit sample.
	if total == 0 || seq == 0 {
		p.sendCloudAudioError(responseID, errors.New("no audio returned"), owner)
		return
	}

	p.writeCloudAudio(cloud.NewMessageWithResponseAudioEnd(&cloud.ResponseAudioEnd{
		ResponseId:          responseID,
		FinalSequenceNumber: seq,
	}), owner)
	log.Printf("Cloud audio: streamed %d bytes in %d chunks for %s\n", total, seq, responseID)
}

// streamCloudAudio forwards the response body to the engine in player-sized
// chunks as the bytes arrive. It returns the number of chunks sent and the
// total byte count read.
func (p *Process) streamCloudAudio(responseID string, body io.Reader, owners ...cloudAudioOwner) (uint32, int, error) {
	owner := p.getAudioOwner(owners)
	var (
		seq     uint32
		total   int
		pending []byte
		buf     = make([]byte, cloudAudioChunkBytes)
	)

	emit := func(data []byte) {
		chunk := make([]byte, len(data))
		copy(chunk, data)
		p.writeCloudAudio(cloud.NewMessageWithResponseAudioChunk(&cloud.ResponseAudioChunk{
			ResponseId:     responseID,
			SequenceNumber: seq,
			Data:           chunk,
		}), owner)
		seq++
	}

	for {
		n, readErr := body.Read(buf)
		if n > 0 {
			total += n
			if total > cloudAudioMaxBytes {
				return seq, total, &cloudAudioFailure{kind: cloud.ResponseAudioErrorType_TooLarge, detail: "answer audio exceeds maximum size"}
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

	if len(pending)%2 != 0 {
		return seq, total, &cloudAudioFailure{kind: cloud.ResponseAudioErrorType_InvalidFormat, detail: "incomplete PCM sample"}
	}
	// Flush the remaining complete samples only after successful termination.
	if tail := len(pending) - len(pending)%2; tail > 0 {
		emit(pending[:tail])
	}
	return seq, total, nil
}

func (p *Process) sendCloudAudioError(responseID string, err error, owners ...cloudAudioOwner) {
	owner := p.getAudioOwner(owners)
	kind := cloud.ResponseAudioErrorType_Provider
	var typed *cloudAudioFailure
	if errors.As(err, &typed) {
		kind = typed.kind
	}
	p.writeCloudAudio(cloud.NewMessageWithResponseAudioError(&cloud.ResponseAudioError{
		ResponseId: responseID,
		Error:      kind,
		Details:    err.Error(),
	}), owner)
}

// openCloudAudio starts the answer-audio request and returns the response body
// for incremental reading.
//
// When the Knowledge service handed back an audio id, the answer audio already
// exists on the server and is fetched directly by id - this is the escape-pod
// style path and needs no text-to-speech configuration on the robot. Otherwise
// the answer text is POSTed to a generic synthesis endpoint.
//
// Handle requests negotiate framed outcomes, decoded here into incremental
// 16 kHz mono s16le PCM. Older servers and generic POSTs still use raw PCM.
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
	if audioID != "" {
		req.Header.Set("Accept", cloudAudioFramedType)
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, fmt.Errorf("answer audio endpoint returned status %d", resp.StatusCode)
	}
	contentType, _, _ := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if audioID != "" && contentType == cloudAudioFramedType {
		return &framedCloudAudio{ReadCloser: resp.Body}, nil
	}
	return resp.Body, nil
}

// joinAudioURL appends the audio id to the configured base endpoint.
func joinAudioURL(endpoint, audioID string) string {
	return strings.TrimSuffix(endpoint, "/") + "/" + url.PathEscape(audioID)
}
