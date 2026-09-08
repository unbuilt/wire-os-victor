#!/bin/bash
# Run from wire-os; artifacts stay below the explicitly supplied build directory.
set -euo pipefail
ulimit -c 0
root=$PWD
victor="$root/anki/victor"
output=${1:?usage: validate_aec_experiment.sh OUTPUT_DIRECTORY [native|vendor]}
mode=${2:-native}
mkdir -p "$output"
output=$(cd "$output" && pwd)
export TMPDIR="$output"
if [[ "$mode" == native ]]; then
  cd "$victor"
  flags=(-std=c++14 -O1 -g -pipe -pthread -ffunction-sections -fdata-sections
    -Wl,--gc-sections -DLINUX=1 '-D__has_warning(x)=0'
    -include cstring -include climits -include algorithm -include mutex -include functional
    -Ilib/audio/include -Ilib/audio/wwise/versions/current/include
    -Ilib/util/source/anki -Ilib/util/source/3rd/jsoncpp -I. -Ilib/das-client/testing/gtest/include
    -Ilib/das-client/testing/gtest)
  sources=(test/animProcess/testAecPlaybackReference.cpp
    test/animProcess/testAecDiagnosticCapture.cpp
    lib/audio/source/engine/plugins/aecDiagnosticCapture.cpp
    test/engine/testCloudAudioPlaybackState.cpp test/animProcess/testStreamingWaveDataInstance.cpp
    lib/audio/source/engine/audioTools/streamingWaveDataInstance.cpp
    lib/util/source/anki/util/logging/logging.cpp lib/util/source/anki/util/logging/callstack.cpp
    lib/util/source/anki/util/logging/iLoggerProvider.cpp
    lib/das-client/testing/gtest/src/gtest-all.cc lib/das-client/testing/gtest/src/gtest_main.cc)
  g++ "${flags[@]}" "${sources[@]}" -o "$output/test-audio"
  "$output/test-audio"
  g++ "${flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${sources[@]}" -o "$output/test-audio-sanitized"
  "$output/test-audio-sanitized" --gtest_filter='Aec*'
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/tools -p 'testAec*.py' -v
elif [[ "$mode" == vendor ]]; then
  cc=/home/unbuilt/.anki/vicos-sdk/dist/5.3.0-r07/prebuilt/bin/arm-oe-linux-gnueabi-clang
  export LD_LIBRARY_PATH="$root/poky/build/tmp-glibc/sysroots-components/x86_64/glib-2.0-native/usr/lib:$root/poky/build/tmp-glibc/sysroots-components/x86_64/pcre2-native/usr/lib"
  qemu="$root/poky/build/tmp-glibc/sysroots-components/x86_64/qemu-native/usr/bin/qemu-arm"
  rootfs="$root/poky/build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs"
  cmake --build "$victor/_build/vicos/Release" --target signal_essence
  for variant in vicos-highres vicos; do
    se="$victor/3rd/signalEssence/v009/$variant"
    # Public TD config exposes the model, not its internal reference FIR.
    # The supported vendor 16 kHz FIR is seven int16 coefficients (14 bytes).
    nm -S "$se/platform/anki_victor_example/build/libmmfx.a" |
      awk '$4 == "pAecPrefilterCoef16kHz" { found++; if ($2 != "0000000e") exit 1 }
           END { if (found != 1) exit 1 }'
    if [[ "$variant" == vicos-highres ]]; then
      project=("$victor/_build/vicos/Release/lib/libsignal_essence.a")
    else
      project=("$victor/lib/signalEssence/aecExperiment.c"
        "$se/project/anki_victor/crossoverconfig.c" "$se/project/anki_victor/fdsearchconfig.c"
        "$se/project/anki_victor/mmif_proj.c" "$se/project/anki_victor/policy_actions.c"
        "$se/project/anki_victor/spatialfilterconfig.c"
        "$se/project/anki_victor_vad/nfbin_f32_anki.c" "$se/project/anki_victor_vad/svad.c"
        "$victor/3rd/signalEssence/v009/vicos-highres/platform/anki_victor_example/build/shim.c")
    fi
    "$cc" -std=gnu99 -O2 -DVICOS -DANKI_AEC_EXPERIMENT_SUPPORTED=1 \
      -march=armv7-a -mfloat-abi=softfp -mfpu=neon-vfpv4 \
      -I"$victor/lib/signalEssence" -I"$se/se_lib_public" -I"$se/project/anki_victor" \
      -I"$se/project/anki_victor_vad" -I"$se/se_lib_public/cpu_arm" \
      "$victor/tools/audio/aecExperimentSmoke.c" -Wl,--start-group "${project[@]}" \
      "$se/platform/anki_victor_example/build/libmmfx.a" -Wl,--end-group -lm -o "$output/smoke-$variant"
    for experiment in off reference on; do
      ANKI_AEC_EXPERIMENT="$experiment" "$qemu" -L "$rootfs" "$output/smoke-$variant"
    done
    env -u ANKI_AEC_EXPERIMENT "$qemu" -L "$rootfs" "$output/smoke-$variant"
    status=0
    ANKI_AEC_EXPERIMENT=on "$qemu" -L "$rootfs" "$output/smoke-$variant" --cancel-only-control || status=$?
    test "$status" = 5
    echo "PASS $variant: original cancellation-only guard fails at a trained gap"
    status=0
    ANKI_AEC_EXPERIMENT=on "$qemu" -L "$rootfs" "$output/smoke-$variant" --no-history-control || status=$?
    test "$status" = 5
    echo "PASS $variant: complete-current-block-only guard fails during trained recovery"
    status=0
    ANKI_AEC_EXPERIMENT=on "$qemu" -L "$rootfs" "$output/smoke-$variant" --unsupported-ref-delay || status=$?
    test "$status" = 134
    echo "PASS $variant: unsupported vendor reference delay rejected"
    for setting in 'ANKI_AEC_EXPERIMENT=invalid' 'ANKI_AEC_MIC_AGE_MS=-1' 'ANKI_AEC_REF_DELAY_MS=201'; do
      status=0
      env ANKI_AEC_EXPERIMENT=on "$setting" "$qemu" -L "$rootfs" "$output/smoke-$variant" || status=$?
      test "$status" = 134
    done
  done
  # Native stream tests cannot expose the deployed ARM char/EOF ABI mismatch.
  # Link the actual production object, not a separately compiled recorder copy.
  object="$victor/_build/vicos/Release/lib/audio/CMakeFiles/audio_engine.dir/source/engine/plugins/aecDiagnosticCapture.cpp.o"
  test "$object" -nt "$victor/lib/audio/source/engine/plugins/aecDiagnosticCapture.cpp"
  test "$object" -nt "$victor/lib/audio/include/audioEngine/plugins/aecDiagnosticCapture.h"
  mkdir "$output/arm-writer"
  sha256sum "$object" > "$output/arm-writer/production-object.sha256"
  cxx=/home/unbuilt/.anki/vicos-sdk/dist/5.3.0-r07/prebuilt/bin/arm-oe-linux-gnueabi-clang++
  "$cxx" -std=c++14 -Os -g -fsigned-char -pthread -DVICOS \
    -march=armv7-a -mfloat-abi=softfp -mfpu=neon-vfpv4 -mthumb -funwind-tables -frtti -fexceptions \
    -I"$victor/lib/audio/include" -I"$victor/lib/audio/wwise/versions/current/include" \
    -I"$victor/lib/das-client/testing/gtest/include" -I"$victor/lib/das-client/testing/gtest" \
    "$victor/test/animProcess/testAecDiagnosticCapture.cpp" \
    "$victor/lib/das-client/testing/gtest/src/gtest-all.cc" \
    "$victor/lib/das-client/testing/gtest/src/gtest_main.cc" \
    "$object" -o "$output/arm-writer/test-writer"
  # Rotation is covered natively. QEMU's 32-bit readdir can reject the host
  # filesystem's 64-bit directory cookies with EOVERFLOW; these are writer tests.
  TMPDIR="$output/arm-writer" "$qemu" -L "$rootfs" "$output/arm-writer/test-writer" \
    --gtest_filter='AecDiagnostic.Writer*' | tee "$output/arm-writer/results.log"
else
  echo "Unknown validation mode: $mode" >&2
  exit 2
fi
