# Isolated Chinese wake-word trial

The replay tools run **offline**, without connecting to a robot or changing
installed firmware. The experimental live backend can also be included in an
OTA using the explicit opt-in below. Standard OTAs default to Picovoice and
exclude sherpa's runtime and models. The only configured sherpa phrase is
**你好小维** ("ni hao xiao wei").
The token file uses the selected model's partial-pinyin vocabulary:
`n ǐ h ǎo x iǎo w éi @你好小维`.

## Model and runtime

Run commands from the outer `wire-os` directory, not the nested `anki/victor`
repository. Downloads, builds, and replay results stay under
`wire-os/_build/sherpa-kws`.

```sh
bash anki/victor/tools/audio/sherpa_kws/prepare_model.sh
```

The model script verifies the archive SHA256 and selects the INT8 encoder,
decoder, and joiner from
`sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01`.
Model assets are not committed; only the selected files are packaged when the
experimental firmware feature is enabled. See the
[upstream model documentation](https://k2-fsa.github.io/sherpa/onnx/kws/pretrained_models/index.html)
for model provenance. Model redistribution terms must be reviewed before wider firmware distribution.

`prepare_model.sh fp32` additionally extracts the matching floating-point
models from the same verified archive for comparison. It does not change
the live backend or the OTA's selected INT8 assets.

The runner uses sherpa-onnx 1.12.14's C API, ONNX Runtime 1.17.1, a CPU provider,
and one inference thread. Prepare the pinned host runtime, then the runner:

```sh
JOBS=4 bash anki/victor/tools/audio/sherpa_kws/prepare_runtime.sh host
bash anki/victor/tools/audio/sherpa_kws/build_trial.sh host _build/sherpa-kws/host
bash anki/victor/tools/audio/sherpa_kws/build_trial.sh tests
bash anki/victor/tools/audio/sherpa_kws/build_trial.sh vad
```

The native source build requires the existing compiler/CMake/Ninja toolchain.
Host runtime preparation is for Linux x86_64. The target source build pins a
compatible CMake executable locally if necessary. It uses a checked-in
FlatBuffers compatibility patch for Clang 20; it does not edit the SDK.

`vad` builds the repository's actual ARM SVad implementation with the Vector
SDK and vendor library. It does not substitute a new energy threshold or
Silero VAD. The local replay script runs that executable under the repository's
QEMU with its existing target rootfs.

## Try the phrase

Replay requires `ffmpeg` and the QEMU/rootfs produced by the repository's
build. For a synthetic fixture, also prepare the optional offline Mandarin
voice and use a Python environment with `requirements-fixture.txt` installed.
This voice is workstation-only and is not part of the KWS runtime:

```sh
bash anki/victor/tools/audio/sherpa_kws/prepare_fixture.sh
# If the optional Python package is not already available:
python3 -m venv _build/sherpa-kws/fixture-env
_build/sherpa-kws/fixture-env/bin/pip install \
  -r anki/victor/tools/audio/sherpa_kws/requirements-fixture.txt
KWS_FIXTURE_PYTHON="$PWD/_build/sherpa-kws/fixture-env/bin/python" \
bash anki/victor/tools/audio/sherpa_kws/run_trial.sh
```

Each run creates a new output directory. Alternatively, use a local human
recording, which does not require the TTS model or Python fixture package:

```sh
bash anki/victor/tools/audio/sherpa_kws/run_trial.sh /path/to/recording.wav
```

The script adds two seconds of leading silence and four seconds of trailing
silence, converts to mono PCM16/16 kHz, obtains SVad activity decisions, and
runs both continuous and VAD-gated KWS in **separate host processes**.
No audio is sent to an external service.

Inspect `continuous.jsonl` and `gated.jsonl` in the printed output directory.
A `keyword` event must contain `"keyword":"你好小维"` to establish detection.
A successful exit for a supplied recording means replay completed, not that
the phrase was found. The no-argument synthetic trial requires exactly one
detection in each mode and fails otherwise. Direct runner invocations can
likewise use `--expect-detections COUNT`.
Synthetic speech is only an infrastructure smoke fixture; it cannot establish
human accuracy, false-activation rates, or far-field performance.

The experimental defaults are score **3** and threshold **0.1**. The selected
synthetic phrase was missed at score 1 / threshold 0.25; the more permissive
settings are a starting point for the trial, not calibrated production values.
Do not infer acceptable false-activation rates from a few positive/negative clips.

## Gate behavior

The default gate retains 500 ms of pre-roll. It feeds continuous audio,
including pauses, until SVad has been inactive for another 1000 ms. SVad
already has its own approximately 750 ms hangover. The extra 1000 ms mirrors
the intent of the production microphone processor's default quiet cooldown;
this replay is not a bit-exact recreation of its robot-state logic.

Each new speech session creates a **fresh sherpa stream**, not just a decoder
reset on old feature state. A detection resets keyword decoding within the
current stream. One second of zero padding at EOF lets the finite-file
experiment flush the last detection; live integration would feed real audio.

For direct use:

```sh
_build/sherpa-kws/replay-host/kws-replay \
  --model-dir _build/sherpa-kws/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01 \
  --keywords anki/victor/tools/audio/sherpa_kws/keywords.txt \
  --wav input.wav \
  --activity input.activity \
  --score 3 --threshold 0.1 --pre-roll-ms 500 --hangover-ms 1000
```

Omit `--activity` for continuous inference. Activity files must contain exactly
one `0` or `1` per 10 ms block, including the final partial block. They must
correspond to the same recording. Invalid lengths and values are rejected.

## Interpreting measurements

- `cpu_rtf`: replay CPU seconds divided by input audio seconds. Model loading
  is excluded, but EOF flushing is included. This is not live CPU utilization.
- `wall_rtf`: elapsed replay time divided by audio duration; below one is only
  a throughput prerequisite, not a guarantee against live queue stalls.
- `max_push_ms`: worst synchronous block submission, including pre-roll
  catch-up; not measured end-to-end wake latency.
- `peak_rss_kib`: whole-process Linux `VmHWM`, including runtime, loaded
  WAV, model, and decoding state. `pre_model_peak_rss_kib` is a separate
  high-water reading, not a precise allocation baseline.
- `fed_audio_seconds`: audio actually submitted to KWS, including EOF padding
  when a stream is active. It is not a measured CPU-saving percentage.

The VAD decisions are precomputed: KWS timings **exclude live VAD CPU cost**.
Host timings and QEMU timings must never be presented as Vector timings.
`available_at_s` is the input position when detection becomes available,
including pre-roll catch-up. It is not wall time or keyword onset.

## Measuring the physical robot

The standalone runner accepts `--model-precision int8|fp32` (default `int8`),
`--threads 1` (range 1..4), and `--execution host|qemu|robot`. The execution
label records where the caller actually runs the executable; it does not
connect to that environment.

After preparing both model precisions and building the target replay tool,
compare them on the robot with a previously generated trial directory:

```sh
bash anki/victor/tools/audio/sherpa_kws/prepare_model.sh fp32
bash anki/victor/tools/audio/sherpa_kws/build_trial.sh target _build/sherpa-kws/target
KWS_BENCHMARK_PRECISIONS='int8 fp32' \
bash anki/victor/tools/audio/sherpa_kws/benchmark_robot.sh ROBOT_HOST _build/sherpa-kws/trial.XXXXXXXX
```

This explicitly connects over SSH, uploads temporary replay inputs, and defaults
to the already-installed `/anki/lib` runtime. An optional third argument points
to a local candidate runtime installation: its two libraries are uploaded beside
the replay executable and selected only for that subprocess. Installed libraries
are never overwritten. It does not restart services, change
the active backend, record microphones, or alter CPU/thermal controls. Temporary
remote files are removed on exit; results and input/runtime hashes remain under
`_build/sherpa-kws/robot-benchmark.XXXXXXXX`. `KWS_SSH_KEY` can select an existing
SSH identity instead of `anki/victor/robot_sshkey`.

By default, only INT8 is replayed to avoid unnecessary load.
`KWS_BENCHMARK_THREADS` selects 1..4 inference threads (default 1). A candidate
with multiple threads should explicitly disable ONNX Runtime session spinning:
otherwise idle decoder/joiner thread pools can burn CPU and distort the tradeoff.

The optional `optimized` runtime variant enables ARM ONNX Runtime contrib ops
(including quantized-matmul fusions) and disables actual intra/inter-op session
spinning. It preserves the caller's thread count. Its source/build trees and
`host-optimized` / `target-optimized` installations are separate from the baseline;
neither the existing OTA nor its baseline runtime libraries are overwritten:

```sh
bash anki/victor/tools/audio/sherpa_kws/prepare_runtime.sh target optimized
KWS_BENCHMARK_THREADS=2 \
bash anki/victor/tools/audio/sherpa_kws/benchmark_robot.sh ROBOT_HOST \
  _build/sherpa-kws/trial.XXXXXXXX _build/sherpa-kws/target-optimized
```

Use `host optimized` to prepare the corresponding host variant. Build success
and emulated detection do not establish an on-robot performance improvement.

Each replay is limited to 30 CPU seconds, including model loading. It adds
load alongside the running robot, so do not repeatedly benchmark a hot robot.
The helper refuses extra load if any reported thermal zone is at least 75 C,
both before uploading and before each replay. This is a conservative benchmark
guard, not a change to the robot's thermal policy.
Compare `replay_cpu_seconds` and `replay_wall_seconds` against
`fed_audio_seconds`, not just the silence-padded WAV duration: a low whole-file
RTF can hide failure to keep up during an active speech session. The fixed
500 ms pre-roll is submitted in a burst, not a deliberate wake-response timer.

An initial warm-robot comparison at a 729.6 MHz CPU cap fed 3.57 seconds of
audio. INT8 needed 5.24 CPU / 5.93 wall seconds; FP32 needed 5.88 CPU / 6.68
wall seconds. Both detected the synthetic phrase, but neither kept up with
the submitted audio under those conditions. FP32 was not an improvement.
These are standalone replay measurements alongside the live application,
not end-to-end wake latency or isolated KWS RAM overhead.

## Before live integration

Vector requires ARMv7 `softfp` and a compatible libc/C++ runtime. Stock
`arm-linux-gnueabihf` libraries are not a substitute. The target preparation
script builds ONNX Runtime and sherpa from source with the existing SDK:

```sh
JOBS=4 bash anki/victor/tools/audio/sherpa_kws/prepare_runtime.sh target
bash anki/victor/tools/audio/sherpa_kws/build_trial.sh target _build/sherpa-kws/target
KWS_REPLAY_PLATFORM=target bash anki/victor/tools/audio/sherpa_kws/run_trial.sh
```

`target` runs the ARM binary under QEMU, never on a network-connected robot.
The optional `KWS_FIXTURE_PYTHON` setting also applies to this command.
`execution.txt` in each trial directory labels host versus target/QEMU replay.

Inspect `target/abi-report.txt` and `target/runtime-ready.txt`. The runtime
built with this SDK requires GLIBC symbols through **2.38** plus compatible
libc++/libunwind; this is not a package for original Anki-era firmware.
The target runner and libraries have been exercised under QEMU using this
repository's rootfs. That proves execution compatibility in the emulated
environment, not performance or behavior on a physical robot.

The production VAD operates on a raw microphone channel, while KWS receives
processed mono audio. Replaying SVad on the mono fixture does not model this
difference. The production pipeline also forces activity during some robot
noise states. Check those conditions, mute handling, short pauses, quiet/distant
speech, TV playback, robot speech, thermal load, and audio queue pressure on
hardware before considering production use.

## Experimental live backend

The live `SpeechRecognizerSherpaOnnx` adapter uses the same gate as the replay.
It receives processed mono PCM and the **VAD flag captured with that same
audio block**, not the newest flag from the raw-audio worker. It retains 500 ms
of pre-roll. The live flag already contains SVad's hangover and the existing
microphone quiet cooldown, so the adapter does not add a second 1000 ms tail.

Detections use the existing wake response, direction, earcon, and command
stream callback. Alexa and the AEC configuration are unchanged. Mute
transitions clear the stream/pre-roll on the recognition thread, and pending
mute resets suppress callbacks from in-flight inference.
The microphone queue also rejects pre-transition capture sequences after mute
or unmute, so buffered old audio cannot restart a discarded speech session.
The adapter emits
`SpeechRecognizerSherpaOnnx.Stats` about every ten seconds of received audio,
including submitted audio duration, thread CPU time, and maximum update time.
These logs are for evaluating real hardware load.
The stock `console_filter_config.json` disables the `SpeechRecognizer`
channel, so those INFO messages are filtered unless that channel is enabled.
Their absence from the journal does not mean the recognizer is idle.

The live build uses CMake option `SHERPA_KWS`, which exports
`ANKI_SHERPA_KWS=0/1` to the animation library and its dependents. An ordinary
build leaves this feature off. An enabled build selects the backend at process startup using:

1. `/data/data/com.anki.victor/persistent/kws/backend`, if present.
2. `ANKI_KWS_BACKEND` from the process environment otherwise.
3. Picovoice if neither specifies a backend.

Supported values are `sherpa_onnx` and `picovoice`. Invalid settings or failed
sherpa initialization are logged and fall back to Picovoice. A failed sherpa
stream also logs the failure and attempts Picovoice recovery. Only one engine
is fed audio; the Picovoice engine is initialized lazily when needed.

The sherpa assets are installed at:

```text
/anki/data/assets/cozmo_resources/assets/sherpaKws/
```

For data-driven checks of the actual production adapter, use a neural-positive
trial directory printed by `run_trial.sh`:

```sh
bash anki/victor/tools/audio/sherpa_kws/validate_live.sh host _build/sherpa-kws/trial.XXXXXXXX
bash anki/victor/tools/audio/sherpa_kws/validate_live.sh target _build/sherpa-kws/trial.XXXXXXXX
```

The target integration runner uses QEMU's Cortex-A7 CPU profile. It still
does not substitute for measurements on the physical robot.

On the robot, this persistent override restores Picovoice without reflashing:

```sh
mkdir -p /data/data/com.anki.victor/persistent/kws
printf '%s\n' picovoice > /data/data/com.anki.victor/persistent/kws/backend
systemctl stop vic-engine.service && \
  systemctl restart vic-anim.service && \
  systemctl start vic-engine.service
```

Restart both application layers in this order: the engine exits when its
animator connection disappears, and `vic-engine.service` has `Restart=no`.
Restarting only the animator can leave the face running but the behavior
engine stopped. After switching, both services must be active and the engine
must reach `Running` / `robot.engine_ready`, not merely have a live PID.

Write `sherpa_onnx` instead to select the experimental backend again. The
override survives OTA updates; it takes precedence over the OTA's default.
This requires a sherpa-enabled build. Picovoice-only firmware does not contain
the sherpa backend or assets; a stale sherpa override logs an unavailable-backend
error and uses Picovoice instead.
The experimental sensitivity (score 3, threshold 0.1) is deliberately not
presented as a calibrated production setting.

## Building a development OTA (Picovoice default)

`build_ota.sh` uses the existing `vic-yocto-builder-7:latest` container. Its
interface is `build_ota.sh VERSION ACTION [--backend picovoice|sherpa_onnx]`.
The default is **Picovoice**: its scoped BitBake configuration sets
`SHERPA_KWS=OFF`, removes stale sherpa payloads, and leaves the animation
environment without a backend override. No sherpa runtime or model preparation
is required; `prepare` is a no-op for this backend.

The helper does not edit `local.conf`, flash a robot, or change AEC/Alexa
settings. OTA `3.0.1.118d` uses this Picovoice-only configuration. For another
build, choose an unused version; existing OTA files cannot be overwritten:

```sh
version=119
bash anki/victor/tools/audio/sherpa_kws/build_ota.sh "$version" configure
bash anki/victor/tools/audio/sherpa_kws/build_ota.sh "$version" compile
bash anki/victor/tools/audio/sherpa_kws/build_ota.sh "$version" build
bash anki/victor/tools/audio/sherpa_kws/build_ota.sh "$version" package
```

Sherpa remains explicitly experimental. To reproduce a sherpa-enabled build,
prepare the baseline target runtime first, then run `prepare`, `configure`,
`compile`, `build`, and `package` with `--backend sherpa_onnx` on **every**
invocation. That mode sets `SHERPA_KWS=ON` and packages
`ANKI_KWS_BACKEND=sherpa_onnx`, the selected models, runtime libraries, and notices.

The `package` step decrypts the actual OTA and checks the boot signature, image
metadata, linked binaries, and backend-specific payload. For Picovoice, this
includes confirming that sherpa libraries, models, and notices are absent.
Evidence stays under `_build/sherpa-kws-ota-VERSION-validation/`. To repeat
verification without rebuilding:

```sh
bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 118 verify
bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 117 verify --backend sherpa_onnx
```
