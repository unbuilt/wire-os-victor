# Knowledge Graph Cloud Audio: OTA and Lycopod Test Runbook

_Status: 104d main path confirmed on hardware; upstream-integrated 105d built, hardware retest pending_

_Last updated: 5 September 2026_

_Related document:_

- [knowledge_graph_cloud_audio_implementation_design.md](knowledge_graph_cloud_audio_implementation_design.md)

## 1. Goal

This runbook tests the Knowledge Graph cloud-audio feature end to end with:

- WireOS OTA `_build/vicos-3.0.1.105d.ota` (upstream integration).
- The matching lycopod cloud-audio changes.
- A development-signed OTA-capable Vector.
- The robot and lycopod host on the same reachable network.

A passing test proves that lycopod generates answer audio, returns an `audio_id`,
serves progressive PCM over HTTP, and that Vector fetches and plays that audio
instead of synthesizing the answer locally with Acapela.

### Retesting the upstream integration

Keep the already-working lycopod checkout and configuration unchanged and flash
**105d**. This integrates upstream Victor through `c227d847` with the cloud-audio
changes that worked in 104d. It preserves the KG deadline, playback flow control,
short-clip handling, and failure reporting, while adopting upstream's ready-prompt
fix and other updates.

104d remains the hardware-confirmed fallback. The 103d failure also required
the server fix that yields the actual Knowledge Graph response instead of
`None`; testing against an older server still will not exercise the working path.

Start `logcat` before saying "I have a question", then "How far away is the
Sun?" Look for the new `Knowledge Graph response` and
`Cloud audio: starting fetch` messages in section 9. Do not launch an SDK
playback client alongside this test.

## 2. How the path works

1. Vector sends a Knowledge Graph request to lycopod over Chipper gRPC.
2. Lycopod generates the answer and starts producing its spoken audio.
3. Lycopod stores the live PCM stream under an `audio_id`.
4. Lycopod yields a Knowledge Graph response containing answer text and
   `audio_id`. `vic-cloud` adds `response_id` and `cloud_audio_available` to
   the local intent delivered to the engine.
5. `vic-cloud` fetches `GET /v1/kg-audio/{audio_id}` from lycopod.
6. `vic-cloud` forwards the stream to `vic-engine` as
   `ResponseAudioStart`, `ResponseAudioChunk`, and `ResponseAudioEnd`.
7. `vic-engine` plays it through the existing SDK streaming-audio path.

The answer text remains available for local-TTS fallback.

Audio is raw 16 kHz, mono, signed 16-bit little-endian PCM. Lycopod serves it
progressively using chunked HTTP transfer, so playback can begin after the first
sentence while later sentences are still being generated.

## 3. Known OTA artifacts

From the WireOS repository root:

```bash
ls -lh _build/vicos-3.0.1.105d.ota
sha256sum _build/vicos-3.0.1.105d.ota
```

Expected:

```text
161976320 bytes
d56463ce7ff6eb5faf013ec9ede4ab3334e0128fe928b261067cf903ef7f1779  _build/vicos-3.0.1.105d.ota
```

105d was built from Victor merge commit
`77a7881a6d686459f956bcb9f4ad297e4912988e`, with `EXTERNALS` pinned to
`6aec7f1ddf341a898692768650fe945e334fb865`. Later documentation-only commits
do not change the firmware in this artifact.

The repositories use `origin` for `unbuilt/wire-os` and
`unbuilt/wire-os-victor`, and `upstream` for the corresponding `os-vector`
repositories. Both repositories preserve these branches:

| Branch | Purpose |
| --- | --- |
| `feat/kg-cloud-audio` | Working baseline before incorporating newer upstream commits |
| `integrate/kg-cloud-audio-upstream` | Upstream-integrated firmware used for 105d |

The Victor baseline is `888ee2f9`; the matching WireOS baseline is `3db789c3d`.
Publish the Victor branch before its matching WireOS branch: the parent records
a specific Victor commit, not a moving branch.

The unchanged fallback `_build/vicos-3.0.1.104d.ota` is 161843200 bytes with
SHA-256 `3d9061a40ae8bd2cc95edb28781cf989bea5d31af430aeaaf4c56d96e3dfbb15`.
Only its main cloud-audio path has been confirmed on hardware; the failure and
interruption cases below still need dedicated hardware coverage.

If the artifact is missing or the source has changed since it was generated,
build a new increment instead of reusing `105`:

```bash
cd <wire-os-root>
GOPROXY=https://goproxy.cn GOSUMDB=off \
  ./build/build.sh -bt dev -v <new-increment>
```

The output is `_build/vicos-3.0.1.<new-increment>d.ota`.

## 4. Test variables

Choose these values before starting:

```bash
export WIRE_OS_ROOT=/home/unbuilt/T909/vectoros/wire-os
export LYCOPOD_ROOT=/home/unbuilt/T909/vectoros/lycopod
export ROBOT_IP=<robot-ip>
export SERVER_IP=<lycopod-host-ip-visible-to-the-robot>
export OTA_PORT=8000
export LYCOPOD_WEB_PORT=8080
```

`SERVER_IP` must be a LAN address the robot can reach. Do not use `127.0.0.1`,
`localhost`, or a WSL-only/NAT-only address.

Basic connectivity checks:

If plain SSH reports `Permission denied (publickey)`, the development firmware
may require the key supplied with this tree rather than your personal SSH key.
Older robot SSH servers also require legacy RSA user authentication:

```bash
chmod 600 "$WIRE_OS_ROOT/poky/victor/meta-qcom/recipes-connectivity/openssh/files/ssh_root_key"
ssh -o IdentitiesOnly=yes -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -i "$WIRE_OS_ROOT/poky/victor/meta-qcom/recipes-connectivity/openssh/files/ssh_root_key" \
  root@"$ROBOT_IP" 'cat /etc/os-version'
```

To use the shorter SSH commands throughout this runbook, add a **host-specific**
entry to your local `~/.ssh/config`, substituting the actual IP and absolute
checkout path:

```sshconfig
Host <robot-ip>
    User root
    IdentityFile /absolute/wire-os/poky/victor/meta-qcom/recipes-connectivity/openssh/files/ssh_root_key
    IdentitiesOnly yes
    PubkeyAcceptedAlgorithms +ssh-rsa
```

Do not enable legacy algorithms globally or disable host-key verification.
This shared development key is not appropriate for an Internet-exposed robot.
If `/etc/os-version` reports `0.9.3.0-Unlock`, the robot is running the unlock
image, not OTA 105d; successful SSH alone does not establish that the update
was installed.

```bash
ping -c 3 "$ROBOT_IP"
ssh root@"$ROBOT_IP" 'echo robot-ssh-ok'
```

Make sure the host firewall allows:

| Port | Protocol | Purpose |
| --- | --- | --- |
| 443 | TCP | Primary lycopod Chipper gRPC |
| 8084 | TCP | Secondary compatibility gRPC, if enabled |
| 8080 | TCP | Lycopod health/UI and `/v1/kg-audio` |
| 8000 | TCP | Temporary OTA file server |

## 5. Prepare lycopod

### 5.1 Use the changed lycopod tree

The required server changes include:

- `src/lycopod/audio/answer_store.py`
- `src/lycopod/web/routes_kg_audio.py`
- Cloud-audio integration in the conversation and Chipper paths.

Install or refresh the editable environment using the ASR/provider extras needed
by the existing lycopod setup:

```bash
cd "$LYCOPOD_ROOT"
python -m pip install -e ".[dev]"
```

If the current environment already runs lycopod and its models/providers, do not
replace its working provider setup merely for this test.

### 5.2 Enable Knowledge Graph cloud audio

In lycopod's active `data/apiConfig.json`, retain the existing provider,
endpoint, key, model, STT, certificates, and server settings. Ensure the
`knowledge` object includes:

```json
{
  "knowledge": {
    "enable": true,
    "provider": "<existing-provider>",
    "endpoint": "<existing-provider-endpoint>",
    "key": "<existing-key-if-required>",
    "model": "<existing-model-if-required>",
    "intentgraph": false,
    "intent_fallback": true,
    "cloud_audio": true,
    "cloud_audio_first_sentence": true
  }
}
```

The two relevant settings are:

| Setting | Expected | Effect |
| --- | --- | --- |
| `knowledge.cloud_audio` | `true` | Publish the answer stream and return an `audio_id` |
| `knowledge.cloud_audio_first_sentence` | `true` | Return after the first synthesized sentence so playback can start early |

`knowledge.enable`, a valid provider, and a valid provider endpoint must also be
present or lycopod will not create a conversation engine.

### 5.3 Run the focused lycopod tests

```bash
cd "$LYCOPOD_ROOT"
python -m pytest -q \
  tests/test_answer_store.py \
  tests/test_kg_audio_route.py \
  tests/test_zhix_cloud_audio.py \
  tests/test_chipper_cloud_audio.py \
  tests/test_config.py
```

Do not continue to the robot test if these fail.

### 5.4 Start lycopod with debug logging

Use the same data directory that contains the working certificates and
`apiConfig.json`:

```bash
cd "$LYCOPOD_ROOT"
python -m lycopod --data-dir ./data --web-port "$LYCOPOD_WEB_PORT" --debug
```

The equivalent installed command is:

```bash
lycopod --data-dir ./data --web-port "$LYCOPOD_WEB_PORT" --debug
```

Expected startup evidence includes:

```text
Conversation engine: provider=<provider> cloud_audio=True
Starting gRPC server on port 443
```

Keep this terminal open for the whole test.

### 5.5 Check the lycopod HTTP service

From the WireOS/build host:

```bash
curl --fail --show-error \
  "http://$SERVER_IP:$LYCOPOD_WEB_PORT/healthz"
```

Expected body:

```text
ok
```

Confirm that the cloud-audio route is registered:

```bash
curl -i \
  "http://$SERVER_IP:$LYCOPOD_WEB_PORT/v1/kg-audio/not-a-real-id"
```

Expected result:

```text
HTTP/1.1 404 Not Found
unknown audio_id
```

A `404` here is correct: it proves the route exists, but the test id does not.
A connection refusal or timeout is not correct.

## 6. Confirm the robot points to lycopod

Inspect the robot's active server configuration:

```bash
ssh root@"$ROBOT_IP" 'cat /data/data/server_config.json'
```

The `chipper` host must resolve to the lycopod machine. With the default lycopod
web port, `vic-cloud` derives this endpoint automatically:

```text
http://<chipper-host>:8080/v1/kg-audio
```

The robot resolves the cloud-audio endpoint in this order:

1. `VECTOR_KG_TTS_URL` in the `vic-cloud` environment.
2. `kg_audio` in `/data/data/server_config.json`.
3. The host derived from `chipper`, using port `8080`.

If lycopod uses a different web port, or automatic derivation is not suitable,
add an explicit value while preserving the rest of the JSON:

```json
{
  "kg_audio": "http://192.168.1.50:8080/v1/kg-audio"
}
```

Restart `vic-cloud` after changing this file:

```bash
ssh root@"$ROBOT_IP" 'systemctl restart vic-cloud'
```

Hosts ending in `.anki.com` intentionally do not derive a cloud-audio endpoint.
A robot still pointed at production will use local TTS.

## 7. Flash OTA 105d

### 7.1 Serve the OTA

Open a new terminal:

```bash
cd "$WIRE_OS_ROOT/_build"
python -m http.server "$OTA_PORT" --bind 0.0.0.0
```

Confirm the file is downloadable through the same address the robot will use:

```bash
curl --fail --head \
  "http://$SERVER_IP:$OTA_PORT/vicos-3.0.1.105d.ota"
```

### 7.2 Start the update

The robot must accept development-signed OTAs:

```bash
ssh root@"$ROBOT_IP" \
  "update-os http://$SERVER_IP:$OTA_PORT/vicos-3.0.1.105d.ota"
```

Alternatively, run this from the robot console:

```text
ota-start http://<server-ip>:8000/vicos-3.0.1.105d.ota
```

Wait for the install and automatic reboot. Do not interrupt power.

### 7.3 Verify the installed version

Wait until SSH returns:

```bash
until ssh -o ConnectTimeout=3 root@"$ROBOT_IP" true 2>/dev/null; do
  sleep 5
done

ssh root@"$ROBOT_IP" 'cat /etc/os-version; cat /anki/etc/version'
```

The reported development version must identify build `105`. Do not rely only
on the downloaded filename: a cached version recipe can leave an incremental
image reporting an earlier version.

After the robot reconnects, repeat the server-config check because the test
depends on the robot still pointing to lycopod:

```bash
ssh root@"$ROBOT_IP" 'cat /data/data/server_config.json'
```

## 8. Run the end-to-end test

### 8.1 Watch robot logs

The services run through `/usr/bin/logwrapper`, whose default destination is
the Android system log. Capture that output as well as the systemd journal;
an empty journal is not proof that the robot did not attempt an HTTP fetch:

```bash
ssh root@"$ROBOT_IP" 'logcat -v threadtime' | tee robot-cloud-audio.log
```

Start this before asking the question, and stop it with Ctrl-C afterward.
Do not clear the log buffer: earlier errors may explain the failure.

Open one terminal for `vic-cloud`:

```bash
ssh root@"$ROBOT_IP" \
  'journalctl -f -u vic-cloud --no-pager'
```

Open another for `vic-engine`:

```bash
ssh root@"$ROBOT_IP" \
  'journalctl -f -u vic-engine --no-pager'
```

For a cleaner capture, start just before the question:

```bash
ssh root@"$ROBOT_IP" \
  'journalctl -u vic-cloud -u vic-engine --since "1 minute ago" -f --no-pager'
```

Keep the lycopod debug terminal visible as well.

### 8.2 Ask a multi-sentence question

Use a question likely to produce at least two sentences. For example:

```text
Hey Vector, I have a question.
Explain why the sky is blue in two short sentences.
```

A multi-sentence response is useful because it exercises progressive delivery,
including the gap between sentences.

### 8.3 Expected audible behavior

A pass should sound like this:

- Vector enters the normal Knowledge Graph searching animation.
- Speaking begins once cloud audio meets the existing 750 ms audio prebuffer
  (or a shorter answer completes). Ready audio interrupts the searching animation;
  it does not wait for the current loop or a success-exit animation to finish.
- The answer uses lycopod's/provider's voice, not Vector's local Acapela voice.
- Cloud answers omit the extra Knowledge Graph response acknowledgement chime;
  wake/listening cues and local-only response cues remain unchanged.
- Later sentences continue without truncating the answer.
- Vector does not repeat the full answer in the local voice.
- The normal success reaction occurs after playback.

## 9. Required success evidence

### 9.1 Lycopod

With debug logging enabled, expect one stream id to appear through the lifecycle:

```text
Created answer audio stream <audio_id>
Streaming answer audio <audio_id> to <robot-ip>
Answer audio <audio_id> complete: <bytes> bytes (<seconds>s)
Answer audio <audio_id> sent: <bytes> bytes (<seconds>s)
```

`Created answer audio stream` is a debug message, which is why `--debug` is
recommended.

### 9.2 `vic-cloud`

Expect:

```text
Cloud audio: streamed <bytes> bytes in <chunks> chunks for <response_id>
```

In the corrected firmware this is an ordinary information log, as are:

```text
Knowledge Graph response: audio_id_present=true cloud_audio_available=true response_id=<response_id>
Cloud audio: starting fetch for <response_id> (audio_id_present=true)
```

These distinguish a missing gRPC response from a failed HTTP fetch without
enabling verbose transcript logging. In 103d the final streamed-byte message
was verbose-only and these two diagnostic messages were absent.

There must not be:

```text
Cloud audio: fetch failed:
Cloud audio: stream failed:
```

### 9.3 `vic-engine`

Expect the same `response_id` through these events:

```text
UserIntentComponent.CloudAudio.Start
UserIntentComponent.CloudAudio.End
Cloud audio ready for <response_id> (16000 Hz, 1 ch)
Streaming cloud audio for <response_id> starting with <bytes> buffered bytes
Cloud audio stream for <response_id> ended after <bytes> bytes
Cloud audio playback complete for <response_id>
```

The exact final completion path can vary slightly because one log reports the
stream being closed and another reports anim playback completion. The critical
evidence is:

1. A cloud-audio start was accepted.
2. Playback started with 16 kHz mono PCM.
3. A non-zero byte count was sent.
4. No fallback or playback-failure line appeared.

## 10. Directly inspect a generated stream

When lycopod logs an `audio_id`, a non-robot client can fetch it:

```bash
curl -N \
  "http://$SERVER_IP:$LYCOPOD_WEB_PORT/v1/kg-audio/<audio_id>" \
  --output answer.pcm
```

The stream is removed after it is consumed, so this competes with the robot for
the same one-use `audio_id`. Use this only for a dedicated test request or while
debugging a robot fetch failure.

Inspect the result:

```bash
ls -lh answer.pcm
ffplay -f s16le -ar 16000 -ac 1 answer.pcm
```

When writing a Python streaming probe, use `read1(n)` rather than `read(n)`.
`read(n)` may wait for all `n` bytes or EOF and make progressive delivery look
buffered.

## 11. Fallback tests

Run these only after the normal path passes.

### 11.1 Disable server-side cloud audio

Set in lycopod's `apiConfig.json`:

```json
"cloud_audio": false
```

Restart lycopod and ask another question.

Expected:

- Lycopod reverts to its legacy SDK reverse-connection playback path. That
  requires the existing SDK robot credentials/connectivity.
- No `audio_id` stream is created.
- No answer-audio HTTP fetch is expected.

This is not an isolated local-Acapela test. To test local TTS without legacy SDK
playback, leave this setting `true` and use the robot-side override below.
Restore `true` afterward.

### 11.2 Disable the robot endpoint

Set `VECTOR_KG_TTS_URL=off` in the `vic-cloud` service environment and restart
`vic-cloud`.

Expected:

- `cloud_audio_available=false` is used on the robot side.
- The answer uses local TTS.
- Normal Knowledge Graph text behavior remains functional.

Remove the override after the test.

### 11.3 Make the HTTP endpoint unreachable

Temporarily set `kg_audio` to an unused port, restart `vic-cloud`, and ask a
question:

```json
"kg_audio": "http://192.168.1.50:65530/v1/kg-audio"
```

Expected:

```text
Cloud audio: fetch failed:
Cloud audio unavailable for <response_id> (error); falling back
```

The returned answer text should then be spoken locally. Restore the valid endpoint
and restart `vic-cloud`.

## 12. Failure behavior in the corrected implementation

| Situation | Expected result |
| --- | --- |
| No cloud-audio endpoint resolves | Local Acapela TTS |
| Lycopod does not return cloud audio | Local Acapela TTS |
| HTTP fetch fails before playback | Local Acapela TTS |
| Dedicated self-hosted KG request has no response within 60 seconds | Report request failure; the engine allows 65 seconds so it does not discard an otherwise valid response early |
| Audio is not ready within 20 seconds after the answer | Local Acapela TTS |
| Stream fails/stalls before 1.5 seconds has been confirmed played | Cancel cloud playback and speak the returned answer text with local TTS |
| Stream fails/stalls after 1.5 seconds has been confirmed played | Drain available audio, then show a failure reaction instead of success; do not repeat the answer |
| Anim reports `PrepareFailed`, `PostFailed`, `AddAudioFailed`, or `BufferOverflow` | Cancel playback; fall back locally only if less than 1.5 seconds was confirmed played |
| Answer exceeds 60 seconds of PCM | Reject/end the stream |
| Gap occurs between synthesized sentences | Keep the stream alive and pad the gap with silence |

The longer request budget applies only to the dedicated "I have a question"
Knowledge Graph RPC when a cloud-audio endpoint resolves. Normal intent and
Intent Graph requests retain their existing nine-second budget. Test the
two-step Knowledge Graph interaction first; do not use a slow single-utterance
Intent Graph answer to assess the dedicated KG deadline fix.

Playback now budgets from confirmed played-frame counters, including audio
still in transit to anim. Silence does not create extra send capacity. Success
requires anim's playback-complete event; watchdog expiry cancels the player
rather than falsely reporting completion.

With `cloud_audio_first_sentence=true`, the initial gRPC response may contain
only the first sentence's text. Local fallback can speak that returned text,
not later sentences that were never delivered as text. The normal cloud-audio
path continues streaming the rest of the spoken answer.

## 13. Troubleshooting

### Lycopod health check is unreachable

- Confirm lycopod printed that the web server started on port `8080`.
- Check host firewall rules.
- Confirm `SERVER_IP` belongs to the lycopod host and is reachable from the LAN.
- If lycopod runs in Docker, publish both `443` and `8080`.
- If it runs in WSL, ensure the robot can reach the exposed Windows/WSL address.

### The robot answers, but always with local Acapela TTS

Check in this order:

1. Lycopod startup says `cloud_audio=True`.
2. The active `apiConfig.json` has `knowledge.enable=true`.
3. The robot's `chipper` host points to lycopod.
4. `/data/data/server_config.json` has the correct optional `kg_audio`.
5. No `VECTOR_KG_TTS_URL=off` override is active.
6. `knowledgeGraphQuestion.json` in the OTA has
   `cloudAudioEnabled: true`.
7. Look for `Cloud audio unavailable ... falling back` in `vic-engine`.

### Lycopod creates audio, but the robot never fetches it

Audio generation alone does not prove the robot received the gRPC answer.
Check the stages below in order before rebuilding or changing timeouts.

**1. Identify the installed daemon.**

```bash
ssh root@"$ROBOT_IP" \
  'cat /etc/os-version; sha256sum /anki/bin/vic-cloud'
```

The daemon packaged in the upstream-integrated 105d system image has SHA-256:

```text
416aedba897907b735656a92dde8e2c793762f3d73f8f123969c207018824c81
```

That binary contains `maybeSendCloudAudio` and `VECTOR_KG_TTS_URL`. It is
UPX-compressed, so searching the installed binary directly with `strings`
can incorrectly suggest that those symbols are absent. A different checksum
means a different binary, not necessarily an invalid one.

**2. Capture the response lifecycle, not only cloud-audio messages.**

Use the `logcat` capture from section 8.1. After reproducing the question, filter
the saved host-side log:

```bash
grep -Ei \
  'vic-cloud|CCE error|deadline|Ignoring.*(response|result)|CloudAudio|cloud audio' \
  robot-cloud-audio.log
```

Look for `Intent response -> *chipper.KnowledgeGraphResponse`, which is logged
even without verbose mode when the response arrives successfully. Also look
for `CCE error:`, `context deadline exceeded`, or
`Ignoring response on closed context`.

In **103d**, `vic-cloud` uses a **9-second Chipper request deadline**. It
starts when the voice stream is created, so connection setup, speaking,
end-of-speech detection, and waiting for the first answer all consume that
budget. It is not nine seconds after the user finishes speaking.

Lycopod can finish generating/saving audio after the robot has timed out.
Compare the timestamps of `KG request`, `Cloud answer ... started`, and
`streaming conversation response` with the robot's deadline/error logs.
If the response arrives too late, the HTTP fetch never starts. The separate
6-second engine audio-ready timeout and 60-second HTTP timeout only matter
after the gRPC response has been accepted; changing them does not fix this
earlier failure.

104d and 105d use a dedicated cloud-audio KG budget of 60 seconds, and the engine
waits 65 seconds. A nine-second timeout in the two-step interaction on these builds
suggests the cloud-audio endpoint is disabled/unresolved or the wrong daemon
is running. Normal Intent Graph requests still have the nine-second budget.

**3. Check endpoint overrides without exposing unrelated credentials.**

Inspect only the `chipper` and `kg_audio` fields in the robot's active
`/data/data/server_config.json`. Also inspect the running daemon's override:

```bash
ssh root@"$ROBOT_IP" '
  for p in /proc/[0-9]*; do
    if [ "$(cat "$p/comm" 2>/dev/null)" = "vic-cloud" ]; then
      tr "\000" "\n" < "$p/environ" | grep "^VECTOR_KG_TTS_URL="
    fi
  done
'
```

No output means no such environment override was found; confirm that
`vic-cloud` is running before interpreting that result. `off` disables the
fetch. A non-empty override takes precedence over `kg_audio`. Redact any
credentials in custom URLs before sharing output.

If the gRPC response arrives but no HTTP request appears at lycopod, check
for a wrong derived host, DNS resolution failure, or a non-default web port.
Do not replace the complete server-config file with a `kg_audio`-only object.

**4. Probe from the robot, not just the server host.**

In a robot SSH shell, if `curl` is installed:

```bash
curl --connect-timeout 3 --max-time 5 -i \
  http://<lycopod-lan-ip>:8080/v1/kg-audio/not-a-real-id
```

Expect `404` with `unknown audio_id`. A timeout/refusal is a network or
listener issue. This fake-id probe does not consume a real answer stream.
If curl is unavailable, use the robot's available HTTP client instead.

Capture the lycopod log covering the same request. Distinguish
`Created answer audio stream`/`Cloud answer ... started` from
`Streaming answer audio ... to ...`: only the latter proves a client fetched
the stream. Include the startup `cloud_audio=True/False` line; the legacy
SDK playback path can also generate audio without publishing a fetchable id.

Do not enable unrestricted verbose logging or share whole configuration files
unless necessary: transcripts, provider URLs, and credentials may be present.

### The route returns `404 unknown audio_id`

Possible causes:

- The id is wrong.
- The 120-second store TTL expired.
- A previous client already consumed the one-use stream.
- The in-memory store evicted it because more than 16 streams were active.

Generate a new answer and use its new id.

### Audio starts, then falls back or stops

Search `vic-engine` for:

```text
UserIntentComponent.CloudAudio.SeqGap
UserIntentComponent.CloudAudio.Error
Cloud audio for <response_id> failed
Cloud audio for <response_id> stalled
Cloud audio playback failed
```

Also inspect lycopod for producer errors, first-byte stalls, client disconnects,
or an answer exceeding the 60-second limit.

### Audio is distorted or plays at the wrong speed

Confirm the route and producer use:

```text
pcm_s16le
16000 Hz
1 channel
```

The robot does not accept WAV headers on this path; it expects raw PCM.

## 14. Pass/fail record

Record one row per test:

| Field | Value |
| --- | --- |
| Date/time | |
| Robot ESN/name | |
| Robot IP | |
| OTA version | `3.0.1.105d` |
| OTA SHA-256 verified | yes / no |
| Lycopod commit/tree state | |
| Provider/model | |
| Question | |
| `audio_id` | |
| `response_id` | |
| Bytes/chunks | |
| First audible response latency | |
| Correct cloud voice | yes / no |
| Full answer completed | yes / no |
| Unexpected local-TTS fallback | yes / no |
| Result | pass / fail |
| Relevant log excerpt | |

The normal-path test passes only when the audible cloud voice and the server,
`vic-cloud`, and `vic-engine` evidence all agree.
