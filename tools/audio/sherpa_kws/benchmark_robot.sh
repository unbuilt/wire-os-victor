#!/bin/bash
# Replay synthetic/local fixtures on a robot without changing its live recognizer.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 || -z "$1" || "$1" == -* ]]; then
  echo "usage: benchmark_robot.sh [USER@]HOST TRIAL_DIR [LOCAL_RUNTIME_DIR]" >&2
  exit 2
fi
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
host=$1
trial=$2
runtime=${3:-}
threads=${KWS_BENCHMARK_THREADS:-1}
case "$threads" in
  1|2|3|4) ;;
  *) echo "KWS_BENCHMARK_THREADS must be 1..4." >&2; exit 2 ;;
esac
read -r -a precisions <<< "${KWS_BENCHMARK_PRECISIONS:-int8}"
for precision in "${precisions[@]}"; do
  case "$precision" in
    int8|fp32) ;;
    *) echo "KWS_BENCHMARK_PRECISIONS must contain int8 and/or fp32." >&2; exit 2 ;;
  esac
done
if [[ ${#precisions[@]} == 0 ]]; then
  echo "At least one model precision is required." >&2
  exit 2
fi
build="$root/_build/sherpa-kws"
model="$build/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01"
key=${KWS_SSH_KEY:-"$root/anki/victor/robot_sshkey"}
files=("$build/replay-target/kws-replay" "$trial/input.wav" "$trial/input.activity"
  "$model/tokens.txt" "$root/anki/victor/tools/audio/sherpa_kws/keywords.txt")
for component in encoder decoder joiner; do
  for precision in "${precisions[@]}"; do
    suffix=.int8.onnx
    if [[ "$precision" == fp32 ]]; then suffix=.onnx; fi
    files+=("$model/$component-epoch-12-avg-2-chunk-16-left-64$suffix")
  done
  if [[ -n "$runtime" ]]; then
    files+=("$runtime/lib/libsherpa-onnx-c-api.so" "$runtime/lib/libonnxruntime.so.1.17.1")
  fi
done
for file in "${files[@]}" "$key"; do
  if [[ ! -s "$file" ]]; then
    echo "Missing or empty benchmark input: $file" >&2
    exit 2
  fi
done
if [[ "$host" != *@* ]]; then host="root@$host"; fi
connection=(-i "$key" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8)
check_temperature() {
  ssh "${connection[@]}" "$host" '
    set -e
    for path in /sys/class/thermal/thermal_zone*/temp; do
      value=$(cat "$path")
      case "$value" in
        ""|*[!0-9]*) echo "Unsupported temperature from $path: $value" >&2; exit 1 ;;
      esac
      if [ "$value" -ge 1000 ]; then value=$((value / 1000)); fi
      printf "%s %s C\n" "$path" "$value"
      if [ "$value" -ge 75 ]; then
        echo "Refusing extra benchmark load at 75 C or above; let the robot cool." >&2
        exit 1
      fi
    done
  '
}
check_temperature
remote=$(ssh "${connection[@]}" "$host" 'mktemp -d /tmp/sherpa-kws-benchmark.XXXXXXXX')
if [[ ! "$remote" =~ ^/tmp/sherpa-kws-benchmark\.[a-zA-Z0-9]+$ ]]; then
  echo "Unexpected remote temporary path; refusing upload or cleanup: $remote" >&2
  exit 1
fi
cleanup() {
  ssh "${connection[@]}" "$host" "
    set -e
    for component in encoder decoder joiner; do
      rm -f '$remote'/\"\$component\"-epoch-12-avg-2-chunk-16-left-64.onnx \
            '$remote'/\"\$component\"-epoch-12-avg-2-chunk-16-left-64.int8.onnx
    done
    rm -f '$remote/kws-replay' '$remote/input.wav' '$remote/input.activity' \
          '$remote/tokens.txt' '$remote/keywords.txt' \
          '$remote/libsherpa-onnx-c-api.so' '$remote/libonnxruntime.so.1.17.1'
    rmdir '$remote'
  "
}
trap cleanup EXIT
output=$(mktemp -d "$build/robot-benchmark.XXXXXXXX")
printf 'Physical robot replay; services and installed models remain unchanged.\nResults: %s\n' "$output"
sha256sum "${files[@]}" > "$output/inputs.sha256"
scp -q -O "${connection[@]}" "${files[@]}" "$host:$remote/"
library_path=/anki/lib
if [[ -n "$runtime" ]]; then library_path="$remote:/anki/lib"; fi
runtime_library_dir=/anki/lib
if [[ -n "$runtime" ]]; then runtime_library_dir=$remote; fi
ssh "${connection[@]}" "$host" "
  set -e
  chmod 700 '$remote/kws-replay'
  sha256sum '$runtime_library_dir/libsherpa-onnx-c-api.so' '$runtime_library_dir/libonnxruntime.so.1.17.1'
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
" > "$output/runtime.txt"
for precision in "${precisions[@]}"; do
  check_temperature > "$output/$precision-temperature.txt"
  ssh "${connection[@]}" "$host" "
    set -e
    ulimit -c 0
    ulimit -t 30
    LD_LIBRARY_PATH='$library_path' exec '$remote/kws-replay' \
      --model-dir '$remote' --keywords '$remote/keywords.txt' \
      --wav '$remote/input.wav' --activity '$remote/input.activity' \
      --model-precision '$precision' --execution robot --threads '$threads'
  " | tee "$output/$precision.jsonl"
done
