# On-robot playback-reference / AEC feasibility experiment

This is an **opt-in bench experiment**, not full-duplex conversation, barge-in, or
next-turn capture. Nothing is enabled by installing the image. No server changes
are needed. **OTA111 produced intact diagnostic recordings and a short,
timing-valid speaker-only ON capture, but incremental acoustic benefit remains
inconclusive. The subsequent human ON overlap take failed readiness twice.**
Sustained timing reliability and human speech preservation under ON remain
unproven. Runtime settings are volatile; verify the current mode rather than
inferring it from a historical trial. No lycopod changes are part of this work.

For the simpler proposed next milestone, see
[Default-on Automatic Follow-up Multi-turn Mode](automatic_follow_up_mode.md).
It reopens listening after playback and does not require adaptive AEC or duplex.

## OTA111: corrected binary WAV writer

**New diagnostic recordings require OTA111 or newer.** OTA110's first hardware
capture exposed binary corruption in its writer, despite the robot reporting
`saved`, `write_complete=true` and `errors=0`. The host helper now rejects OTA110
before SDK initialization. Instrumentation revision 110 / schema 1 remain
unchanged; they are not the firmware version or proof of the writer fix.
The clock controller and reference-only cancellation/adaptation restrictions
are unchanged.

### First OTA110 hardware recording: retained, not usable for alignment

At the user's request, the volatile `90-aec-experiment.conf` was installed with
reference mode, mic age 10 ms and reference delay 0 ms, followed by a service
restart. Normal SDK control was granted. The non-repeating 10-second noise
fixture played at volume 25, and the 15-second diagnostic trial was downloaded:

```
_build/aec-results/110-diagnostic-speaker-volume25-20260906-2036/
  trial_1210_343126172461/
```

| Artifact | Declared frames | Expected bytes | Actual bytes |
|---|---:|---:|---:|
| Four-channel `raw.wav` | 234560 | 1876524 | 1876523 |
| Mono `reference.wav` | 480256 | 960556 | 960555 |

The source robot's file lengths match the downloads. CSV sample counts agree
with the WAV headers, but each WAV is one byte short. Their byte-alignment
changes indicate interior corruption, not a safely removable final partial
frame. The original six files are preserved unchanged; **no padding, insertion,
repair, or waveform-alignment claim is made**. `integrity-report.json` beside
the trial retains independent SHA-256 hashes and exact length discrepancies.
The strict offline analyzer rejected this trial rather than trusting the
firmware's completion status or checksums of already-corrupted bytes.

The mic clock independently rejected its startup fit (about +2387.9 ppm
period correction, outside the existing 2000 ppm bound); reference tracking
remained healthy during the capture. CSV-only observations are retained in
`csv-observation-report.json`. They remain host timing observations, not
physical oscillator measurements. The legacy SDK also raised `CancelledError`
during disconnect after playback and firmware completion; that cleanup error
does not explain the writer's byte corruption.

### Reproduced cause and bounded correction

Offline execution with the deployed ARM libc++ runtime and the **unchanged
OTA110 production writer object** reproduced silent interior deletions:
`-fsigned-char` callers pass `0xff` as -1 to `ostream::put(char)`, and the
runtime overflow path interprets it as EOF. Flush/close still report success.
Synthetic all-FFFF, complete-PCM16-range and isolated-boundary patterns match
the deletion model byte-for-byte. Exact-object controls place the problematic
input positions at zero-based whole-file offsets `4096*k`, not `4096*k-1`.
This establishes the runtime failure mechanism, **not the exact original
missing-byte positions in the robot recordings**.

The corrected writer:

* Serializes explicit little-endian WAV bytes in 4096-byte worker-side chunks
  using POSIX writes, handling interruption/short writes and rejecting zero
  progress. It does not change the global compiler ABI or runtime library.
* Checks fsync/close, the exact expected 44-byte header and complete PCM length.
* Independently hashes the intended serialization and requires matching
  readback bytes/hash before publishing the final manifest. Hashing whatever
  happened to reach disk is no longer sufficient.

No real-time producer, buffer lifetime, clock algorithm or recorded schema is
changed. Evidence is under `_build/aec-writer-arm-investigation/`, including
`writer-repair-report.json`, `exact-legacy-object/report.json` and expanded
full-range pattern controls. The corrected production-object writer passed
byte-exact ARM regressions. A broader QEMU rotation run hit a host-directory
32-bit offset overflow; that environment-specific result is not claimed to be
a hardware rotation failure or a passing rotation test.

The corrected image now has two intact physical diagnostic captures, described
below. Do not use the old corrupt WAVs as an AEC baseline or loosen the timing
guards.

### OTA111 artifact and validation

```
_build/vicos-3.0.1.111d.ota
162027520 bytes
SHA-256 afea2988a7ae756f1b0ff2f5998b20296fa59b533ad1c123c1974b1c6449095e
```

The image is default OFF. Whole-firmware build and decrypted BOOT/SYSTEM
manifest checks passed; `/etc/os-version` is `3.0.1.111d`, and packaged
`vic-anim`, `vic-robot`, `vic-engine` and `libaudio_engine.so` match their
compiled binaries. The host diagnostic helper requires firmware >=111 while
retaining instrumentation revision 110 / schema 1.

Validation passed **60 native / 52 sanitized / 24 Python tests**, both actual
ARM vendor variants, and four corrected-production-object ARM writer tests
with six independent waveform byte comparisons. These cover all-FFFF and
every PCM16 value, interrupted/partial writes, no-progress failure, truncated
headers/payloads and same-size content corruption. Production-object writer
regressions are integrated into the existing vendor validation command.

The broader ARM diagnostic run was **12/13, not fully passed**: rotation hit
`EOVERFLOW` from 32-bit directory enumeration on the QEMU host filesystem.
An independent 64-bit enumeration control found all 103 entries; native
rotation passed. Production directory enumeration was not changed for this
writer correction, and this limitation is retained explicitly in the report.

Executed build/validation commands from the repository root:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 111 build 111
bash anki/victor/tools/audio/validate_aec_experiment.sh _build/aec-experiment-111-validation native
bash anki/victor/tools/audio/build_aec_experiment.sh 111 vendor 111
bash anki/victor/tools/audio/build_aec_experiment.sh 111 package 111
```

`_build/aec-experiment-111-validation/summary.json` records scope, hashes,
commands, results and the QEMU limitation. Packaging and decrypted verification
are in `package.log`; the final host-only minimum-version gate is covered by
`python-after-helper-gate.log`. OTA101-110 and all six original recording files
were checked unchanged. No further robot deployment or playback was performed
while implementing and packaging the repair.

### OTA111 physical diagnostics (2026-09-06, 21:56 and 21:59)

After the user installed 111, reference mode was enabled with mic age 10 ms
and reference delay 0 ms. Both ten-second non-repeating noise playbacks used
normal SDK control, with cancellation and adaptation disabled:

| Volume | Trial | Local directory |
|---|---|---|
| 25 | `trial_1201_257063997648` | `_build/aec-results/111-diagnostic-speaker-volume25-20260906-2154/` |
| 50 | `trial_1201_441005762536` | `_build/aec-results/111-diagnostic-speaker-volume50-20260906-2200/` |

Both complete artifact sets passed the host's strict checks for manifest,
checksums, exact WAV/CSV lengths, trial identity, sample/source continuity and
recorder errors. **The writer repair worked in these hardware captures.**
Original files were not modified. The SDK still raised its legacy disconnect
`CancelledError` after firmware save confirmation; the independently downloaded
artifacts passed validation.

The first playback occurred with both clocks healthy and complete reference
coverage. The mic subsequently faulted at about 200.3 seconds of nominal clock
elapsed time (`EnvelopeStep`, first logged at 21:58:07), before the second
recording. Source errors remained zero and reference timing remained healthy
through the second trial. Timing-invalid capture was intentional diagnostic
behavior, not permission to run ON AEC.

The initial offline defaults rejected every window at both volumes as low
energy. Actual raw mic RMS values were about 32-47 counts for channels 0-2 at
volume 25 and 41-73 at volume 50; channel 3 was lower still. These raw ADC
samples contain DC bias and are not gain-normalized full-scale audio. Raising
playback volume alone did not meet the old fixed 100-count microphone floor.

The host analyzer now exposes `--min-mic-rms-pcm` (default still 100; bounded
1..1000), computed after DC removal. The chosen value and all window/search
parameters are recorded in its JSON. This is an explicit amplitude-unit
selection, not sample amplification or a change to robot timing policy:
the 0.35 normalized-correlation cutoff, repeated-peak rejection, reference
energy floor, clipping checks and artifact integrity requirements are
unchanged. Additional regressions verify low-amplitude/DC-offset matching,
unrelated-noise rejection, true silence, repeated matches, parameter bounds
and parameter reporting; the full host AEC suite passed 26 tests.

Both recordings were reanalyzed identically:

```sh
python3 anki/victor/tools/audio/analyze_aec_diagnostic.py TRIAL_DIRECTORY \
  --window-seconds 0.25 --min-mic-rms-pcm 12.5
```

Shorter local windows reduce the within-window misalignment from differing
sample rates. The original inconclusive default reports are retained alongside
`analysis-raw-adc-quarter-second.json` and `summary.json`.

* Volume 25: nine qualifying windows, all on channel 1; correlations
  0.356-0.396 and sample-axis lag slope about **-943 ppm**. This is weaker
  evidence than the louder trial.
* Volume 50: 41 qualifying windows across channels 0/1/2, with slopes
  **-877 / -876 / -875 ppm**. Channel 1 supplied 20 windows over 9.5 seconds,
  correlations 0.395-0.528, and sample-axis lag falling from about 199.0 to
  190.7 ms. The 49 weak-correlation and 30 low-energy windows remain rejected;
  channel 3 has no usable trend.
* On channel 1, median model lag was about **9.0 ms** during the earlier
  healthy-clock capture and **37.8 ms** in the later faulted capture; median
  observation lag was about 4.4 and 6.3 ms respectively. The later model
  prediction was already faulted and frozen, not a valid calibration target.

Sample-axis lag includes different file start edges. Its slope is not an
absolute physical oscillator measurement; model/observation lags include the
documented timestamp and pipeline biases. These results establish useful
waveform correspondence and a model disagreement, **not echo cancellation,
an exact acoustic-delay setting, or a reason to enable ON**. No clock limits
or firmware were changed during these recordings.

### First bounded reference-versus-ON comparison (2026-09-06, 22:12 and 22:17)

This was a separately authorized, short **speaker-only** test on unchanged
OTA111, not normal-use AEC or double-talk. Each condition used three repetitions
of the same two-second sentence at SDK volume 75, mic age 10 ms and reference
delay 0 ms. Fresh calibration preceded each capture. Both legacy raw and
processed WAV pairs were saved, downloaded and checked for complete PCM data:

| Mode | Recording | Local evidence directory |
|---|---|---|
| Reference | `miccapture_0000_0002` | `_build/aec-results/111-ab-reference-20260906-2210/` |
| ON | `miccapture_0000_0003` | `_build/aec-results/111-ab-on-20260906-2214/` |

The ON phase had a robot-side 180-second restoration timer and host exit cleanup.
Reference mode was restored after recording, confirmed by the 22:17:40 startup
log; the fallback timer was stopped and its temporary helpers removed.
The legacy SDK disconnect cancellation occurred after recording, not before
the WAV save confirmations. No safety priority override was used.

**AEC actually ran.** Four complete 500-block telemetry windows spanning each
capture had full reference coverage and zero timing-invalid blocks, clock
faults, drops, sink errors or input overflows. In ON, cancellation and adaptation
bypasses were both zero, and all four filters developed nonzero coefficients.
The reference condition kept both bypasses set.

During speaker playback, mean SE thread CPU increased from roughly 2.9-3.3 ms
in reference to 5.9-6.3 ms in ON. The observed ON maximum was 10.241 ms: one
block exceeded the nominal 10.24 ms budget by about 1 us. Wall-time spikes also
occurred, but there was no observed input loss in these comparison windows.
This supports bounded bench operation, not sustained production headroom.

**Incremental cancellation benefit is inconclusive.** A broad matched interval
(0.75-10 nominal WAV seconds, 100-6000 Hz zero-phase bandpass) gave processed RMS
38.01 in reference versus 37.68 in ON, about -0.08 dB. This is total processed
output power, **not ERLE or isolated echo attenuation**. Raw speaker levels
were about 0.9-1.8 dB lower during ON, and background activity differed.
Processed output was already near background levels in the reference run;
other SE processing and gain changes prevent interpreting this small difference
as either demonstrated AEC benefit or proof that the filter cannot work.

`energy-comparison.json` retains the method, waveform hashes, per-half-second
levels and background measurements. `comparison-summary.json` retains the
capture-window telemetry and limitations; `journal-complete.txt` covers the
last ON capture blocks through restoration. No near-end speech preservation
or simultaneous-speech result is claimed. The next efficacy measurement must
isolate echo-correlated residual from the rest of the processing, rather than
loosening guards or treating nonzero coefficients as acoustic success.

### Supervised human speech attempt (2026-09-06, 23:07-23:20)

The user was present and authorized three takes: voice alone in reference,
voice overlapping robot playback in reference, and the same overlap with
guarded ON. The natural human cue was `What's the weather tomorrow?`, repeated
three times from the same position. Playback used the existing two-second
sentence at volume 75. `--speech-prompt` changes only the displayed human cue;
it does not change or synthesize the robot's speech.

| Take | Result | Local directory under `_build/aec-results/` |
|---|---|---|
| Voice alone, reference | Raw and processed `miccapture_0000_0004` saved | `111-human-near-reference-20260906-2303/` |
| Overlap, reference | Raw and processed `miccapture_0000_0005` saved | `111-human-double-reference-20260906-2308/` |
| Overlap, ON attempt 1 | Readiness failed; no overlap playback/capture started | `111-human-double-on-20260906-2312/` |
| Overlap, ON attempt 2 | Readiness failed; no overlap playback/capture started | `111-human-double-on-20260906-2315/` |

Each saved WAV contains 239840 complete PCM16 frames (14.99 nominal seconds);
raw is four-channel and processed is mono. None clipped. Whole-recording
100-6000 Hz processed RMS was approximately 1270 alone and 1213 during overlap.
These levels establish non-silent output, **not intelligibility, retained words,
AEC attenuation, or an isolated human-speech measurement**. The first near-only
utterance may have started before capture; later repetitions are retained.

Both reference takes had already faulted their mic timing. For the overlap
process specifically, PID 3115 first reported a source-sequence discontinuity:
expected 1974748, observed first/last 1974757/1974758, fault reason 7 with source
reason 3 at 72.376 seconds. Its first reported offset was +45071 us. This is
not PID 2796's earlier phase fault and does not establish DMA-side loss.
Keep these recordings as qualitative listening baselines, not a clean
timing-valid AEC comparison.

The two ON processes failed the unchanged mic phase guard before the planned
overlap stimulus: PID 3502's first-fault window minimum was +2030 us at
75.110 seconds; PID 4117's was -2032 us at 65.095 seconds (latest readiness
minimum -2132 us). Neither reported a source fault, and their reference clocks
remained healthy. Incidental pre-test adaptation in the first process is not
an ON human take. No further retry was made.

Host exit cleanup and independent robot-side timers restored reference after
each failure. The final drop-in and startup log confirmed reference, mic age
10 ms, reference delay 0 ms, cancellation/adaptation disabled, and the anim
service active. Both fallback timers were inactive; their temporary helper
files were removed. The actual reference drop-in was retained.

An attempted replay of the processed overlap file subsequently failed normal
SDK behavior-control acquisition before audio started. Passive status showed
`EmergencySleep`; no safety override was used. Unchanged, hash-matching copies
for local listening are available at:

```
_build/aec-results/111-human-listening/voice-alone.wav
_build/aec-results/111-human-listening/voice-over-robot-reference.wav
```

`speech-trial-summary.json` in the reference-overlap directory retains file
hashes, signal measurements, PID-specific first-fault telemetry and both ON
readiness failures. Original journals/WAVs and the failed replay log are
preserved. No speech was sent to external transcription services.
**The human ON comparison remains blocked, not passed.** Listening can assess
the existing reference-mode processing; automatic barge-in/next-turn listening
still requires separate turn/streaming work.

At 23:27, after local listening, the user reported that the second file
(`voice-over-robot-reference.wav`) "sounds ok". This is a positive qualitative
assessment of the reference-mode overlap recording, with adaptive AEC disabled.
It supports using this configuration as a baseline for a limited conversation
prototype, but does not establish word-recognition accuracy, ON efficacy, or
robustness at other positions, volumes or noise levels.

### Reference-mode offline recognition follow-up (2026-09-06)

The user chose to proceed with reference mode rather than wait for adaptive
AEC. The robot's reference/10 ms/0 ms drop-in and active anim service were
reconfirmed without restarting it. The completed human trial is retained as
a reference baseline; its failed ON conditions are deferred, **not passed**.

`tools/audio/transcribe_aec_recordings.py` now provides a reproducible host-only
recognition check. It accepts complete, uncompressed mono PCM16 16 kHz WAVs of
up to 30 seconds, rejects raw four-channel input and truncated payloads, loads
only local safetensors weights, and uses CPU greedy decoding without an
expected-phrase prompt. No gain adjustment, denoising, trimming or resampling
was applied. JSON reports contain source/model hashes, library versions,
transcripts and limitations. Existing reports are not overwritten; unfinished
decoding is an error rather than a silently truncated transcript.

The first model was `openai/whisper-base.en`, revision
`911407f4214e0e1d82085af863093ec0b66f9cd6`. One stronger comparison used
`openai/whisper-small.en`, revision
`e8727524f962ee844a7319d92be39ac1bd25655a`. Public model files were downloaded
separately; inference was offline and no recordings were uploaded. Both
models received the same four unchanged files:

| Recording | Base English model | Small English model |
|---|---|---|
| Human alone, reference | "What's the Wedded Tomorrow?" | "tomorrow what's the weather tomorrow what's the weather tomorrow" |
| Human over robot, reference | "What is the standard model? What is the standard model?" | "What's the budget model?" |
| Processed robot-only reference control | "I'm not sure." | "So" |
| Original clean robot WAV | "The blue lantern is beside the window." | "The blue lantern is beside the window." |

**Acceptable listening did not establish reliable overlap recognition.** The
stronger model recovered the intended weather question from the near-only
recording, but not the overlap recording. These are model hypotheses compared
with the instructed cue, not a verified verbatim ground truth or WER score.
This does not prove the exact cause of the errors, nor predict the performance
of the production recognizer. The robot-only control also produced unrelated
text: do not treat any nonempty transcript as evidence of a human turn.
No VAD or speaker attribution is implemented by this offline tool.

Reports are preserved in
`_build/aec-results/111-human-listening/offline-asr-base-en.json` and
`offline-asr-small-en.json`. Original WAVs and prior results are unchanged.

After the user reported the robot ready, one fresh reference-only overlap
attempt used the cue `Set a timer for five minutes.` Normal SDK control timed
out before any playback or capture started. Its log is under
`_build/aec-results/111-reference-asr-timer-20260906/`; there is no new successful
WAV take. The current passive sleep messages did not establish EmergencySleep,
so the earlier emergency-sleep diagnosis is not reused for this attempt.
A read-only SDK battery request subsequently reported LOW (level 1), about
3.582 V, off the charger and not charging. This is relevant readiness evidence,
not proof of the exact behavior-control refusal cause. No safety override,
ON retry, firmware change or lycopod access was performed.

Reference mode remains an experimental baseline. Reliable overlap recognition,
live streaming and turn acceptance remain unimplemented/unproven, rather than
being declared ready on the basis of human listening alone.

#### Reproduce the offline check

The optional host environment was exercised with Python 3.11, NumPy 1.24.3,
PyTorch 2.0.1, Transformers 4.29.2, Hub 0.15.1 and safetensors 0.4.5. Existing
host libraries were reused; the missing safetensors dependency was installed
only in an isolated environment, not into the robot or global Python.
From the wire-os root:

```sh
python3 -m venv --system-site-packages _build/aec-asr-python
_build/aec-asr-python/bin/python -m pip install -r anki/victor/tools/audio/requirements-aec-transcription.txt
```

The requirements describe this tested optional environment, not a production
ASR deployment. On a fresh host, install the matching CPU PyTorch wheel from
the official PyTorch CPU index before the requirements if CUDA dependencies
are unwanted. Downloads need network access; transcription does not.

For the stronger model, download only the public model/config/tokenizer files:

```sh
HF_HUB_DISABLE_IMPLICIT_TOKEN=1 HF_HUB_DISABLE_TELEMETRY=1 _build/aec-asr-python/bin/python -c 'from huggingface_hub import snapshot_download; snapshot_download("openai/whisper-small.en", revision="e8727524f962ee844a7319d92be39ac1bd25655a", local_dir="_build/aec-asr-models/whisper-small.en", local_dir_use_symlinks=False, allow_patterns=["*.json", "merges.txt", "model.safetensors"])'
```

Choose a new report name for each invocation:

```sh
HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 HF_HUB_DISABLE_TELEMETRY=1 \
  _build/aec-asr-python/bin/python anki/victor/tools/audio/transcribe_aec_recordings.py \
  --model-dir _build/aec-asr-models/whisper-small.en \
  --output _build/aec-results/111-human-listening/offline-asr-small-en-repeat.json \
  _build/aec-results/111-human-listening/voice-alone.wav \
  _build/aec-results/111-human-listening/voice-over-robot-reference.wav \
  _build/aec-results/111-ab-reference-20260906-2210/miccapture_0000_0002/miccapture_0000_0002.wav \
  _build/sentence.wav
```

## OTA110: diagnostic-first PCM and clock capture (default OFF)

**110 does not retune the controller.** The 109 startup fit, independent
validation, 2 ms phase/1 ms innovation limits, bounded period changes, permanent
faults and real vendor freeze/history guard are unchanged. The reviewed
current-sample first-fault prediction fix is retained. Installing 110 does not
enable recording or AEC. Implementation, testing and packaging performed **no
robot/SSH/restart/flash/playback/capture or lycopod action**.

109's reference-only attempt failed before playback: reference reason 5 at
70,336 ms (minimum 1,754 us, zero updates), then mic reason 5 at 90,132 ms
(minimum -103 us after 1,446 us, latest offset 1 us, four updates). These
aggregate values cannot distinguish delivery-envelope movement, cadence change
and acoustic alignment. They are not a reason to widen limits. The separately
preserved 109 physical-attempt section remains the evidence record.
The parent's `_build/aec-experiment-110-evidence/109-observations.json` preserves
39 aggregate records and the original journal SHA-256
`96e619b61348cc1344651b3cc05a2f7bede933f996362f6ff07e29f4d831ee45`.
Selected adjacent 1,000-transmit-frame receipt intervals (5.12 nominal seconds)
correspond to 1004.8, 979.9, 739.1, 886.9, 885.3, 994.9, 817.2 and 838.0 ppm
relative to nominal, while the post-fault model stayed at 1081.18 ppm.
These are **host-receipt/model quantities, not oscillator measurements**.
Preceding reference-window observations and audio were not recorded in 109;
110 does not synthesize a missing historical trace.

### Capture architecture and lifetime

`AecDiagnosticCapture` is a separate recorder, not an extension of the legacy
`MicDataInfo` asynchronous raw/processed save jobs:

* `AecPlaybackReference::PushClocked` copies the **actual successfully accepted
  mono 32 kHz PCM16** from the ALSA write buffer before the reference FIR. It is
  downstream of real mixing/gain, not a copy of the SDK fixture. Its immutable
  process-lifetime accepted-sample index advances by the accepted frame count,
  independently of clock calibration. Short/invalid-timestamp positive writes
  are also captured, explicitly invalid, rather than reusing a previous clock
  observation. Failed writes/recovery are explicit errors.
* The mic payload callback copies the **same raw four-channel 160-sample
  payload** associated with its transmitted 80-sample frame-counter pair,
  HAL receipt timestamp and immutable raw-stream sample index. File writing
  converts the payload's planar layout to interleaved WAV outside the callback.
  This path does not depend on a processed-audio job finishing or asynchronously
  guessing which old raw WAV belongs with the reference.
* Begin accepts **1..15 wall-clock seconds and the processor's cached runtime
  REFERENCE configuration only**. OFF and ON are rejected. Only one diagnostic
  capture or writer may be active; the existing comparison capture cannot be
  scheduled over it. A previous comparison collection must also finish before
  diagnostic capture begins. Legacy comparison WAV saving remains unchanged.
* Begin allocates and touches both bounded lane buffers on the control thread.
  At 15 seconds the PCM/record/history allocation is approximately **4.2 MiB,
  tested below 5 MiB**. Capacity: 512,000 accepted reference samples and 2,048
  reference records; 2,048 raw mic blocks / 327,680 four-channel frames. There
  is one extra nominal second of reference capacity, not unlimited growth.
  OFF allocates none of these buffers.
* Each producer performs only bounded copies/field assignments and lock-free
  atomics in the recorder: no allocation, file I/O, callback logging, mutex or
  wait. The existing surrounding audio/mic pipeline locks are not redesigned.
  Each lane has exactly one writer. The first append also copies at most 64
  fixed-size clock-history entries; it does not allocate a trace list.
* Stop publishes `writing` before waiting for the in-flight writer count to
  become zero. A late entrant rechecks state before touching storage; a delayed
  old-trial callback is excluded by its receipt-time interval. A worker then
  owns and frees the buffers. Repeated captures join the completed worker
  before publishing another buffer. Shutdown cancels/joins outside the producer
  callbacks; the owning recorder outlives the audio producers.

The new dedicated directory is:

```
/data/data/com.anki.victor/cache/micdata/aecDiagnostic/trial_PID_MONOTONIC_NS/
```

It keeps at most 100 matching trial directories. Rotation occurs before
publication, only inside this directory and only for recognized artifact
filenames; unknown contents cause refusal, not recursive deletion. Existing
`aecExperiment/` captures and its own 100-capture rotation are untouched.

### Artifact contract: schema 1, instrumentation revision 110

| File | Contents |
|---|---|
| `raw.wav` | Interleaved four-channel PCM16, **nominal 15,625 Hz** header |
| `reference.wav` | Actual accepted pre-FIR mono PCM16, nominal 32,000 Hz header |
| `mic.csv` | Every captured mic block: trial ID, stream index, file offset/count, host callback receipt, transmitted frame pair/validity, HAL observation and clock snapshot |
| `reference.csv` | Every captured accepted write: trial ID, accepted-sample index, file offset/count, callback receipt, sink/ring counters, ALSA head observation and clock snapshot |
| `clocks.csv` | Bounded pre-capture mic/reference window history and first fault already retained at the first append |
| `manifest.json` | Final completion marker, runtime reference configuration, flags, exact counts, interval and per-artifact byte lengths/FNV-1a-64 checksums |

All CSV timestamps are signed host-monotonic nanoseconds; indices/counts are
sample frames, not interleaved scalar counts. `received_ns` is callback receipt,
not physical acquisition. `observed_ns` is HAL parsing time for mic and the
unsmoothed ALSA head estimate for reference. Neither is a physical clock truth.
The two streams need not begin/end on the same sample: use their indices,
offsets and timestamps, **not matching WAV frame zero**. The old raw-capture
16 kHz header is deliberately not accepted as a schema-1 diagnostic WAV.
The existing microphone mute is respected. Counts describe samples actually
delivered during the receipt interval, not a promise of exactly 15 physical
seconds in both WAVs. Inspect clip coverage; a short/silent interval may yield
no lag or trend even when all artifact writes completed.

Clock records include observed time, predicted time **before and after** the
update, nominal elapsed/duration, residual, window minimum **and its sample-count
position**, maximum, window start/span, previous minimum, before/after rate ppb,
window/update counters, closed/ready flags and fault reason. Continuous tracking
pivots keep the before/after current prediction equal; startup affine locking
is separately visible. Snapshotting does not alter the clock inputs/controller.

Within a recorded block, the explicit model/sample mapping is:

```
mic target(i) = predicted_ns - (mic_age_ms + ref_delay_ms) * 1e6
                + i * 64000 * (1 + rate_after_ppb / 1e9)
reference(j) = predicted_ns + j * 31250 * (1 + rate_after_ppb / 1e9)
```

Here `i`/`j` are offsets from that row's `index` / `file_offset`, not from a
presumed shared capture start. `observed_ns` remains the untouched input
observation; `observed_ns - predicted_before_ns` is the pre-update model offset
(also recorded as `residual_ns` for evaluated observations). A startup mapping
change remains visible in `predicted_ns - predicted_before_ns`. These formulas
describe the software model, not proof of physical ADC/DAC sample timing.

Each experiment clock retains 64 closed-window/first-fault events (roughly
five minutes), plus its immutable first fault. This fixed storage is present
without an active capture, **only clock observations in experiment mode update
it**. Capture soon after the failure to retain startup. If startup aged out,
`clocks.csv` does not invent it; analysis explicitly reports history coverage.
It cannot recover old PCM from before the capture interval.
Faults arising during capture are in the per-block CSV observations. Analysis
extracts `first_faults` from retained history or a captured transition; if the
first record was already faulted without retained onset, it explicitly marks
the onset unknown rather than fabricating an event time.

Recorder `errors` is a bitmask:

| Bit | Meaning |
|---:|---|
| 1 | Fixed PCM/record capacity overflow; retained prefix is not a complete usable trial |
| 2 | Sample-index or transmitted-source-frame gap / malformed frame pair |
| 4 | Sink error, short write or missing valid presentation observation |
| 8 | One audio stream absent |
| 16 | Shutdown/cancel interrupted the capture |
| 32 | Unsupported/invalid PCM format |
| 64 | Reference processing ring dropped samples during capture |
| 128 | Mic processing queue overflow during capture |
| 256 | Missing/impossible observation or unavailable source timing metadata |

Timing faults alone do **not** set these recording-integrity flags: recording
while faulted is the point of this reference-only diagnostic task. Existing
prior cumulative sink/ring counters are retained in records; changes during
the interval are rejected by analysis. Syscon transmit counters still cannot
detect DMA-side acquisition loss.

The worker closes, checks, fsyncs and reads back all five artifacts before
writing `manifest.part`, fsyncing it, atomically renaming it to `manifest.json`
and syncing the trial directory. Only then can status become `saved`. A
partial-write failure has no complete manifest and status is `failed`; a
complete set with integrity flags has status **`invalid`**, never `saved`.
`write_complete` distinguishes finished artifact writing from data usability. FNV checks
detect accidental association/corruption, **not malicious tampering**; offline
analysis additionally emits SHA-256 for every artifact.

### Future operator procedure — not executed by this implementation

1. Separately authorize/install the whole 111-or-newer OTA, then explicitly opt into
   **reference**, using the existing runtime configuration procedure below.
   Do not select ON and do not bypass emergency sleep/behavior-control rules.
2. Use a **non-repeating 10–12-second** mono PCM16/16 kHz stimulus at a modest
   volume. The existing two-second sentence remains unchanged and may establish
   a local lag, but is too short for a useful long-span drift estimate.
   An optional deterministic, low-level broadband stimulus can be created
   locally without overwriting any existing fixture (this does not play it):

```sh
python3 - <<'PY'
from pathlib import Path
import hashlib, wave
import numpy as np
from scipy.signal import firwin, lfilter
p = Path("_build/diagnostic-nonrepeating-10s.wav")
assert not p.exists(), "preserve the existing fixture"
x = lfilter(firwin(63, 3200, fs=16000), [1],
            np.random.RandomState(110).normal(0, 2000, 160000))
x[:320] *= np.linspace(0, 1, 320)
x[-320:] *= np.linspace(1, 0, 320)
with wave.open(str(p), "wb") as w:
    w.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
    w.writeframes(np.clip(x, -10000, 10000).astype("<i2").tobytes())
print(hashlib.sha256(p.read_bytes()).hexdigest(), p)
PY
```

The fixture is already prepared locally at that path, without playback:
160,000 frames / 10 seconds, SHA-256
`58864556441d68360c5a876891e2c4febcc566d85cb503499d2ee8c4481cadbf`.
Reuse it rather than overwriting it. It is low-level filtered noise, not speech.

3. Run the explicit diagnostic helper. It checks installed OTA/capture schema
   and cached runtime reference mode before SDK initialization, then checks the
   same trial ID through writer completion. Scheduling is not saved evidence.
   Unlike comparison mode's three repetitions, diagnostic mode plays the
   stimulus **once**, avoiding manufactured identical correlation peaks.
   Use the existing Python 3.8 SDK environment on this workstation:

```sh
_build/aec-sdk-py38/bin/python anki/victor/tools/audio/aec_experiment_capture.py \
  --robot-ip ROBOT_IP --serial ROBOT_SERIAL \
  --wav _build/diagnostic-nonrepeating-10s.wav \
  --condition speaker --volume 25 --diagnostic
```

The input file is a stimulus, **never the analysis reference**. With
`--token-stdin`, credentials remain transient as before. Clock readiness is not
required for this explicitly labeled diagnostic run; cancellation and adaptation
remain disabled. Without `--diagnostic`, legacy comparison semantics and timing
acceptance requirements remain unchanged. No helper changes configuration or
restarts the robot.

For operator-managed shorter/ambient captures, the future console functions are
`AecDiagnosticCapture` with integer seconds 1..15 and `AecDiagnosticStatus` with
no arguments, through the existing `/consolefunccall` POST endpoint. Responses
contain `AEC_DIAGNOSTIC` followed by JSON. `active`/`writing` are not completion;
require matching `trial`, `state=saved`, `write_complete=true`, `errors=0`. Both functions expose the
actual `/etc/os-version`; 109 cannot impersonate diagnostic capability and
110 is rejected because its binary writer can corrupt PCM.

4. Retrieve the **entire reported trial directory**, including its final
   manifest, through the separately authorized existing file-transfer workflow.
   Retain the matching journal too when available. Do not merge trial files.
5. On the workstation, with the existing NumPy/SciPy installation:

```sh
python3 anki/victor/tools/audio/analyze_aec_diagnostic.py \
  _build/aec-results/YOUR-DIAGNOSTIC-TRIAL > _build/aec-results/YOUR-ANALYSIS.json
```

Validated dependency versions here are NumPy **1.24.3** / SciPy **1.10.1**;
these are host-only tools, not new robot dependencies. No dependency was
installed for this task. Recording contains speech: obtain consent and retain
locally; nothing is uploaded automatically.

### Offline interpretation and limits

Analysis first verifies manifest/schema/mode, file checksums, exact PCM/CSV
lengths, trial association, monotonically contiguous sample indices and
transmitted-frame pairs. It refuses recorder errors, gaps and large observation
discontinuities rather than zero-filling or correlating across them. Refusal is
an invalid trial, **not zero delay**.
CLI exit 2 means invalid input; exit 3 means a valid artifact set but no
unambiguous lag (JSON rejection details are still emitted). Exit 0 permits
local lag results; it does not promise a usable drift trend or physical cause.

Actual 32 kHz reference PCM is rationally resampled by 125/256 to the nominal
mic grid. Local zero-mean normalized FFT correlation runs per mic channel with
0.75 s windows / 0.5 s steps; the default bounded search is ±5 s around an
observation-derived prior. It reports weak/silent, clipped, boundary-limited and
ambiguous repeated peaks, including competitors at either search endpoint and
the competing peak score. No estimate is
returned for those windows. Optional `--window-seconds`, `--step-seconds` and
`--search-seconds` keep explicit bounded limits. `--min-mic-rms-pcm` selects
the DC-removed raw-microphone amplitude floor without changing correlation
or ambiguity thresholds; its conservative default is 100 counts. The 111
hardware record above explains the explicit 12.5-count analysis choice.

For unambiguous windows:

* `sample_axis_lag_ms` includes the files' different start edges; it is not
  automatically physical acoustic delay.
* `observation_lag_ms` compares HAL receipt minus nominal block duration with
  the matched ALSA observation. It includes delivery/pointer bias.
* `model_lag_ms` compares the captured mic target mapping (including its actual
  configured age/extra delay) with the matched reference mapping.
* `sample_axis_lag_slope_ppm` is the trend of sample-axis lag, **not a measured
  oscillator correction**. At least three unambiguous windows spanning two
  seconds are required. Large fit residuals are labeled nonstationary.
* Peak width, competing peaks and fit residual/standard error describe
  diagnostic uncertainty, not calibrated probability/confidence intervals.

These comparisons can expose disagreement between waveform alignment and the
timestamp/controller model. They cannot by themselves identify a physical
thermal cause, separate all fixed acoustic/processing latencies, detect every
unobserved DMA loss, or establish AEC attenuation/near-end/double-talk quality.
The first real 110 capture failed WAV integrity as recorded above. The two
corrected 111 captures passed integrity and provided diagnostic lag estimates;
independent capture-overhead and AEC-effectiveness measurements remain pending.

### OTA110 artifact and validation

```
_build/vicos-3.0.1.110d.ota
162027520 bytes
SHA-256 7601a091dc0dd3f05b87504fada22b1269a9fa1e166ab8dabeb335d55b9a534d
```

* **56 native gtests / 48 AEC ASan+UBSan tests / 24 Python tests passed.**
  All pre-existing clock/controller tests remain. New tests cover mode/duration
  refusal, no OFF buffer allocation, bounded maximum memory, active-writer
  stop fencing, concurrent mic/reference producers, repeated capture, shutdown,
  rotation, partial writes without a manifest, explicit gaps/overflow/missing
  observations, exact clock-window/first-fault history and actual production
  tap placement before FIR. The C++ writer's raw/reference files are consumed
  by the Python validator, not just separately mocked formats.
* Synthetic unique-wave tests recover a known 30 ms acoustic delay with different
  clip edges and a 150 ppm sample-axis rate difference while clock faults remain
  latched. Other tests reject repeated peaks, silence, clipping, source/sample
  gaps, corrupt/truncated/extra WAV data, schema/count mismatch, invalid
  association and negative search-window wraparound. Inconclusive CLI results
  do not exit as successful lag estimation. Review regressions reproduced and
  fixed omitted competing matches at both search endpoints; the host-only
  correction does not change the OTA. `python-review.log` records the final
  24-test Python run.
* The diagnostic helper's preflight, OFF/ON/old-OTA rejection, single stimulus,
  matching trial ID, explicit completion marker and failure/invalid states are
  tested. Historical comparison playback and animation-cache disabling remain.
* Both actual ARM v009 vendor variants passed off/reference/on/unset tests,
  trained-filter freeze and history refill; both known negative controls still
  fail as required. No controller/vendor retuning was made.
* Whole firmware built successfully. Decrypted BOOT/SYSTEM payload hashes and
  lengths match manifest and packaging inputs. `/etc/os-version` is
  `3.0.1.110d`; `vic-robot`, `vic-anim`, `vic-engine`, and `libaudio_engine.so`
  match built binaries. Diagnostic console/schema/clock-column markers are in
  the actual packaged binaries. Both anim service/environment files have no
  AEC opt-in.

Executed from the wire-os root:

```sh
bash anki/victor/tools/audio/validate_aec_experiment.sh \
  _build/aec-experiment-110final-validation native
bash anki/victor/tools/audio/build_aec_experiment.sh 110 build 110final
bash anki/victor/tools/audio/build_aec_experiment.sh 110 vendor 110final
bash anki/victor/tools/audio/build_aec_experiment.sh 110 package 110final
python3 anki/victor/tools/audio/verify_aec_ota.py 110 --workspace 110final
```

Logs and verified payloads are under
`_build/aec-experiment-110final-validation/`: `native.log`,
`python-final.log`, `vendor.log`, `build.log`, `package.log`, `verify.log`,
`pre-package-images.log`, `preserved-otas.log`, and `payloads/`.
`diag-test-*` directories contain **synthetic native-test buffers**, not
recordings from a robot. Final packaging inputs are
`_build/aec-experiment-110final/`; a superseded unreleased provisional image is
retained at `_build/aec-experiment-110/provisional-vicos-3.0.1.110d.ota`.
Build/package refuse to overwrite the finished OTA; use `110final` for
verification rather than the provisional workspace.
`synthetic-correlation-proof.json` records 68 unambiguous channel-windows,
first model lag 30.192 ms (known synthetic acoustic delay 30 ms plus the
documented age convention), and estimated sample-axis slope -152.48 ppm against
the synthetic -150 ppm target. This is numerical validation, not hardware data.

OTA101–109 matched the pre-build checksum snapshot, including retained 109
`b76507d2988af1ff5abc4e642da448d11341b4f65c5c761442895cd940a22e24`.
Only `anki-version` was cleaned for the version change. The exact stale
generated sparse sysfs image was inspected before regeneration; Victor source,
old OTA/capture directories and unrelated dirty files were not cleaned,
committed or pushed.

## OTA109: bounded continuous period tracking, still default OFF

109 is a local firmware revision, **not a completed hardware trial or an AEC
effectiveness result**. No robot connection, service restart, flashing, playback,
capture or lycopod action was performed while implementing/building it. The
108 physical-attempt record below remains historical evidence, not successful
audio capture. The host helper's `cache_animation_lists=False` fix is retained.

### Diagnosis and choice

The 108 fixed startup fit can become inconsistent with later observations even
without a detected transport-counter gap. Run 2097 learned +918,287 ppb mic
period correction and +31,805 ppb DAC correction. Mic window minima at windows
13–19 were **835, 1062, 1731, 1976, 2023, 2443, 2664 us**; the first mic fault
was at window 17, while DAC remained near zero and `source_errors=0`. Run 1758
learned +815,084/+59,335 ppb, then showed nonmonotonic mic residual growth and
slow negative DAC residual growth; DAC crossed -2 ms before mic crossed +2 ms.
This identifies a **fixed-model mismatch**, not its physical cause.

In 1758 the DAC first fault was at **14:09:44**, window 40 minimum **-2046 us**;
mic first fault was at **14:10:10**, window 46 minimum **2353 us**, with source
errors still zero. The coincident `ref_discontinuities=1`, `valid=499` and one
missing reference sample **do not establish an independent ALSA/output drop**:
`AecPlaybackReference::PushClocked` explicitly clears FIR history and increments
the segment generation and discontinuity counter on the first clock fault.
`ReadMicrophone` then refuses interpolation between the previous and new
generations. A regression reproduces **exactly one missing interpolation sample
and one discontinuity with uninterrupted accepted PCM, zero dropped samples and
zero sink errors**. The archived aggregate record cannot identify the exact
sample, but the counter itself is an expected clock-fault consequence.

The long-run regression includes the supplied windows 13/20/30/40/50/60/70/80/
100/110 and additional turning points through window 115, not merely a constant
rate proxy. The source receipt/frame-count rates over 51.2-second intervals
(814, 812, 852, 835, 800, 834, 810, 880, 772, 827 ppm relative to nominal) also
show why the fixed 815.084 ppm startup fit cannot be assumed exact forever.
Those are host-observed aggregate intervals, **not physical oscillator
measurements**; no thermal or DMA-loss attribution follows from them.

Code inspection retains the distinction: syscon's `TIM15` divider defines the
nominal 15,625 Hz cadence; its DMA ISR produces 20 samples per half/full event
and explicitly can lose samples if delayed. `Mics::transmit` copies the current
80-sample buffer; `Comms::tick` increments the **transmitted frame** counter.
HAL's timestamp is spine parsing time, not a DMA acquisition timestamp.
Supervisor combines two frames and uses the final frame's HAL time. Anim
dispatch/queue delay is measured separately. The Q6 ALSA pointer remains
period-quantized. None of these metadata establish whether later envelope
motion is oscillator/thermal change, changing delivery bias, or unobserved
body-side sample loss. No body firmware/protocol change is justified by those
aggregate journals alone.

Rather than widen the 2 ms bound or automatically clear faults/re-identify,
109 uses an explicitly bounded phase/frequency feedback loop:

* Keep the same 12 × 5 s startup fit, ≤1 ms fit error, ±2,000 ppm total period
  correction, and independent thirteenth validation window. No adjustment
  occurs during that independent window. The frozen 108 implementation remains
  selectable by the timing class's default constructor for historical
  regression controls; both production clocks explicitly select tracking.
* Once validated, every complete 5 s lower-envelope window supplies residual
  `e` against the **current continuous sample-count mapping**. Check the
  unchanged `|e| ≤ 2 ms` bound first. Reject `|e - previous_e| > 1 ms` as an
  envelope step; it cannot become a new epoch. Instantaneous 100 ms ADC / one
  period + 2 ms DAC bounds, monotonic timestamp checks and the seven-day
  arithmetic horizon also remain.
* Propose a period change
  `0.5 * (e - previous_e) / window_duration + e / 40 seconds`.
  This damped derivative/phase feedback avoids the undamped phase-only
  integral loop. Limit each applied change to **±100 ppm per window** and the
  total period correction to **±2,000 ppm**; a requested change over ±200 ppm
  or a total-rate violation faults. These are engineering acceptance bounds,
  not measurements or vendor oscillator specifications.
* Pivot the new slope at the current sample's predicted timestamp; that time
  does not jump. Only this and later samples' periods use the new slope.
  Previously queued DAC timestamps and each queued mic block's timestamp and
  period stay immutable. There is no reanchor, timestamp interpolation across
  a discontinuity, or resetting of an error counter.
* Any fault is permanent until process restart; no period updates occur after
  fault. Source loss/duplicates/malformed metadata, mic queue overflow, sink
  errors, ring drops and reference discontinuities retain the existing
  fail-closed gating. Complete coverage is still insufficient. Reference mode
  never cancels/adapts. ON still requires a healthy covered current block and
  **13 previously processed healthy blocks**, with the real vendor adaptation
  mode disabled throughout fault/history refill.

An 80-sample loss is 5.12 nominal ms, well outside the phase/innovation bounds
once represented in the delivery envelope. Detected transport gaps are rejected
immediately by counters. Envelope-only step detection can take up to a complete
window (a partially affected window can retain its old minimum). A change
hidden inside dispatch jitter, a sub-bound phase step, slowly varying delivery
bias, or DMA loss without observable metadata cannot be conclusively classified.
In particular **a smoothly changing timestamp bias can be observationally
identical to rate drift**. Tracking readiness means consistency with these
bounds, not proof of physical alignment. Fixed acoustic offset and independent
long-run reference-only hardware evidence remain mandatory before ON evaluation.

### First-fault diagnostics

The paired `AEC_EXPERIMENT.Tracking` log has `revision=109`, adjustment counts,
and immutable first-fault information rather than only the last 500th block:

* `mic/ref_fault_reason`: 0 none, 1 invalid observation, 2 instantaneous bound,
  3 startup fit, 4 2-ms phase bound, 5 1-ms envelope innovation, 6 tracking-rate
  bound, 7 source continuity/timestamp validity.
* `mic/ref_fault_elapsed_ms`, `mic/ref_fault_offset_us` (observed minus
  predicted at first fault), `mic/ref_fault_min_us` (minimum accumulated at that
  event). Mic additionally retains full `mic_fault_observed_ns` and
  `mic_fault_predicted_ns`. These first-event fields remain unchanged even when
  later windows, counters or residuals change.
  For an invalid observation in a started, in-horizon clock, prediction is for
  the **current** sample-count position, including its calibrated correction,
  not the previous block's timestamp. Before a valid epoch exists, beyond the
  seven-day arithmetic horizon, or if checked prediction arithmetic cannot be
  represented, the field falls back to the last safe prediction (zero before
  startup). The invalid observation itself never enters prediction arithmetic.
* `source_fault_reason`: 0 none, 1 unavailable metadata, 2 nonadjacent halves,
  3 unexpected first frame. `source_expected`, `source_fault_first`,
  `source_fault_last` retain exact uint32 values, including wrap. They are
  transmitted frames, not acquired-sample counters. Invalid source timestamps
  can fault mic timing without increasing the frame-continuity error count.
* DAC fault fields use lock-free 32-bit atomic publication; the fault reason is
  released after its immutable fields. Normal DAC diagnostics are independent
  producer snapshots, not synchronized per-period traces. Logging stays in the
  existing periodic consumer stats path, never the sink callback.

Tracking adds O(1) arithmetic once per window, fixed storage, and no allocation,
I/O, lock or wait to the real-time tap. Startup still fits only 12 fixed points.
The source, current-block and vendor-history guards are not replaced by an
elapsed-time “success” label.

### OTA109 artifact and offline validation

```
_build/vicos-3.0.1.109d.ota
161996800 bytes
SHA-256 b76507d2988af1ff5abc4e642da448d11341b4f65c5c761442895cd940a22e24
```

Completed locally:

* **47 native gtests / 39 AEC ASan+UBSan tests / 12 Python tests passed.**
  All frozen-108 regressions remain. New regressions use synthetic per-block
  observations shaped by the actual **2097 and 1758 aggregate envelope values**,
  including the latter's opposite mic/DAC trends; they are not recovered
  timestamp traces or recorded waveforms. Frozen controls fault while bounded
  tracking stays continuous and within 2 ms of the synthetic ground truth.
  Other tests cover ±80 ppm slow rate variation over 20 minutes, 100 ppm/update
  and total-rate limits, 1.5/5.12 ms steps, excessive rate change, bounded
  quantized delivery, immutable first-fault events, zero/impossible timestamps,
  transport gaps and uint32-wrap diagnostics.
  Three additional review regressions first failed against the previous image's
  source, then passed after fixing invalid-observation telemetry's one-block
  prediction error. Exact mic/DAC predictions and observations are checked
  before lock and with active calibrated tracking, plus startup, INT64 epoch/
  observation limits and seven-day-horizon fallback. Audio timing policy and
  invalid-call return behavior are unchanged by this telemetry fix.
* Both actual ARM v009 vendor variants passed off/reference/on/unset,
  trained-coefficient freeze and 13-prior-block refill tests. Both negative
  controls still fail as required; unsupported settings remain rejected.
  Production vendor guard code was not changed for 109.
* Incremental whole-firmware build passed. Actual decrypted BOOT/SYSTEM lengths
  and hashes match the manifest and packaging inputs. Decrypted version is
  `3.0.1.109d`; `vic-robot`, `vic-anim`, `vic-engine`, and `libaudio_engine.so`
  match the built binaries. Packaged tracking/first-fault markers are present.
  Both anim service/environment files contain no AEC opt-in.
* OTA101–108 hashes matched the pre-build snapshot, including retained 108
  `c543af075d43022750d770471c786f290ee91ef34a24a928e92ee1fab138a9c9`.
  The exact stale generated sparse sysfs image was inspected before packaging
  regenerated it. Only `anki-version` was cleaned for version change; Victor
  source and unrelated dirty work were not cleaned, committed or pushed.

From the wire-os root, the executed commands are:

```sh
bash anki/victor/tools/audio/validate_aec_experiment.sh \
  _build/aec-experiment-109review-validation native
bash anki/victor/tools/audio/build_aec_experiment.sh 109 build 109review
bash anki/victor/tools/audio/build_aec_experiment.sh 109 vendor 109review
bash anki/victor/tools/audio/build_aec_experiment.sh 109 package 109review
python3 anki/victor/tools/audio/verify_aec_ota.py 109 --workspace 109review
```

The completed OTA makes build/package refuse overwrite; validation and
verification can be rerun. Logs are
`_build/aec-experiment-109review-validation/{native,vendor,build,package}.log`;
`pre-package-images.log` records the inspected stale image; `payloads/` retains
the decrypted verification files and manifest. The checksum preservation list
is `_build/aec-experiment-109-validation/preserved-otas.sha256`. Final packaging
inputs are `_build/aec-experiment-109review/`. The unreleased provisional image is
retained at `_build/aec-experiment-109/provisional-vicos-3.0.1.109d.ota`, not the
final OTA path; final firmware adds explicit acquire-before-field-load ordering
to first-fault logging (C++14 logging argument evaluation is unordered).
The pre-review image is also retained at
`_build/aec-experiment-109final/pre-review-vicos-3.0.1.109d.ota` (SHA-256
`0f54b6725baf56df7204166fb466a20b2d49f1d68454771dd0ba714c6f176eb1`).
It is superseded by the reviewed artifact above. `telemetry-before-fix.log` in
the review validation directory retains the three failing regression controls;
`native.log` records their corrected native and sanitized passes.
Process-isolated original 108 analyses are
`_build/aec-experiment-109-validation/108-{1758,2097}-analysis.json`; they retain
the original timing-invalid counts rather than substituting synthetic results.
The parent's independently structured
`_build/aec-experiment-109-evidence/observations.json` was cross-checked against
the regression fixtures: **all 34 selected 1758 points and all seven 2097
post-fit points match**, and both original input journal hashes match.
`_build/aec-experiment-109review-validation/aggregate-fixture-check.json` records
this check. For the paired 1758 snapshots, the mic-window index is a common
**synthetic** interpolation axis; the separately running DAC window counter
can differ. Startup origin, interpolation and per-block jitter are constructed,
not recovered hardware timestamps or physical ground truth.

**Still hardware-pending:** sustained readiness under real playback/load,
physical ADC/DAC alignment, true DMA-loss observability, rate/bias attribution,
fixed acoustic delay, reference-vs-ON CPU headroom, speaker attenuation,
near-end preservation and double-talk. No old invalid trial is relabeled valid.

### OTA109 hardware readiness attempt (2026-09-06, 15:20)

The installed version reported `3.0.1.109d`, with reference mode, mic age 10 ms
and reference delay 0 ms. Passive sleep status reported `Awake`. No service
restart or SDK playback was performed in this attempt.

Startup calibration did not lead to sustained readiness:

* DAC first fault: `ref_fault_reason=5` (envelope innovation), at elapsed
  **70,336 ms**, with first-fault window minimum **1,754 us**. No tracking
  updates had been applied to the DAC clock.
* Mic first fault: `mic_fault_reason=5`, at elapsed **90,132 ms**, after four
  tracking updates. Its successive reported minima changed from **1,446 us**
  to **-103 us**, exceeding the 1 ms window-change bound even though the
  latter minimum was closer to zero. The first-fault instantaneous offset was
  only **1 us**; that field is not the window-change metric.
* `source_errors=0`, `source_fault_reason=0`, and the drop, sink-error and
  input-overflow counters stayed zero in the saved detailed windows.
  `ref_discontinuities=1` is consistent with the clock-fault generation
  boundary described above, not independent evidence of PCM loss.
* Both timing faults stayed latched; cancellation and adaptation remained
  bypassed. Complete reference coverage did not make these windows valid.

The new telemetry distinguishes the rejection rule, but not whether the
envelope changes arose from delivery bias, actual cadence changes, or feedback
response. Detailed logging was enabled after the DAC's first event; immutable
first-fault fields survived, but its preceding per-window observations were
not recorded. Do not infer those missing observations or treat the model as
hardware-proven from its synthetic regressions.

No fixture was played and no WAV capture was scheduled. The readiness journal
and offline analysis are retained at
`_build/aec-results/109-reference-speaker-volume75-20260906-1521/`; the directory
name describes the intended trial, not a successful speaker recording.
Further clock-observation/controller investigation is required before an ON
comparison; repeatedly restarting or loosening guards is not a demonstrated
solution.

## OTA108: source continuity and startup clock calibration

The retained 107 trial is
`_build/aec-results/107-reference-speaker-volume75-20260906-123307/`.
Its six windows contain **3,000/3,000 complete reference blocks, zero missing
samples, and 3,000 timing-invalid blocks**. Both clocks had already faulted.
The earlier journal is retained at
`_build/aec-results/107-reference-attempt-20260906-123134/`.
Across the speaker trial, mic minimum-residual growth corresponds to roughly
555–819 ppm (median 611), DAC 55–94 ppm (median 85), using nominal block elapsed
time. Those are **aggregate residual trends, not measured oscillator rates**.
The intervening larger mic change cannot be separated into dispatch delay,
source loss, or clock change from these logs. Mean SE thread CPU ~2.8–4.4 ms
and ~5.6 ms observed maxima do not explain the accumulated phase error;
larger wall-time spikes include scheduling/blocking.

107 correctly stopped adaptation, but treating nominal divider/sample rates as
exact host-monotonic rates guarantees eventual failure for independent clocks.
108 does not remove the guard or substitute arbitrary wider drift tolerances:

* The existing syscon **transmit framecounter** now travels with both 80-sample
  halves through HAL, supervisor and the `MicData` CLAD payload. HAL timestamps
  parsing of the last spine frame in host monotonic nanoseconds, before anim's
  33 ms dispatch batching. Both halves must be adjacent and must follow the
  preceding message, including uint32 wrap. Lost/duplicate/reordered frames,
  missing metadata and backwards timestamps latch invalid immediately.
  This detects spine/host/message loss, **not DMA sample loss inside syscon**:
  its microphone ISR explicitly can fall behind, and the transmit counter is
  not a DMA acquisition counter. No syscon protocol or body firmware changed.
* Each clock fits one affine time mapping to **12 five-second lower-envelope
  observations** (about 60 seconds). The fit must have ≤1 ms maximum residual
  and a period correction within ±2,000 ppm. This is an experimental acceptance
  bound, not a hardware oscillator specification. It covers the observed
  hundreds/tens-ppm trends without accepting arbitrary clocks. Identification
  is always timing-invalid; its instantaneous nominal-clock allowance includes
  only that bounded rate times elapsed time.
* Rates and offsets then **freeze for the process lifetime**. One independent
  five-second window must validate before any healthy vendor history is credited.
  Later window minima exceeding ±2 ms, instantaneous errors over the existing
  100 ms ADC / 34 ms DAC bounds, invalid counters or seven-day arithmetic-horizon
  exhaustion latch invalid until restart. No online rate steering can reinterpret
  a lost block as frequency correction. A startup phase step that spoils the fit
  fails closed; changing jitter can also reject an otherwise good clock.
* DAC accepted-sample timestamps and ADC read targets both use their learned
  periods, including fractional nanoseconds. The first calibrated DAC block
  starts a new reference segment; queued timestamps are never rewritten.
  Explicit output sequence adjacency prevents interpolation across dropped PCM,
  even though adjacent calibrated timestamps need not differ by exactly 62.5 µs.
  Calibration switches occur while invalid, and the unchanged **13 previously
  processed healthy blocks** fence vendor history before ON can resume.

This is **bounded startup calibration**, not a production adaptive PLL.
Lower-envelope fit quality is not a statistical confidence interval or proof of
absolute acquisition time. Constant transport/DAC latency still needs acoustic
calibration. A fixed fit with biased 33 ms batching is intentionally shown to
fault later in regression rather than silently steer; moving the mic observation
to HAL removes that particular anim batching source. Temperature drift and
unobserved DMA loss remain reasons to reject a run, not claim synchronization.
Only a new reference-only hardware run can establish whether the measured
source cadence is stable enough. Existing raw WAVs are expected to contain robot
audio: they are upstream of AEC and are not an effectiveness metric.

## OTA107 diagnosis: sample clocks are not dispatch clocks

The retained trial is at the wire-os root,
`_build/aec-results/reference-speaker-20260906-112259/`. Across its five logged
windows: **2,500 blocks, 87.08% complete reference, 18,618 missing samples**;
`dropped=0`, `sink_errors=0`. `clock_resets` grows from 658 to 663.
The 11:23:05/10/15 windows have 436/435/436 complete blocks and wall-time SE
maxima 9,987/22,526/12,494 µs. Adapted microphones remain zero in reference mode.

Two implementation defects are identifiable from the actual acquisition and
playback code:

1. **The nominal ADC divider rate is 15,625 Hz, not 16,000 Hz.**
   `robot/syscon/common/hardware.h` defines 48 MHz;
   `robot/syscon/src/timer.h` sets the prescaler to 16 and decimation to 96;
   `mics.cpp` sets the LR timer period to twice the prescaler.
   Therefore `48,000,000 / (16 * 2 * 96) = 15,625` nominally; this does not
   measure the physical oscillator against the host clock.
   An 80-sample spine frame lasts 5.12 ms. `robot/hal/src/main.cpp` runs from
   `HAL::Step()`/spine frames; `robot/supervisor/src/messages.cpp` combines two
   frames into one 160-sample message: **10.24 ms**. Its timestamp is assigned
   during dispatch, not acquisition. The old 10 ms extrapolator loses 240 µs
   per block and reaches 100 ms error in ~417 blocks / **4.267 seconds**,
   before considering 33 ms anim batching. This explains the repeated
   reanchors, not an expensive algorithm. The archived anim tick progression
   independently gives a coarse **10.263 ms/block**, consistent with 10.24 ms,
   not a calibrated ADC-rate measurement.
2. **The Q6 ALSA playback position is period-quantized.**
   `kernel/msm-3.18/sound/soc/msm/qdsp6v2/msm-pcm-q6-v2.c` advances
   `pcm_irq_pos` on `ASM_DATA_EVENT_WRITE_DONE_V2`; `msm_pcm_pointer()` returns
   that position, not a continuously advancing DAC sample counter.
   The sink's 1,024 samples at 32 kHz form a **32 ms period**.
   `now + snd_pcm_delay - written_duration` therefore has period-phase
   uncertainty. The old ring treated changes beyond just 2 ms as discontinuities,
   resetting FIR phase and inserting timestamp holes/overlaps despite contiguous
   successful writes. `sink_errors=0` did not count these software-created
   discontinuities. The new regression reproduces missing samples under a
   period-quantized pointer and eliminates those manufactured gaps using the
   accepted PCM sample-count timeline.

The archive contains **aggregate stats, not per-period DAC timestamp traces**.
It cannot prove which individual missing samples caused exactly 87.08% coverage,
or partition them between pointer phase, queue availability, and clock reanchors.
The ADC fix alone does not establish the missing-reference fix on hardware.
Do not relabel this old trial valid or infer acoustic effectiveness from it.
The nominal 16 kHz WAV header is also not an independent cadence measurement:
240,000 raw frames are 15.000 header seconds but **15.360 seconds at the nominal
divider rate if contiguous**. Capture limits are sample-count based.

The exact retained fixture is `_build/sentence.wav`, “The blue lantern is beside
the window.”, 32,027 frames, 16 kHz mono PCM16 (2.0016875 nominal seconds),
SHA-256 `08b1443a0a8796cca8c0e3f8ef2931a30cdea5c25da37740428c3add28cd17b0`.
Preserve it rather than regenerating the similar example sentence below.

## What is actually connected

* The real hardware `AkAlsaSink` taps mono PCM **after a successful ALSA write**,
  downstream of Wwise mixing/gain and both SDK and KG playback. It does not use
  arriving TTS packets as a substitute for played audio.
* `snd_pcm_delay` plus the monotonic clock estimates DAC presentation of the
  accepted period. Failed/short writes, invalid delay and unexpected formats are
  counted as errors. ALSA recovery is also counted. The existing Alexa playback
  callback is untouched: it is a single subscriber, called earlier at Wwise
  submission, and is not an adequate DAC-time reference.
* The hardware format is explicitly checked: **32 kHz, signed PCM16, mono**.
  A stateful 15-tap half-band FIR decimates to 16 kHz, compensating its
  7-input-sample group delay in the timestamps. This modest filter is for the
  experiment, not a claim of production-quality asynchronous rate conversion.
* A static SPSC ring holds at most 16,383 samples (about 1.024 seconds, roughly
  384 KiB including sequence metadata). The tap has no allocation, mutex,
  file I/O, logging or waits.
  Overflow drops incoming reference and increments a counter.
* The microphone reader linearly interpolates the filtered reference onto the
  **calibrated ADC grid** (nominally 64 µs, 160 samples / 10.24 ms). SE still receives
  its original 160-sample blocks; raw/processed WAV headers and the normal
  processing path are unchanged. This does not resample the microphones or
  retune the vendor's nominal 16 kHz configuration. Interpolation requires
  adjacent reference samples from the same continuous segment, never a gap,
  overflow, or recovery boundary. Missing samples become zeros and are counted; **any missing sample
  bypasses cancellation and disables channel-model adaptation for that block
  and the subsequent reference-history refill**, rather than training on
  fabricated silence or re-enabling with contaminated prior samples.
  Future samples are not consumed early, and stale samples are not replayed.
* ADC/DAC sample clocks use the bounded startup calibration described above,
  never individual message arrival intervals. Unsanitized DAC delay observations
  still reach the clock guard. A newly detected DAC clock fault breaks the
  FIR/segment and increments `ref_discontinuities`; ALSA errors explicitly break
  it too. Neither boundary may be interpolated across. The one-time calibration
  segment switch is expected and is not counted as a stream error.
  The 34 ms immediate DAC bound reflects pointer granularity, not acoustic
  alignment accuracy. The post-lock long-term bound remains 2 ms.
* Mic input overflows, reference drops, sink errors, discontinuities, either
  clock fault, and clock warm-up all prevent adaptation/cancellation, even on
  otherwise completely covered blocks. Reads continue to drain the fixed ring;
  there are no added sleeps, mutexes, allocations or unbounded queues in the tap.
  Clock state is producer-owned; only 32-bit/boolean lock-free atomics publish
  DAC diagnostics. Mic timing travels with each existing buffered payload.
* `ANKI_AEC_MIC_AGE_MS` estimates the age of the **first mic sample** at the
  calibrated lower-envelope HAL receipt of the final spine frame, **not anim
  message arrival as in 107**. Recalibrate this offset; do not reuse a claimed
  107 acoustic calibration. `ANKI_AEC_REF_DELAY_MS` is additional acoustic/transport delay:
  positive values select **older** DAC reference. These are calibration knobs,
  not measured hardware constants. The short half-band plus linear interpolation
  is an experimental rate converter, not a production-quality asynchronous SRC.

### The existing vendor AEC really is used

The selected firmware configuration is `SE_V009=ON`, `SE_HIGHRES=ON`, hence
`3rd/signalEssence/v009/vicos-highres`. `lib/signalEssence/CMakeLists.txt` compiles
the project `mmif_proj.c` source and links the prebuilt `libmmfx.a`; changing the
project configuration therefore affects the executable.

The vendor default already constructs a time-domain LMS canceller with **960
taps / 60 ms per microphone** and the correct implementation callbacks. The old
`ConfigAec(..., 0.0)` merely bypassed cancellation and adaptation on all four
mics; its zero argument is not the actual allocated filter length.
`aecExperiment.c` preserves those defaults, verifies the supported type/length,
and removes the startup per-mic bypasses only for `on`. At runtime the shared
`AnkiAecExperimentPrepareBlock` guard sets both `MMIfSetAecBypass` and
`MMIfSetAecChanModelUpdateMode`: incomplete reference selects
`SE_AF_DISABLE_ADAPTATION` for all microphones. A fully covered current `on`
block is **necessary but not sufficient** to restore `SE_AF_NORMAL_ADAPTATION`.
The shared guard requires **13 previously processed, consecutive, fully covered,
timing-valid blocks**; the following block can enable cancellation/adaptation.
`AnkiAecExperimentCompleteBlock` credits coverage only after the production
`MMIfProcessMicrophones` call returns. Every missing sample or timing-invalid
block resets that count, including clock warm-up. Latched timing faults keep it
reset until restart. `reference` never restores adaptation;
`off` preserves the original per-mic cancellation/update bypass configuration.
Startup leaves opt-in cancellation and adaptation bypassed until this history is
populated. All block-control writes and diagnostic reads run on the existing MMIf
microphone-processing thread, after startup initialization, around processing.

The length is not guessed from wall time. Startup reads
`MMIfGetAecLenChanModel()` and rejects anything other than the configured,
tested **960-tap TD / 160-sample / nominal 16 kHz** implementation. Its first
sample needs 959 prior model samples: six completed blocks would cover **only
that model history**, not the whole vendor reference path. The actual public
`RefProcConfig` also specifies **two 100 Hz DC-removal poles, zero vendor delay,
160 samples**. Unsupported filter/delay/block configurations fail startup,
rather than silently applying this guard to a different path.

The guard conservatively budgets **960 samples of HPF settling + six prior
samples of the vendor's seven-tap 16 kHz reference FIR + 959 TD history samples
= 1,925 samples**, rounded up to **13 completed blocks / 2,080 samples**.
The linked vendor runner verifies the FIR coefficient symbol's 14-byte size;
inspection of `AecTdInitChanUpdater` confirms the seven-tap default.
`MultiAecCancelEchoes` advances the reference updater before testing bypass, so
frozen production blocks really do replace history. No private struct offsets
or vendor pointers are used by production code.
The HPF is IIR, so this is a bounded-settling criterion, not a claim of exactly
finite impulse response: the supported vendor pole is below 0.961. The real
public `DcRemovalFilter_f32` regression exercises all 16 sign combinations of
four initial state errors of ±1,048,576 PCM units (well beyond PCM16 history
differences). From sample 960 onward the worst homogeneous (zero-input) residual
is **1.05885e-9 PCM sample**, below the required 1e-6 sample bound.
Floating-point arithmetic is not exactly linear: a separate paired-state test
drives the actual cascaded vendor filters with full-scale DC, alternating and
pseudorandom PCM. Its worst residual is **0.0253906 PCM sample**, below the
one-PCM16-quantization-step criterion. Neither test claims bit-identical IIR
state or perfectly zero error after a finite interval.
The remaining FIR/model refill follows that settling period. At the nominal
15,625 Hz ADC cadence, 13 blocks span **133.12 ms**; no wall-clock timeout
credits history. Calibration does not shorten this guard.

This separation is required: the actual vendor diagnostic help for
`mmfx_bypass_echo_canceller` says **“1=Bypass echo cancellation. Does not affect
channel model adaptation.”** The public update-mode API controls the separate
`mmfx_aec_adaptation_mode` enum, whose disabled mode means “disable or freeze
adaptation.” The helper validates that diagnostic at startup and reads it back;
it does not retain a pointer to the vendor's one-time config or depend on private
binary structure offsets. This is distinct from the pre-existing
`FBF_FORCE_ECHO_CANCEL_WITH_NR` microphone/beam suppression policy.

Actual ARM/QEMU execution found that enabling cancellation needed more scratch
memory: the original 2 KB H pool failed with **651 bytes missing**. Both VICOS
v009 project variants now provide 8 KB. Off/reference/on then initialized and
processed successfully against the real vendor library. After 500 synthetic
blocks (including one second with a near-end component), all four `on` filters
had nonzero coefficients; off/reference filters stayed zero. This proves the
configuration reaches a real adaptive filter, **not acoustic echo reduction,
double-talk intelligibility, or real-time CPU headroom on the robot**.

VICOS v009 normal-resolution uses the same helper. macOS, v008 and standalone
simulation reject opt-in explicitly. Invalid mode/delay values terminate startup
with `AEC_EXPERIMENT ERROR`; there is no silent success/fallback.

## Startup-only controls

| Environment | Default | Meaning |
|---|---|---|
| `ANKI_AEC_EXPERIMENT` | unset / `off` | Original processing and zero reference; tap inactive |
| same | `reference` | Real reference, cancellation/adaptation bypassed |
| same | `on` | Real reference; cancellation/adaptation only after valid history refill |
| `ANKI_AEC_MIC_AGE_MS` | `10` | Integer 0..200 |
| `ANKI_AEC_REF_DELAY_MS` | `0` | Integer 0..200 |

`reference` and `on` force the **same** existing SE beamforming-off processing
path, including in low-power mode, so changing power/motor state cannot silently
substitute the single-mic low-power path. VAD, wake-word handling, streaming and
conversation policy are not changed. Use `reference` as the AEC-OFF comparator;
unset/off is a separate unmodified-system sanity check.

On the robot, over SSH as root, install this **volatile, experiment-only**
systemd drop-in (preserve any other drop-ins):

```sh
mkdir -p /run/systemd/system/vic-anim.service.d
cat > /run/systemd/system/vic-anim.service.d/90-aec-experiment.conf <<'EOF'
[Service]
Environment=ANKI_AEC_EXPERIMENT=reference
Environment=ANKI_AEC_MIC_AGE_MS=10
Environment=ANKI_AEC_REF_DELAY_MS=0
EOF
systemctl daemon-reload
systemctl restart anki-robot.target
systemctl is-active vic-anim.service
journalctl -u vic-anim.service --since '5 minutes ago' --no-pager
journalctl -b --since '5 minutes ago' --no-pager | grep AEC_EXPERIMENT
```

This WireOS build compiles Android `liblog` with its syslog backend.
`logcat` can therefore report `Unable to open log device 'main'`; that does
not establish an AEC failure. If the journal query has no matching entries,
check `grep AEC_EXPERIMENT /var/log/messages | tail -30`. To watch new messages,
use `journalctl -f -u vic-anim.service` or `tail -f /var/log/messages`.

The `Microphones` INFO log channel is disabled in the default configuration.
After each service restart, enable it from the workstation to see the AEC
counters and recording-success messages (warnings alone omit the cause):

```sh
curl --fail --show-error -X POST \
  --data-urlencode 'key=Microphones' \
  --data-urlencode 'value=true' \
  http://ROBOT_IP:8889/consolevarset
```

Check that the response accepts the setting, then wait at least 70 seconds
and inspect `AEC_EXPERIMENT.Stats`. `ReferenceInvalid` is emitted periodically,
even without a capture job: it means any incomplete reference, timing-invalid
block or CPU-clock measurement error in that window. No reference
while the sink is idle can explain zero coverage; the warning alone cannot
distinguish that from an actual timing or playback error. Examine the counters
before drawing conclusions and again during SDK playback.

For AEC-ON, rewrite that same small file with `ANKI_AEC_EXPERIMENT=on` and
restart again. Restart between trials when comparing cold filter adaptation;
do not change environment inside a running process. The startup line must show
the intended `mode=1` or `mode=2`, `taps=960`, and `cancel/adapt=0` or `1`.
Those startup flags describe permission, not the first block's active state:
both runtime bypasses start at 1 until 13 valid blocks have completed and the
following fully covered ON block is prepared. Stats `history_remaining` is the
number still required **after** the last processed block; zero with
`last_bypass=1` can therefore describe the final frozen refill block.
`valid` still counts current-block coverage only, not permission to adapt.
If the service fails, inspect the logs and revert—do not treat it as an ON trial.
The drop-in disappears on reboot; the OTA contains no enabled experiment setting.

## Fixed sentence and 15-second capture

1. Use a quiet room. Put the robot on a stable surface/charger, no motor activity.
   Keep head angle, room, speaker/master volume and microphone mute state
   identical. Start at SDK volume **50**, with the user **50 cm in front**.
   Do not change ALSA mixer controls between trials.
2. Prepare **one** WAV of “The blue lantern is beside the quiet window.”
   Record it once or export it once from your existing TTS. It must be mono
   PCM16, 16 kHz, 1..3 seconds; do not regenerate it for each trial.
   The capture helper prints its SHA-256. Keep the same file for every trial.
3. Use an already-authenticated Vector Python SDK on the workstation.
   The SDK snapshot in this repository needs Python 3.8/3.9: Python 3.10+
   removes the `asyncio.Event(loop=...)` interface it uses. An isolated
   environment can be prepared from the WireOS root without changing an
   existing application environment:

```sh
/path/to/python3.8 -m venv _build/aec-sdk-py38
_build/aec-sdk-py38/bin/python -m pip install \
  ./anki/victor/tools/sdk/vector-python-sdk-private/sdk 'protobuf>=3.20,<3.21'
```

   The repository SDK documents `robot.audio.stream_wav_file`.
   This enters the existing SDK streaming player and then the same
   Wwise/ALSA path used by KG audio. It isolates downstream AEC feasibility;
   it does **not** establish that the KG network route was retested.
   Do not use `aplay` directly: that bypasses this sink/tap.
4. From the Victor repository on the workstation:

```sh
python3 tools/audio/aec_experiment_capture.py \
  --robot-ip ROBOT_IP --serial ROBOT_SERIAL --wav sentence.wav \
  --condition speaker --volume 50
```

The helper connects the SDK first, requests a 15-second capture, waits one
second, plays the exact fixture three times with 0.5-second gaps, and waits
for the recording interval. It validates format/duration and rejects capture
errors or an overlong playback schedule. It does not install dependencies,
enable AEC, flash firmware, or claim a recording has been saved.

If the saved SDK token is rejected with HTTP 401, an SSH-authorized operator can
use WireOS's runtime token without printing it or modifying SDK credentials.
Run from the WireOS root, substituting the robot IP and serial:

```bash
set -o pipefail
ssh -o BatchMode=yes -o IdentitiesOnly=yes \
  -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -i poky/victor/meta-qcom/recipes-connectivity/openssh/files/ssh_root_key \
  root@ROBOT_IP 'cat /run/vic-cloud/perRuntimeToken' \
  | _build/aec-sdk-py38/bin/python anki/victor/tools/audio/aec_experiment_capture.py \
      --robot-ip ROBOT_IP --serial ROBOT_SERIAL --wav _build/sentence.wav \
      --condition speaker --volume 50 --token-stdin
```

Do not enable shell tracing, print the token, or put it in command-line arguments.
The helper uses the supplied IP for both SDK and capture HTTP connections.
An SDK behavior-control timeout occurs before recording is requested: it is not
a completed capture or an AEC result. Check that the robot is awake, upright,
not in a physical reaction, and not controlled by another client. Do not override
physical safety behaviors merely to obtain an audio recording.

Repeat using:

* `--condition near`: no robot playback; speak “Please count one two three four
  five” at each printed repetition cue. This is the near-end-only control.
* `--condition double`: speak that same phrase **during** each robot sentence.
  Keep speaking after the robot stops to reveal recovery/attenuation.

Use `--speech-prompt "What's the weather tomorrow?"` for a natural alternative
human cue in both conditions. This is displayed text only; the robot still
plays the WAV supplied through `--wav`. Without this option, the counting cue
above remains the default.

For each `reference` and `on` setting, run speaker-only, near-only and double-talk,
preferably three repetitions of each trial, alternating mode order. Also record
one unset/off sanity trial before and after the experiment. Do not use wake
phrases in either fixture. If robot behavior moves the head, lift or wheels,
discard that trial.

### Capture command without the helper

The anim web server is on **port 8889**, not engine port 8888:

```sh
curl --fail --show-error -X POST \
  --data-urlencode 'func=AecExperimentCapture' \
  --data-urlencode 'args=15' \
  http://ROBOT_IP:8889/consolefunccall
```

`AecExperimentCapture` is dev-cheat-only, accepts **1..15 seconds**, rejects
overlapping experiment capture jobs, and uses the existing `MicDataInfo` writer
for raw four-channel and processed mono WAVs **without fade-in**. The reply says
“scheduled”, not “saved”. These diagnostic captures intentionally do not emit
the legacy pre-write `MicRecordingComplete` notification.

The reply prints the actual directory, normally:

```
/data/data/com.anki.victor/cache/micdata/aecExperiment/
  miccapture_0000_NNNN/
    miccapture_0000_NNNN_raw.wav
    miccapture_0000_NNNN.wav
```

Wait for both `MicDataInfo.WriteRawWaveFile` and
`MicDataInfo.WriteProcessedWaveFile` success logs. Save failures are explicitly
logged as errors. Check both files exist and have sensible duration before
copying. There is a bounded 100-capture rotation **within this dedicated
directory**; retrieve data before it rotates. Raw/processed collection uses
different existing processing threads and can differ slightly at clip edges.
Do not assume sample-perfect start alignment; align/trim offline.

```sh
mkdir -p aec-results/reference-speaker-01
scp -r root@ROBOT_IP:/data/data/com.anki.victor/cache/micdata/aecExperiment/miccapture_0000_NNNN \
  aec-results/reference-speaker-01/
ssh root@ROBOT_IP "journalctl -b --since '10 minutes ago' --no-pager" \
  > aec-results/reference-speaker-01/journal.txt
```

If the relevant messages are only in `/var/log/messages`, retrieve that file
over SCP as well and retain the trial's time interval.

Use the exact directory reported on your device if it differs. Record fixture
hash, mode, both delays, volume, distance, head angle, trial condition, build
version and relevant log interval. These files contain human speech: retain
locally with the participants' consent; do not upload automatically.

## Do not confuse “enabled” with “usable”

Every 500 microphone blocks (**5.12 nominal-divider seconds**), `AEC_EXPERIMENT.Stats` reports:

* `valid`: fully covered reference blocks out of 500; only those can run ON AEC.
* `missing_samples`: uncovered samples, including initial warm-up.
* `dropped`, `sink_errors`, `input_overflows`, `ref_discontinuities`: separate
  cumulative invalidation counters. Overflow is no longer mislabeled a clock
  reset. `mic_clock_fault` / `ref_clock_fault` are latched flags, not reanchor
  counters; correct the cause and restart, never ignore a fault.
* `mic_residual_us`, `ref_residual_us`: latest unsmoothed observation minus the
  nominal clock during identification, then minus the calibrated clock (frozen
  in 108, bounded continuous period tracking in 109).
  `mic_drift_us`, `ref_drift_us`: latest post-lock five-second minimum residual.
  They remain zero before the independent validation window; zero alone is not
  readiness. Require `timing_invalid=0` after both ≥13 clock windows.
* The paired `AEC_EXPERIMENT.Clock` record reports `mic/ref_windows`,
  `mic/ref_rate_ppb` (period correction; positive means slower sample clock),
  `mic/ref_fit_error_us`, uncorrected `mic/ref_raw_residual_us`, and the last
  window minima/maxima. `source_errors` must remain zero. `source_first`,
  `source_last` and `source_received_ns` identify the final processed payload;
  `transport_us` measures HAL parse to anim receipt separately from
  `max_queue_us`. Retain both log types, including startup. DAC diagnostics are
  atomic producer snapshots, not a synchronized per-period hardware trace.
* `timing_invalid`: blocks for which clocks are warming up/faulted or a cumulative
  overflow/error/discontinuity is present. `valid` still measures coverage only;
  **valid=500 is insufficient when timing_invalid is nonzero**.
* `last_bypass`: final block's global bypass, **not** proof of the entire window.
* `last_update_bypass`: final block's vendor adaptation-mode readback; 1 means
  adaptation disabled across all mics, 0 means normal adaptation permitted.
  In `on`, both bypasses must be 1 on an incomplete/timing-invalid/refill block
  and 0 only after a covered, timing-valid block has sufficient prior history;
  in `reference`, both remain 1.
* `adapted_mics`: number of filters with nonzero coefficients; should become
  nonzero during ON playback, and remain zero in a fresh reference-only run.
  Nonzero coefficients alone do not prove useful cancellation.
* `ref_mean_square`: reference energy; must rise above silence during playback.
* `max_se_wall_us` / `mean_se_wall_us`: monotonic elapsed time around MMIf.
  `max_se_cpu_us` / `mean_se_cpu_us`: **CLOCK_THREAD_CPUTIME_ID** execution
  charged to this thread over the same call, excluding time it is not running.
  `max_se_non_cpu_us`: maximum paired wall-minus-CPU interval (clamped at zero),
  including preemption, blocking and measurement overhead; **not a pure
  scheduler-latency measurement**. Do not subtract unrelated window maxima.
  `cpu_clock_errors` must be zero; a missing CPU clock is not zero CPU cost.
* `se_wall_over_budget` / `se_cpu_over_budget`: separate counts exceeding the
  **10,240 µs nominal-divider input-block interval**. `max_queue_us` measures host
  payload arrival to raw-processing dequeue, separately from MMIf execution.
  None of these alone is complete end-to-end latency or CPU utilization.
  Leave substantial headroom for reference conversion, VAD, recording and
  other processes. Compare **reference vs on** CPU distributions under the
  same workload/thermal state. OTA106's old `max_se_us` was wall time only:
  its 10–22 ms maxima do **not** establish that the algorithm is too expensive.

Discard ON acoustic comparisons with materially incomplete reference, any
overflow/recovery/clock fault, no reference energy during audible playback, or
unexplained wall-budget overruns/backlog. CPU and non-CPU causes require
different diagnosis; neither may be discarded. Ignore startup warm-up when checking coverage,
but retain it to assess convergence. All-zero output or muted microphones is
not AEC success.

### Delay sweep and acceptance worksheet

Start at mic-age 10 ms / extra-delay 0 ms. If speaker-only reduction is weak,
repeat the same fixed fixture at extra delays **0, 10, 20, 40, 60 ms**, restarting
each time. Then refine the promising region and, if needed, mic-age. The 60 ms
filter can model only a bounded causal residual echo path; a wrong offset can
make a perfectly real adaptive filter ineffective. Do not choose delay based
only on lower output loudness: check user-only and double-talk preservation.

For matching speech-active windows after initial convergence:

1. Confirm raw mic echo levels and clipping rates are comparable between runs.
2. Measure processed speaker-only RMS and spectrum, avoiding silence intervals.
   `20*log10(RMS_reference_mode / RMS_on_mode)` is a useful *output attenuation*
   comparison. It is **not isolated algorithmic ERLE**, because beamforming,
   gains, noise reduction and limiting remain in the pipeline.
3. Listen/transcribe near-only and double-talk clips. Compare word recovery,
   chopped syllables, near-end level, distortion and post-playback recovery.
   Raw capture provides a check that the person actually spoke at a comparable
   level. Do not infer intelligibility from an energy decrease.
4. Track CPU budget, reference coverage, errors and filter adaptation alongside
   audio metrics. A plausible initial gate is ≥6 dB speaker-only attenuation
   with no meaningful loss of near-end words/level, but that is a proposed
   evaluation criterion, **not a measured result or guarantee**.
5. Only after this bench test, optionally replay the same kind of KG answer
   through the existing service and repeat capture. No new server behavior is
   part of this experiment.

## Revert

Remove only the experiment file created above, then restart:

```sh
rm /run/systemd/system/vic-anim.service.d/90-aec-experiment.conf
systemctl daemon-reload
systemctl restart anki-robot.target
systemctl is-active vic-anim.service
```

Do not remove other drop-ins or use broad cleanup. With the environment unset,
the reference tap stays off and normal processing policy is restored. Retain
the previous OTA files. Installing/flashing an OTA is a separate operator action.

## Software validation

`test/animProcess/testAecPlaybackReference.cpp` uses the existing gtest framework
to cover default-off gating, format rejection, FIR/resampling, split periods,
timestamp jitter, no early/stale reads, gaps/delay, bounded overflow and concurrent
producer/consumer access. `tools/audio/aecExperimentSmoke.c` cross-links the
**production** project and vendor archives, checks bypass readback and verifies
that only ON adapts all four filters on synthetic delayed echo plus near-end
energy. After 500 training blocks it checks all four active and backup
coefficient arrays across 100 partial gaps, 100 wholly absent reference blocks,
two later isolated gaps (one interrupting refill), three timing-invalid but
covered blocks, and **44 fully covered refill blocks**. Every frozen block
must leave the trained active and backup coefficients unchanged. The expected
counter is independently checked before and after every production helper call;
the current block cannot credit its own prior history. All four filters resume
changing only after safe refill. Off, unset and reference remain zero throughout.

Importantly, vendor weighted-coefficient diagnostics are mirrors refreshed on
cancellation, not live model storage. The test inserts cancellation-only
diagnostic refreshes with adaptation disabled before comparing trained weights;
these extra passes are deliberately not credited to the production guard
(conservative extra vendor history, not early guard release).
Checking stale mirrors under global bypass alone would falsely pass. A negative
control (`--cancel-only-control`) uses the original cancellation-only guard with
normal vendor adaptation and must fail on changed coefficients (exit 5). This
reproduces the defect on the real vendor binary, without mocks or forced
adaptation. A second negative control (`--no-history-control`) restores the old
complete-current-block-only behavior: it must fail at covered recovery block
700, after the gap ended. Both controls fail with exit 5 against both actual
vendor variants. The corrected guard remains frozen through block 712 and
re-enables at 713; interrupted and timing-invalid refill re-enable at 920 and
946. A nonzero vendor reference delay is explicitly rejected.

It runs with the existing Yocto `qemu-arm` and built image rootfs.
Neither replaces the on-robot protocol above.

From the **wire-os root**, reproduce the analysis, native regressions, firmware
build, vendor regression and encrypted-image verification using checked-in tools:

```sh
python3 anki/victor/tools/audio/analyze_aec_timing.py \
  _build/aec-results/107-reference-speaker-volume75-20260906-123307
bash anki/victor/tools/audio/validate_aec_experiment.sh \
  _build/aec-experiment-108-validation native
bash anki/victor/tools/audio/build_aec_experiment.sh 108 vendor
python3 anki/victor/tools/audio/verify_aec_ota.py 108
```

The native runner uses existing gtest, GCC sanitizers and standard-library Python
unittest; no installed source is patched. The Docker vendor runner uses the
production high-resolution archive and independently compiles the existing
normal-resolution project/shim. Logs are under the versioned validation directory.
To create another image, choose an **unused** version, then run `build` followed
by `package` with that version. These were the 108 build commands; they refuse
to overwrite its completed artifact:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 108 build
bash anki/victor/tools/audio/build_aec_experiment.sh 108 package
```

The builder cleans **only `anki-version` when the rootfs version differs**, never
Victor. It refuses to overwrite
an existing OTA/package directory. Packaging checks rootfs version/freshness,
removes only the exact stale generated `apq8009-robot-sysfs.ext4`, and verifies
manifest version, decrypted payload lengths/hashes, `/etc/os-version`, and the
packaged binaries against the compiled ones. All operations are local; none
contacts or installs on a robot.

For the unreleased 107 regeneration, the superseded OTA was moved into its old
`_build/aec-experiment-107/` package directory rather than deleted. The final
commands used a fresh package workspace:

```sh
bash anki/victor/tools/audio/build_aec_experiment.sh 107 build 107final
bash anki/victor/tools/audio/build_aec_experiment.sh 107 package 107final
python3 anki/victor/tools/audio/verify_aec_ota.py 107 --workspace 107final
```

These build/package commands deliberately refuse to overwrite the now-existing
final artifact. Preserve/archive an unreleased superseded artifact first, or
choose an unused version. A provisional package created during this review is
retained separately in `_build/aec-experiment-107final-provisional/`, not the
final OTA path.

### OTA108 artifact and offline validation

The new **default-OFF** development image is:

```
_build/vicos-3.0.1.108d.ota
161986560 bytes
SHA-256 c543af075d43022750d770471c786f290ee91ef34a24a928e92ee1fab138a9c9
```

Completed locally:

* **36 native gtests / 28 AEC ASan+UBSan tests / 11 Python tests passed.**
  New tests cover ±700 ppm source drift, 80 ppm DAC drift, independent validation,
  startup and post-lock phase steps, later rate changes, biased batching that
  must fault rather than steer, impossible timestamps/rates and seven-day
  exhaustion, source loss/duplicates/within-payload gaps and uint32 wrap.
  A five-minute two-clock sine-wave test checks actual interpolated samples,
  not only coverage, at independently drifting ADC/DAC rates.
* Both actual ARM v009 vendor variants passed off/reference/on/unset tests.
  The trained-weight freeze/refill tests passed; cancellation-only and
  current-block-only negative controls still failed as required. No vendor
  recovery guard was shortened or replaced by elapsed-time warm-up.
* Incremental production firmware build passed, including CLAD regeneration,
  HAL, supervisor, anim and engine. The updated `MicData` protocol is bundled
  coherently; do not deploy individual old/new process binaries together.
* Actual encrypted BOOT/SYSTEM payload lengths and SHA-256 values matched
  manifest and packaging inputs. Decrypted `/etc/os-version` is `3.0.1.108d`;
  `vic-anim`, `vic-robot`, `vic-engine`, and `libaudio_engine.so` matched their
  compiled files. Both packaged anim service/environment files have no AEC
  opt-in. The new clock/source diagnostics and existing vendor guard markers
  were verified in the packaged executable.
* OTA101–106 matched the retained checksum list; OTA107 and the fixed sentence
  matched their previously recorded hashes. Old recordings, packages and
  unrelated notes were preserved. No commit, push or hardware action occurred.

Logs, binaries, decrypted verification payloads and reproducible JSON analyses
of **both** saved 107 journals are under
`_build/aec-experiment-108-validation/`; packaging intermediates are under
`_build/aec-experiment-108/`. Key logs are `native.log`, `vendor.log`, `build.log`,
`package.log`, and `preserved-otas.log`. `107-speaker-analysis.json` explicitly
retains 100% coverage **and** all 3,000 timing-invalid blocks; it does not relabel
the old recording valid. Source uses the sink's existing CRLF line endings;
`git -c core.whitespace=cr-at-eol diff --check` passed.

**Pending physical evidence:** stable source-frame cadence, clock-fit accuracy,
fixed acoustic offset, long-run drift rejection, reference vs ON CPU headroom,
speaker attenuation and near/double-talk preservation. Offline calibration and
QEMU are not those measurements. The next step remains the separately authorized
reference-only procedure below, not automatic ON or a claim of normal mode.

### OTA108 hardware retries (2026-09-06)

Reference-mode startup calibration can pass on hardware, but **sustained timing
readiness is not established**:

* The initial run reported one source-continuity error and about 40 ms of
  microphone residual offset. Its timing guard correctly remained latched.
* A subsequent run reached nine consecutive complete, timing-valid 500-block
  windows with no source errors. Playback could not start because normal SDK
  behavior control was blocked by `EmergencySleep`. Later monitoring found
  both clock faults and one reference discontinuity; brief readiness is not
  evidence of long-run stability.
* After the robot woke, a fresh run started at 14:16:02 and passed calibration
  with no source errors. At 14:17:32, the microphone window minimum reached
  2,023 us, exceeding the 2 ms post-fit bound. Microphone timing faulted with
  `source_errors=0`; the playback clock remained healthy. This recurrence
  cannot be attributed solely to detected source-frame loss.
* Normal SDK control was granted at 14:17:31, but the SDK's optional
  `ListAnimations` startup request timed out before capture was scheduled.
  The host capture helper now uses `cache_animation_lists=False`: audio trials
  do not require those RPCs. This does not change behavior-control priority,
  bypass emergency sleep, or address the microphone clock fault.

No fixture playback or new WAV capture completed in these attempts, and ON was
not enabled. Do not widen the timing bound merely to obtain a valid label.
The saved logs distinguish cumulative source errors, successful startup fits,
and later residual drift; they do not establish its physical cause.

Evidence is retained locally under
`_build/aec-results/108-reference-readiness-20260906-1403/`,
`_build/aec-results/108-reference-retry-20260906-1405/`,
`_build/aec-results/108-reference-speaker-volume75-20260906-1407/`, and
`_build/aec-results/108-reference-retry-20260906-1416/`. The last directory
includes pre-restart logs, the SDK failure, and `current-process/` diagnostics
filtered to the new anim process so separate calibrations are not combined.

### Preserved OTA107 artifact and checks

The preserved development image is **`3.0.1.107d`**, default **OFF**, at the wire-os root:

```
_build/vicos-3.0.1.107d.ota
161986560 bytes
SHA-256 4d9b64cb0988f210383d82c5d2787b0e37af3b7c56326a7852ac520f2450d8f7
```

Validation completed:

* **27 native gtests passed**, covering the existing eight reference tests, physical ADC cadence,
  one hour of batched arrival jitter, intentional interruption and accumulated
  100 ppm drift, period-quantized DAC timestamps, ADC-grid interpolation, gaps,
  CPU-vs-wall accounting and a real sleeping-thread CPU-clock check, plus the
  eight existing KG playback-state/streaming-wave regressions.
* **19 AEC tests passed** with AddressSanitizer/UndefinedBehaviorSanitizer.
* Nine Python unit tests cover the capture helper (including IP/token handling)
  and offline timing analysis. No SDK credentials or helper behavior were changed.
* Actual ARM vendor off/reference/on smoke tests passed; malformed mode and
  delay controls were rejected, and an unset environment retained zero filters.
* The corrected trained-filter regression passed against both v009 VICOS vendor
  variants under QEMU. High-resolution training produced 949/948/955/954 nonzero
  active taps; active and backup weighted coefficients stayed unchanged across
  all 202 incomplete-reference, three timing-invalid and 44 covered history-refill
  blocks, and all four mics resumed adaptation after safe history.
  The original cancellation-only negative control failed at the first gap
  (block 500); the complete-current-block-only control failed at recovery block
  700, using normal, not forced, vendor adaptation.
  Normal-resolution validation uses the existing libc math compatibility shim;
  the packaged production firmware is high-resolution.
* Final VICOS firmware build completed successfully. Both decrypted OTA payload
  hashes and lengths match the actual manifest. The packaged system reports
  `3.0.1.107d`; packaged `vic-anim` and `libaudio_engine.so` match the built files.
* Existing OTA 101–106 checksums were unchanged. No firmware was flashed and no
  source commits or pushes were made.

Build/test/package logs and executables are under
`_build/aec-experiment-107final-validation/`; final package intermediates are under
`_build/aec-experiment-107final/`. `native.log`, `vendor.log`, `build.log`,
`package.log` and `verify.log` record this review iteration.
The original timing analysis, prior OTA checksum list and superseded validation
remain under `_build/aec-experiment-107-validation/`.
The packaged `vic-anim` SHA-256 is
`08dd62f26f408fc750cecba43323867195e939402ebcb1926eddcd15a8ee4a33`;
`libaudio_engine.so` is
`2ccd524b968d0d07a575c329fd5e54cac04a3e36ea360f3062e31e394ebf99f6`.
The timing/CPU, separate adaptation-bypass and history-guard diagnostic strings
are present in the actual encrypted OTA's extracted executable.
Decrypted BOOT is 17,862,656 bytes, SHA-256
`d0a526649e38d1b3a661428b86a3036b9e65e74bacb4a8bde387512c9e7dcc44`;
SYSTEM is 845,475,840 bytes, SHA-256
`b79d3cab109e00269abff5a03006e6d883d5551100b4924208578873c49b85f4`.

The prior corrected OTA106 remains unchanged, SHA-256
`349ee0bd73c5d8b5d90993dc5bdcf80cf90e037c96830627647db338b1677ccc`.
Its original artifacts, recordings and validation logs remain preserved.

### Next operator action: reference-only hardware retest

Installing 108 is a separate, explicit operator action. The robot's current mode
has not been confirmed in this iteration. After installation, verify unset/off operation
first, then deliberately enable **reference only** using the volatile drop-in
above. Do not jump directly to ON or reinstate the removed experiment file
automatically. Wait at least 70 seconds and beyond both clocks' independent
validation windows; retain every stats and clock-diagnostic window.

1. With a safe thermal/battery state, repeat the same archived fixture/capture
   conditions. Never bypass thermal or behavior safety to obtain a trial.
2. Require zero overflows, sink errors, reference discontinuities and clock
   faults. After warm-up, require `timing_invalid=0`; inspect complete-block
   coverage and energy specifically during playback. Neither synthetic 100%
   coverage nor installing this image is proof of physical reference coverage.
3. Compare wall, CPU, non-CPU and queue measurements. A long wall interval with
   low thread CPU implicates scheduling/blocking rather than algorithm execution;
   sustained CPU overruns still fail feasibility. The new robot values are
   **pending**, not the native/QEMU timings.
4. Observe phase/drift for longer than the initial 15-second capture. A latched
   drift fault is evidence to investigate, not permission to enlarge thresholds.
   Observe at least ten minutes before an ON decision. Check source sequence
   errors separately from calibration fit error, raw drift and post-fit residual.
   The frozen fit and five-second guard are not a replacement for production
   long-duration adaptive clock tracking. If 108 rejects the source, save those
   diagnostics rather than changing bounds or enabling ON.
5. Only after reference timing passes, consider a separately authorized ON
   comparison with the full speaker/near/double-talk protocol. None of those
   acoustic acceptance results exists for 108 yet.
