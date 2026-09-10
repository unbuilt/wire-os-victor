#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
victor="$root/anki/victor"
source="$victor/tools/audio/sherpa_kws"
output="$root/_build/sherpa-kws"
mode=${1:?usage: build_trial.sh tests|vad|host|target [SHERPA_INSTALL]}
mkdir -p "$output"
case "$mode" in
  tests)
    mkdir -p "$output/tests"
    # The bundled GoogleTest predates current GCC warning diagnostics.
    g++ -std=c++14 -pthread -Wall -Wextra -Werror \
      -Wno-missing-field-initializers -Wno-maybe-uninitialized \
      -I"$victor/lib/util/source/anki" \
      -I"$victor/lib/das-client/testing/gtest/include" \
      -I"$victor/lib/das-client/testing/gtest" \
      "$victor/test/animProcess/testKwsVadGate.cpp" \
      "$victor/lib/das-client/testing/gtest/src/gtest-all.cc" \
      "$victor/lib/das-client/testing/gtest/src/gtest_main.cc" \
      -o "$output/tests/test-vad-gate"
    "$output/tests/test-vad-gate"
    ;;
  vad)
    sdk=${VICOS_SDK:-${VICOS_SDK_HOME:-"$root/anki-deps/vicos-sdk/dist/5.3.0-r07"}}
    se="$victor/3rd/signalEssence/v009/vicos-highres"
    "$sdk/prebuilt/bin/arm-oe-linux-gnueabi-clang" \
      -std=gnu99 -O2 -DVICOS -march=armv7-a -mfloat-abi=softfp -mfpu=neon-vfpv4 \
      -I"$se/se_lib_public" -I"$se/se_lib_public/cpu_arm" \
      -I"$se/project/anki_victor_vad" \
      "$source/vectorVadReplay.c" \
      "$se/project/anki_victor_vad/svad.c" "$se/project/anki_victor_vad/nfbin_f32_anki.c" \
      "$se/platform/anki_victor_example/build/shim.c" \
      "$se/platform/anki_victor_example/build/libmmfx.a" \
      -lm -o "$output/vector-vad-replay"
    ;;
  host|target)
    install=${2:?Provide the matching sherpa C API installation directory}
    install=$(cd "$install" && pwd)
    args=()
    if [[ "$mode" == target ]]; then
      args+=("-DCMAKE_TOOLCHAIN_FILE=$victor/cmake/vicos.oelinux.toolchain.cmake"
        "-DVICOS_SDK=${VICOS_SDK:-${VICOS_SDK_HOME:-$root/anki-deps/vicos-sdk/dist/5.3.0-r07}}")
    fi
    cmake -S "$source" -B "$output/replay-$mode" -DCMAKE_BUILD_TYPE=Release \
      "-DSHERPA_ROOT=$install" "${args[@]}"
    cmake --build "$output/replay-$mode" --parallel 2
    ;;
  *) echo "Unknown mode: $mode" >&2; exit 2 ;;
esac
