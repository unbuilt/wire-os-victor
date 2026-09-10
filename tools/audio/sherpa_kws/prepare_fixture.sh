#!/bin/bash
# Optional, host-only synthetic speech fixture. Not a firmware dependency.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
work="$root/_build/sherpa-kws"
name=vits-icefall-zh-aishell3
archive="$work/downloads/$name.tar.bz2"
sha=ab468db3a3308cdd861495e0db2f25d79418a0c00639f74944c7cdf5dd8c6ec1
mkdir -p "$work/downloads"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/$name.tar.bz2" \
    --output "$archive.part"
  printf '%s  %s\n' "$sha" "$archive.part" | sha256sum --check
  mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
tar -xjf "$archive" -C "$work" "$name/model.onnx" "$name/tokens.txt" "$name/lexicon.txt"
echo "Local synthetic voice prepared. No audio generated or uploaded."
