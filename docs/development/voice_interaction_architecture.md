# Vector Voice Interaction Architecture and Duplex Feasibility

_Last reviewed: 19 July 2026_

## Scope

This document describes the normal Vector/Victor voice-command path in `wire-os`, including:

- Microphone capture and preprocessing
- Wake-word detection and utterance streaming
- Cloud and cloudless speech processing
- Intent delivery and behavior execution
- Text-to-speech and speaker playback
- Current turn-taking constraints
- Feasibility of continuous multi-turn interaction, barge-in, and full duplex

The Alexa implementation is discussed only as a comparison because it uses a separate AVS interaction stack.

## Summary

The normal Vector voice system is **one-shot and half-duplex at the interaction layer**:

1. Wait for the wake word.
2. Play the listening get-in/earcon.
3. Capture one utterance.
4. Receive one final intent.
5. Stop and close the microphone/cloud stream.
6. Exit the listening behavior.
7. Execute a response behavior or play TTS.
8. Return to idle and wait for another wake word.

Microphone capture and speaker playback can physically run at the same time. However, dependable full-duplex speech recognition is not currently supported because the microphone DSP does not receive a real speaker playback reference and therefore cannot reliably remove Vector's own speech from microphone input.

## Component Map

| Layer | Process/component | Responsibility |
|---|---|---|
| Microphone hardware | Robot/syscon/HAL | Continuously captures four microphone channels |
| Robot transport | Robot supervisor | Packages and sends microphone data to the head process |
| Audio preprocessing | `vic-anim` `MicDataProcessor` | VAD, Signal Essence processing, beamforming, wake-word input |
| Wake and stream control | `vic-anim` `MicDataSystem` | Creates one stream job and sends audio over `mic_sock` |
| Speech/intent processing | `vic-cloud` or `vic-cloudless` | Streams speech to Chipper or processes it locally with Vosk |
| Intent state | `vic-engine` `UserIntentComponent` | Converts one cloud result into one pending user intent |
| Interaction UX | `BehaviorReactToVoiceCommand` | Runs listening animations and exits when an intent arrives |
| Response output | Behaviors, Wwise, Acapela TTS | Executes actions and plays speech/audio through the speaker |

## Voice Input Path

### 1. Physical Microphone Capture

The robot captures four microphone channels continuously. The HAL exposes the latest microphone samples, and the supervisor deinterleaves and forwards them as `RobotInterface::MicData` messages.

Relevant code:

- [`robot/hal/src/hal.cpp`](../../robot/hal/src/hal.cpp#L1015-L1019)
- [`robot/supervisor/src/messages.cpp`](../../robot/supervisor/src/messages.cpp#L718-L764)
- [`robot/clad/src/clad/robotInterface/messageRobotToEngine.clad`](../../robot/clad/src/clad/robotInterface/messageRobotToEngine.clad#L119-L127)

Microphone privacy mute is implemented later in `vic-anim` by dropping incoming microphone payloads before buffering. Normal turn-taking does not disable microphone hardware capture.

### 2. Audio Processing in `vic-anim`

`MicDataProcessor` consumes the four-channel audio and produces processed mono audio. Its responsibilities include:

- Signal Essence microphone processing
- Optional beamforming
- Voice activity detection
- Microphone direction estimation
- Supplying audio to wake-word recognition
- Collecting processed audio for an active stream job

The processing format is based on 10 ms chunks. A normal command stream is limited to 6,050 ms, consisting of a 6,000 ms window plus 50 ms of trigger overlap.

Relevant code:

- [`lib/micData/micDataTypes.h`](../../../lib/micData/micDataTypes.h#L31-L37)
- [`animProcess/src/cozmoAnim/micData/micDataProcessor.cpp`](../../animProcess/src/cozmoAnim/micData/micDataProcessor.cpp#L150-L235)
- [`animProcess/src/cozmoAnim/micData/micDataProcessor.cpp`](../../animProcess/src/cozmoAnim/micData/micDataProcessor.cpp#L500-L593)

The VAD is used to avoid running wake-word recognition during prolonged silence. Robot movement or known speaker noise can force the activity state on so the system does not miss a wake word in noisy conditions.

### 3. Wake-Word Detection

Processed audio is continuously supplied to the configured speech recognizer. When the wake word is detected, `TriggerWordDetectCallback` first checks whether a streaming job already exists.

If a stream job exists, additional trigger detections are ignored:

- [`animProcess/src/cozmoAnim/micData/micDataProcessor.cpp`](../../animProcess/src/cozmoAnim/micData/micDataProcessor.cpp#L185-L205)

The system waits for the configured listening get-in animation or earcon to finish before creating the microphone stream. This intentionally prevents the listening sound from being captured as user speech.

After the earcon callback succeeds, `CreateStreamJob` creates the single active stream job and includes a short processed-audio overlap:

- [`animProcess/src/cozmoAnim/micData/micDataProcessor.cpp`](../../animProcess/src/cozmoAnim/micData/micDataProcessor.cpp#L253-L326)

### 4. Audio Streaming to `vic-cloud`

`MicDataSystem` owns a local IPC server for the microphone path. It sends CLAD `CloudMic` messages including:

- `hotword`
- `audio`
- `audioDone`
- `stopSignal`
- `streamOpen`
- `result`
- `error`

The main sockets are:

- `mic_sock`: microphone audio and microphone control
- `ai_sock`: cloud result delivery to `vic-engine`

Cloud process setup:

- [`vic-cloudless/cloud/main.go`](../../../vic-cloudless/cloud/main.go#L137-L171)
- The normal `vic-cloud` process uses the same interaction shape.

When a stream job begins, `MicDataSystem` sends one `hotword` message followed by processed audio chunks. It stops when:

- `vic-cloud` sends `stopSignal`, or
- The local 6,050 ms stream timeout is reached.

Relevant code:

- [`animProcess/src/cozmoAnim/micData/micDataSystem.cpp`](../../animProcess/src/cozmoAnim/micData/micDataSystem.cpp#L470-L550)

After actual capture stops, the stream job may be retained temporarily to satisfy a minimum listening UX duration. While retained, it still blocks another wake-triggered stream even though it is no longer recording:

- [`animProcess/src/cozmoAnim/micData/micDataSystem.cpp`](../../animProcess/src/cozmoAnim/micData/micDataSystem.cpp#L560-L579)

## Speech and Intent Processing

### Normal Cloud Path

`vic-cloud` accepts only one current voice `Streamer`. Receiving another hotword while one exists closes the previous stream.

The stream sends audio to Chipper in 120 ms groups and uses a 9-second request timeout. On the first final intent:

1. `vic-cloud` sends `stopSignal` to `vic-anim`.
2. It sends one `result` message to `vic-engine`.
3. It closes the cloud stream.
4. It sets the current stream pointer to `nil`.
5. It waits for another hotword before opening another stream.

Relevant code:

- [`cloud/internal/voice/process.go`](../../cloud/internal/voice/process.go#L143-L265)
- [`cloud/internal/voice/process.go`](../../cloud/internal/voice/process.go#L272-L305)
- [`cloud/internal/voice/process.go`](../../cloud/internal/voice/process.go#L365-L400)

This is one of the primary one-shot interaction gates.

### Cloudless Path

`vic-cloudless` keeps the same `mic_sock`, `ai_sock`, and `CloudMic` message transaction but replaces remote speech processing with local Vosk recognition and phrase-to-intent matching.

Its WebRTC VAD processes 10 ms frames. Endpointing currently requires:

- More than 18 active frames, approximately 190 ms of speech
- Followed by 23 inactive frames, approximately 230 ms of silence

Relevant code:

- [`vic-cloudless/internal/voice/vtr/vad.go`](../../../vic-cloudless/internal/voice/vtr/vad.go#L18-L69)

When endpointing fires, Vosk returns one final transcription and resets its global recognizer:

- [`vic-cloudless/internal/voice/vtr/vosk.go`](../../../vic-cloudless/internal/voice/vtr/vosk.go#L34-L54)

The text is matched to one intent, first by exact match and then by allowed substring match:

- [`vic-cloudless/internal/voice/vtr/process.go`](../../../vic-cloudless/internal/voice/vtr/process.go#L6-L34)

Although processing is local, the surrounding transaction is still one utterance producing one final intent.

## Intent Handling in `vic-engine`

Cloud messages arriving through `ai_sock` are passed to `UserIntentComponent`.

When a `result` arrives, the component:

1. Converts the cloud JSON into a typed user intent.
2. Marks the cloud stream closed.
3. Clears the pending trigger-stream state.

Errors and timeouts also mark the stream closed. A `streamOpen` message marks it open.

Relevant code:

- [`engine/aiComponent/behaviorComponent/userIntentComponent.cpp`](../../engine/aiComponent/behaviorComponent/userIntentComponent.cpp#L796-L864)

`BehaviorReactToVoiceCommand` independently tracks the stream state and listening UX. While in `ListeningLoop`, it stops listening as soon as any user intent becomes pending. It also exits on an error or timeout.

Relevant code:

- [`engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorReactToVoiceCommand.cpp`](../../engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorReactToVoiceCommand.cpp#L500-L583)

This enforces the interaction transition from listening to intent execution before response playback begins.

## Voice Output Path

Normal Vector responses are generally not streamed speech audio returned from Chipper. The cloud returns an intent, and the engine executes a corresponding behavior. That behavior may:

- Play a canned animation and Wwise event
- Change robot state or start an action
- Generate local speech through Acapela TTS

### TTS Lifecycle

The engine's `TextToSpeechCoordinator` sends these messages to `vic-anim`:

- `TextToSpeechPrepare`
- `TextToSpeechPlay`
- `TextToSpeechCancel`

Relevant code:

- [`engine/components/textToSpeech/textToSpeechCoordinator.cpp`](../../engine/components/textToSpeech/textToSpeechCoordinator.cpp#L140-L298)

`vic-anim` generates PCM data and supplies it to Wwise through `StreamingWavePortalPlugIn`. It also supports stopping active TTS:

- [`animProcess/src/cozmoAnim/textToSpeech/textToSpeechComponent.cpp`](../../animProcess/src/cozmoAnim/textToSpeech/textToSpeechComponent.cpp#L467-L520)
- [`animProcess/src/cozmoAnim/textToSpeech/textToSpeechComponent.cpp`](../../animProcess/src/cozmoAnim/textToSpeech/textToSpeechComponent.cpp#L535-L620)

TTS cancellation therefore already exists and can be reused for a future barge-in implementation.

## Why the Current Interaction Is One-Shot

The system contains several independent one-shot gates:

1. **Only one `vic-anim` stream job** can exist.
2. **Wake words are ignored** while that job exists.
3. **Streaming starts after the wake earcon**, not concurrently with it.
4. **Only one `vic-cloud` streamer** is current.
5. A new hotword **closes an existing cloud stream**.
6. The first final intent **sends `stopSignal`** to the microphone path.
7. The first final intent **closes and destroys the cloud stream**.
8. `UserIntentComponent` **marks the stream closed** when it receives a result.
9. `BehaviorReactToVoiceCommand` **stops listening** when an intent becomes pending.
10. Response behavior and TTS occur **after the listening state exits**.
11. Hard limits stop capture after 6.05 seconds and cloud processing after 9 seconds.
12. A minimum UX hold can temporarily block a new stream after real capture has ended.

These constraints are distributed across `vic-anim`, `vic-cloud`, and `vic-engine`, so changing one timeout or flag is insufficient to create multi-turn interaction.

## Simultaneous Capture and Playback

The physical and process architecture permits simultaneous microphone capture and speaker playback:

- Syscon and the supervisor continue delivering microphone samples.
- `MicDataProcessor` continues processing microphone data.
- Wwise independently renders audio to the speaker.

The limitation is acoustic, not basic I/O exclusivity.

### Missing Playback Reference

In the Signal Essence processing path, `MicDataProcessor` passes a static zero-filled `dummySpeakerOut` as the speaker input:

- [`animProcess/src/cozmoAnim/micData/micDataProcessor.cpp`](../../animProcess/src/cozmoAnim/micData/micDataProcessor.cpp#L575-L593)

This means the DSP does not receive the actual audio being rendered by Wwise. Consequently, it cannot correlate and remove Vector's own speech from the microphone signal in a reliable playback-aware way.

The fallback policy is named `FBF_FORCE_ECHO_CANCEL` or `FBF_FORCE_ECHO_CANCEL_WITH_NR` depending on the Signal Essence version:

- [`animProcess/src/cozmoAnim/micData/micDataProcessor.cpp`](../../animProcess/src/cozmoAnim/micData/micDataProcessor.cpp#L861-L899)

However, this name should not be interpreted as evidence of complete acoustic echo cancellation. Without a synchronized real speaker reference, it cannot provide dependable full-duplex cancellation of arbitrary TTS and audio playback.

The underlying generated/project Signal Essence DSP sources are not present in the checked-in `lib/signalEssence` directory, so the strongest repository evidence is the zero-filled reference supplied by the wrapper.

## Feasibility Assessment

### Continuous Sequential Multi-Turn

**Feasibility: High**  
**Estimated architectural effort: Moderate**

This means:

- The user says the wake word once.
- Vector listens to one utterance.
- Vector responds.
- Vector automatically listens for the next utterance without requiring another wake word.
- Listening and speaking still happen in alternating turns.

The engine already supports wake-wordless follow-up capture:

- [`engine/aiComponent/behaviorComponent/userIntentComponent.cpp`](../../engine/aiComponent/behaviorComponent/userIntentComponent.cpp#L940-L948)
- [`animProcess/src/cozmoAnim/micData/micDataSystem.cpp`](../../animProcess/src/cozmoAnim/micData/micDataSystem.cpp#L214-L271)

A new conversation coordinator could use this after response completion. Required work includes:

- A persistent conversation/session state in `vic-engine`
- Explicit rules for when to continue or end a conversation
- Automatic wake-wordless capture after response completion
- Removal or cleanup of the fake minimum-stream hold
- Conversation context passed to the speech/LLM backend
- Per-turn timeout, cancellation, and error recovery
- UX states for listening, thinking, speaking, and follow-up listening

The existing cloud path creates a new Chipper stream per utterance. Multi-turn context must therefore either live above the voice stream or be supported by a new backend/session protocol.

### Wake-Word Barge-In

**Feasibility: Medium**  
**Estimated architectural effort: Moderate to high**

This means a user can say the wake word while Vector is speaking, causing Vector to stop speaking and listen.

Useful existing pieces include:

- Wake-word processing runs continuously outside an active normal stream.
- TTS can be cancelled from engine to `vic-anim`.
- Behavior/action delegation can be cancelled or preempted.

Required work includes:

- Allowing or specially handling wake detection during response playback
- Cancelling active TTS, Wwise events, and response behaviors
- Distinguishing real wake words from playback self-triggering
- Clearing the current response state safely
- Starting the next listening stream immediately
- Defining behavior priority and rollback rules

This can be prototyped before full AEC, but reliability will depend on volume, distance, room acoustics, and wake-word self-trigger suppression.

### Natural Speech Barge-In and True Full Duplex

**Feasibility with current architecture: Low**  
**Feasibility after DSP/protocol redesign: Possible**  
**Estimated architectural effort: High**

This means Vector can speak while continuously listening, detect the user speaking over it, stop or adapt its response, and continue the same conversation.

Required capabilities include:

1. A synchronized copy of final speaker PCM from Wwise or the audio output path.
2. A real acoustic echo canceller using that speaker reference.
3. Delay alignment between playback and microphone capture.
4. Double-talk detection so user speech is preserved while Vector speaks.
5. Residual echo suppression and noise reduction.
6. Speech activity detection on echo-cancelled audio.
7. Partial and final ASR results rather than one final intent only.
8. A persistent bidirectional conversation session.
9. Streaming or incrementally cancellable response generation.
10. Immediate cancellation of TTS, Wwise playback, animations, and actions.
11. A conversation state machine that supports overlapping input and output events.
12. Extensive testing across speaker volume, robot motion, distance, and room acoustics.

The hardware and processes can perform input and output concurrently, but the current acoustic and interaction layers cannot provide dependable full-duplex speech.

## Alexa Comparison

Alexa uses a separate AVS stack with its own:

- `AudioInputProcessor`
- Shared audio stream
- Speech synthesizer and media players
- Dialog UX state aggregator
- `LISTENING`, `EXPECTING`, `THINKING`, `SPEAKING`, and `IDLE` states

Vector's Alexa UX explicitly supports a transition from speaking back to listening for a follow-up question:

- [`animProcess/src/cozmoAnim/alexa/alexa.cpp`](../../animProcess/src/cozmoAnim/alexa/alexa.cpp#L580-L627)
- [`animProcess/src/cozmoAnim/alexa/alexaClient.cpp`](../../animProcess/src/cozmoAnim/alexa/alexaClient.cpp#L348-L373)

This is a useful design reference for a normal Vector conversation state machine, but it is not a switch that enables multi-turn behavior in the normal `CloudMic` path.

## Recommended Implementation Sequence

### Phase 1: Sequential Multi-Turn Prototype

1. Add a `ConversationSession` state owned by `vic-engine`.
2. Enter the session after the initial wake-triggered intent.
3. Keep session context across response behaviors.
4. After TTS/response completion, call `StartWakeWordlessStreaming`.
5. End the session on explicit stop intent, silence timeout, errors, touch/button cancellation, or a maximum turn count.
6. Preserve current half-duplex behavior during each turn.

This is the lowest-risk path and validates the conversation UX without first solving acoustic echo cancellation.

### Phase 2: Wake-Word Barge-In

1. Permit wake recognition while response audio is playing.
2. Route a detected wake word to a global conversation interruption event.
3. Cancel active TTS and response behaviors.
4. Start a new listening turn.
5. Add self-trigger tests using every Vector TTS voice and common Wwise event.

### Phase 3: Acoustic Echo Cancellation

1. Identify the final PCM tap closest to physical speaker output.
2. Timestamp and feed that signal into the microphone DSP.
3. Replace `dummySpeakerOut` with the synchronized playback reference.
4. Enable or integrate an AEC implementation with double-talk support.
5. Validate echo return loss enhancement and user-speech preservation on-device.

### Phase 4: Speech Barge-In and Full Duplex

1. Run VAD and ASR on echo-cancelled audio during playback.
2. Interrupt output when confident near-end speech is detected.
3. Add partial ASR and incremental conversation events.
4. Introduce a persistent bidirectional backend protocol.
5. Add cancellation and supersession IDs for generated responses.
6. Tune thresholds and latency on production hardware.

## Recommended First Target

The recommended first implementation is **wake once, then sequential multi-turn interaction**. It offers the largest UX improvement using existing mechanisms:

- Wake-wordless streaming already exists.
- TTS completion and cancellation already exist.
- Listening and response behaviors already have explicit state transitions.
- No immediate DSP changes are required.

Full duplex should be treated as a later audio-platform project, with speaker-reference AEC as a prerequisite rather than an optional enhancement.

## Cloud-Generated Audio Responses

### Feasibility

Vector can accept a cloud-generated audio stream instead of generating all speech locally. Most of the speaker-side machinery already exists in the SDK external-audio path. Supporting both cloud audio and local TTS is also practical, provided they are treated as alternative response sources with explicit ownership rather than two voices mixed at the same time.

The recommended modes are:

| Mode | Response source | Intended use |
|---|---|---|
| `Local` | Existing Acapela TTS and Wwise assets | Offline commands, low latency, existing robot behaviors |
| `Cloud` | Audio generated by the conversation service | Conversational or higher-quality generated responses |
| `Auto` | Prefer cloud; fall back to local text/TTS | Recommended production default |

A response can still combine a cloud-spoken answer with local animations, eye movement, and sound effects. “Support both” should normally mean selecting one speech renderer for each response, not playing local TTS and cloud speech simultaneously.

### Existing Streamed-Playback Path

The public `ExternalAudioStreamPlayback` gRPC method already accepts a bidirectional stream with:

- `ExternalAudioStreamPrepare`
- `ExternalAudioStreamChunk`
- `ExternalAudioStreamComplete`
- `ExternalAudioStreamCancel`
- Completion and failure responses

The cloud gateway converts each request into a `GatewayWrapper` and forwards it to `vic-engine`:

- [`cloud/cloud/message_handler.go`](../../cloud/cloud/message_handler.go#L3573-L3680)

`SDKComponent` converts those protobuf messages to robot CLAD messages and sends them to `vic-anim`:

- [`engine/components/sdkComponent.cpp`](../../engine/components/sdkComponent.cpp#L713-L800)

`SdkAudioComponent` appends the received PCM to a Wwise `StreamingWavePortalPlugIn` data instance and plays it through Vector's speaker:

- [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L160-L266)

It also supports cancellation, completion callbacks, and cleanup:

- [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L60-L90)
- [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L270-L337)

### Existing PCM Contract

The current external-audio implementation expects:

- Mono PCM
- Signed 16-bit little-endian samples
- Sample rate from 8,000 through 16,025 Hz
- At most 1,024 bytes per engine-to-anim chunk
- Volume from 0 through 100

The CLAD messages are defined in:

- [`robot/clad/src/clad/robotInterface/messageEngineToRobot.clad`](../../robot/clad/src/clad/robotInterface/messageEngineToRobot.clad#L694-L721)

Playback starts after more than 200 ms of audio has been buffered. The implementation rejects a stream when queued-but-unplayed audio grows beyond 100,000 frames, which is approximately 6.25 seconds at 16 kHz:

- [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L27-L38)
- [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L105-L131)
- [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L195-L229)

The engine receives `ChunkAdded`, `Completed`, `Cancelled`, and failure states. The public API reports completion, buffer overflow, or playback failure:

- [`robot/clad/src/clad/types/sdkAudioTypes.clad`](../../robot/clad/src/clad/types/sdkAudioTypes.clad#L9-L38)
- [`engine/components/sdkComponent.cpp`](../../engine/components/sdkComponent.cpp#L205-L245)

### Current Voice-Cloud Limitation

The normal `CloudMic` voice result contains only:

- Intent name
- JSON parameters
- Text metadata

It has no response-audio messages or binary audio field:

- [`cloud/internal/clad/cloud/mic.go`](../../cloud/internal/clad/cloud/mic.go#L254-L331)

The Chipper response adapter similarly converts the service response into an intent or knowledge-graph text parameters. It does not receive or forward synthesized audio:

- [`cloud/internal/voice/stream/context.go`](../../cloud/internal/voice/stream/context.go#L140-L188)

Consequently, robot playback support already exists, but the normal voice backend and `CloudMic` protocol must be extended to deliver cloud-generated speech.

### Coexistence with Local TTS

Local TTS and SDK-streamed audio use separate streaming-plugin IDs:

- Local TTS uses plugin ID `0`: [`animProcess/src/cozmoAnim/textToSpeech/textToSpeechComponent.cpp`](../../animProcess/src/cozmoAnim/textToSpeech/textToSpeechComponent.cpp#L47-L49)
- SDK audio uses plugin ID `100`: [`animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp`](../../animProcess/src/cozmoAnim/audio/sdkAudioComponent.cpp#L35-L38)

This allows both implementations to be installed and independently buffered. It does not establish safe simultaneous speech. Both use the `TextToSpeech` Wwise game object, and the checked-in code does not define a central arbitration policy between local TTS and SDK playback.

A new response-audio coordinator should therefore guarantee that only one speech owner is active. Starting cloud speech should cancel local TTS, and starting local TTS should cancel or reject an active cloud response.

### Proof-of-Concept Option

The quickest prototype can reuse `ExternalAudioStreamPlayback` unchanged:

1. Generate 16 kHz mono signed 16-bit PCM in the cloud service.
2. Open `ExternalAudioStreamPlayback` to the robot gateway.
3. Send `Prepare`, paced PCM chunks, then `Complete`.
4. Use the existing completion response before reopening wake-wordless listening.
5. Send `Cancel` when the conversation is interrupted.

This demonstrates cloud speech with minimal robot changes. It is not the recommended final architecture because the route is SDK-owned and has several limitations:

- No conversation ID, response ID, or stream ID
- One active SDK stream with no reentrance
- No explicit response-to-intent association
- Overflow detection instead of receiver-driven backpressure
- No direct integration with behavior ownership or local TTS arbitration
- Public SDK control and authorization semantics may not match an internal voice service
- Status broadcasts can be ambiguous if multiple audio clients exist

### Recommended Production Architecture

Create a first-class `CloudResponseAudio` path while reusing the Wwise streaming implementation. The response protocol should carry an envelope such as:

| Field | Purpose |
|---|---|
| `conversation_id` | Associates all turns in one conversation |
| `turn_id` | Associates audio with one user utterance |
| `response_id` | Supports cancellation and supersession |
| `sequence_number` | Detects missing, duplicated, or reordered chunks |
| `format` | PCM initially; optional Opus support later |
| `sample_rate` | Normally 16 kHz |
| `is_final` | Marks the final audio chunk |
| `text` | Enables local-TTS fallback and diagnostics |
| `animation_cues` | Optional timing hints for speaking UX |

The preferred flow is:

1. The backend returns an intent/action envelope plus response text.
2. If cloud audio is enabled, it streams audio chunks tagged with the same turn and response IDs.
3. `vic-cloud` forwards those chunks through a dedicated internal IPC route or new `CloudMic` audio messages.
4. A `ResponseAudioCoordinator` in `vic-engine` chooses cloud audio or local TTS and owns interruption policy.
5. `vic-anim` plays cloud PCM through a dedicated plugin ID and reports prepared, started, progress, completed, cancelled, underflow, and failed states.
6. On cloud timeout or failure before meaningful playback, the coordinator synthesizes the supplied text locally.
7. On completion, the conversation coordinator starts the next wake-wordless listening turn.

The existing `SdkAudioComponent` should be generalized into a source-neutral streaming player or used as a template. Reusing the public SDK RPC internally without adding IDs and ownership would make cancellation and multi-turn race handling fragile.

### Cloud Audio Format Choice

For the first version, use 16 kHz mono signed 16-bit little-endian PCM because it matches the existing player and avoids on-robot decoder work. Its bandwidth is:

$$
16000 \times 16 \times 1 = 256000\ \text{bits/s} \approx 32\ \text{kB/s}
$$

This is reasonable on Wi-Fi for short speech responses. If bandwidth becomes important, Opus is preferable to MP3 for interactive speech, but it requires a decoder, jitter buffer, packet-loss handling, and careful latency control on the robot. Alexa's media player contains compressed-audio decoding patterns, but it belongs to the separate AVS stack and should not be directly coupled to normal Vector voice responses.

### Buffering and Backpressure

Cloud speech should not send the entire response as fast as possible. The production player should expose receiver credit or target-buffer status. A reasonable initial policy is:

- Start playback after 150–250 ms is buffered.
- Maintain approximately 300–750 ms of queued audio.
- Pause network reads or issuance when the high watermark is reached.
- Report underflow separately from completion.
- Cap total memory and response duration.
- Drop chunks from cancelled or superseded response IDs.

The existing 200 ms startup threshold is a useful starting point, but the current 100,000-frame overflow threshold is a safety limit rather than a flow-control protocol.

### Hybrid Selection and Fallback Policy

Recommended `Auto` policy:

1. Use local Wwise assets for fixed robot earcons and sound effects.
2. Use local TTS for offline commands, low-latency confirmations, private/local-only data, and existing behaviors that compose their final text on the robot.
3. Use cloud audio for conversational answers generated by the cloud service.
4. Always include response text with cloud audio so local TTS can act as fallback.
5. If cloud audio fails before playback starts, synthesize the complete text locally.
6. If cloud audio fails after substantial playback, do not restart the full sentence; either end gracefully or resume from a sentence boundary if the protocol supplies boundaries.
7. Never start local TTS and cloud speech for the same response concurrently.

Replacing every existing local-TTS use with cloud audio would be unnecessarily invasive. Many behaviors generate text from local state after intent execution. A hybrid response renderer preserves those behaviors while allowing conversational services to supply their own voice.

### Interaction and Duplex Impact

Cloud-generated playback does not by itself enable full duplex. Like local TTS, it will leak into the microphones unless its final rendered PCM is supplied as the AEC speaker reference.

Cloud audio should therefore support immediate cancellation for wake-word barge-in, but natural speech barge-in still depends on real acoustic echo cancellation. A future speaker-reference tap should include all final output—local TTS, cloud audio, Wwise effects, and mixing—not only the original cloud PCM.

### Security and Reliability Requirements

- Accept audio only from the authenticated conversation session.
- Enforce format, sample-rate, chunk-size, duration, and total-byte limits.
- Reject stale, duplicate, reordered, or superseded chunks.
- Avoid fetching arbitrary audio URLs directly on the robot unless URLs are signed and strictly validated.
- Cancel playback when the session disconnects, the user mutes audio, or a higher-priority behavior takes control.
- Keep response text for accessibility, telemetry, and local fallback, subject to privacy policy.
- Define deterministic priority among safety alerts, timers, local behaviors, SDK audio, Alexa, local TTS, and cloud speech.

### Recommended Delivery Sequence

1. **Prototype:** Stream 16 kHz PCM through `ExternalAudioStreamPlayback` and verify latency, quality, cancellation, and completion.
2. **Coordinator:** Add engine-level response-source selection and mutual exclusion with local TTS.
3. **Protocol:** Add conversation, turn, response, sequence, and cancellation IDs plus real backpressure.
4. **Integration:** Drive speaking animations and automatic follow-up listening from cloud-audio lifecycle events.
5. **Fallback:** Carry response text and fall back to local TTS on cloud failure.
6. **AEC:** Feed the final speaker mix—not merely cloud PCM—into the microphone echo-cancellation path before implementing natural barge-in.
