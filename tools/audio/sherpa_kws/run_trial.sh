#!/bin/bash
# Local replay only. No microphone upload, robot connection, or firmware changes.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
source="$root/anki/victor/tools/audio/sherpa_kws"
build="$root/_build/sherpa-kws"
model="$build/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01"
platform=${KWS_REPLAY_PLATFORM:-host}
case "$platform" in
  host|target) ;;
  *) echo "KWS_REPLAY_PLATFORM must be host or target (QEMU)." >&2; exit 2 ;;
esac
runner="$build/replay-$platform/kws-replay"
if [[ $# -gt 1 ]]; then
  echo "usage: run_trial.sh [RECORDING.wav] (no argument: synthetic Mandarin smoke trial)" >&2
  exit 2
fi
test -x "$runner"
test -x "$build/vector-vad-replay"
test -f "$model/tokens.txt"
trial=$(mktemp -d "$build/trial.XXXXXXXX")
printf 'Trial artifacts: %s\n' "$trial"
expect=()
if [[ $# == 1 ]]; then
  recording=$1
  printf 'Input: %s\n' "$recording"
else
  recording="$trial/synthetic.wav"
  "${KWS_FIXTURE_PYTHON:-python3}" "$source/generate_fixture.py" \
    --model-dir "$build/vits-icefall-zh-aishell3" --output "$recording"
  expect=(--expect-detections 1)
  echo "Synthetic speech: infrastructure smoke trial, not human wake-word accuracy."
fi
ffmpeg -hide_banner -loglevel error -n -i "$recording" \
  -af 'adelay=2000,apad=pad_dur=4' -ar 16000 -ac 1 -c:a pcm_s16le "$trial/input.wav"
ffmpeg -hide_banner -loglevel error -n -i "$trial/input.wav" -f s16le "$trial/input.raw"

qemu="$root/poky/build/tmp-glibc/sysroots-components/x86_64/qemu-native/usr/bin/qemu-arm"
rootfs="$root/poky/build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs"
qemu_libs="$root/poky/build/tmp-glibc/sysroots-components/x86_64/glib-2.0-native/usr/lib:$root/poky/build/tmp-glibc/sysroots-components/x86_64/pcre2-native/usr/lib"
LD_LIBRARY_PATH="$qemu_libs" \
  "$qemu" -L "$rootfs" "$build/vector-vad-replay" "$trial/input.raw" > "$trial/input.activity"

run=("$runner")
if [[ "$platform" == target ]]; then
  sdk=${VICOS_SDK:-${VICOS_SDK_HOME:-"$root/anki-deps/vicos-sdk/dist/5.3.0-r07"}}
  run=(env "LD_LIBRARY_PATH=$qemu_libs" "$qemu" -L "$rootfs"
    -E "LD_LIBRARY_PATH=$build/target/lib:$sdk/sysroot/usr/lib" "$runner")
fi
printf 'execution=%s\nphysical_robot=false\n' "$platform" > "$trial/execution.txt"
sha256sum "$runner" "$build/vector-vad-replay" "$source/keywords.txt" \
  "$build/$platform"/lib/*.so* \
  "$model/"*-epoch-12-avg-2-chunk-16-left-64.int8.onnx "$model/tokens.txt" \
  "$trial/input.wav" "$trial/input.activity" > "$trial/inputs.sha256"
execution=host
if [[ "$platform" == target ]]; then execution=qemu; fi
args=(--model-dir "$model" --keywords "$source/keywords.txt" --wav "$trial/input.wav"
  --execution "$execution" "${expect[@]}")
"${run[@]}" "${args[@]}" > "$trial/continuous.jsonl"
"${run[@]}" "${args[@]}" --activity "$trial/input.activity" > "$trial/gated.jsonl"
grep '"event":"summary"' "$trial/continuous.jsonl" "$trial/gated.jsonl"
