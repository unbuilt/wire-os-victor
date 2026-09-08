# Default-on Automatic Follow-up Multi-turn Mode

Status: **OTA116 built and shipping-byte verified with first-sentence interruption support; coordinated manual firmware/backend deployment and hardware acceptance pending (section 21). Earlier sections retain historical rollout status.**  
Date: 2026-09-08

Related documents:

- [Sequential multi-turn implementation design](multi_turn_voice_interaction_implementation_design.md)
- [Knowledge Graph cloud-audio deployment](knowledge_graph_cloud_audio_deployment.md)
- [AEC feasibility experiment](aec_feasibility_experiment.md)

## 1. Decision and scope

Implement automatic follow-up listening before attempting full duplex:

> After a successful spoken answer has completely finished, automatically open
> another bounded listening turn without requiring the wake word again.

This is **sequential, half-duplex conversation**. Microphone processing may
continue in the background, but this feature does not open follow-up recognition
while the robot is speaking. Adaptive AEC, overlap recognition and speech
barge-in are not prerequisites.

The implemented first release:

- Be enabled by default, with an explicit opt-out restoring single-turn behavior.
- Start and continue sessions only for allowlisted Knowledge Graph answers,
  covering both prompted questions and direct/bypass questions.
- Support the current cloud-audio renderer and successful local-TTS fallback.
- Keep the initial ordinary wake on IntentGraph; route automatic Knowledge-mode
  follow-ups to KnowledgeGraph and dispatch their answers directly to the KG renderer.
- End on silence, errors, cancellation, safety events or configured limits.
- Leave ordinary single-turn behavior unchanged when disabled.

It should not add full duplex, wake-word barge-in, an always-listening ambient
mode, a new ASR model, automatic continuation after arbitrary actions, or
conversation persistence across restart. No lycopod changes are assumed or
authorized by this design.

### Relationship to the earlier design

The broader sequential multi-turn design already proposes a session component
and a follow-up behavior. Reuse that architecture rather than adding a second
conversation controller.

This document defines the narrower first rollout and updates its policy:
current cloud-audio playback is included; successful playback must be explicit;
there are no automatic reprompts or activation retries; and the proposed
session admission budget is 120 seconds rather than the earlier 45 seconds.
The default-on policy supersedes the earlier design's opt-in rollout.
Generic intent deactivation alone is not a sufficient success signal.

## 2. User experience

```text
Wake word + question
        |
        v
Capture -> resolve intent -> play answer completely
                                  |
                      finish response get-out / settle
                                  |
                                  v
                         show listening indicator
                                  |
                      open one follow-up capture
                         /                  \
                    user speaks          silence
                         |                  |
                    KnowledgeGraph       end session
                         |
                eligible spoken answer
                         |
                    repeat, within limits
```

The robot must not repeat its initial "ready for a question" prompt on every
turn. Use the existing visible listening indication. Any optional get-in sound
or motor animation must finish before capture starts.

Example: an answer takes 25 seconds. The robot finishes playback, completes
any audible response animation, waits the short settle interval, and then
opens listening. None of those 25 seconds consumes the next capture budget.
The user begins speaking when the listening indicator appears.

If the user says nothing, the session ends once. Do not synthesize an answer
to silence, repeatedly reopen the microphone, or keep asking another question.
Speech during the answer is outside this version's supported interaction.

## 3. Current mechanisms and limits

These are existing mechanisms, not evidence that automatic follow-up exists:

| Surface | Current mechanism | Relevance |
|---|---|---|
| Engine capture request | `UserIntentComponent::StartWakeWordlessStreaming` | Opens a turn without another wake word |
| Listening UX | `BehaviorPromptUserForVoiceCommand` | Existing speak-then-listen sequencing and normal intent handling |
| Microphone capture | `MicDataSystem` streaming job | One bounded stream, not a continuous conversation |
| Cloud playback | `SDKAudioStreamingState::Completed` handled by the KG behavior | Actual playback completion, distinct from server end-of-stream |
| Local playback | Delegated KG TTS behavior | Needs an explicit successful-finish outcome, not merely deactivation |
| Voice transport | Fresh Chipper session ID per stream | Separate per-utterance requests, not automatically shared dialogue context |

Relevant source:

- [`userIntentComponent.cpp`](../../engine/aiComponent/behaviorComponent/userIntentComponent.cpp)
- [`behaviorPromptUserForVoiceCommand.cpp`](../../engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorPromptUserForVoiceCommand.cpp)
- [`behaviorKnowledgeGraphQuestion.cpp`](../../engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/behaviorKnowledgeGraphQuestion.cpp)
- [`micDataSystem.cpp`](../../animProcess/src/cozmoAnim/micData/micDataSystem.cpp)
- [`micDataTypes.h`](../../lib/micData/micDataTypes.h)
- [`voice/process.go`](../../cloud/internal/voice/process.go)
- [`voice/stream/api.go`](../../cloud/internal/voice/stream/api.go)
- [`voice/stream/connect.go`](../../cloud/internal/voice/stream/connect.go)

### The listening window is not one 10-second setting

The current microphone cap is **6.05 nominal seconds of samples**. Normal
voice requests have a **9-second request deadline**. The enabled cloud-audio
KG path instead has a **60-second request budget**, with a default **65-second
engine response guard**. Those longer budgets also cover waiting for an answer;
they do not make microphone capture last 60 seconds.

For the first rollout, preserve the existing per-turn capture/request limits
and start a fresh turn only after response completion. Do not advertise this
as "10 seconds to start speaking": the existing microphone cap includes the
utterance itself. Sample-count duration is also not an exact wall-clock timer.

A later true 10-15 second no-speech allowance would require coordinated capture,
speech-onset/end, request and engine deadlines. Increasing only an engine
timeout cannot provide it.

## 4. Session ownership and state

`ConversationSessionComponent` is a registered dependency-managed component;
its deterministic policy is `ConversationSessionState`.
`BehaviorConversationFollowUp` is registered in the behavior factory and is
the last/lowest-priority delegate in `VoiceFeatures`. Both now exist in source.

The session owner survives between responder behaviors. The follow-up behavior
claims one pending listen request, handles listening UX and stream opening,
then yields valid intents to the normal intent system. It must not execute
those intents itself or permanently hold control over responders.

Initial eligibility is the KG question/bypass answer family. Automatic
follow-ups now use `CloudMic::StreamType::KnowledgeGraph`. UIC promotes a
validated, owned `knowledge_response` into `knowledge_response_bypass` without
changing its answer or audio identity. The ordinary dispatcher activates the
existing KG answer renderer, not another prompted question. Non-answer results
end the automatic turn without dispatch or a reprompt. Initial ordinary wake
requests remain `Normal` / `StreamingIntentGraph`; explicitly prompted KG
sub-requests retain their existing active-listener handling. The Normal
follow-up route described in historical sections 13–16 shipped through OTA113
and is superseded by section 17.

Suggested state flow:

| State | Event | Action |
|---|---|---|
| Inactive | Eligible initial intent is claimed with feature enabled | Create session; record turn 1 and intent activation identity |
| Responding | Matching response succeeds and owning behavior finishes | Request settle, if continuation is still allowed |
| Settling | Get-out finished, settle expired, no competing intent/stream | Claim exactly one follow-up activation |
| Opening | Microphone/stream start confirmed for this turn | Enter listening UX and existing bounded request flow |
| Listening / awaiting result | Valid matching KG answer arrives | Promote to bypass answer; yield to dispatcher and track responder |
| Listening / awaiting result | Silence, unmatched result, error or timeout | End once with the appropriate reason |
| Any active state | Cancellation, mute, safety, superseding interaction or disable | Invalidate continuation and clean up |
| Ending | Owned resources released | Return to inactive; leave no pending follow-up |

A follow-up activation must time out rather than remain queued until the robot
later becomes available. Recheck policy both when scheduling and when opening:
a safety event can occur during the settle interval.

## 5. Successful completion contract

### Cloud audio

Use the renderer's accepted `Completed` event for the current response after
the stream's end marker was sent. Do not infer completion from:

- The HTTP response ending or the last PCM chunk being queued.
- A temporary quiet/starved audio buffer.
- An estimated answer duration or a completion watchdog expiring.
- `_dVars.cloudAudioResponseFinished` alone: current cancellation and failure
  paths also set that flag.

The renderer adapter should emit a typed outcome such as `Succeeded`, `Failed`
or `Cancelled`, bound to the current session, turn and response identity.
Watchdog expiry remains failure and must not authorize follow-up.

### Local TTS and fallback

Successful local-TTS completion may authorize follow-up. Cancellation, failed
generation, failed playback or forced behavior deactivation may not.

If cloud audio fails and the existing policy falls back to local TTS, wait
for the fallback to succeed. Emit one final turn outcome, not one continuation
for cloud failure and another for TTS completion. Preserve existing playback
failure/fallback policy rather than replaying an answer merely to get a
successful completion signal.

### Get-out and settle

The current cloud completion handler can play `KnowledgeGraphSuccessReaction`.
Completion of the answer PCM alone therefore may not mean all response audio
or motor activity has ended. Wait for the owning response behavior/get-out to
finish, then apply the proposed 250 ms settle interval.

Discard prior-turn capture state and do not include samples from playback or
the settle interval in the new request. Reuse the existing wake-wordless capture
path, verifying its buffer boundary instead of assuming a fresh API call means
fresh samples. The settle interval is a starting policy, not proof that every
room's acoustic tail has disappeared.

## 6. Identity, routing and cleanup invariants

- At most one conversation, follow-up activation and microphone stream exist.
- Allocate one monotonically increasing turn ID per admitted user turn.
- Bind completion to the expected intent activation and response identity.
- Observe an actual pending -> claimed -> completed lifecycle; "no active
  intent" alone cannot establish success.
- Close/quiesce the previous stream before starting another. An engine
  `IsCloudStreamOpen() == false` flag alone is not an acknowledgement that all
  anim-side capture work has stopped.
- Clear consumed prior-turn response text, audio state, errors and deadlines
  without discarding an unrelated new pending intent.
- Ignore duplicate/late completion, result, error and timeout callbacks.
- Invalidate the session generation before asynchronous cleanup, so cleanup
  callbacks cannot reopen listening.
- Release only resources owned by this session; never cancel a newer interaction.

Audit correlation through engine, anim and vic-cloud. Where existing messages
lack sufficient identity or stop acknowledgement, add the minimal internal
protocol support needed. An engine-only counter does not make untagged late
transport messages safe.

## 7. Implemented default-on policy

These names/defaults are implemented. **OTA113 includes section 15's UX fixes;
section 17's KG routing correction is not yet packaged.**

| Setting or policy | Implemented first default |
|---|---|
| `multiTurnVoice.enabled` | `true`; explicit `false` restores single-turn behavior |
| Eligible continuing responses | KG answers only, including successful local-TTS fallback |
| `maxTurns` | 5, including the initial user turn |
| `sessionTimeout_sec` | 120 seconds; admission deadline for new follow-ups |
| `followUpSettleTime_ms` | 250 ms after response get-out completes |
| Follow-up activation wait | At most 2 seconds; no automatic retry |
| Stream-opening watchdog | At most 5 seconds; existing shorter transport failures still win |
| Capture / request budgets | Existing route-specific limits from section 3 |
| Silence | End session, no spoken reprompt |
| Unmatched result | End session, no automatic retry or reprompt |
| Feature disable | Revoke pending follow-ups and stop owned follow-up capture |

The admission deadline uses monotonic time and starts when the session is
created. It is independent of each new capture budget. If it expires during
an already admitted bounded request/answer, allow that turn to finish normally
but open no further turn. It is **not a hard 120-second total-lifetime guarantee**:
existing request/playback watchdogs still bound in-flight work. Safety and
explicit cancellation take effect immediately.

Disabling this optional feature alone should not cut off an already playing
answer. It prevents further automatic listening; ordinary safety/interruption
rules continue to apply. Reject invalid configuration explicitly, and do not
treat an explicit `false` as a missing value. In a build implementing this
feature, an absent configuration section or absent `enabled` key uses `true`.
This intentional default change does not enable follow-up on older firmware.
It also does not start listening on boot: a new eligible user-initiated
interaction is still required to begin a session.

## 8. Ending and failure handling

| Cause | Required handling |
|---|---|
| Recognized `silence` | Consume the terminal follow-up outcome; close with normal listening get-out, no answer |
| `unmatched_intent` | End without another automatic capture |
| Transport timeout/auth/network error | Log the actual error, retain appropriate existing failure UX, end |
| Playback/generation failure with no successful fallback | End; no follow-up |
| Spoken stop in automatic KG capture | End for the exact supported user transcripts in section 17; do not infer commands from answer text |
| Stop/cancel or `system_sleep` outside KG capture | Existing normal handling and session cancellation remain |
| New accepted wake word / competing intent / Alexa / SDK control | End or supersede the old session; do not fight normal behavior priorities |
| Mute, sleep, critical battery, thermal protection, shutdown | Stop according to existing safety policy; never override it |
| Session or turn admission limit | Finish any already admitted bounded turn; open no next turn |
| Feature disabled or process restarted | No latent follow-up and no restored conversation session |

Do not label every timeout as silence. A quiet user, failed connection and lost
result must remain distinguishable in telemetry even though all end the loop.
An empty KG `query_text` is unavailable transcript metadata, not proof of silence
(section 19 supersedes OTA114's rejection rule). A nonempty answer may proceed;
explicit noaudio and empty answers remain terminal. Missing/wrong-type JSON fields
still fail validation. Without a transcript, spoken stop cannot be recognized.
A backend answer hallucinated on silence cannot be reliably identified by this
contract. Silence-only hardware trials must catch self-conversation before
rollout; a nonempty ASR string is not proof of speech either.

## 9. Conversation context and privacy

The first milestone can be an **independent-question loop** using the existing
backend API. Each turn gets a fresh transport request. The current Chipper
connection code generates a new random session ID per stream; that is not
automatically a stable conversation ID.

Contextual replies such as "And tomorrow?" need a separate confirmed contract:
stable conversation identity, distinct turn/request IDs, ordering, expiry and
reset on session end. Verify backend support before claiming contextual
multi-turn memory. Do not reuse one transport session ID as a shortcut or
modify lycopod as part of the robot-only prototype without separate scope.

Keep the listening indication visible, preserve mute and existing data-collection
preferences, and upload only through the existing opted-in voice path. This
feature must not introduce background recording, retained transcripts or audio
uploads outside active listening turns. Diagnostic recording remains separately
authorized; do not log tokens or transcript content as routine session telemetry.

## 10. Implementation sequence

1. Add the default-on policy, explicit opt-out and deterministic session state
   machine in the existing engine architecture. With the flag explicitly off,
   add no capture or response changes.
2. Add typed successful/failed/cancelled response outcomes for cloud audio and
   local TTS, including fallback and get-out completion.
3. Add/register the follow-up behavior and its normal-priority activation
   condition. Reuse wake-wordless streaming, visible listening UX and normal
   intent routing; do not repeat the initial KG prompt.
4. Wire turn identity, stream closure, silence/error handling, session limits
   and cancellation across the required boundaries. Do not declare a local
   state-machine-only implementation complete.
5. Run targeted existing-runner tests and perform an explicitly authorized,
   supervised hardware trial of both default-on and explicit-off behavior.
   No automatic flashing or safety overrides.

Main touchpoints are the KG response behavior, `UserIntentComponent`, behavior
component/behavior registration, the follow-up behavior and configuration.
Anim/vic-cloud changes are conditional on the lifecycle/correlation audit,
not a reason to expand this milestone into a continuous-stream protocol.

## 11. Acceptance tests

Use the existing engine/voice test runners. Extend the playback-state tests
where relevant and add session-policy tests alongside existing engine tests.
Documentation alone does not satisfy these acceptance criteria.

| Scenario | Required result |
|---|---|
| Configuration section/key absent, or explicitly true | Follow-up enabled for eligible sessions; no unsolicited capture at boot |
| Feature explicitly false | Existing single-turn behavior, no extra microphone start |
| Malformed configuration | Explicit configuration error, not silently treated as missing/default-on |
| Successful cloud answer | Exactly one follow-up, after actual completion and get-out/settle |
| Server finishes before queued playback | No early follow-up |
| Playback stalls or completion event is missing | Watchdog failure, never success-shaped continuation |
| Successful local answer / cloud-to-local fallback | One follow-up after final successful playback |
| Cancelled or failed TTS | No follow-up |
| 25-second answer | Fresh capture budget after the answer; not consumed during playback |
| Three independent questions | No repeated wake word; each matching response precedes the next capture |
| No user speech | One listening attempt, clean end, no self-conversation |
| Robot-only sound / audible success animation | Not included as next-turn input; no self-conversation |
| Reply near capture end | Existing capture limit is explicit; no claim of an extra 10-second onset allowance |
| Unmatched result / authentication or network failure | One terminal outcome with correct reason; no retry loop |
| Duplicate completion / stale result from old turn | No extra capture and no mutation of the current turn |
| Pending new intent or accepted wake word during settle | Old follow-up revoked; new interaction not cleared |
| Mute/safety/SDK preemption during settle, opening or listening | Normal priority wins; no delayed reopen |
| Turn/session admission limit | No next turn, no abrupt truncation of an admitted answer |
| Disable or restart | No lingering stream, indicator or session continuation |
| Backend context unsupported | Independent questions work; contextual memory is not claimed |

Suggested first human sequence: "What's the weather tomorrow?", then after the
listening indicator "Tell me a short joke", then one independent question,
then silence. Test contextual follow-ups separately only if backend support is
confirmed. Do not combine initial acceptance with overlapping speech.

Log session/turn IDs, response outcome, playback completion, behavior completion,
settle end, capture/request start and stop, and final session-end reason. Check
that the next capture begins after the required response boundary and that
only one request was admitted. Keep diagnostic audio retention separate.

## 12. Rollout and rollback

Ship enabled by default after acceptance criteria are met, retaining an explicit
opt-out. First validate on a supervised, normally ready robot with the small
KG allowlist and bounded session policy. The existing AEC/reference
configuration is independent: this feature must neither enable AEC nor require
reference mode to be usable.

Before deployment or a trial, record the actual firmware and effective feature
settings and confirm backend connectivity. Release documentation must explain
that eligible answers now open a follow-up listening turn by default and show
how to opt out. After a session, confirm listening ends, the indicator
clears and no follow-up request remains. Expand the allowlist only after each
responder's completion and cancellation semantics are covered.

Rollback means disabling the implemented follow-up flag and confirming return
to ordinary wake-word-required single turns. See the actual configuration
location and workstation evidence below. Hardware acceptance above remains
outstanding, not an implied deployment instruction.

## 13. Implementation details and configuration (2026-09-07)

Routing/deadline descriptions in this historical implementation record reflect
OTA112/113. Section 17 supersedes the Normal follow-up route and 10-second guard;
configuration, ownership and completion invariants remain applicable.

### Configuration

The resource configuration is:

`resources/config/engine/behaviorComponent/behaviors/victorBehaviorTree/highLevelDelegates/knowledgeGraph/knowledgeGraphQuestion.json`

```json
"multiTurnVoice": {
  "enabled": true,
  "maxTurns": 5,
  "sessionTimeout_sec": 120,
  "followUpSettleTime_ms": 250
}
```

Set `multiTurnVoice.enabled` to **false** in this resource configuration to
build single-turn behavior. Missing section/key means true. This is a loaded
engine resource, not a lycopod setting, environment variable, or app setting.
The existing engine console variable `kAutomaticFollowUpEnabled`, category
`MultiTurnVoice`, is an additional runtime kill switch where console variables
are available; it cannot override a resource opt-out. Disabling revokes the
session and stops automatic capture, but lets an already pending/playing KG
answer finish. Re-enabling does not resurrect the cancelled session.

Malformed types, unknown keys, nonfinite times, `maxTurns` outside 1–20,
`sessionTimeout_sec` outside (0, 600], or settle outside [0, 2000] log
`ConversationSession.InvalidConfig` and disable continuation.

### Completion, routing, and lifecycle

- UIC reports actual eligible voice-intent activation IDs and matching
  deactivation to the session owner. Non-voice app/debug intents do not start a session.
  A process-lifetime increasing token invalidates old completion callbacks.
- KG reports success only after the accepted renderer completion, or the TTS
  behavior's successful playback **and get-out** outcome. For an admissible
  continuing response, the extra `KnowledgeGraphSuccessReaction` celebration is
  now omitted (section 15); otherwise its successful action is still required.
  The owning KG behavior must then
  deactivate. Watchdog expiry/cancel/failure never grants success; the existing
  short-playback fallback policy remains unchanged.
- TTS callbacks carry an utterance generation. Its `PlaybackOutcome` survives
  deactivation so the delegating KG behavior can distinguish successful finish
  from cancellation or failed playback.
- After deactivation the component requests capture shutdown and checks the
  matching anim `MicStreamState` quiescence acknowledgement before beginning
  the 250 ms settle. Neither server EOF nor `IsCloudStreamOpen()==false`
  substitutes for playback completion or capture acknowledgement.
- A single follow-up is admitted at most two seconds after settle; opening
  has a five-second watchdog. The follow-up uses `Normal` routing and the
  ordinary listening earcon, completed **before** the fresh capture boundary.
  It has no spoken prompt or motor get-in. Fresh turns now defer backpack lights,
  capture acknowledgement and cloud request opening until the first accepted
  fresh processed block, not job allocation or earcon start. The face listening
  loop follows that acknowledgement and gets out when capture closes, even if
  the cloud result is still pending. There are no retries or reprompts.
- Valid results remain pending for normal behavior dispatch, not execution by
  the follow-up behavior. Silence/unmatched results from the owned capture are
  consumed and terminate the session. Other commands run normally, without
  authorizing another listen. The ten-second engine lost-result guard starts
  at capture acknowledgement; normal cloud request timeout remains nine seconds
  and microphone samples remain capped at approximately 6.05 seconds.
- Pending/playing responders are not held beneath the follow-up behavior.
  Errors, lost-result/activation/opening deadlines, cancellation, new wake words,
  competing intents, mute, off-treads, sleep, SDK/Alexa activity, low battery,
  battery thermal protection, CPU temperature ≥90°C, and shutdown suppress
  continuation. The off-treads and low-battery admission checks are deliberately
  conservative. Existing behavior/safety priorities still take precedence.
- Prompted KG also now exits on an opening timeout or transport error rather
  than waiting forever when a stream never opens.

### Correlation and compatibility

All real capture requests now have an internal `uint32` identity. Anim-issued
wake/button requests occupy the upper half; engine-issued wake-wordless
requests occupy the lower half. Identity is echoed by `TriggerWordDetected`,
anim capture acknowledgements, cloud open/result/error/close, and all response
audio messages. Zero remains available for legacy developer/test injection.
These are process-local IDs, **not** backend dialogue context.

`StartWakeWordlessStreaming.freshCapture` is independent of identity. Only
automatic follow-ups set it: ordinary capture overlap is retained, including
when the feature is off. Fresh capture rejects already queued samples, the
boundary-straddling block, and older source-timestamp samples that arrive late.
`StopWakeWordlessStreaming(streamId)` retires only the matching collector;
anim acknowledges after collectors are quiesced, including a pending start
that was cancelled before it could allocate a job. Mute is also included in
the capture-state notification.

UIC uses an ordered bounded control-event queue instead of a single slot, so
an open/result/close sequence in one engine tick cannot overwrite the result.
Stale identities, duplicate results, expired follow-up results and late audio
are rejected. Vic-cloud additionally checks worker identity and cancels old
HTTP audio generations, including replacement of legacy ID-zero requests.
The renderer's prepare/chunk/complete/cancel/status messages carry a separate
`playbackId`, preventing stale completion or cleanup from affecting a newer
KG/SDK player.

The CLAD changes require **matching engine, anim, cloud, robot supervisor and
engine shared-library binaries**; mixing old/new binaries is not supported.
No lycopod, duplex, AEC mode, capture cap or backend context contract changes
were made. Existing AEC instrumentation and unrelated dirty work were preserved.
The initial implementation validation below did not package an OTA; the
subsequent complete OTA112 build is recorded in section 14.

### Changed source map

Paths are relative to `anki/victor`; paired `.cpp/.h` entries mean both files:

- `engine/aiComponent/behaviorComponent/`
  - new `conversationSessionState.h`, `conversationSessionComponent.cpp/.h`
  - `userIntentComponent.cpp/.h`, `sleepTracker.h`
  - `behaviorComponent.cpp`, `behaviorComponents_fwd.h`,
    `behaviorComponents_impl.cpp`, `behaviorFactory.cpp`, `behaviorSystemManager.h`
  - new `behaviors/robotDrivenDialog/behaviorConversationFollowUp.cpp/.h`
  - `behaviors/knowledgeGraph/behaviorKnowledgeGraphQuestion.cpp/.h`
  - `behaviors/animationWrappers/behaviorTextToSpeechLoop.cpp/.h`
- `engine/components/sdkComponent.cpp/.h`
- `animProcess/src/cozmoAnim/`
  - `animProcessMessages.cpp`, `audio/sdkAudioComponent.cpp/.h`
  - `micData/micDataSystem.cpp/.h`, `micData/micDataProcessor.cpp/.h`,
    `micData/micDataInfo.cpp/.h`
- `clad/src/clad/cloud/mic.clad`
- `clad/src/clad/types/behaviorComponent/behaviorClasses.clad`, `behaviorIDs.clad`
- `robot/clad/src/clad/robotInterface/messageEngineToRobot.clad`,
  `messageRobotToEngine.clad`, `messageFromAnimProcess.clad`
- `cloud/internal/clad/cloud/mic.go` (regenerated), `cloud/internal/voice/process.go`,
  `cloudaudio.go`, new `correlation_test.go`
- Resources under `resources/config/engine/behaviorComponent/behaviors/victorBehaviorTree/`:
  `highLevelDelegates/knowledgeGraph/knowledgeGraphQuestion.json`,
  new `highLevelDelegates/knowledgeGraph/conversationFollowUp.json`,
  `reactions/voiceFeatures.json`
- New tests: `test/engine/testConversationSessionState.cpp`,
  `test/engine/testConversationProtocol.cpp`,
  `test/animProcess/testMicCaptureBoundary.cpp`,
  `test/tools/testMicMessageDispatch.py` and its C++ fixture
- This document and the implementation-status note in
  `docs/development/multi_turn_voice_interaction_implementation_design.md`.

### Workstation evidence and remaining validation

Existing GTest/Go tooling was used, without a new test framework:

- 24 native session-policy and existing cloud-playback-state tests passed,
  including ASan/UBSan: default-on/off, completion/deactivation, settle,
  single admission, deadlines, five-turn limit, fallback, stale callbacks,
  silence/error termination and disable/safety.
- Five native tests exercise the **actual generated C++ CLAD serializers** for
  capture identity/freshness, wake identity, quiescence/mute, playback ownership,
  and cloud result/error/audio identity.
- Four native ASan/UBSan microphone-boundary tests passed, including a stale
  collector running after stop and old source samples arriving after start.
- Targeted `internal/voice` cloud-audio and new correlation tests passed with
  `go test -tags nolibopusfile -race -count=1`, using the existing Go 1.24.4
  toolchain and native Opus.
- Real ARM `vic-engine`, `vic-anim`, and `vic-cloud` targets built in the
  existing `vic-yocto-builder-7:latest` environment. Engine source lists were
  regenerated with the existing metabuild tool and CMake was reconfigured
  without cleaning recipes/worktrees. Existing build version 111 was retained;
  **no image or OTA was built, packaged, flashed, or changed**.
- The configuration resource target was built and its three behavior resources
  were compared with source, including the lowest-priority follow-up registration.
  Actual build outputs are `_build/vicos/Release/bin/vic-engine`,
  `_build/vicos/Release/bin/vic-anim`, and **`_build/vicos/Release/vic-cloud`**
  relative to `anki/victor`. The Go build emits outside `bin`; the old installed
  `bin/vic-cloud` copy was not used as build evidence and was not installed.

Evidence is under workspace `_build/followup-validation/` (native,
protocol and cloud test logs and incremental ARM build logs) and
`_build/followup-transport-validation/` (transport build/capture test artifacts).
Only targeted tests were run successfully; this is not a claim that the entire
legacy engine/voice suite or a live behavior-tree simulation passed.

### Queued capture-state forwarding regression (2026-09-07)

Review found that `SendMicStreamState` enqueued capture acknowledgements but
`MicDataSystem::Update` did not dispatch their tag. It now forwards
`msg->micStreamState` through `RobotInterface::SendAnimToEngine`. Without this
branch, engine capture state remains unknown and successful answers time out
in `Quiescing` instead of admitting a follow-up.

All producers using this mic queue were inspected: `triggerWordDetected`,
`micDirection`, `beatDetectorState`, and `micStreamState` are now handled.
Renderer status uses a separate direct `AnimProcessMessages::SendAnimToEngine`
path, not this queue; no other newly added queued type was missing here.

Regression command, run from the workspace root:

```sh
MIC_DISPATCH_OUTPUT="$PWD/_build/followup-validation/mic-dispatch" \
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s anki/victor/test/tools -p testMicMessageDispatch.py -v
```

Both unittest cases passed. The harness compiles the **actual source bodies**
of `SendMicStreamState`, `SendMessageToEngine`, and the `Update` queue-drain/
dispatch block with generated CPPLite messages and existing GTest. A recording
transport and minimal stream-display context replace hardware services; the
production dispatcher is not duplicated in the fixture. Three GTests pass
with each of `ANKI_DEV_CHEATS=0` and `1`: ordered open/close/mute delivery and
single drain, all existing producer types, and successful response/deactivation
→ delivered close acknowledgement → full settle → single follow-up admission
→ delivered open acknowledgement → listening. This last test drives the real
session policy, not the full UIC/behavior tree. Removing the forwarding branch
only in the generated test translation unit makes all three tests fail,
including the successful-answer regression.

Existing binaries were rerun: 24 session/playback tests passed both normally
and under ASan/UBSan; five protocol tests and four ASan/UBSan capture-boundary
tests passed. Logs are `mic-dispatch-tests.log`,
`mic-dispatch-existing-tests.log`, and `mic-dispatch/*/results.log` under
workspace `_build/followup-validation/`.

The existing `vic-yocto-builder-7:latest` container was run as `unbuilt` with
the workspace mounted at its original path, `anki-deps` mounted at
`/home/unbuilt/.anki`, the existing Go/user/ccache mounts, and `TMPDIR` set
inside workspace `_build/followup-validation`. The exact incremental target
command was `cmake --build anki/victor/_build/vicos/Release --target vic-anim -- -j4`.
It compiled `micDataSystem.cpp`, relinked `libvictor_anim.a`, and relinked the
actual ARM EABI5 `bin/vic-anim` successfully (three build steps).
`build-mic-dispatch.log` records the build and binary verification.
Binary SHA-256: `b8133dd330ad4347c2e8945899f2688966033b1ae35218a98990ea9acf58f490`.
No recipe cleaning, version change, packaging, deployment, or robot/lycopod
access was performed.

No robot was contacted, played audio, restarted, or flashed. Silence-only
self-conversation, real acoustic tails, UX timing, repeated independent
questions, and default-off/on hardware behavior still require a separately
authorized supervised trial. Backend contextual memory is not guaranteed.

## 14. OTA112 build and installation handoff (2026-09-07)

**Built and verified, not installed. OTA111 remains the reported installed
firmware; no robot or lycopod access was performed during this build.**

| Artifact | Verified value |
|---|---|
| Version | `3.0.1.112d` |
| Image | `/home/unbuilt/T909/vectoros/wire-os/_build/vicos-3.0.1.112d.ota` |
| Size | `162037760` bytes |
| SHA-256 | `a8a186c615ca091be947effdf0d986655fa79884ebcda520d50a0cc0629991bc` |
| Package inputs | Workspace `_build/aec-experiment-112/` |
| Evidence and decrypted payloads | Workspace `_build/aec-experiment-112-validation/` |

Version 112 and both output directories were checked unused before building.
The existing `vic-yocto-builder-7:latest` image built the full firmware as
`unbuilt`, with the existing workspace/dependency/cache mounts. Commands,
executed from the workspace root, were:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 112 build
bash anki/victor/tools/audio/build_aec_experiment.sh 112 package
```

These are historical commands, **not instructions to overwrite/rebuild 112**.
Choose another unused version for a subsequent build. Only the version recipe
was cleaned to refresh the embedded version. The exact stale generated
`apq8009-robot-sysfs.ext4` was inspected and regenerated by the existing packaging
script; no broad/recipe clean was performed. Existing packaged images/results
were retained, and all eleven prior OTA files (101–111) passed their
prebuild SHA-256 checks afterward. No commit or push was made.

### Byte-level and build verification

- `build.log`: all 4569 Yocto tasks succeeded (4539 reused).
- `victor-compile.log`: 1403 build steps, including regenerated cloud/behavior
  CLAD, engine C++ and robot CPPLite protocol serializers, robot supervisor
  `messages.cpp`, the corrected mic queue dispatcher, session component and
  follow-up behavior. All firmware targets were built/installed together.
- `package.log`: both encrypted payloads were extracted/decrypted/decompressed;
  their sizes and SHA-256 matched the manifest and package inputs.
  Decrypted `/etc/os-version` is `3.0.1.112d`.
- `followup-verification.log` and `.json`: decrypted `vic-engine`, `vic-anim`,
  `vic-robot`, `vic-cloud`, `libcozmo_engine.so`, `libaudio_engine.so`,
  `libankiutil.so`, and `libutil_audio.so` match the actual compiled outputs.
  In particular, cloud matches **`anki/victor/_build/vicos/Release/vic-cloud`**,
  SHA-256 `060c1718c7689e6f1e54436a508eb95e1759680a7f84147bdf1e6fc50bbc8b86`,
  not merely the formerly stale `bin/vic-cloud`. The full build also refreshed
  that installed copy. Engine implementation lives in `libcozmo_engine.so`;
  matching only the small `vic-engine` launcher would not be sufficient.
- The three decrypted behavior JSON resources match both source and build.
  Follow-up is enabled, with five turns, 120-second admission limit and 250 ms
  settle; `ConversationFollowUp` is registered exactly once, last in
  `VoiceFeatures`. Packaged anim environment/service contain no AEC opt-in.
  The audio library is byte-identical to verified OTA111; absent
  `ANKI_AEC_EXPERIMENT` continues to select **off**.
- `mic-dispatch-tests.log`: both production-dispatch unittest cases pass,
  including the negative missing-forwarding check. `native-tests.log`: 24
  session/playback tests pass normally and under ASan/UBSan, five generated
  protocol tests pass, and four ASan/UBSan capture-boundary tests pass.
- `cloud-tests.log`: 20 targeted cloud-audio/correlation tests pass with
  `go test -tags nolibopusfile -race -count=1`. The builder lacked native Opus
  development metadata for host tests, so these ran using the already-installed
  host Opus 1.3.1 and existing Go 1.24.4 with cached dependencies; no installation
  or backend access was necessary. Firmware itself was built in Docker.

### What else needs changing?

1. **Manually install the complete OTA112 and reboot**, using the established
   development-signed OTA procedure on a compatible development/unlocked robot.
   This is not a retail/production-signed or OSKR-signed image. Keep OTA111 for
   rollback. Do not hot-swap only engine/anim/cloud: their internal protocol,
   supervisor and shared library must move together. Installation was not
   performed by this build task.
2. **Keep the existing working backend, providers and server configuration.**
   Repository inspection shows unchanged Chipper request APIs and unchanged
   cloud-audio HTTP GET-by-`audio_id` / PCM or text-POST contracts. New stream
   and playback IDs are local CLAD ownership fields, not backend request
   schema changes. No lycopod change is required by this implementation.
   Existing endpoint reachability/authentication remain prerequisites, not
   something verified by a workstation build.
3. **No follow-up enable setting or AEC mode configuration is required.**
   Follow-up is already on in the image; sequential listening does not require
   reference/AEC mode. Optional single-turn rollback is the resource
   `multiTurnVoice.enabled=false` or the available runtime kill switch described
   in section 13. Experimental systemd overrides under `/run`, including prior
   AEC experiments, disappear on reboot and are not supplied by this OTA.
   Do not recreate AEC overrides merely to use follow-up. Any independently
   required backend endpoint override must retain its existing supported
   configuration; ephemeral `/run` overrides are not persistent configuration.
4. **Perform the supervised hardware acceptance sequence after installation.**
   Start with eligible KG answers and independent questions, wait for the
   listening indicator, then test silence, cancellation and the turn limit.
   Other command families do not authorize continued listening. Validate
   default-off separately. Real acoustic behavior and timing are unverified.

Automatic follow-up means fresh per-utterance requests, not guaranteed
contextual dialogue: `cloud/internal/voice/stream/connect.go` still creates a
fresh Chipper session ID for each stream. Shared backend memory, pronoun
resolution and conversational history were neither changed nor verified.

## 15. OTA112 follow-up timing investigation and correction (2026-09-07)

### Hardware evidence, and what it does not establish

Read-only SSH confirmed `/etc/os-version = 3.0.1.112d`. Only the current boot's
`journalctl -b -a --no-pager -o short-monotonic` and log-directory metadata were
read; `-a` matters because normal journal output hides structured engine
messages as `[nB blob data]`. No robot configuration, restart, playback,
recording, capture request, deployment, or lycopod access was performed.

Evidence is in workspace `_build/followup-validation/ota112-ux-timeline.log`
and `ota112-close-timeline.log`. The retained timeline contains lifecycle/type
metadata, not transcripts, answers or credentials. Representative boot-monotonic
timestamps:

| Event | First answer | Second answer | Third answer |
|---|---:|---:|---:|
| Success celebration starts | 248.358 | 297.923 | 328.882 |
| KG behavior ends | 251.661 | 301.221 | 332.180 |
| Vic-cloud new connection diagnostic | 252.857 | 302.426 | 333.377 |
| Follow-up face listening animation starts | 252.921 | 302.477 | 333.438 |
| Backend `IntentGraphResponse` received | 256.760 | 304.124 | 335.602 |
| Listening get-out / normal command routing | 256.818 | 304.161 | 335.662 |

The celebration consumes **3.30 seconds**; behavior end to face listening takes
another **1.26 seconds**. The accepted cloud-renderer `Completed` handler invokes
that celebration, so its start is a source-supported completion proxy, **not a
direct acoustic last-sample measurement**. The existing 250 ms settle and
completion of the ordinary earcon are additional sequential boundaries.
The connection diagnostic is likewise only a request-start proxy, not an exact
first-microphone-sample timestamp.

The two short face intervals are **1.684 seconds** and **2.223 seconds**. Both
end immediately after an actual backend response, not a nine-second timeout.
The third proceeds into normal drive-off-charger routing. The first response
also produced a missing-required-weather-parameters warning. These observations
must not be described as a universal two-second local recording limit, or proof
of silence/acoustic self-triggering. OTA112 did not emit sufficient shipping
capture/LED/sample telemetry to determine exact raw sample boundaries or what
the user said; no diagnostic audio was acquired to infer it.

### Root causes and surgical corrections

1. **Unnecessary post-answer delay:** continuing KG answers always played the
   additional ~3.3-second success celebration. Omit that extra animation only
   for the current, safe, still-admissible conversation response. Single-turn,
   disabled, unsafe, expired and final-turn answers retain their existing
   celebration. Explicit successful renderer/TTS completion, required local
   TTS get-out, owning behavior deactivation, capture quiescence and the full
   settle remain mandatory. This does not open recognition during playback.
2. **Backpack indication preceded actual capture:** the old wake-wordless path
   called `SetWillStream(true)` before the earcon completed; backpack streaming
   lights use `willStream || isStreaming`. Fresh-follow-up lights now stay off
   through the earcon. Allocating a fresh job is no longer reported as capture
   ready: the first accepted fresh processed block gates the backpack callback,
   matching `MicStreamState(open=true)` acknowledgement and cloud hotword/request
   start in anim's update. Duplicate starts cannot manufacture an early ack;
   cancellation during the earcon and absent cloud clients do not light or open.
   Legacy wake-word/single-turn light and overlap behavior is unchanged.
3. **Face indication could outlast capture:** the behavior formerly looped the
   listening animation while awaiting a cloud result even after anim had closed
   capture. It now performs listening get-out on matching capture closure, while
   retaining ownership and accepting the later result under the existing guard.
   It neither prematurely cancels recognition nor replays the get-out each tick.
4. **Insufficient shipping evidence:** added structured `voice.followup.*`
   events for successful response boundary, quiesced/settle, open request,
   accepted capture readiness, engine acknowledgement, collector stop and session
   end. Cloud logs now correlate request open, first audio, microphone-done,
   terminal result/error, elapsed time and nominal received audio duration by
   stream ID. Result telemetry includes the action name, never parameters or
   transcript/answer text. These separate visual readiness, processed capture,
   uploaded samples and cloud termination on the next supervised trial.

The continuously running raw microphone/DSP path is **not** the same as the
bounded follow-up collector. The existing `freshCapture` sequence boundary
(`captureSequence + 2`) and source-time boundary (creation time + one 10 ms block)
still reject queued, straddling and late old-source blocks; no playback/settle
overlap is added. Vic-cloud batches accepted samples into **120 ms** requests.
That batching does not explain a fixed multi-second wait. Fresh collector
creation does not advance `_streamingAudioIndex`; only transmitted accepted
chunks consume its existing sample-count cap (~6.05 s nominal, not an exact
wall-clock timer). There is no global capture/request-limit change.

**Known limitations:** the ordinary earcon still completes before capture, so
follow-up is not instantaneous at answer EOF. The observed extra 3.3 s
celebration is removed, not overlapped with capture. Processing/transport and
engine/LED update ticks still add latency. A backend can return an intent,
silence, unmatched result or error before the mic cap; a ~2-second window can
therefore still be legitimate. Holding a fake listening light or extending a
timeout after that response would mislead the user, not restore recognition.
Source timestamps of zero still use the pre-existing local-arrival fallback;
these tests cannot prove acoustic freshness when the upstream timestamp is
unavailable. Real acoustic tails and early endpoint decisions require a
supervised trial, not inferred transcripts.

### Validation and packaging boundary

All paths below are relative to the workspace root. Targeted validation:

```sh
TMPDIR="$PWD/_build/followup-validation" \
FOLLOWUP_TIMING_OUTPUT="$PWD/_build/followup-validation/ux-timing" \
MIC_DISPATCH_OUTPUT="$PWD/_build/followup-validation/ux-dispatch" \
PYTHONPATH="$PWD/anki/victor/test/tools" PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest testFollowUpTiming testMicMessageDispatch -v
```

- Four unittest cases pass: six GTests compile **actual source bodies** for
  capture start/stop, update's request-opening block, successful-response get-out
  and face behavior; four ASan/UBSan tests compile the actual collector/readiness
  methods against `MicDataInfo`'s real header; the existing dispatcher harness
  passes its three GTests with cheats off/on and negative missing-dispatch test.
  Hardware services and animation delegates are recording doubles, not a live
  behavior tree. Logs: `ux-targeted-tests.log`, `ux-timing/`, `ux-dispatch/`.
- 25 session/playback GTests rebuilt and passed with ASan/UBSan, including the
  new current-token/admission/disabled/final-turn celebration policy checks.
  Evidence: `ux-native/session-results.log`.
- The existing 20 cloud-audio/correlation/request-budget tests pass with
  Go 1.24.4, `-tags nolibopusfile -race -count=1`; `ux-cloud-tests.log`.
  Host Go 1.19 is too old; use `anki-deps/go/dist/1.24.4/go/bin/go`.
- Actual incremental ARM builds pass for `vic-anim`, `vic-engine`
  (including `libcozmo_engine.so`) and `vic-cloud`. The builder is the existing
  `vic-yocto-builder-7:latest`, user `unbuilt`, original workspace and existing
  dependency/Go/user-cache/ccache mounts, with workspace `TMPDIR`. Target command:
  `cmake --build anki/victor/_build/vicos/Release --target vic-anim vic-engine vic-cloud -- -j4`.
  Logs: workspace `_build/aec-experiment-112-validation/ux-timing-arm-build.log`
  and `ux-timing-arm-final.log`. No Yocto clean, install or package target ran.

An initial overly broad Python discovery also selected unrelated AEC diagnostic
tests without their required `TMPDIR`; those setup failures are recorded in
`ux-production-tests.log`, not claimed as a passing full suite. Native host
compilation needed the existing GCC/GTest compatibility define
`-D__has_warning(x)=0`; the targeted collector harness excludes unused disk/FFT
services rather than adding mock implementations to production.

OTA112 remains byte-identical, SHA-256
`a8a186c615ca091be947effdf0d986655fa79884ebcda520d50a0cc0629991bc`;
all eleven prior OTA images passed their existing checksum list. Existing
packaged payloads, dirty AEC/cloud/follow-up work and old evidence are retained.
Only incremental source-build outputs and new, uniquely named validation logs
were updated. These are **not a newly packaged or installed firmware**.

The subsequent explicitly ordered **new-version complete OTA113** is built and
verified in section 16, without replacing 112. The remaining next step is a
manual installation followed by a supervised trial. Check the new correlated events against
the face/backpack observation, confirm the ~3.3-second celebration gap is gone,
and distinguish backend early results from a collector/request timeout. Test
speech after the ready cue, silence, cancellation, repeated eligible answers and
default-off behavior. No backend changes or longer global timeouts are justified
by the currently available evidence.

## 16. OTA113 timing-fix build and installation handoff (2026-09-07)

**Historical build handoff: built and verified without installation by that task.**
The user subsequently confirmed OTA113 installed and recording looks good.
The packaging task did not access the robot or lycopod, flash,
restart, configure, capture or play audio, or create a commit.

| Artifact | Verified value |
|---|---|
| Version | `3.0.1.113d` |
| Image | `/home/unbuilt/T909/vectoros/wire-os/_build/vicos-3.0.1.113d.ota` |
| Size | `162037760` bytes |
| SHA-256 | `397d939649b4a5dd27f46467f4665dae82be1e39f44992a132ddd35fa373d9f6` |
| Package inputs | Workspace `_build/aec-experiment-113/` |
| Evidence and decrypted payloads | Workspace `_build/aec-experiment-113-validation/` |

Version 113 and both output directories were unused before this run. The full
firmware build used the existing `vic-yocto-builder-7:latest` image as `unbuilt`,
with the original workspace/dependency/cache mounts and a workspace-local
`TMPDIR`. Historical commands, from the workspace root:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 113 build
bash anki/victor/tools/audio/build_aec_experiment.sh 113 package
make -C ota verify-boot-dev
python3 _build/aec-experiment-113-validation/verify-followup.py
```

Do not rerun these commands over the retained version-113 evidence; choose a
new unused version for another build. `build.log` records all **4569 tasks
succeeded, 4539 reused**. This is an incremental complete firmware build, not a
claim of recompiling every source: the previously rebuilt matching CLAD,
robot, engine/shared-library, anim and cloud targets were retained, resources
were refreshed, and the complete firmware was installed into a new versioned
rootfs. `victor-compile.log` and `victor-install.log` retain those steps,
including updating the installed `bin/vic-cloud` from the real Go target.
Only `anki-version` was cleaned to refresh the embedded version. The packaging
script regenerated only its exact stale generated `apq8009-robot-sysfs.ext4`,
whose metadata was saved first; no worktree clean or broad deletion occurred.

### Byte-level verification and regression evidence

- `package.log`: encrypted boot/system payloads were extracted, decrypted and
  decompressed; sizes and SHA-256 match both manifest and packaging inputs.
  The decrypted OS version is exactly `3.0.1.113d`.
- `boot-signature.log`: the development boot signature passes the existing
  Makefile check and a separate verification of the signature extracted from
  the **decrypted packaged boot**. Its unsigned content matches the build input,
  with only zero padding after the signature.
- `followup-verification.log` and `.json`: decrypted `vic-engine`, `vic-anim`,
  `vic-robot`, **`vic-cloud`**, `libcozmo_engine.so`, `libaudio_engine.so`,
  `libankiutil.so` and `libutil_audio.so` match actual build outputs.
  Cloud is compared to **`anki/victor/_build/vicos/Release/vic-cloud`**, not an
  assumed-fresh installed `bin/vic-cloud`. All three follow-up behavior resources
  match both source and build, including exactly one lowest-priority registration.
- The three changed timing binaries are byte-identical to the reviewed
  incremental timing build recorded in section 15 and different from original
  OTA112. `timing-build-equivalence.log`, `prebuild-timing-artifacts.sha256` and
  the verifier establish this, rather than relying on a version bump alone:

  | Binary | SHA-256 |
  |---|---|
  | `vic-anim` | `f73a2feadb82c550ee162bae9183c22b2c7f419c173dc1d98fab87ca72845f54` |
  | `libcozmo_engine.so` | `e1f76a5000f9659297c2760ca632698b8b494607c120681c721ee4ad859dbde2` |
  | `vic-cloud` | `e8de16c314f37ddde1942e5399dcd5d75a30af5aa8625710d1905a40d1a94837` |

- Packaged session/capture timing diagnostic markers are present. Follow-up is
  **default on** (five turns, 120-second admission limit, 250 ms settle).
  Packaged anim environment/service contain no AEC opt-in; the audio library
  remains byte-identical to OTA111/112, so AEC remains **off by default**.
- `targeted-tests.log`: all four `testFollowUpTiming` / `testMicMessageDispatch`
  Python regression cases pass, rebuilding their production-source C++ harnesses
  in the new evidence directory. `native-tests.log`: the existing 25 sanitized
  session/playback tests and five generated-protocol tests pass again.
  `cloud-tests.log`: all 20 `internal/voice` tests pass with existing host
  Go 1.24.4/Opus, `-tags nolibopusfile -race -count=1`, offline cached dependencies.
  These are targeted tests, not a full legacy suite or hardware acceptance.
- `prior-artifacts-preserved.log`: all **1902** pre-existing files under the old
  `aec-experiment-*` package/evidence directories and OTA101–112 pass their
  prebuild checksums. OTA112 retains SHA-256
  `a8a186c615ca091be947effdf0d986655fa79884ebcda520d50a0cc0629991bc`.
  `source-preservation.log` verifies all 83 pre-existing dirty source files were
  unchanged by building, packaging and testing; only this handoff document was
  subsequently updated. Original timing-test results were not overwritten.

### Manual next steps and the short-window limitation

1. On the workstation, verify the image before transferring/installing:

   ```sh
   cd /home/unbuilt/T909/vectoros/wire-os
   printf '%s  %s\n' \
     397d939649b4a5dd27f46467f4665dae82be1e39f44992a132ddd35fa373d9f6 \
     _build/vicos-3.0.1.113d.ota | sha256sum -c -
   ```

2. Manually install the **complete OTA113** with the established
   development-signed OTA procedure on the compatible development/unlocked
   robot, then reboot and check `/etc/os-version` is `3.0.1.113d`. This is not
   production/retail- or OSKR-signed. Keep the intact OTA112 for rollback;
   do not hot-swap individual binaries or libraries.
3. Keep the working backend/provider configuration. **No follow-up enable,
   reference mode, AEC configuration or lycopod change is needed.** Do not add
   an AEC override for this sequential-listening feature.
4. Ask an eligible knowledge-graph question normally. After the answer, wait
   for the listening indicator, then speak a **natural complete utterance**—no
   artificial two-second cutoff or specially shortened phrasing. Compare the
   response-to-ready delay and whether the lights accurately cover capture.
   Also check silence, cancellation and repeated eligible questions under
   supervision. Default-off acceptance remains a separate optional trial.

The verified code omits the extra ~3.3-second celebration on admissible
continuing answers; real-device latency improvement is **not yet measured**.
The ordinary earcon, capture quiescence, full settle and processing/transport
boundaries still apply. OTA112's observed ~2-second windows ended after the
**backend returned early**, not at a proven local two-second limit.
**Model/backend endpointing remains unproven and unchanged**; this OTA does
not promise longer backend recognition windows or conversational memory.
If a natural utterance is cut short, correlate the new stream-ID timing
events for capture-ready/stop, uploaded duration and terminal response before
attributing it to the robot, acoustics, model or backend endpointing. No fake
extended listening indication or global timeout extension was introduced.

## 17. Approved KnowledgeGraph follow-up routing correction (2026-09-07)

**Historical OTA114 policy:** section 19 corrects the empty-query assumption
after read-only on-device evidence. Other routing and cleanup guards remain.

The user confirmed OTA113 installed and recording looks good, then approved
correcting the automatic Knowledge-mode turn to `StreamingKnowledgeGraph`.
This section records **source changes and workstation validation only**:
no image/package/version was produced, no robot was accessed, and lycopod
was neither accessed nor changed. Sections 13–16 retain historical Normal-route
evidence; those traces are not evidence of the route implemented here.

### End-to-end routing

1. An ordinary wake remains `Normal` → `WithIntentGraphOptions` →
   `NewIntentGraphStream` → `StreamingIntentGraph`. Initial prompted questions
   still use their existing separate KG capture.
2. Only `UserIntentComponent::StartFollowUpStreaming` now sends
   `KnowledgeGraph`, retaining the same stream ID, `freshCapture=true`,
   earcon-before-capture, fresh processed-block/light acknowledgement and
   approximately **6.05-second sample cap** from OTA113.
3. Vic-cloud's existing mode branch selects `WithKnowledgeGraphOptions` →
   `NewKGStream` → **`StreamingKnowledgeGraph`**. Its existing adapter returns
   `intent_knowledge_response_extend` (non-bypass). No backend/API/schema change
   is required.
4. UIC validates that owned automatic result before making it pending, then
   changes only the dispatch name to `intent_knowledge_response_extend_bypass`.
   `answer`, `query_text`, `response_id`, `audio_id`, and
   `cloud_audio_available` remain intact. The existing KG bypass path consumes
   the ready answer and pins its cloud-audio buffer; it does not generate/play
   the “ready for a question” prompt or start another KG question capture.
   Existing answer get-in/searching/renderer animations are retained.
5. The result enters `AwaitingClaim`; a matching voice-intent claim and successful
   cloud playback or TTS fallback can authorize the next KG follow-up after
   deactivation/quiescence/settle. Normal `knowledge_question`, unknown or
   unrelated result types in this automatic capture are terminal, not
   redispatched into another question prompt.

The automatic engine result guard is now **65 seconds from capture-ready
acknowledgement**, allowing the existing **60-second cloud-audio KG request**
to finish. A result immediately before the guard is accepted; at or after it,
it is rejected. Capture closure clears listening UX but does not cancel the
answer wait. With cloud audio unconfigured, vic-cloud retains its 9-second
transport timeout; that actual error still ends the turn earlier than the
conservative 65-second engine guard. Ordinary Normal requests retain 9 seconds.
Opening remains bounded by 5 seconds and responder claim by 2 seconds.
The 120-second limit still controls admission, not an already admitted answer.

### Silence and spoken cancellation: the actual available contract

Inspection was limited to wire-os, its Go adapter, and its locally cached
Chipper client/protobuf dependency:

- `KnowledgeGraphResponse.QueryText` is explicitly the transcribed user text.
  `sendKGResponse` forwards it as `query_text` without interpretation.
  `SpokenText` becomes `answer`; it is **not** a user transcript.
- `CommandType` is a free-form string forwarded as `answer_type`; this client
  exposes no typed KG silence/stop signal or guaranteed vocabulary for it.
  No assumptions about lycopod's implementation were made.
- Empty/whitespace `query_text` now ends an automatic turn as `empty_query`,
  even if an answer/audio handle was supplied. This is fail-closed behavior,
  **not proof of acoustic silence**: proto3 cannot distinguish an omitted
  transcript from an empty transcript. A backend omitting transcripts for valid
  speech will therefore terminate these automatic turns instead of answering.
- Missing/malformed fields, empty/whitespace answers, explicit
  `intent_system_noaudio`, unknown/unmatched results and transport errors end
  without answer dispatch, a spoken reprompt or another automatic capture.
  Timeout/error telemetry is not relabeled as silence. Session cleanup stops
  only the owned capture/cloud worker; prebuffered rejected-answer audio is
  never handed to the renderer.
- A deliberately narrow local stop policy accepts the **entire user
  transcript** `stop`, `cancel`, `stop listening`, or `end conversation`,
  ignoring ASCII case, outer whitespace and trailing `.`, `!`, `?`.
  These exact English phrases terminate without playing the returned answer.
  Substrings such as “what does stop listening mean?” do not match, and no
  answer-text keyword detection is used.
- Spoken cancellation is available only when the final KG response exposes
  the recognized transcript. It is not immediate ASR/barge-in, is not guaranteed
  for other phrases/languages or transcription mistakes, and cannot stop an
  answer already playing merely because someone speaks. Existing physical,
  mute, safety, superseding-wake and explicit disable cancellation still apply.
  A backend hallucinating **both** a nonempty query and answer on silence
  cannot be detected reliably from this contract. Supervised hardware silence
  and stop acceptance remains outstanding.

### Context is not created by changing the endpoint

Each utterance still receives a fresh Chipper session ID. The initial ordinary
wake uses IntentGraph; the first automatic follow-up uses KnowledgeGraph with
no forwarded initial transcript, answer, shared session ID or conversation
history. Consequently a first follow-up such as “how big is it?” has **no
client-provided antecedent**, even if later backend behavior appears contextual.
The same limitation applies between consecutive KG follow-ups. Independent
complete questions are supported; backend memory/pronoun resolution is neither
implemented nor verified. No persistence, duplex recognition or AEC change was
introduced.

### Changed files and reproducible evidence

Production changes are restricted to:

- `engine/aiComponent/behaviorComponent/userIntentComponent.cpp`: automatic
  KG request and validated answer promotion before pending dispatch.
- New `engine/aiComponent/behaviorComponent/knowledgeFollowUpRouting.h`:
  automatic-only result/empty-query/stop policy.
- `engine/aiComponent/behaviorComponent/conversationSessionState.h`: 65-second
  KG result guard.

Regression files: `test/engine/testConversationSessionState.cpp`,
new `test/tools/testKnowledgeFollowUpRouting.py` and
`test/tools/fixtures/testKnowledgeFollowUpRouting.cpp`, and
new `cloud/internal/voice/stream/followup_routing_test.go`.
No production cloud, anim, CLAD, resource, capture-limit or renderer source
changes were necessary.

Workspace evidence directory: **`_build/kg-followup-validation/`**.

- `native/results.log`: **34 GTests pass under ASan/UBSan**, including nine
  new routing tests plus existing session and cloud-playback policy tests.
  The new harness compiles unchanged production UIC start/stop, control-event
  drain, cloud identity/audio handling, KG update and answer-consumption bodies,
  plus real generated cloud messages and the production routing helper.
  It covers Normal versus automatic KG requests, answer-field preservation,
  no extra question capture, another admitted KG turn, empty/silence/stop/
  malformed/error outcomes, 10/60/65-second boundaries, duplicate/stale results,
  response pinning and stale/current cleanup. Hardware transport, intent-map
  dispatch and animation services are recording doubles: this is not a live
  behavior-tree/renderer integration test.
- `go-tests.log`: **21 top-level targeted Go tests pass with `-race`**.
  The new RPC test drives the existing actual Chipper client against a local
  TLS in-memory gRPC server, observing `StreamingIntentGraph` versus
  `StreamingKnowledgeGraph` and their session fields. The adapter test verifies
  that KG returns non-bypass results and preserves empty queries, stop
  transcripts, answers and audio correlation. Existing request-budget,
  HTTP/cloud-audio and stale-worker/cancellation tests also pass.
- `existing-native.log`, `timing/`, `dispatch/`: all four existing production
  timing/dispatch Python cases pass (including the deliberate missing-dispatch
  negative test), retaining OTA113 collector/light/earcon/celebration/get-out
  coverage. The existing five generated-protocol tests also pass.
- `arm-engine.log`: actual ARM `vic-engine` changed target succeeds in
  `vic-yocto-builder-7:latest`, compiling UIC, the session component,
  follow-up behavior and dependent engine sources, then relinking
  `libcozmo_engine.so` and `bin/vic-engine` (seven build steps).
  Both outputs are verified ARM EABI5 ELF, not native test stubs:
  - `bin/vic-engine`: `0e4d1a395492d26c0b31528b648e5af92b58ebc5520464388fe741f83071b583`
  - `lib/libcozmo_engine.so`: `499e11cce5f4e6767a05db5d92bf9eae3d0c31282435d75de834be43d04119ec`
- `prior-otas.log`: OTA101–113 all match their existing checksums.
  `source-preservation.log`: all 83 prior dirty source files checked against
  the OTA113 source manifest; only this document, UIC, session deadline and its
  corresponding test changed. New routing helper/tests are additional files.

Commands from the workspace root:

```sh
KG_FOLLOWUP_OUTPUT="$PWD/_build/kg-followup-validation/native" \
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s anki/victor/test/tools -p testKnowledgeFollowUpRouting.py -v

FOLLOWUP_TIMING_OUTPUT="$PWD/_build/kg-followup-validation/timing" \
MIC_DISPATCH_OUTPUT="$PWD/_build/kg-followup-validation/dispatch" \
PYTHONPATH="$PWD/anki/victor/test/tools" PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest testFollowUpTiming testMicMessageDispatch -v
```

Go uses existing `anki-deps/go/dist/1.24.4/go/bin/go` and host Opus with cached
dependencies, workspace `TMPDIR`, and `-tags nolibopusfile -race -count=1`
on `./internal/voice ./internal/voice/stream`, selecting
`Test(FollowUp|KnowledgeGraphRequestBudget|CloudAudio|MaybeSendCloudAudio|SendCloudAudio|StreamCloudAudio|StreamIdentity|CorrelatedCloud|LateLegacy|InvalidStart|RetiredReceiver)`.
The builder lacks native Opus metadata and host default Go is 1.19; the final
passing run uses the already available correct toolchain without installation.
The ARM command, using the existing workspace/dependency/cache mounts, is
`cmake --build anki/victor/_build/vicos/Release --target vic-engine -- -j4`.
No packaging, flashing, robot restart/playback, commit, broad clean or backend
access was performed. Old OTA/package artifacts and unrelated dirty work remain.

### Review correction: release rejected prebuffered audio (2026-09-07)

The original section-17 cleanup test incorrectly expected the current answer
buffer to survive `StopConversationStream(current, true)`. Although the answer
was not rendered, an empty-query, spoken-stop or malformed automatic result
could retain up to **1,920,000 bytes of PCM** after terminating the session.

UIC now resets the buffer and expected response ID under its **already-held
mutex** when automatic result dispatch fails, rejects subsequent stream
messages and drops queued control events for that rejected turn. It does not
call the recursively locking `ClearCloudAudio`. The existing
`streamId == _expectedStreamId && rejectResults` stop branch also resets the
buffer and response ID. Capture-only stops (`rejectResults=false`), successful
dispatch and successful result/stream-close ordering do not clear answer audio;
stale terminal stops cannot clear a newer stream.

Only UIC, the two existing routing regression files and this document changed
from the 87-file pre-review dirty-source manifest. No session policy, routing
helper, cloud/anim/renderer source, package, version or robot/backend operation
was changed by this review correction.

New evidence: **`_build/kg-followup-review-validation/`**.

- `native/results.log` and `plain-native/results.log`: **35 GTests pass** with
  ASan/UBSan and without sanitizers respectively. Tests send actual generated
  audio-start/chunk messages to the extracted production UIC bodies, prebuffer
  the full 1.92 MB limit, and assert rejected buffer contents/capacity,
  correlation IDs and metadata are reset. Empty/whitespace queries, all local
  stop phrases, missing fields and malformed JSON are covered. Current terminal
  stop clears; a stale stop preserves the newer full buffer. Capture-only stop
  and successful dispatch retain the PCM until the extracted production
  `ConsumeCloudAudioPcm` returns the exact bytes to the renderer-facing caller.
- `rejected-cleanup-negative/results.log`: removing only the new cleanup from
  generated harness bodies reproduces **four failing regression tests**;
  the success path still passes. Production source is never modified for this
  negative control.
- `existing-native.log`: all **four** existing timing/dispatch Python cases
  pass. `go-tests.log`: all **21** targeted production Go route/audio tests pass
  with `-race`, including actual local TLS gRPC IG/KG endpoint selection.
- `arm-engine.log`: actual ARM `vic-engine` target passes and relinks
  `libcozmo_engine.so` and `bin/vic-engine`. An initial incorrect dependency
  mount failed to find the SDK; CMake regeneration also expired cached
  command-line defines, causing an unrelated `ANKI_DISABLE_ALEXA` error.
  The final run mounts workspace `anki-deps` at `/home/unbuilt/.anki`, restores
  the existing Release-script defines and uses the existing dependency/cache
  mounts. Regeneration required 1,132 build steps; no source workaround or
  broad clean was used. Both outputs are ARM EABI5 ELF (`arm-artifacts.log`):
  - `bin/vic-engine`: `0e4d1a395492d26c0b31528b648e5af92b58ebc5520464388fe741f83071b583`
  - `lib/libcozmo_engine.so`: `46cfc9f41813f98e215642582e05a2024b45b16e97dbfa6494b9d2a50d328150`
- `source-preservation.log`: the other **83** pre-review dirty files match
  their recorded SHA-256 hashes. Existing OTA/package artifacts were not used
  or modified.

Commands from the workspace root (the evidence-local Python wrapper invokes
the existing unittest runner for sanitized/plain/negative-control variants):

```sh
PYTHONDONTWRITEBYTECODE=1 \
  python3 _build/kg-followup-review-validation/validate-native.py

FOLLOWUP_TIMING_OUTPUT="$PWD/_build/kg-followup-review-validation/timing" \
MIC_DISPATCH_OUTPUT="$PWD/_build/kg-followup-review-validation/dispatch" \
PYTHONPATH="$PWD/anki/victor/test/tools" PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest testFollowUpTiming testMicMessageDispatch -v

GO="$PWD/anki-deps/go/dist/1.24.4/go/bin/go"
export TMPDIR="$PWD/_build/kg-followup-review-validation/go-work"
(cd anki/victor/cloud && "$GO" test -tags nolibopusfile -race -count=1 \
  ./internal/voice ./internal/voice/stream \
  -run 'Test(FollowUp|KnowledgeGraphRequestBudget|CloudAudio|MaybeSendCloudAudio|SendCloudAudio|StreamCloudAudio|StreamIdentity|CorrelatedCloud|LateLegacy|InvalidStart|RetiredReceiver)' -v)
```

The final ARM configure/build invocation is recorded in
`_build/kg-followup-review-validation/arm-command.sh`; it uses
`cmake --build anki/victor/_build/vicos/Release --target vic-engine -- -j4`.
No robot or lycopod access, packaging, deployment or commit was performed.

## 18. OTA114 approved KG routing build and manual handoff (2026-09-07)

**Historical artifact:** OTA114 contains the empty-query rejection diagnosed in
section 19. The source correction there has not been packaged or installed.

**Built and verified; not installed by this task.** This complete firmware
packages section 17, including its reviewed rejected-audio cleanup, while
retaining OTA113 recording/light timing. Section 17's source-only statements
describe those earlier implementation/review tasks, not this subsequent build.
No robot or lycopod access, flashing, restart, configuration, recording,
playback or commit was performed.

| Artifact | Verified value |
|---|---|
| Version | `3.0.1.114d` |
| Image | `/home/unbuilt/T909/vectoros/wire-os/_build/vicos-3.0.1.114d.ota` |
| Size | `162037760` bytes |
| SHA-256 | `2b76e083ef01e00cf9abe685868c90c2cd8e9b8cce16e70df038ff63cb2c8400` |
| Package inputs | Workspace `_build/aec-experiment-114/` |
| Evidence and decrypted payloads | Workspace `_build/aec-experiment-114-validation/` |

### Complete build and byte-level verification

OTA114, package and validation paths were all unused before reservation.
Historical commands, run from the workspace root:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 114 build
bash anki/victor/tools/audio/build_aec_experiment.sh 114 package
python3 _build/aec-experiment-114-validation/verify-followup.py
make -C ota verify-boot-dev
python3 _build/aec-experiment-114-validation/verify-boot.py
```

Do not rerun over retained evidence; select a new unused version/workspace for
another build. The full build used existing `vic-yocto-builder-7:latest`, user
`unbuilt`, original workspace/dependency/cache mounts and workspace-local
`TMPDIR`. `build.log` records **4569 tasks succeeded, 4539 reused**.
`victor-compile.log` records **1146 build steps** and installation, including
rebuilding the engine/shared library and refreshing installed `bin/vic-cloud`
from the actual Go target. `victor-install.log` records rootfs installation.
This is an incremental complete firmware build, not an engine-only image or a
claim that every unchanged source was recompiled. Only `anki-version` was
cleaned; packaging regenerated its exact stale generated sysfs intermediate
after recording metadata. No broad clean or destructive worktree operation ran.

- `package.log`: encrypted boot and system payloads were decrypted/decompressed;
  their sizes and SHA-256 match both manifest and package inputs. Decrypted
  `/etc/os-version` is exactly `3.0.1.114d`.
- `followup-verification.log` / `.json`: decrypted engine, anim, robot, cloud,
  `libcozmo_engine.so`, `libaudio_engine.so`, `libankiutil.so` and
  `libutil_audio.so` match actual compiled outputs. Cloud is compared to
  **`anki/victor/_build/vicos/Release/vic-cloud`**, not an assumed-fresh
  `bin/vic-cloud`. The small engine launcher alone is not the implementation.
- `reviewed-build-equivalence.log`: decrypted `libcozmo_engine.so` is
  **`46cfc9f41813f98e215642582e05a2024b45b16e97dbfa6494b9d2a50d328150`**,
  identical to the section-17 reviewed cleanup ARM build and different from
  OTA113. Anim and cloud remain byte-identical to OTA113, retaining its
  recording/readiness diagnostics and transport behavior. `arm-artifacts.log`
  confirms the four executables and engine library are ARM ELF outputs.
- All three decrypted follow-up JSON resources match source and build:
  **default on**, five turns, 120-second admission limit, 250 ms settle, with
  exactly one lowest-priority `ConversationFollowUp` registration.
  Packaged anim service/environment contain no AEC opt-in; the unchanged audio
  library retains **AEC off by default**.
- `boot-signature.log`: the existing Makefile development-signature check
  passes, as does an independent public-key verification of the signature
  extracted from the **decrypted packaged boot**. Its unsigned bytes match
  build input and all trailing signature/image padding is zero.

### Regression and preservation evidence

- `targeted-tests.log`: all **five** selected Python cases pass. These rebuild
  the **35 ASan/UBSan GTests** in `native/results.log` for production KG
  request/result/audio handling, 65-second guard, rejected prebuffer cleanup,
  stale ownership and successful audio consumption, plus the four existing
  OTA113 timing/dispatch cases, including the negative missing-dispatch check.
  These use extracted production bodies and recording service doubles, not
  live hardware or full behavior-tree integration.
- `go-tests.log`: all **21** targeted top-level route/audio/correlation tests
  pass with existing Go 1.24.4, host Opus, `-tags nolibopusfile -race -count=1`
  and offline cached dependencies. RPC selection is tested with a local TLS
  in-memory gRPC server, not a deployed backend.
- `protocol-tests.log`: the existing unchanged generated-protocol test binary
  was rerun; all **five** tests pass. It was not rebuilt during this task.
  These are targeted regressions, not a claim of full legacy-suite coverage.
- `prior-artifacts-preserved.log`: all **2112** pre-existing files in old
  OTA/package/evidence paths (including KG implementation/review evidence)
  match their prebuild checksums. OTA101–113 and old packages remain intact.
- `source-preservation.log`: all **91** prebuild dirty-source files matched
  their recorded hashes after build/package/tests. Only this handoff document
  was subsequently updated; production source was not changed by packaging.
  The source manifest, dirty patch and status snapshot are retained here.

### Manual installation and contract limitations

1. Verify the image locally before transfer:

   ```sh
   cd /home/unbuilt/T909/vectoros/wire-os
   printf '%s  %s\n' \
     2b76e083ef01e00cf9abe685868c90c2cd8e9b8cce16e70df038ff63cb2c8400 \
     _build/vicos-3.0.1.114d.ota | sha256sum -c -
   ```

2. **Manually install the complete OTA114**, using the established
   development-signed procedure on a compatible development/unlocked robot,
   then reboot and check `3.0.1.114d`. This is not retail/production- or
   OSKR-signed. Keep intact OTA113 for rollback; do not hot-swap binaries,
   shared libraries or resources.
3. Keep working backend/provider configuration. No enable setting, AEC mode
   or lycopod change was made or is required by this client routing change.
   **Historical OTA114-only contract, superseded by OTA115 (section 20):**
   a valid **nonempty `query_text` is required** for an automatic
   answer: omitted/empty transcripts fail closed even if answer/audio exists.
   Backend reachability and that transcript contract were not verified here.
4. After an eligible answer, wait for capture readiness and ask an independent
   complete question. Automatic capture uses `StreamingKnowledgeGraph`; a
   valid owned result is promoted to bypass without changing answer/audio IDs,
   avoiding a second question prompt. Empty/malformed/stop results terminate
   and release rejected owned prebuffered audio. Test silence, exact supported
   stop phrases, successful audio/TTS fallback, repeated turns and default-off
   behavior in a supervised trial. The 65-second **result guard is not a
   65-second recording window**; OTA113's capture cap and light timing remain.

**No contextual-memory guarantee:** each utterance still has a fresh backend
session, without forwarded conversation history or a client-provided
antecedent for “it.” Changing the endpoint does not implement memory.
Silence/stop reliability depends on final transcript fidelity, is not barge-in,
and is not proven by workstation tests. Real acoustic timing, backend behavior
and post-install hardware acceptance remain outstanding.

## 19. OTA114 second-turn audio diagnosis and source correction (2026-09-07)

**Historical diagnosis/source-validation phase; reviewed fix now packaged and
verified as OTA115 in section 20.** Read-only
SSH to the authorized robot confirmed `/etc/os-version` **`3.0.1.114d`**.
Only that file and `journalctl -b -a --no-pager -o short-monotonic` were read.
No restart, configuration, capture, playback, installation or lycopod operation
was performed. Existing AEC changes/default-off policy and OTA artifacts remain.

### Observed failure, not a presumed renderer/cleanup race

Evidence: workspace **`_build/ota114-kg-audio-fix-validation/robot-lifecycle.log`**.
`read-robot-evidence.py` retains only allowlisted monotonic timing, event/type,
stream/response IDs, termination reason and byte counts. Raw journal output,
user transcripts, generated answers and authentication/session tokens are not
saved.

| Boot time (seconds) | Observed event |
|---|---|
| 151.839304 | First answer reports successful response completion |
| 152.204032 / 153.076598 | Automatic stream 2 opens / capture becomes ready |
| 158.196038 | Stream 2 returns non-bypass KG result; fetch begins for response `35157976-d368-4904-b7f2-015742b39a5c` |
| **158.199299** | **Engine ends turn 2 as `empty_query`**, before responder activation |
| 158.225076 | Owned capture-stop acknowledgement |
| 163.921916 | Cloud HTTP reader completes **176474 bytes / 173 chunks** |
| 203.201998 / 204.094717 | Another independent conversation opens automatic stream 4 / capture ready |
| 207.488750 | Stream 4 returns KG result; fetch starts for `c27cecfc-b657-49f2-b4b6-7161ca453ec4` |
| **207.520678** | **Again engine ends turn 2 as `empty_query`** |
| 209.188016 | Cloud HTTP reader completes **91716 bytes / 90 chunks** |

The two follow-ups have no intervening KG get-in/searching/answer lifecycle.
The `empty_query` reason is emitted only after successful parameter parsing,
KG type validation and string-field validation: the result reached the engine
but **was rejected**, not accepted and subsequently lost during renderer startup.

This is stronger than an observed backend GET. Production
`cloud/internal/voice/cloudaudio.go::openCloudAudio` returns a body only for
**HTTP 200**; `sendCloudAudio` logs `streamed` only after nonempty bounded
body consumption reaches EOF without error. These matching response-ID logs
therefore establish successful HTTP/body receipt. They do **not** establish PCM
delivery to engine/anim: `writeCloudAudio` silently drops a retired generation,
and its byte/chunk counters still advance. Rejection triggers actual session
cleanup → UIC owned stop → anim `cancelStream` → cloud `cancelAudioOwner`.
That cancellation, buffer clearing and dropping late audio are correct
consequences of rejection, not its root cause.

The journal also contains a later stream-5 `transport_error` at 445.357455.
That separate outcome is not relabeled as this query-validation bug; no claim
is made that this correction resolves backend/transport failures.

### Surgical contract correction

`knowledgeFollowUpRouting.h` no longer interprets an empty/whitespace
**string** `query_text` as acoustic silence. The KG protobuf string has no
presence distinction; the existing Go adapter always serializes it, including
when the backend omitted it. A valid nonempty answer can now be promoted to
bypass without fabricating a transcript or modifying answer/audio correlation.
Missing/wrong-type JSON query/answer fields still fail closed.

Unchanged: explicit `intent_system_noaudio`, empty answers, exact supported
stop transcripts, malformed/unrelated results, result deadline, duplicate and
stale stream/response guards, buffer limit, rejected-owned cleanup, normal
first wake, fresh capture/light timing, responder claim and playback IDs.
No production UIC cleanup, session lifecycle, cloud, anim or renderer change
was needed. The production fix is limited to the routing helper.

**Contract limitation:** when the backend supplies no transcript, the client
cannot recognize a spoken stop or distinguish a spurious nonempty answer on
silence from a valid answer. This intentionally replaces OTA114's incompatible
“empty transcript means reject” assumption; it does not claim to solve acoustic
silence detection. Explicit noaudio/empty answers still terminate. Backend
transcript fidelity and supervised silence/stop testing remain release gates.
No conversational-memory or pronoun-resolution guarantee is introduced.

### Actual lifecycle regressions and ARM validation

Changed regression files:
`test/tools/testKnowledgeFollowUpRouting.py` and
`test/tools/fixtures/testKnowledgeFollowUpRouting.cpp`.
The harness now additionally compiles unchanged production session
`UpdateDependent`/`CancelFollowUp`, follow-up behavior update/deactivation,
UIC readiness/error/completion/clear methods, and KG
`BeginResponseCloudAudio`/`UpdateCloudAudioStreaming` bodies.

- `native/results.log`: **38 GTests pass under ASan/UBSan**. Valid
  empty/whitespace/nonempty-query answers exercise actual accepted result →
  `AwaitingClaim` → follow-up `CancelSelf`/deactivation → component update →
  responder claim/consumption → SDK prepare and bounded PCM chunks.
  Buffered and delayed-start audio survive the handoff with no transport abort.
  Retired behavior/stream/response cleanup preserves the new owner; rejected
  results and genuine listener cancellation run actual owned terminal cleanup.
  A delayed second sentence after playback starts emits the exact expected PCM
  and completes only after audio-end. Existing error, deadline, duplicate,
  ordinary-wake and empty-answer/stop tests remain.
- The fixture now value-initializes its generated `ResponseAudioStart` (the
  previous fixture copied an uninitialized bool); UBSan is configured to halt
  on errors rather than silently allowing a passing test result with diagnostics.
- `negative-control/results.log`: the **SHA-256-verified exact pre-fix helper**
  is shadowed only in the generated test include path. The new accepted-lifecycle
  regression fails at `AwaitingClaim`, reproducing OTA114 before renderer
  preparation. Production source is never reverted for this negative control.
- `timing-dispatch.log`: all **four** existing production capture/light and
  dispatch Python tests pass, including missing-forwarding negative control.
- `go-tests.log`: **21** selected top-level production Go tests pass with
  `-race`, including actual local TLS IG/KG RPC selection, adapter preservation
  of empty query metadata, HTTP success/failure/body handling and stale workers.
- `arm-command.sh` / `arm-build.log`: existing builder successfully builds
  **`vic-engine vic-cloud vic-anim`** targets. Three incremental steps compile
  UIC and relink engine library/launcher; unchanged cloud/anim are up to date.
  `arm-artifacts.log` / `.sha256` record actual ARM EABI5 outputs, including the
  implementation library rather than only its small engine launcher.
- `source-preservation.log`: **83 of 87** prior dirty files are byte-identical;
  only this document, the routing helper and its two regression files changed.
  `prior-artifacts.log`: all **2112** historical artifacts match the retained
  OTA114 prebuild manifest, and OTA114 itself still matches its published hash.

These are production-method integration tests with recording SDK/animation,
intent-dispatch and safety services, not a full robot behavior-tree/audio-device
test. They verify renderer-facing prepare/chunk/complete requests and ownership,
not audible playback. At this source-validation stage no new package/version,
commit or deployment was created; subsequent packaging is recorded below.

Reproduce from workspace root using unused output paths for new evidence:

```sh
KG_FOLLOWUP_OUTPUT="$PWD/_build/ota114-kg-audio-fix-validation/native" \
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s anki/victor/test/tools -p testKnowledgeFollowUpRouting.py -v
PYTHONDONTWRITEBYTECODE=1 \
  python3 _build/ota114-kg-audio-fix-validation/negative-control.py
bash _build/ota114-kg-audio-fix-validation/arm-command.sh
```

## 20. OTA115 approved empty-transcript fix and manual handoff (2026-09-07)

**Complete development-signed OTA built and verified; not installed.**
This packages the reviewed section-19 routing correction without changing
production source, backend configuration, lycopod, AEC policy or robot state.
OTA114 and every earlier workspace artifact/evidence file remain intact.

| Item | Verified value |
|---|---|
| Version | `3.0.1.115d` |
| Full OTA | `/home/unbuilt/T909/vectoros/wire-os/_build/vicos-3.0.1.115d.ota` |
| Size | **162037760 bytes** |
| SHA-256 | `a5d297b5e22a93a7a1aec51a4b186fa969d867ea35b2d0793750369e74ecaf12` |
| Package inputs | Workspace `_build/aec-experiment-115/` |
| Evidence/decrypted payloads | Workspace `_build/aec-experiment-115-validation/` |

### Build and shipping-byte verification

Version 115, its OTA, package and validation paths were unused before reservation.
The existing `vic-yocto-builder-7:latest` Docker builder ran the full versioned
firmware/image workflow, not a binary hot-swap:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 115 build
bash anki/victor/tools/audio/build_aec_experiment.sh 115 package
python3 _build/aec-experiment-115-validation/verify-followup.py
python3 _build/aec-experiment-115-validation/verify-boot.py
```

- `build.log`: all **4569 tasks** succeeded (4539 already current), including
  version installation, victor compile/install and full rootfs/image assembly.
  `victor-compile.log` records reconfiguration with existing Go 1.24.4, incremental
  assets and installation of the already-reviewed ARM implementation library.
  Unchanged binaries legitimately remain byte-identical; a new OS version does
  not imply every executable changes.
- `package.log`: both encrypted archive payloads were decrypted/decompressed;
  lengths and SHA-256 match manifest and package inputs. BOOT is **17862656**
  bytes; SYSTEM is **845475840** bytes. Both manifest sections are full images
  (`delta=0`), with development mode `ankidev=1`; `/etc/os-version` is exactly
  `3.0.1.115d`.
- `followup-verification-final.log` / `followup-verification.json`: decrypted robot, engine and anim
  binaries match actual Release outputs, and cloud matches the actual Go output
  **`anki/victor/_build/vicos/Release/vic-cloud`**, not an assumed-fresh
  `bin/vic-cloud`. **37** application `.so` libraries match Release outputs.
  The implementation `libcozmo_engine.so` hash is
  **`7029b74f80ab46a53a1895892c3d100ddb09069d27853cc862d87f8e13933878`**,
  identical to section 19's reviewed ARM build and different from OTA114.
  Its old `empty_query` rejection marker is absent (present in OTA114).
  `arm-artifacts.log` records actual ARM EABI5 outputs.
- The three follow-up resources match both source and build: **default on**,
  maximum **5 turns**, **120-second** session admission timeout, **250 ms**
  settle, and follow-up last/lowest in the voice dispatcher. Capture/session
  diagnostic markers remain. AEC opt-in remains absent from packaged service
  and environment files; the audio library remains unchanged.
- `boot-signature.log`: the development RSA boot signature inside the decrypted
  packaged boot verifies against `ota/vble-qti.key.pub`; unsigned boot bytes
  match the build and trailing padding is zero. This is **development boot
  signing, not a production/retail or OSKR signature**, and does not claim a
  separately signed manifest.
- `native/results.log`: **38 GTests pass under ASan/UBSan** after the full build,
  including accepted empty-query answer → owned handoff → renderer-facing
  prepare/PCM/completion and delayed second-sentence delivery.
  `timing-dispatch.log`: all **4** production timing/dispatch Python cases pass.
  `go-tests-host.log`: **21** targeted top-level production Go tests pass with `-race`,
  `-tags nolibopusfile -count=1`, existing host Opus and offline dependencies.
  The initial Docker-host-test attempt lacked native Opus; the established
  host command passed without installing dependencies or changing firmware.
  Initial evidence-script library-selection failures are retained separately;
  the final verifier checks shipping application libraries, not unshipped
  `.full` debug files or system-runtime libraries at a different install path.
- `prior-artifacts-preserved.log` verifies **9910** pre-existing workspace
  artifact/evidence checksums. `source-preservation.log` records that only this
  handoff document changed among **88** prebuild dirty files.

### Compatibility and remaining manual acceptance

**Backend `query_text` is now optional transcript metadata for a valid nonempty
answer. No backend change is needed to play such an answer.** The existing Go
protobuf adapter serializes an omitted backend transcript as an empty JSON
string; the client still rejects missing/wrong-type JSON query/answer fields.
It does not fabricate query text or alter response/audio IDs. Explicit noaudio,
empty answers, malformed/error results and recognized exact stop transcripts
still terminate; duplicate/stale ownership checks, bounded buffers, result
deadline, maximum turns and session admission guards remain.

**Without a transcript the client cannot locally classify spoken stop or infer
silence from the query.** A spurious nonempty backend answer on silence can
therefore proceed. This limitation is intentional compatibility behavior, not
proof of acoustic silence/stop reliability. Bounded session guards are retained;
transcript fidelity and supervised silence/stop trials remain necessary.
No conversation history or pronoun-resolution guarantee is added.

1. Verify the exact image before manual installation:

   ```sh
   cd /home/unbuilt/T909/vectoros/wire-os
   printf '%s  %s\n' \
     a5d297b5e22a93a7a1aec51a4b186fa969d867ea35b2d0793750369e74ecaf12 \
     _build/vicos-3.0.1.115d.ota | sha256sum -c -
   ```

2. Manually install the **complete OTA115** using the established
   development-signed procedure on a compatible development/unlocked robot,
   reboot, and confirm `3.0.1.115d`. Keep previous full OTAs for rollback; do not
   mix engine/anim/cloud/robot binaries, shared libraries or resources.
3. Keep the working backend/provider configuration. Ask an independent complete
   follow-up after capture-ready and verify the previously silent second answer
   is audible, including late sentence/chunk delivery and repeated turns.
4. Supervise silence-only and exact supported stop trials, explicit noaudio,
   network/error paths, cancellation, maximum-turn/session limits and default-off
   behavior. Stop testing if silence causes self-conversation. Workstation
   integration tests prove renderer-facing requests/ownership, **not audible
   playback, acoustic timing or full on-device behavior-tree acceptance**.

No robot/backend access, configuration, restart, flash, playback, lycopod action
or commit occurred during this packaging task. Manual installation and hardware
acceptance remain outstanding.

## 21. OTA116 first-sentence graceful interruption handoff (2026-09-08)

**Complete development-signed OTA built and verified, not installed.** This
packages the current negotiated cloud-audio protocol and UIC/KG quiet-interruption
handling together with the earlier follow-up routing fixes. It is not a binary
hot-swap and does not package or deploy Python lycopod code.

| Item | Verified value |
|---|---|
| Version | `3.0.1.116d` |
| Full OTA | `/home/unbuilt/T909/vectoros/wire-os/_build/vicos-3.0.1.116d.ota` |
| Size | **162037760 bytes** |
| SHA-256 | `b99cb22373f44f5468f5220b8e3303f7fa498e2d3e43fcf8f5f02a9f2e240c91` |
| Package inputs | `/home/unbuilt/T909/vectoros/wire-os/_build/aec-experiment-116/` |
| Evidence/decrypted payloads | `/home/unbuilt/T909/vectoros/wire-os/_build/aec-experiment-116-validation/` |
| Shipping `libcozmo_engine.so` SHA-256 | `8bf7a706ac9423e64333d4f231be3257820421254f52cb75787f93a26e7011bf` |
| Shipping `vic-cloud` SHA-256 | `f23ab84e2de125331a1854f3aeea19efd398bb917d8404e344d7321e2737682e` |

### Build provenance and reproducible evidence

Version 116 and its OTA/package/validation paths were absent before reservation
(`path-reservation.log`). The existing Docker image
`vic-yocto-builder-7:latest` performed the complete versioned firmware workflow:

```sh
cd /home/unbuilt/T909/vectoros/wire-os
bash anki/victor/tools/audio/build_aec_experiment.sh 116 build
bash anki/victor/tools/audio/build_aec_experiment.sh 116 package
python3 _build/aec-experiment-116-validation/verify-release.py
```

The first two commands document the completed build; **do not rerun them over
the existing release**. Use the next unused version for any subsequent build.

- `build.log`: **4569 tasks succeeded, 4539 already current**, including
  anki-version, victor compile/install, rootfs and complete image assembly.
  `victor-compile.log` and `victor-install.log` retain the actual recipe logs.
  The native compile includes generated `clad/cloud/mic.cpp`. The previously
  reviewed interruption ARM engine/cloud outputs remain byte-identical after
  the versioned build; both differ from the shipped OTA115 implementations.
  OS version changes do not require every unchanged executable to change.
- `package.log`: decrypted BOOT is **17862656 bytes**, SHA-256
  `d0a526649e38d1b3a661428b86a3036b9e65e74bacb4a8bde387512c9e7dcc44`;
  SYSTEM is **845475840 bytes**, SHA-256
  `d8552764d331b8b89972c8ec510ea56efc9f445c93d7d6fa4d98ce04b029d6ed`.
  Both match manifest and package inputs; both are full images (`delta=0`).
  Manifest and decrypted `/etc/os-version` agree on `3.0.1.116d`, `ankidev=1`.
- `release-verification.log` / `.json`: decrypted engine/anim/robot executables,
  **actual Go output `anki/victor/_build/vicos/Release/vic-cloud`** (not the
  potentially stale `bin/vic-cloud`), and **37 shipping application `.so` files**
  match compiled outputs. The cloud executable is UPX-packed: a separate copy
  of the decrypted executable was unpacked solely for inspection. It contains
  the negotiated MIME type, missing-END/unclean-HTTP-END diagnostics and all
  cancellation/transport/provider terminal handlers. The shipping ARM engine
  implementation contains the quiet-stop diagnostic and lacks the obsolete
  `empty_query` rejection. No shipping executable was modified for inspection.
- Three follow-up resources match source and build: **default on**, **5 turns**,
  **120-second** admission budget, **250 ms** settle, last/lowest dispatcher
  priority. Capture/session timing markers remain. Packaged AEC opt-in is
  absent and the audio library retains the previously verified hash.
- The RSA development signature **inside decrypted BOOT** verifies against
  `ota/vble-qti.key.pub`; unsigned bytes match the build and trailing padding
  is zero. This is not production/retail/OSKR signing or a claim of a separately
  signed manifest.
- `native/results.log`: **41 production-method GTests pass under ASan/UBSan**,
  including typed interruptions before readiness and during playback, quiet
  cancellation without fallback/follow-up, established-stream readiness timeout,
  ordinary provider error policy, correlation and accepted empty-query handoff.
  `timing-dispatch.log`: **4** existing capture/light/dispatch Python tests pass.
- `go-tests-targeted.log`: **26 top-level tests pass with `-race`, `-count=1`,
  `-tags nolibopusfile`**, covering incremental delivery before late abort,
  explicit terminal outcomes, malformed/truncated/missing END, clean HTTP
  completion, ordinary fetch failures, retired workers and actual local TLS
  IG/KG RPC selection. Dependencies were reused offline with existing host Opus.
  An initial compiler-path typo and an unnecessarily broad stream-suite attempt
  are retained in separate logs. The broad attempt was stopped after the voice
  package passed but the unrelated stream suite did not finish; **no full Go
  suite pass is claimed**. The bounded relevant selection subsequently passed.
- `backend-tests.log`: **210 selected Python regressions pass**, including
  first-sentence publication, terminal framing, immutable per-invocation
  `TurnResult` audio-handle ownership, overlapping requests and retired-session
  readers. This fresh targeted run is separate from the earlier reported
  216-test run. The concurrency correction is Python/backend-only and requires
  no additional firmware beyond the negotiated protocol packaged here.
- `prior-artifacts.json` and the final verifier check **10029 pre-existing
  workspace artifact/evidence files byte-for-byte**, including OTA115 and its
  decrypted validation evidence and the prior interruption ARM evidence.
  Before handoff edits, all **105** snapshotted dirty source files were unchanged;
  this task subsequently changes only this document and lycopod's README.
  No production source/config changes, dependency installs, commits, broad
  clean, robot access, backend-service access, restart or flash were performed.

Reproduce the targeted tests from the workspace root into a **new unused**
evidence directory, using the retained commands:

```sh
bash _build/aec-experiment-116-validation/reproduce-tests.sh \
  _build/aec-experiment-116-validation/recheck-1
```

### Protocol and exact interruption policy

With `knowledge.cloud_audio: true` and `cloud_audio_first_sentence: true`,
lycopod publishes the first sentence and its live handle without waiting for
the entire synthesis. Both are enabled in the current local configuration
(only those booleans were read, not a configuration dump); first-sentence mode
also defaults to true in settings and the conversation constructor. Explicit
false still requests full-answer buffering. This restores the low-latency
first-sentence path, **not a measured on-device latency guarantee**.

Updated cloud negotiates `application/vnd.lycopod.answer-audio.v1`. Frames have
`kind:u8`, `length:u32be`, payload; kind 0 carries 1–1024 PCM bytes. Zero-length
terminal frames are **1 END, 2 explicit cancellation, 3 upstream transport
failure/timeout, 4 provider failure**. These frame kinds are not the numeric
values of the separate generated CLAD error enum. Successful completion requires
END and clean HTTP body termination; EOF alone, truncated frames, or a broken
HTTP terminator after END is not success.

- **Source abort before publication:** no answer text/audio handle is published.
  KG returns an empty answer; IntentGraph conversation fallback returns noaudio.
  No synthesized spoken fallback is created. An ordinary already-active KG
  question can still follow its existing no-response animation path.
- **Published cancellation or transport interruption:** cloud emits typed
  `Cancelled`/`Transport`, including negotiated failure before the first PCM.
  UIC clears buffered PCM, marks incomplete/error and classifies interruption;
  KG cancels playback, marks the session outcome **Failed**, cancels itself and
  permits neither local-TTS repetition nor failure animation nor follow-up.
  An established audio response missing readiness, or upstream delivery stall,
  also takes the quiet unsuccessful path. This is not successful EOS.
- **Provider/ordinary errors retain ordinary policy:** authentication, non-200
  HTTP status and connection establishment failures before an audio response,
  explicit provider failure and format/size errors are not relabeled as source
  cancellation. They can use the configured local-TTS fallback (during playback,
  subject to the existing played-audio salvage threshold), or the existing
  failure reaction. Successful fallback can still authorize follow-up. Local
  playback failures/watchdogs are not universally made quiet.

**Already played audio cannot be retracted.** First-sentence mode can stop future
audio only after interruption is observed. It cannot promise no audible speech
for a late-aborted answer, full duplex, speech barge-in, or zero network delay.
Full buffering remains the explicit opt-out when withholding all pre-completion
speech is required. Existing request, audio-size and session budgets remain.

### Coordinated manual deployment and remaining acceptance

**Both updates are required:** manually install the complete matching OTA116
on a compatible development/unlocked robot **and separately restart/reload the
local updated lycopod checkout** using the user's normal service procedure.
An OTA does not contain Python backend code and cannot activate `TurnResult`,
first-sentence configuration or backend terminal framing in a running process.
No backend restart, firmware installation or hardware trial has been performed.

1. Verify before manual installation:

   ```sh
   cd /home/unbuilt/T909/vectoros/wire-os
   printf '%s  %s\n' \
     b99cb22373f44f5468f5220b8e3303f7fa498e2d3e43fcf8f5f02a9f2e240c91 \
     _build/vicos-3.0.1.116d.ota | sha256sum -c -
   ```

2. Manually install/reboot and confirm `3.0.1.116d`; keep all previous full OTAs
   for rollback. Do not mix binaries, implementation libraries or resources.
   Separately activate the matching lycopod source with the two booleans true.
3. Verify first-sentence speech precedes full synthesis completion; successful
   END waits for actual playback completion before one follow-up capture.
   Exercise repeated follow-ups and omitted-transcript valid answers.
4. Supervise explicit abort before publication, after publication before PCM,
   and during playback; upstream disconnect/timeout, missing terminal/partial
   HTTP body, provider failure and ordinary fetch errors. Confirm quiet typed
   failures stop queued playback without repetition/failure animation/follow-up,
   while ordinary errors preserve their distinct fallback policy.
5. Verify concurrent requests retain their own text/audio IDs, old-turn teardown
   cannot cancel a replacement, and long/delayed sentences stay inside existing
   limits. Check silence, supported stop transcripts, safety/mute/cancellation,
   max turns/session budget and explicit feature-off behavior. Missing backend
   transcripts still prevent reliable local silence/spoken-stop classification.
   Stop supervised testing if silence creates self-conversation.

**Backward compatibility is not graceful-interruption compatibility.** Old
firmware negotiates legacy raw PCM, which cannot receive typed post-header abort.
Even with updated lycopod, OTA115 and older firmware may still play an error or
fallback on late abort. New firmware with a raw-only old backend likewise cannot
distinguish abort from loss; raw interruption is quiet only after PCM has arrived.
Do not promise graceful late-abort behavior until **both sides** are updated.
The unchanged `cloud_audio: false` SDK path has no new late-abort guarantee.

Workstation tests prove protocol, compiled bytes, renderer-facing requests and
ownership—not audible playback, physical acoustic latency or full on-device
behavior-tree acceptance. Those coordinated hardware release gates remain open.
