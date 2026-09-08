# Sequential Multi-Turn Voice Interaction: Implementation Design

_Status: Proposed_  
_Last updated: 19 July 2026_  
_Related architecture: [voice_interaction_architecture.md](voice_interaction_architecture.md)_

**Implementation update (2026-09-07):** the narrower, default-on KG-only
[automatic follow-up mode](automatic_follow_up_mode.md#13-implementation-details-and-configuration-2026-09-07)
is implemented and workstation-tested in the working tree, not deployed.
It uses the session-component/follow-up-behavior architecture below, but requires
explicit successful playback/get-out plus matching intent deactivation, includes
cloud audio and successful local fallback, uses a 120-second admission deadline,
and adds correlated capture/playback protocols. Its configuration and scoped
source/test/build inventory are documented there. The opt-in policy, generic
deactivation-as-success, and excluded cloud audio in this original broader
proposal are superseded for that implemented subset; other extensions remain
proposed. Hardware acceptance and backend contextual memory are not claimed.

## 1. Purpose

This document defines the first implementation milestone for multi-turn voice interaction in Vector/Wire OS.

The target experience is:

1. The user says the wake word once.
2. Vector captures and handles one utterance.
3. Vector completes its response or action.
4. Vector automatically listens for another utterance without requiring the wake word again.
5. The loop continues until an explicit or policy-driven termination condition occurs.

This milestone remains **sequential and half-duplex**. Vector does not listen for ordinary speech while speaking. Full duplex, natural speech barge-in, cloud-generated audio, and acoustic echo cancellation are separate projects.

## 2. Goals

### 2.1 Functional goals

- Support multiple voice turns after one wake word.
- Preserve all existing single-turn behavior when the feature is disabled.
- Reuse the existing `StartWakeWordlessStreaming` microphone path.
- Reuse the existing listening UX and intent-routing system.
- Allow existing user-intent behaviors to execute without being rewritten.
- Start follow-up listening only after the current intent behavior has completed.
- Bound sessions by turn count and timeouts.
- End cleanly on silence, errors, explicit stop, safety events, or higher-priority interactions.
- Make session state observable in logs and telemetry.
- Provide a path from independent-command turns to contextual backend conversation.

### 2.2 Engineering goals

- Keep session policy separate from microphone transport.
- Keep follow-up listening UX separate from session policy.
- Avoid inferring response completion from arbitrary speaker activity when possible.
- Avoid holding one microphone/Chipper gRPC stream open across robot responses.
- Introduce changes behind runtime and build-time feature controls.
- Make stale callbacks and delayed cloud responses harmless through session and turn IDs.

## 3. Non-Goals for the First Milestone

The first milestone does not include:

- Simultaneous listening and speaking
- Natural speech barge-in
- Wake-word barge-in during playback
- Acoustic echo cancellation
- Cloud-generated response audio
- Partial ASR transcripts
- Incremental LLM responses
- Multiple concurrent conversations
- Persisting a conversation across reboot, sleep, or process restart
- Automatically enabling follow-up for every existing intent on day one
- Keeping one Chipper audio stream open across turns

## 4. Definitions

| Term | Meaning |
|---|---|
| Conversation session | Bounded period beginning with a wake-triggered turn and containing zero or more wake-wordless follow-up turns |
| Turn | One user utterance, one final intent/result, and the corresponding robot response/action |
| Initial turn | First turn opened by the wake word |
| Follow-up turn | Later turn opened by engine request without another wake word |
| Command-loop mode | Turns are independent commands; no backend dialogue context is retained |
| Contextual mode | Backend receives a stable conversation ID and retains dialogue context across turns |
| Session owner | Engine component responsible for session state and continuation policy |
| Turn behavior | Behavior responsible for follow-up listening UX and wake-wordless capture |

## 5. Existing Reusable Mechanisms

### 5.1 Wake-wordless capture

`UserIntentComponent::StartWakeWordlessStreaming` sends `StartWakeWordlessStreaming` to `vic-anim`:

- [`engine/aiComponent/behaviorComponent/userIntentComponent.cpp`](../../engine/aiComponent/behaviorComponent/userIntentComponent.cpp#L940-L948)

`MicDataSystem::StartWakeWordlessStreaming` starts a microphone stream without a wake word after the optional get-in response completes:

- [`animProcess/src/cozmoAnim/micData/micDataSystem.cpp`](../../animProcess/src/cozmoAnim/micData/micDataSystem.cpp#L214-L271)

This mechanism is already used by knowledge graph and robot-driven dialog flows.

### 5.2 Prompt → speak → listen sequencing

`BehaviorPromptUserForVoiceCommand` already demonstrates the required half-duplex sequence:

- Delegate local TTS
- Wait for TTS behavior completion
- Start wake-wordless streaming
- Show listening UX
- Wait for an intent, silence, error, or timeout

Relevant code:

- [`engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorPromptUserForVoiceCommand.cpp`](../../engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorPromptUserForVoiceCommand.cpp#L322-L433)

The new implementation should reuse this pattern, but session ownership must be above a single prompt behavior so arbitrary user-intent behaviors can run between turns.

### 5.3 Intent lifecycle as response completion

A behavior that calls `SmartActivateUserIntent` owns the active intent. `ICozmoBehavior` automatically deactivates it when the behavior deactivates:

- [`engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.cpp`](../../engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.cpp#L1611-L1649)

`UserIntentComponent` moves an intent from pending to active and clears it on deactivation:

- [`engine/aiComponent/behaviorComponent/userIntentComponent.cpp`](../../engine/aiComponent/behaviorComponent/userIntentComponent.cpp#L190-L268)

For existing well-behaved intent handlers, the transition from active intent to no active intent is the best available generic signal that the response/action has completed.

### 5.4 Current per-utterance cloud session

`vic-cloud` currently creates a new random Chipper session ID for every microphone stream:

- [`cloud/internal/voice/stream/connect.go`](../../cloud/internal/voice/stream/connect.go#L98-L128)

This is acceptable for command-loop mode. Contextual conversation requires an additional stable conversation ID above these per-turn transport sessions.

## 6. Proposed Architecture

The first implementation adds two engine-side units:

1. `ConversationSessionComponent`
2. `BehaviorConversationFollowUp`

```text
Initial wake word
      |
      v
BehaviorReactToVoiceCommand
      |
      v
Intent pending -> normal intent behavior activates
      |
      v
ConversationSessionComponent observes intent lifecycle
      |
      v
Intent behavior deactivates / response completes
      |
      v
BehaviorConversationFollowUp becomes activatable
      |
      v
StartWakeWordlessStreaming + listening UX
      |
      v
Next intent pending -> follow-up behavior exits
      |
      +--------------------> normal intent behavior activates
```

### 6.1 `ConversationSessionComponent`

This is a dependency-managed behavior component. It owns state and policy but does not directly own animations or behavior delegation.

Proposed location:

- `engine/aiComponent/behaviorComponent/conversationSessionComponent.h`
- `engine/aiComponent/behaviorComponent/conversationSessionComponent.cpp`

Proposed dependencies:

- `UserIntentComponent`
- `ActiveBehaviorIterator` or the minimum behavior-state dependency needed for diagnostics
- `RobotInfo` for safety/mute/sleep conditions if those signals are not delivered directly

Proposed responsibilities:

- Generate and own `conversationId`.
- Allocate monotonically increasing `turnId` values.
- Track initial versus follow-up turns.
- Observe pending and active user-intent transitions.
- Decide whether another turn is allowed.
- Publish `followUpRequested` for the follow-up behavior condition.
- Track silence, unmatched, timeout, and transport-error counts.
- End sessions and record a reason.
- Reject stale events using conversation and turn IDs.
- Expose read-only session state to behavior conditions and WebViz/telemetry.

It must not:

- Play TTS or animations.
- Directly claim user intents.
- Execute intent-specific actions.
- Keep microphone audio streams open.
- Decide cloud response content.

### 6.2 `BehaviorConversationFollowUp`

This behavior owns one follow-up listening turn.

Proposed location:

- `engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorConversationFollowUp.h`
- `engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorConversationFollowUp.cpp`

It is activated only when `ConversationSessionComponent::IsFollowUpRequested()` is true.

Responsibilities:

- Atomically claim the follow-up request for one turn.
- Configure the trigger response/listening UX.
- Call `StartWakeWordlessStreaming(CloudMic::StreamType::Normal, ...)`.
- Play `VC_ListeningGetIn`, `VC_ListeningLoop`, and `VC_ListeningGetOut` as configured.
- Wait for stream open, intent, silence, error, or timeout.
- Notify the session component of the turn outcome.
- Exit without claiming valid user intents so existing responder behaviors can activate.

It should be based on the listening portion of `BehaviorPromptUserForVoiceCommand`, not on `BehaviorReactToVoiceCommand`, because follow-up turns have no trigger-word event.

### 6.3 Why two units

A component alone cannot safely own behavior animations and delegation. A behavior alone cannot remain active while unrelated intent-response behaviors execute without restructuring the behavior tree.

Separating them allows:

- Session state to survive between behavior activations.
- Normal intent handlers to remain unchanged.
- Follow-up UX to participate in behavior priorities normally.
- Session policy to be unit tested without animation/action dependencies.

## 7. Session State Machine

### 7.1 States

```cpp
enum class ConversationState : uint8_t {
  Inactive,
  InitialListening,
  WaitingForIntentClaim,
  ExecutingResponse,
  FollowUpPending,
  FollowUpListening,
  Ending
};
```

### 7.2 Events

```cpp
enum class ConversationEvent : uint8_t {
  InitialStreamOpened,
  IntentPending,
  IntentActivated,
  IntentDeactivated,
  FollowUpBehaviorActivated,
  FollowUpStreamOpened,
  SilenceReceived,
  UnmatchedIntentReceived,
  StreamError,
  StreamTimeout,
  FollowUpStartFailed,
  ExplicitStop,
  NewWakeWord,
  HigherPriorityInteraction,
  MicrophoneMuted,
  RobotSleeping,
  MaximumTurnsReached,
  SessionDeadlineReached
};
```

### 7.3 Core transitions

| Current state | Event | Next state | Action |
|---|---|---|---|
| `Inactive` | initial voice stream/intent starts | `InitialListening` or `WaitingForIntentClaim` | Create session and turn 1 |
| `InitialListening` | intent pending | `WaitingForIntentClaim` | Record result arrival |
| `WaitingForIntentClaim` | intent activated | `ExecutingResponse` | Record owner and activation ID |
| `ExecutingResponse` | matching intent deactivated | `FollowUpPending` | Schedule follow-up if policy allows |
| `FollowUpPending` | follow-up behavior activated | `FollowUpListening` | Allocate next turn ID |
| `FollowUpListening` | valid intent pending | `WaitingForIntentClaim` | End listening UX and allow normal routing |
| `FollowUpListening` | silence | `Ending` | End with `Silence` |
| `FollowUpListening` | unmatched intent | `FollowUpPending` or `Ending` | Reprompt according to policy |
| any active state | error/timeout | `Ending` | Record failure and clean up |
| any active state | explicit stop/mute/sleep | `Ending` | Cancel stream and clean up |
| `Ending` | cleanup complete | `Inactive` | Clear IDs and pending requests |

### 7.4 Transition invariants

- At most one conversation session exists.
- At most one follow-up request is outstanding.
- At most one microphone stream exists.
- A turn ID is allocated exactly once.
- A valid intent is never claimed by the follow-up behavior.
- Follow-up is never started while an intent is pending or active.
- Follow-up is never started while `UserIntentComponent::IsCloudStreamOpen()` is true.
- Stale events from prior turns do not mutate the current state.
- Ending a session clears all follow-up requests before returning to `Inactive`.

## 8. Starting a Session

### 8.1 MVP trigger

For the first implementation, start a session when all conditions are true:

- Feature is enabled.
- A normal wake-triggered stream produces a valid user intent.
- The intent is in the initial eligibility allowlist.
- No Alexa interaction or other conversation is active.

Starting on valid intent rather than immediately on trigger-word detection minimizes changes to the initial listening path. The first release does not require a conversation ID before the initial audio is uploaded because command-loop turns are independent.

### 8.2 Later contextual trigger

For contextual mode, create the conversation ID when the initial hotword starts so the backend can associate turn 1 with the session. This requires protocol work described in section 14.

### 8.3 New wake word during a session

For the first release, a new accepted wake word replaces the current session:

1. End the old session with `SupersededByWakeWord`.
2. Clear pending follow-up state.
3. Let the normal wake-word path create a new session.

Wake-word detection during active streaming remains ignored by the existing microphone gate. Playback barge-in remains out of scope.

## 9. Detecting Response Completion

### 9.1 MVP completion signal

Use the active-intent lifecycle:

1. Record the intent's `activationID` when it becomes active.
2. Enter `ExecutingResponse`.
3. Wait until the same active intent is deactivated.
4. Apply a short configurable settle delay.
5. Request follow-up if policy allows.

This works because normal responders use `SmartActivateUserIntent`, and the base behavior deactivates the intent when the handler exits.

### 9.2 Required protection

Do not transition merely because `IsAnyUserIntentActive()` becomes false. The component must observe a complete edge for the expected activation ID:

```text
expected intent pending
    -> same intent activated with activation ID N
    -> activation ID N deactivated
```

This prevents follow-up from starting when:

- The intent has not yet been claimed.
- A different nested intent becomes active.
- An old behavior deactivates after a newer turn starts.
- An intent is dropped or times out unclaimed.

### 9.3 Explicit completion API

Add an explicit API for exceptional behaviors:

```cpp
void MarkTurnResponseComplete(
    uint64_t conversationId,
    uint32_t turnId,
    ConversationContinuation continuation);
```

Initial adopters should not need it if active-intent deactivation is accurate. Use it for behaviors that:

- Deactivate the intent before asynchronous output finishes.
- Launch a long-running background action.
- Should end the session after completion.
- Want to continue listening before the full behavior exits.

Long term, explicit completion is preferable to adding speaker-idle heuristics.

## 10. Continuation Policy

### 10.1 Default policy

Continuation is opt-in during rollout. A valid turn continues only if:

- The session is enabled and healthy.
- Maximum turn count has not been reached.
- Session deadline has not elapsed.
- The completed intent is eligible.
- The behavior did not explicitly request session end.
- No pending or active intent remains.
- No microphone stream remains open.
- Robot state allows listening.

### 10.2 Intent policy table

Add configuration rather than hard-coded intent checks:

```json
{
  "multiTurnVoice": {
    "enabled": false,
    "maxTurns": 5,
    "sessionTimeout_sec": 45,
    "followUpSettleTime_ms": 250,
    "silenceEndsSession": true,
    "maxUnmatchedReprompts": 1,
    "eligibleIntents": [
      "knowledge_question",
      "simple_voice_response"
    ],
    "terminalIntents": [
      "global_stop",
      "system_sleep"
    ]
  }
}
```

The actual intent names must use the generated user-intent tags present in the target build.

### 10.3 Why allowlist first

Some existing intents:

- Start long-running games.
- Put the robot to sleep.
- Change system mode.
- Start timers or background activities.
- Delegate to behaviors whose lifecycle does not equal spoken-response completion.

An allowlist reduces regression risk. The list can expand after each behavior's completion semantics are verified.

## 11. Termination Rules

End the session immediately for:

- Explicit stop/cancel intent
- User microphone mute
- Sleep, shutdown, reboot, or recovery state
- Alexa activation
- Safety or critical system behavior
- Cloud authentication failure
- Follow-up stream start failure
- Maximum number of turns
- Overall session timeout
- New normal wake word superseding the session
- Feature disabled while active

Default handling for conversational failures:

| Outcome | Default action |
|---|---|
| Silence on first follow-up | End silently after listening get-out |
| Unmatched intent | One local reprompt, then end |
| Cloud timeout | Play existing failure UX, then end |
| Network error | Play existing failure UX, then end |
| Intent unclaimed | End and log configuration error |
| Follow-up activation blocked | Retry once after short delay, then end |
| Robot becomes busy | End; do not leave a latent follow-up request |

## 12. Follow-Up Listening Behavior

### 12.1 Behavior states

```cpp
enum class FollowUpState : uint8_t {
  GetIn,
  OpeningStream,
  Listening,
  GetOut,
  Finished
};
```

### 12.2 Activation sequence

1. Read and claim the current `FollowUpRequest` from `ConversationSessionComponent`.
2. Verify conversation and turn IDs are current.
3. Push the listening trigger response configuration.
4. Call `StartWakeWordlessStreaming(StreamType::Normal, playGetIn)`.
5. Wait for optional get-in completion and stream-open confirmation.
6. Loop `VC_ListeningLoop`.
7. Stop on pending intent, silence, error, or timeout.
8. Play optional `VC_ListeningGetOut`.
9. Report outcome to the session component.
10. Exit without claiming a valid pending intent.

### 12.3 Timeouts

Recommended initial values:

| Timeout | Initial value | Rationale |
|---|---:|---|
| Stream-open timeout | 5 s | Matches existing voice command minimum wait |
| Maximum listening turn | 10 s | Existing behavior/cloud envelope |
| Microphone capture | Existing 6.05 s | No change in first milestone |
| Follow-up activation wait | 2 s | Avoid latent request when behavior cannot run |
| Response settle delay | 250 ms | Avoid abrupt listen transition after get-out audio |
| Overall session | 45 s | Bounds resource and UX impact |
| Maximum turns | 5 | Prevents accidental indefinite loops |

All values must be configuration-driven and telemetry-visible.

## 13. Engine Code Changes

### 13.1 New files

- `engine/aiComponent/behaviorComponent/conversationSessionComponent.h`
- `engine/aiComponent/behaviorComponent/conversationSessionComponent.cpp`
- `engine/aiComponent/behaviorComponent/conversationSessionTypes.h`
- `engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorConversationFollowUp.h`
- `engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorConversationFollowUp.cpp`
- Follow-up behavior JSON configuration in the existing behavior resource hierarchy
- Unit tests in the existing engine test target adjacent to behavior-component tests

### 13.2 Existing files to modify

#### Component registration

- [`engine/aiComponent/behaviorComponent/behaviorComponents_fwd.h`](../../engine/aiComponent/behaviorComponent/behaviorComponents_fwd.h)
  - Add `ConversationSessionComponent` to `BCComponentID`.
- [`engine/aiComponent/behaviorComponent/behaviorComponents_impl.cpp`](../../engine/aiComponent/behaviorComponent/behaviorComponents_impl.cpp)
  - Link the component type to the enum.
- [`engine/aiComponent/behaviorComponent/behaviorComponent.cpp`](../../engine/aiComponent/behaviorComponent/behaviorComponent.cpp#L120-L205)
  - Construct and register the component after `UserIntentComponent` and required dependencies.

#### Intent lifecycle notifications

- [`engine/aiComponent/behaviorComponent/userIntentComponent.h`](../../engine/aiComponent/behaviorComponent/userIntentComponent.h)
- [`engine/aiComponent/behaviorComponent/userIntentComponent.cpp`](../../engine/aiComponent/behaviorComponent/userIntentComponent.cpp)

Add typed callbacks or a small event subscription interface for:

- Intent pending
- Intent activated with activation ID and owner
- Intent deactivated with activation ID
- Cloud stream open/closed
- Error/timeout

Do not make `ConversationSessionComponent` poll private intent internals or parse logs.

#### Behavior registration

- [`engine/aiComponent/behaviorComponent/behaviorFactory.cpp`](../../engine/aiComponent/behaviorComponent/behaviorFactory.cpp)
  - Register the new follow-up behavior class if a new generated behavior enum is required.
- Existing behavior-class definitions and generated behavior configuration inputs
  - Add `ConversationFollowUp`.
- Behavior tree configuration
  - Add a high-priority but policy-gated branch activated by `IsFollowUpRequested()`.

#### Conditions

Add one condition:

- `ConditionConversationFollowUpPending`

Proposed files:

- `engine/aiComponent/beiConditions/conditions/conditionConversationFollowUpPending.h`
- `engine/aiComponent/beiConditions/conditions/conditionConversationFollowUpPending.cpp`

Register it in the existing condition factory.

### 13.3 Optional explicit behavior API

Add a helper on `ICozmoBehavior` or the behavior external interface:

```cpp
void SetConversationContinuation(ConversationContinuation continuation);
```

This lets an active intent behavior select:

- `Default`
- `Continue`
- `End`
- `ContinueWithPrompt`

The base class can pass this value when it deactivates the active intent. This avoids coupling individual behaviors directly to the component.

## 14. Cloud and Protocol Design

### 14.1 Milestone A: command-loop mode

No cloud protocol change is required.

Each follow-up turn:

- Opens a new microphone stream.
- Receives a new per-stream Chipper session ID.
- Produces an independent intent.
- Shares only robot-side session policy.

This is sufficient to validate wake-once UX and behavior lifecycle integration.

### 14.2 Milestone B: contextual mode

Add a stable conversation envelope separate from the Chipper transport session:

```text
conversation_id   uint64 or UUID
turn_id           uint32
is_follow_up      bool
```

Recommended semantics:

- `conversation_id` remains stable for all turns.
- `turn_id` starts at 1 and increases monotonically.
- Each audio turn may still use a new gRPC stream.
- The backend uses `conversation_id` to retrieve/update context.
- A final `EndConversation` message lets the backend release context immediately.
- Context also expires server-side by TTL.

### 14.3 IPC changes for contextual mode

The current `Hotword` message is created in `vic-anim`, while the engine owns conversation state. Extend the wake-wordless request and stream job so metadata flows:

```text
vic-engine
  StartWakeWordlessStreaming(conversation_id, turn_id, is_follow_up)
      -> vic-anim MicDataInfo / streaming job
      -> CloudMic Hotword/TurnStart
      -> vic-cloud
      -> Chipper/backend metadata
```

Required message changes:

- Extend `RobotInterface::StartWakeWordlessStreaming` with conversation metadata, or add a versioned `StartConversationTurn` message.
- Store metadata on `MicDataInfo` for the stream job.
- Extend `CloudMic::Hotword`, or add a separate `ConversationTurnStart` message sent before audio.
- Add `ConversationEnded` from engine to cloud if immediate backend cleanup is required.

Prefer new versioned messages if changing generated CLAD structures would break compatibility with independently updated processes.

### 14.4 Cloudless contextual mode

Cloudless Vosk currently resets recognition after each utterance. That is compatible with multi-turn capture, but it has no dialogue context.

For command-loop mode, no changes are needed beyond repeated wake-wordless turns.

For contextual mode, add a lightweight context store above Vosk intent matching. The recognizer remains per-utterance; only intent/dialogue interpretation retains `conversation_id` state.

## 15. Compatibility and Feature Control

### 15.1 Feature controls

Use all of the following during rollout:

- Build-time inclusion of the new component/behavior
- Runtime configuration `multiTurnVoice.enabled`
- Intent eligibility allowlist
- Maximum-turn and timeout limits
- Optional developer console override for on-device testing

The production default remains disabled until validation is complete.

### 15.2 Backward compatibility

When disabled:

- No session is created.
- No follow-up condition becomes true.
- Existing trigger, cloud stream, intent, behavior, and TTS paths are unchanged.

If contextual protocol metadata is unsupported by an older cloud process:

- Fall back to command-loop mode, or
- End after the first turn according to negotiated capability.

Do not silently assume backend context exists.

## 16. Concurrency and Race Handling

### 16.1 Required identifiers

Use identifiers in all asynchronous callbacks:

```cpp
struct ConversationTurnToken {
  uint64_t conversationId;
  uint32_t turnId;
  uint32_t intentActivationId;
};
```

A callback is accepted only if all relevant fields match current state.

### 16.2 Important races

#### Delayed result after listening timeout

- Follow-up behavior reports timeout and session begins ending.
- A delayed intent arrives afterward.
- The stale result must not reactivate the ended session.
- Existing intent routing may handle or drop it according to current policy, but no follow-up is scheduled.

#### Intent deactivation after session replacement

- Old intent behavior exits after a new wake word starts another session.
- Activation ID and conversation ID mismatch.
- Ignore the old completion event.

#### Follow-up requested while stream job is still held

`MicDataSystem` may retain a completed stream job for minimum UX duration. `StartWakeWordlessStreaming` contains a workaround for a completed fake-held stream, but the new design should avoid depending on ambiguous `HasStreamingJob()` semantics.

Before rollout, cleanly expose one of:

- `IsRealStreamActive()`
- `IsStreamJobHeldForUxOnly()`
- A stream-closed event to engine

Long term, remove the fake minimum-stream hold described in:

- [`animProcess/src/cozmoAnim/micData/micDataSystem.cpp`](../../animProcess/src/cozmoAnim/micData/micDataSystem.cpp#L560-L579)

#### Higher-priority behavior activates

- Clear the pending follow-up request.
- End the session unless the interruption is explicitly resumable.
- Never resume from a stale pending condition after safety behavior completes.

## 17. UX Design

### 17.1 Initial recommendation

After a response finishes:

1. Wait 250 ms.
2. Play a short follow-up listening get-in/earcon.
3. Enter the existing listening loop and backpack-light state.
4. On valid speech, use normal listening get-out and intent feedback.
5. On silence, use a quiet get-out and end without an error phrase.

### 17.2 Reprompt policy

For the first release:

- Silence ends the session without a spoken reprompt.
- One unmatched intent may trigger a short local phrase such as a configured localized “please try again.”
- A second unmatched result ends the session.
- Network failures use existing error UX and end the session.

Avoid automatically saying “anything else?” after every turn in the MVP. The listening earcon and lights should communicate continuation with lower latency and less repetitive speech.

## 18. Observability

### 18.1 Structured logs

Every state change should log:

- Conversation ID
- Turn ID
- Old and new state
- Triggering event
- Intent tag and activation ID when applicable
- Elapsed session and turn time
- Termination reason

Example event names:

```text
voice.conversation.started
voice.conversation.state_changed
voice.conversation.turn_started
voice.conversation.intent_activated
voice.conversation.intent_completed
voice.conversation.followup_requested
voice.conversation.ended
```

### 18.2 Metrics

Track:

- Sessions started/completed
- Turns per session histogram
- Follow-up stream-open latency
- Follow-up intent latency
- Silence termination rate
- Unmatched rate
- Error and timeout rate
- Sessions superseded by wake word
- Follow-up activation failures
- Intent behaviors completing without explicit/implicit completion
- Percentage of eligible initial turns that reach turn 2

### 18.3 Debug display

Expose in WebViz or existing developer diagnostics:

- Feature enabled
- Current state
- Conversation/turn IDs
- Remaining session time
- Last termination reason
- Current eligible/terminal policy decision

## 19. Testing Strategy

### 19.1 Component unit tests

Test `ConversationSessionComponent` with synthetic events:

1. Disabled feature never creates a session.
2. Eligible initial intent creates session.
3. Ineligible intent remains single-turn.
4. Pending → activated → matching deactivated requests follow-up.
5. Deactivation without matching activation ID is ignored.
6. Follow-up request is emitted once.
7. Valid next intent increments turn count.
8. Silence ends session.
9. Unmatched result follows configured reprompt limit.
10. Maximum turns end session.
11. Session deadline ends session.
12. New wake word supersedes old session.
13. Mute/sleep/Alexa/safety interruption ends session.
14. Stale callbacks from old turns are ignored.
15. Feature disable during active session cleans up.

### 19.2 Follow-up behavior tests

1. Activation claims exactly one follow-up request.
2. Calls wake-wordless streaming once.
3. Does not claim valid pending intent.
4. Plays configured get-in/loop/get-out sequence.
5. Handles stream-open timeout.
6. Handles silence and unmatched outcomes.
7. Exits and reports cancellation on behavior interruption.
8. Cannot start if another cloud stream is open.

### 19.3 Integration tests

Use fake cloud/memory-pipe infrastructure where possible:

1. Wake-triggered turn → intent → response completion → follow-up stream.
2. Two valid turns execute two existing intent behaviors.
3. Follow-up silence returns to idle.
4. Cloud error in turn 2 ends without turn 3.
5. Delayed turn-1 result cannot affect turn 2.
6. Minimum stream-hold overlap does not reject follow-up.
7. Existing single-turn tests pass with feature disabled.
8. Cloudless Vosk recognizer resets correctly between follow-up turns.

### 19.4 Hardware tests

Validate on production robot hardware:

- Follow-up starts only after response audio finishes.
- No response tail is captured as user speech.
- Earcon is not included in uploaded audio.
- Lights and face state are consistent across turns.
- Five-turn session remains stable.
- Silence exit feels natural.
- Wi-Fi loss and recovery do not leave listening state stuck.
- CPU, memory, thermal, and battery impact remain acceptable.

## 20. Rollout Plan

### Stage 0: Instrumentation only

- Add lifecycle events and metrics without scheduling follow-up.
- Measure how reliably active-intent deactivation represents response completion.
- Build an intent eligibility table from real behavior traces.

### Stage 1: Developer-only command loop

- Add component and follow-up behavior.
- Enable via developer console/config only.
- Use two or three known-safe intents.
- Maximum two turns.

### Stage 2: Bounded internal rollout

- Increase to five turns.
- Expand allowlist.
- Add silence and unmatched policies.
- Validate cloud and cloudless paths.

### Stage 3: Contextual backend

- Add stable conversation and turn IDs.
- Add capability negotiation.
- Retain server-side context across per-turn audio streams.
- Add explicit backend session end.

### Stage 4: Production opt-in/default

- Enable for selected locales/configurations.
- Monitor termination and error metrics.
- Keep immediate remote kill switch.

## 21. Implementation Work Breakdown

### Change set 1: lifecycle observability

- Add typed intent lifecycle subscriptions.
- Log activation/deactivation IDs and timing.
- Add no-op conversation instrumentation.
- No UX changes.

### Change set 2: session component

- Add component enum/registration.
- Implement state machine and configuration.
- Add unit tests.
- Still do not open follow-up streams.

### Change set 3: follow-up behavior

- Add behavior and activation condition.
- Reuse wake-wordless capture and listening animations.
- Add behavior tests.
- Enable only in developer configuration.

### Change set 4: eligibility and termination

- Add allowlist/terminal policy.
- Add silence, unmatched, error, and timeout handling.
- Add interruption cleanup.
- Add telemetry.

### Change set 5: anim stream-state cleanup

- Expose real stream-active versus UX-held state.
- Remove dependence on ambiguous fake stream holds.
- Add overlap regression tests.

### Change set 6: contextual protocol

- Add conversation/turn metadata and capability negotiation.
- Update engine → anim → cloud transport.
- Add backend context handling.
- Add stale-turn and reconnect tests.

## 22. Acceptance Criteria for the First Deliverable

The first deliverable is complete when:

- Feature-disabled behavior is identical to current single-turn operation.
- An eligible wake-triggered command can complete and automatically open one follow-up listening turn.
- A valid follow-up command executes through the normal intent-routing system.
- The user can complete at least three turns after one wake word in hardware testing.
- Silence, unmatched speech, cloud error, mute, and maximum-turn conditions terminate cleanly.
- No duplicate follow-up stream is opened.
- No stale callback reopens an ended conversation.
- Existing intent behaviors require no changes for the initial allowlist.
- Automated tests cover state transitions and key races.
- Structured telemetry identifies every session end reason.

## 23. Key Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Active-intent deactivation occurs before response truly completes | Robot records its own response tail | Instrument first; use allowlist; add explicit completion API |
| Follow-up collides with UX-held stream job | Follow-up rejected or delayed | Expose real stream state; clean up fake hold semantics |
| Long-running intent behavior keeps session alive | Unexpected delayed listening | Session deadline; terminal policy; explicit `End` continuation |
| Unmatched/silence loops indefinitely | Poor UX and resource use | Strict reprompt and maximum-turn limits |
| Old cloud result arrives in new turn | Wrong behavior/session continuation | Conversation, turn, and activation IDs |
| Behavior-tree priority blocks follow-up | Latent session resumes unexpectedly | Activation deadline and immediate session cleanup |
| Context assumed but backend treats turns independently | Incoherent dialogue | Explicit command-loop/contextual modes and capability negotiation |
| Feature changes established single-turn behavior | Regression | Disabled-by-default feature gate and unchanged initial path |

## 24. Design Decision Summary

- Implement **sequential half-duplex** multi-turn first.
- Keep one microphone/cloud audio stream per utterance.
- Add an engine-owned session component for policy and identity.
- Add a separate follow-up behavior for listening UX and wake-wordless streaming.
- Reuse active-intent deactivation as the initial response-completion signal.
- Add explicit completion/continuation for exceptional behaviors.
- Begin with an intent allowlist and bounded sessions.
- Deliver command-loop mode before contextual backend mode.
- Add stable conversation IDs later without conflating them with per-stream Chipper session IDs.
- Do not include cloud audio or full duplex in this implementation milestone.
