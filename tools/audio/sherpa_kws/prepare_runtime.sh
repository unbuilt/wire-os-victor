#!/usr/bin/env bash
# Opt-in workstation builds only. Never installs into the SDK or a robot.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
work="$root/_build/sherpa-kws"
cache="$work/runtime"
sherpa_commit=26aa2fa93210376a89de3a65a1a4dd320c37f5e9
sherpa_sha=7c2daea812195ebef5f0799e68a907319e732b152176b0efa4e1dd7660df6572
mode="${1:-help}"
case "$mode" in
  help|--help|-h)
    echo "Usage: bash ${BASH_SOURCE[0]} host|target [optimized]"
    echo "Build pinned sherpa-onnx 1.12.14 C API + ONNX Runtime 1.17.1."
    echo "host: Linux x86_64; target: SDK ARMv7 softfp/libc++ source cross-build."
    echo "Outputs: _build/sherpa-kws/MODE/{include,lib}; JOBS defaults to 2."
    echo "optimized: isolated MODE-optimized install and runtime/optimized builds;"
    echo "enables target ORT contrib fusions and disables session thread spinning."
    echo "Caller num_threads is unchanged. Default artifacts are never reused for writing."
    echo "Target build success is NOT target execution or deployment."
    exit 0 ;;
  host|target) ;;
  *) echo "Unsupported runtime mode: $mode" >&2; exit 2 ;;
esac
variant="${2:-default}"
[[ ($# -eq 1 || ($# -eq 2 && "$variant" == optimized)) && "${JOBS:-2}" =~ ^[1-9][0-9]*$ ]] || {
  echo "Expected host|target [optimized] and positive JOBS." >&2; exit 2;
}
install_mode="$mode"
if [[ "$variant" == optimized ]]; then
  cache="$cache/optimized"
  install_mode="$mode-optimized"
fi
source_dir="$cache/sherpa-onnx-$sherpa_commit"
mkdir -p "$cache/downloads" "$cache/scratch"
export TMPDIR="$cache/scratch" TMP="$cache/scratch" TEMP="$cache/scratch"
exec 9>"$cache/preparation.lock"
flock -n 9 || { echo "Another runtime preparation is running." >&2; exit 2; }
exec > >(tee "$cache/$mode.log") 2>&1
trap 'echo "Preparation failed; see $cache/$mode.log" >&2' ERR

fetch() {
  local url="$1" file="$2" digest="$3"
  if [[ "$variant" == optimized && ! -f "$file" && -f "$work/runtime/downloads/${file##*/}" ]]; then
    cp -p "$work/runtime/downloads/${file##*/}" "$file"
  fi
  if [[ ! -f "$file" ]]; then
    curl --fail --location --retry 3 --output "$file.part" "$url"
    printf '%s  %s\n' "$digest" "$file.part" | sha256sum --check -
    mv "$file.part" "$file"
  fi
  printf '%s  %s\n' "$digest" "$file" | sha256sum --check -
}
fetch "https://codeload.github.com/k2-fsa/sherpa-onnx/tar.gz/$sherpa_commit" \
  "$cache/downloads/sherpa-onnx-26aa2fa.tar.gz" "$sherpa_sha"
if [[ ! -d "$source_dir" ]]; then
  tar -xzf "$cache/downloads/sherpa-onnx-26aa2fa.tar.gz" -C "$cache"
fi
if [[ "$variant" == optimized ]] && ! grep -Fq '#ifdef SHERPA_ONNX_KWS_DISABLE_SPINNING' \
  "$source_dir/sherpa-onnx/csrc/session.cc"; then
  patch --batch --forward --fuzz=0 -p1 -d "$source_dir" \
    < "$root/anki/victor/tools/audio/sherpa_kws/sherpa-disable-spinning.patch"
fi
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || {
  echo "Preparation requires a Linux x86_64 workstation." >&2; exit 2;
}
[[ -z "${CMAKE_TOOLCHAIN_FILE:-}" ]] || {
  echo "Unset CMAKE_TOOLCHAIN_FILE; this script selects the toolchain." >&2; exit 2;
}
build="$cache/$mode-build"
prefix="$work/$install_mode"
mkdir -p "$build"
rm -f "$prefix/runtime-ready.txt"
cmake_command=cmake
extra_args=(-DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=OFF)
unset SHERPA_ONNXRUNTIME_INCLUDE_DIR SHERPA_ONNXRUNTIME_LIB_DIR
if [[ "$mode" == target ]]; then
  export VICOS_SDK="${VICOS_SDK:-${VICOS_SDK_HOME:-$root/anki-deps/vicos-sdk/dist/5.3.0-r07}}"
  toolchain="$root/anki/victor/tools/audio/sherpa_kws/vicos-toolchain.cmake"
  cmake_command="$cache/build-tools/cmake/data/bin/cmake"
  if [[ ! -x "$cmake_command" ]]; then
    wheel=cmake-3.31.6-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
    fetch "https://files.pythonhosted.org/packages/59/e8/096984b89133681533650b9078c5ed1c5c9b534e869b5487f22d4de1935c/$wheel" \
      "$cache/downloads/$wheel" 1c8b05df0602365da91ee6a3336fe57525b137706c4ab5675498f662ae1dbcec
    python3 -m pip install --no-index --no-deps --no-cache-dir \
      --disable-pip-version-check --target "$cache/build-tools" "$cache/downloads/$wheel"
  fi
  fetch https://codeload.github.com/microsoft/onnxruntime/tar.gz/refs/tags/v1.17.1 \
    "$cache/downloads/onnxruntime-v1.17.1.tar.gz" \
    88335a1ccbccccb35d5d3aaadee090a9745cf90d97ca87c4a3ac5c4b694b7b17
  [[ -d "$cache/onnxruntime-1.17.1" ]] || tar -xzf "$cache/downloads/onnxruntime-v1.17.1.tar.gz" -C "$cache"
  # GitLab's archive no longer matches ORT's old SHA1. Use the SAME commit
  # from Eigen's public mirror, with its independently pinned archive hash.
  eigen=e7248b26a1ed53fa030c5c459f7ea095dfd276ac
  fetch "https://codeload.github.com/eigen-mirror/eigen/tar.gz/$eigen" \
    "$cache/downloads/eigen-e7248b26.tar.gz" \
    f9dd558b4e0c4b8cafdec90b902c1722d40cf6230a77d53c947af1cfa27d1afa
  [[ -d "$cache/eigen-$eigen" ]] || tar -xzf "$cache/downloads/eigen-e7248b26.tar.gz" -C "$cache"
  ort_build="$cache/ort-target-build"
  disable_contrib=ON
  [[ "$variant" != optimized ]] || disable_contrib=OFF
  "$cmake_command" -S "$cache/onnxruntime-1.17.1/cmake" -B "$ort_build" -G Ninja \
    --compile-no-warning-as-error \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_SOURCE_DIR_EIGEN="$cache/eigen-$eigen" \
    -Donnxruntime_BUILD_UNIT_TESTS=OFF -Donnxruntime_BUILD_SHARED_LIB=ON \
    -Donnxruntime_ENABLE_PYTHON=OFF -Donnxruntime_USE_CUDA=OFF \
    -Donnxruntime_BUILD_BENCHMARKS=OFF -Donnxruntime_ENABLE_TRAINING=OFF \
    -Donnxruntime_USE_NEURAL_SPEED=OFF -Donnxruntime_DISABLE_ML_OPS=ON \
    -Donnxruntime_DISABLE_CONTRIB_OPS="$disable_contrib" -Donnxruntime_ENABLE_CPU_FP16_OPS=OFF \
    -Donnxruntime_CROSS_COMPILING=ON
  # FlatBuffers 1.12 has an unused private assignment operator that attempts
  # to copy a noncopyable buffer; Clang 20 correctly rejects its definition.
  if ! grep -Fxq '    TableKeyComparator &operator=(const TableKeyComparator &other) = delete;' \
    "$ort_build/_deps/flatbuffers-src/include/flatbuffers/flatbuffers.h"; then
    patch --batch --forward --fuzz=0 -p1 -d "$ort_build/_deps/flatbuffers-src" \
      < "$root/anki/victor/tools/audio/sherpa_kws/flatbuffers-clang20.patch"
  fi
  "$cmake_command" --build "$ort_build" --target onnxruntime --parallel "${JOBS:-2}"
  ort_prefix="$cache/ort-target"
  mkdir -p "$ort_prefix/include" "$ort_prefix/lib"
  cp "$cache/onnxruntime-1.17.1/include/onnxruntime/core/session/"onnxruntime*.h "$ort_prefix/include/"
  cp -a "$ort_build"/libonnxruntime.so* "$ort_prefix/lib/"
  export SHERPA_ONNXRUNTIME_INCLUDE_DIR="$ort_prefix/include"
  export SHERPA_ONNXRUNTIME_LIB_DIR="$ort_prefix/lib"
  extra_args=(-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=ON)
else
# Place ORT where the pinned upstream recipe finds and SHA256-verifies it.
fetch "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v1.17.1/onnxruntime-linux-x64-glibc2_17-Release-1.17.1-patched.zip" \
  "$build/onnxruntime-linux-x64-glibc2_17-Release-1.17.1-patched.zip" \
  cb90c51a195bdd453aaf1582f3ef63b466dafbb15c4b8a552ca4dce3769e1d1e
fi
# Upstream enables INSTALL_RPATH_USE_LINK_PATH after project(), overriding a
# normal -D flag. Set the final target properties after target creation instead.
cat > "$cache/install-rpath.cmake" <<'CMAKE'
cmake_language(DEFER CALL set_target_properties
  sherpa-onnx-c-api sherpa-onnx-cxx-api PROPERTIES
  INSTALL_RPATH "$ORIGIN"
  INSTALL_RPATH_USE_LINK_PATH FALSE)
CMAKE
if [[ "$variant" == optimized ]]; then
  cat >> "$cache/install-rpath.cmake" <<'CMAKE'
cmake_language(DEFER CALL target_compile_definitions sherpa-onnx-core
  PRIVATE SHERPA_ONNX_KWS_DISABLE_SPINNING=1)
CMAKE
fi
"$cmake_command" -S "$source_dir" -B "$build" -G Ninja "${extra_args[@]}" \
  "-DCMAKE_PROJECT_sherpa-onnx_INCLUDE=$cache/install-rpath.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_CXX_STANDARD=17 -DBUILD_SHARED_LIBS=ON \
  -DSHERPA_ONNX_ENABLE_C_API=ON \
  -DSHERPA_ONNX_ENABLE_PYTHON=OFF -DSHERPA_ONNX_ENABLE_TESTS=OFF \
  -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF -DSHERPA_ONNX_HAS_ALSA=OFF \
  -DSHERPA_ONNX_ENABLE_JNI=OFF -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
  -DSHERPA_ONNX_ENABLE_GPU=OFF -DSHERPA_ONNX_ENABLE_DIRECTML=OFF \
  -DSHERPA_ONNX_ENABLE_RKNN=OFF -DSHERPA_ONNX_ENABLE_BINARY=OFF \
  -DSHERPA_ONNX_BUILD_C_API_EXAMPLES=OFF -DSHERPA_ONNX_ENABLE_TTS=OFF \
  -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
  -DSHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY=OFF
"$cmake_command" --build "$build" --parallel "${JOBS:-2}"
"$cmake_command" --install "$build"
if [[ "$mode" == target ]]; then
  readelf="$VICOS_SDK/prebuilt/bin/arm-oe-linux-gnueabi-readelf"
  : > "$prefix/abi-report.txt"
  for lib in "$prefix"/lib/*.so*; do
    "$readelf" -h -A -d -V "$lib" > "$cache/abi-check.txt"
    grep -q 'Class:.*ELF32' "$cache/abi-check.txt"
    grep -q 'Machine:.*ARM' "$cache/abi-check.txt"
    grep -q 'soft-float ABI' "$cache/abi-check.txt"
    grep -Fq 'Library runpath: [$ORIGIN]' "$cache/abi-check.txt"
    if grep -q 'Tag_ABI_VFP_args: VFP registers' "$cache/abi-check.txt"; then
      echo "Hard-float library detected: $lib" >&2
      exit 1
    fi
    printf '\nLibrary: %s\n' "$lib" >> "$prefix/abi-report.txt"
    cat "$cache/abi-check.txt" >> "$prefix/abi-report.txt"
  done
  {
    echo "platform=ARMv7-softfp-libc++; cross-build-only; NOT executed on target"
    echo "sherpa=1.12.14 commit=$sherpa_commit; onnxruntime=1.17.1-source"
    if [[ "$variant" == optimized ]]; then
      echo "variant=optimized; contrib_ops=ON; intra_op_spinning=0; inter_op_spinning=0; threads=caller-configured"
      sha256sum "$root/anki/victor/tools/audio/sherpa_kws/sherpa-disable-spinning.patch"
    fi
    "$VICOS_SDK/prebuilt/bin/arm-oe-linux-gnueabi-clang++" --version
    sha256sum "$prefix"/lib/*.so*
  } > "$prefix/runtime-ready.txt"
  echo "Target cross-build completed: $prefix; this command does not execute target binaries."
  exit 0
fi
LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
python3 - "$prefix" <<'PY'
import ctypes
import pathlib
import sys
prefix = pathlib.Path(sys.argv[1])
assert (prefix / "include/sherpa-onnx/c-api/c-api.h").is_file()
lib = ctypes.CDLL(str(prefix / "lib/libsherpa-onnx-c-api.so"))
for name in ("SherpaOnnxCreateKeywordSpotter", "SherpaOnnxCreateKeywordStream",
             "SherpaOnnxDecodeKeywordStream", "SherpaOnnxGetKeywordResult",
             "SherpaOnnxDestroyKeywordResult", "SherpaOnnxDestroyKeywordSpotter"):
    getattr(lib, name)
print("Host C API load + KWS symbols: PASS (not an inference test)")
PY
{
  echo "platform=Linux-x86_64-host-only"
  echo "sherpa=1.12.14 commit=$sherpa_commit source_sha256=$sherpa_sha"
  echo "onnxruntime=1.17.1; CPU-only; load-and-KWS-symbol-check=PASS"
  if [[ "$variant" == optimized ]]; then
    echo "variant=optimized; intra_op_spinning=0; inter_op_spinning=0; threads=caller-configured"
    sha256sum "$root/anki/victor/tools/audio/sherpa_kws/sherpa-disable-spinning.patch"
  fi
  sha256sum "$prefix"/lib/*.so*
} > "$prefix/runtime-ready.txt"
echo "Host runtime ready: $prefix (NOT compatible with Vector ARM softfp)"
