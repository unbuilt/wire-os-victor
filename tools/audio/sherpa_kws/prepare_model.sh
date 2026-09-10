#!/bin/bash
# Download only public model assets; never install them on a robot.
set -euo pipefail
if [[ $# -gt 1 ]]; then
  echo "usage: prepare_model.sh [int8|fp32]" >&2
  exit 2
fi
precision=${1:-int8}
case "$precision" in
  int8) suffix=.int8.onnx ;;
  fp32) suffix=.onnx ;;
  *) echo "Model precision must be int8 or fp32." >&2; exit 2 ;;
esac
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
test -d "$root/poky"
output="$root/_build/sherpa-kws"
name=sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01
archive="$output/downloads/wenetspeech-3.3M-2024-01-01.tar.bz2"
sha=b2f7c89690dc8ce4c6ed6afeab7cd800c36ad1421fb6b6302b4a4b194cf7f35f
mkdir -p "$output/downloads"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/$name.tar.bz2" \
    --output "$archive.part"
  printf '%s  %s\n' "$sha" "$archive.part" | sha256sum --check
  mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
tar -xjf "$archive" -C "$output" \
  "$name/tokens.txt" \
  "$name/encoder-epoch-12-avg-2-chunk-16-left-64$suffix" \
  "$name/decoder-epoch-12-avg-2-chunk-16-left-64$suffix" \
  "$name/joiner-epoch-12-avg-2-chunk-16-left-64$suffix"
printf 'Model directory: %s/%s\n' "$output" "$name"
