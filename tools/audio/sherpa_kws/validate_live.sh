#!/bin/bash
# Exercise the actual production recognizer, not the standalone replay adapter.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
victor="$root/anki/victor"
work="$root/_build/sherpa-kws"
mode=${1:?usage: validate_live.sh host|target TRIAL_DIRECTORY}
trial=${2:?Provide a neural positive trial directory containing input.wav/input.activity}
trial=$(cd "$trial" && pwd)
case "$mode" in host|target) ;; *) echo "Invalid mode: $mode" >&2; exit 2 ;; esac
mkdir -p "$work/live-model" "$work/live-tests"
model="$work/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01"
for name in encoder decoder joiner; do
  ln -sfn "$model/$name-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
    "$work/live-model/$name-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
done
ln -sfn "$model/tokens.txt" "$work/live-model/tokens.txt"
ln -sfn "$victor/tools/audio/sherpa_kws/keywords.txt" "$work/live-model/keywords.txt"
compiler=g++
flags=(-std=c++14 -O1 -g -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections
  -DANKI_SHERPA_KWS=1 -DANKI_SHERPA_KWS_INTEGRATION_TESTS=1 -DLINUX=1
  -include cstring -include algorithm -include mutex -include functional
  -I"$victor" -I"$victor/animProcess/src" -I"$victor/lib/util/source/anki"
  -I"$victor/lib/util/source/3rd" -I"$work/$mode/include"
  -I"$victor/lib/util/source/3rd/jsoncpp"
  -I"$victor/lib/das-client/testing/gtest/include" -I"$victor/lib/das-client/testing/gtest")
if [[ "$mode" == target ]]; then
  sdk=${VICOS_SDK:-${VICOS_SDK_HOME:-"$root/anki-deps/vicos-sdk/dist/5.3.0-r07"}}
  compiler="$sdk/prebuilt/bin/arm-oe-linux-gnueabi-clang++"
  flags+=(-DVICOS -march=armv7-a -mfloat-abi=softfp -mfpu=neon-vfpv4
    "-Wl,-rpath-link,$work/target/lib" "-Wl,-rpath-link,$sdk/sysroot/usr/lib")
else
  flags+=('-D__has_warning(x)=0')
fi
sources=(
  "$victor/test/animProcess/testSpeechRecognizerSherpaOnnx.cpp"
  "$victor/animProcess/src/cozmoAnim/speechRecognizer/speechRecognizerSherpaOnnx.cpp"
  "$victor/lib/util/source/anki/audioUtil/speechRecognizer.cpp"
  "$victor/lib/util/source/anki/util/logging/logging.cpp"
  "$victor/lib/util/source/anki/util/logging/callstack.cpp"
  "$victor/lib/util/source/anki/util/logging/iLoggerProvider.cpp"
  "$victor/lib/das-client/testing/gtest/src/gtest-all.cc"
  "$victor/lib/das-client/testing/gtest/src/gtest_main.cc")
"$compiler" "${flags[@]}" "${sources[@]}" -L"$work/$mode/lib" -lsherpa-onnx-c-api \
  "-Wl,-rpath,$work/$mode/lib" -o "$work/live-tests/$mode"
export SHERPA_KWS_TEST_MODEL="$work/live-model"
export SHERPA_KWS_TEST_WAV="$trial/input.wav"
export SHERPA_KWS_TEST_ACTIVITY="$trial/input.activity"
if [[ "$mode" == host ]]; then
  "$work/live-tests/host"
else
  qemu="$root/poky/build/tmp-glibc/sysroots-components/x86_64/qemu-native/usr/bin/qemu-arm"
  rootfs="$root/poky/build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs"
  LD_LIBRARY_PATH="$root/poky/build/tmp-glibc/sysroots-components/x86_64/glib-2.0-native/usr/lib:$root/poky/build/tmp-glibc/sysroots-components/x86_64/pcre2-native/usr/lib" \
    "$qemu" -cpu cortex-a7 -L "$rootfs" -E "LD_LIBRARY_PATH=$work/target/lib:$sdk/sysroot/usr/lib" \
    "$work/live-tests/target"
fi
