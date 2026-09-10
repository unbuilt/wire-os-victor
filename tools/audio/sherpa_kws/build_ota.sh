#!/usr/bin/env bash
# Workstation only: build/package a development OTA. Never contacts a robot.
set -euo pipefail
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  cat <<'HELP'
Usage: build_ota.sh VERSION prepare|configure|compile|build|package|verify [--backend picovoice|sherpa_onnx]
Run from any directory. Requires the existing vic-yocto-builder-7:latest image.
Picovoice is the default; it requires no sherpa runtime/model preparation.
For an UNUSED development OTA version:
  bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 118 configure
  bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 118 compile
  bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 118 build
  bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 118 package
For experimental sherpa, first prepare_runtime.sh target, then use prepare,
configure, compile, build and package with --backend sherpa_onnx on EVERY command.
To verify the existing experimental OTA:
  bash anki/victor/tools/audio/sherpa_kws/build_ota.sh 117 verify --backend sherpa_onnx
The package command decrypts and verifies the OTA; verify repeats that check.
Verification records input hashes beside the package, so older OTAs can be
verified after switching the shared build directory to a different backend.
Only --backend sherpa_onnx enables SHERPA_KWS and its backend environment.
Picovoice builds explicitly disable SHERPA_KWS and remove stale KWS payloads.
Existing OTAs are never overwritten. No command installs or flashes a robot.
Robot rollback override (read on process startup):
  /data/data/com.anki.victor/persistent/kws/backend containing picovoice
The sherpa_onnx override requires an image built with the experimental backend.
HELP
  exit 0
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
cd "$root"
version="${1:?usage: build_ota.sh VERSION ACTION [--backend picovoice|sherpa_onnx]}"
mode="${2:?usage: build_ota.sh VERSION ACTION [--backend picovoice|sherpa_onnx]}"
backend=picovoice
if [[ $# -eq 4 && "$3" == --backend ]]; then
  backend="$4"
elif [[ $# -ne 2 ]]; then
  echo "Usage: build_ota.sh VERSION ACTION [--backend picovoice|sherpa_onnx]" >&2
  exit 2
fi
[[ "$version" =~ ^[1-9][0-9]*$ ]]
case "$backend" in
  picovoice) sherpa=OFF ;;
  sherpa_onnx) sherpa=ON ;;
  *) echo "Unknown backend: $backend" >&2; exit 2 ;;
esac
case "$mode" in prepare|configure|compile|build|package|verify) ;; *) exit 2 ;; esac
test -d poky
work="$root/_build/sherpa-kws"
validation="$root/_build/sherpa-kws-ota-$version-validation"
package="$root/_build/sherpa-kws-ota-$version"
ota="$root/_build/vicos-3.0.1.${version}d.ota"
tools="$root/anki/victor/tools/audio/sherpa_kws"
mkdir -p "$validation"
export TMPDIR="$validation"
exec 9>"$validation/build.lock"
flock -n 9 || { echo "This OTA workspace is already in use." >&2; exit 1; }
if [[ "$mode" != verify ]]; then
  test ! -e "$ota" || { echo "Refusing to overwrite $ota" >&2; exit 1; }
fi

in_builder() {
  docker run --rm --user unbuilt \
    -v "$root:$root" -v "$root/anki-deps:/home/unbuilt/.anki" \
    -v "$root/build/gocache:/home/unbuilt/go" \
    -v "$root/build/usercache:/home/unbuilt/.cache" \
    -v "$root/build/cache:/home/unbuilt/.ccache" \
    -e SHELL=/bin/bash -e GOPROXY=https://goproxy.cn -e GOSUMDB=off \
    -e TMPDIR -e KWS_BUILD_VERSION="$version" -e KWS_VALIDATION="$validation" -e KWS_SHERPA="$sherpa" \
    -w "$root" vic-yocto-builder-7:latest /bin/bash -c "$1"
}

case "$mode" in
  prepare)
    if [[ "$backend" == picovoice ]]; then
      echo "Picovoice uses existing platform assets; no sherpa preparation required."
      exit 0
    fi
    test -f "$work/target/runtime-ready.txt" || {
      echo "First run: bash anki/victor/tools/audio/sherpa_kws/prepare_runtime.sh target" >&2
      exit 1
    }
    bash "$tools/prepare_model.sh"
    for library in libsherpa-onnx-c-api.so libonnxruntime.so.1.17.1; do
      report="$(readelf -h -A "$work/target/lib/$library")"
      grep -q 'Class:.*ELF32' <<< "$report"
      grep -q 'Machine:.*ARM' <<< "$report"
      grep -q 'soft-float ABI' <<< "$report"
      if grep -q 'Tag_ABI_VFP_args: VFP registers' <<< "$report"; then
        echo "Hard-float runtime is not supported: $library" >&2
        exit 1
      fi
    done
    licenses="$work/ota-licenses"
    runtime="$work/runtime"
    mkdir -p "$licenses"
    cp "$runtime/sherpa-onnx-26aa2fa93210376a89de3a65a1a4dd320c37f5e9/LICENSE" "$licenses/SHERPA_ONNX_LICENSE"
    cp "$runtime/onnxruntime-1.17.1/LICENSE" "$licenses/ONNXRUNTIME_LICENSE"
    cp "$runtime/onnxruntime-1.17.1/ThirdPartyNotices.txt" "$licenses/ONNXRUNTIME_THIRD_PARTY_NOTICES"
    (
      cd "$runtime/target-build/_deps"
      for file in \
        kaldi_native_fbank-src/LICENSE simple-sentencepiece-src/LICENSE \
        kaldifst-src/LICENSE kaldi_decoder-src/LICENSE cppjieba-src/LICENSE \
        eigen-src/COPYING.README eigen-src/COPYING.MPL2 eigen-src/COPYING.BSD \
        eigen-src/COPYING.MINPACK eigen-src/COPYING.APACHE \
        kissfft-src/COPYING kissfft-src/LICENSES/BSD-3-Clause \
        kissfft-src/LICENSES/Unlicense openfst-src/COPYING; do
        printf '\n===== %s =====\n' "$file"
        cat "$file"
      done
    ) > "$licenses/SHERPA_ONNX_THIRD_PARTY_NOTICES"
    echo "Prepared selected ARM runtime, model and license packaging inputs."
    ;;
  configure)
    in_builder 'set -e
      cd anki/victor
      project/victor/scripts/victor_build_release.sh -I -C "-DSHERPA_KWS=$KWS_SHERPA"'
    ;;
  compile)
    grep -qx "SHERPA_KWS:BOOL=$sherpa" anki/victor/_build/vicos/Release/CMakeCache.txt
    in_builder 'set -e
      /home/unbuilt/.anki/cmake/dist/3.30.4/bin/cmake \
        --build anki/victor/_build/vicos/Release --target vic-anim --parallel 2' \
      2>&1 | tee "$validation/compile.log"
    ;;
  build)
    if [[ "$backend" == sherpa_onnx ]]; then
      test -f "$work/ota-licenses/SHERPA_ONNX_THIRD_PARTY_NOTICES"
    fi
    # A post-read configuration applies only to this invocation, never local.conf.
    printf 'SHERPA_KWS:pn-victor = "%s"\n' "$sherpa" > "$validation/sherpa-kws.conf"
    in_builder 'set -e
      cd poky
      source build/conf/set_bb_env.sh
      export ANKI_BUILD_VERSION="$KWS_BUILD_VERSION" AUTO_UPDATE=0
      export MACHINE=apq8009-robot DISTRO=msm-perf VARIANT=perf PRODUCT=robot
      os_version=build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs/etc/os-version
      if test "$(cat "$os_version")" != "3.0.1.${KWS_BUILD_VERSION}d"; then
        bitbake -R "$KWS_VALIDATION/sherpa-kws.conf" -c cleansstate anki-version
      fi
      build-dev -R "$KWS_VALIDATION/sherpa-kws.conf"' 2>&1 | tee "$validation/build.log"
    ;;
  package)
    test ! -e "$package"
    images="$root/poky/build/tmp-glibc/deploy/images/apq8009-robot-robot-perf"
    rootfs="$root/poky/build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs"
    test "$(cat "$rootfs/etc/os-version")" = "3.0.1.${version}d"
    if [[ "$backend" == sherpa_onnx ]]; then
      grep -qx 'ANKI_KWS_BACKEND=sherpa_onnx' "$rootfs/anki/etc/vic-anim.env"
    else
      cmp anki/victor/_build/vicos/Release/etc/vic-anim.env "$rootfs/anki/etc/vic-anim.env"
      if grep -q '^ANKI_KWS_BACKEND=' "$rootfs/anki/etc/vic-anim.env"; then
        echo "Picovoice image must not contain a backend environment override." >&2
        exit 1
      fi
      test ! -e "$rootfs/anki/data/assets/cozmo_resources/assets/sherpaKws"
      test ! -e "$rootfs/anki/data/licenses/sherpaKws"
    fi
    grep -qx "SHERPA_KWS:BOOL=$sherpa" anki/victor/_build/vicos/Release/CMakeCache.txt
    test "$images/apq8009-robot-raw-sysfs.ext4" -nt anki/victor/_build/vicos/Release/bin/vic-anim
    # Isolate signing and sparse-image generation; preserve earlier OTAs/deploy images.
    mkdir -p "$package/images"
    cp --reflink=auto "$images/apq8009-robot-boot.img.nonsecure" "$package/images/"
    cp --reflink=auto "$images/apq8009-robot-raw-sysfs.ext4" "$package/images/"
    (cd ota
      make IMG_DIR="$package/images" devsign
      make IMG_DIR="$package/images" BUILD="$package" OTA_FILE="$ota" \
        UPDATE_VERSION="3.0.1.${version}d" ANKIDEV=1)
    python3 "$tools/verify_ota.py" "$version" --backend "$backend" | tee "$validation/verification.log"
    ;;
  verify)
    python3 "$tools/verify_ota.py" "$version" --backend "$backend" | tee "$validation/verification.log"
    ;;
esac
