#!/bin/bash
# Workstation only. Does not install, enable, or contact a robot.
set -euo pipefail
root=$PWD
version=${1:?usage: build_aec_experiment.sh VERSION [build|package|vendor] [WORKSPACE]}
mode=${2:-build}
workspace=${3:-$version}
[[ "$version" =~ ^[0-9]+$ ]]
[[ "$workspace" =~ ^${version}[a-zA-Z0-9-]*$ ]]
test -d "$root/poky"
validation="_build/aec-experiment-$workspace-validation"
mkdir -p "$validation"
export TMPDIR="$root/$validation"
in_builder()
{
  docker run --rm --user unbuilt \
    -v "$root:$root" -v "$root/anki-deps:/home/unbuilt/.anki" \
    -v "$root/build/gocache:/home/unbuilt/go" \
    -v "$root/build/usercache:/home/unbuilt/.cache" \
    -v "$root/build/cache:/home/unbuilt/.ccache" \
    -e SHELL=/bin/bash -e GOPROXY=https://goproxy.cn -e GOSUMDB=off \
    -e TMPDIR -e AEC_BUILD_VERSION="$version" -e AEC_VALIDATION="$validation" \
    -w "$root" vic-yocto-builder-7:latest /bin/bash -c "$1"
}
case "$mode" in
  build)
    test ! -e "_build/vicos-3.0.1.${version}d.ota"
    in_builder 'set -e
      cd poky
      source build/conf/set_bb_env.sh
      export ANKI_BUILD_VERSION="$AEC_BUILD_VERSION" AUTO_UPDATE=0
      export MACHINE=apq8009-robot DISTRO=msm-perf VARIANT=perf PRODUCT=robot
      if test "$(cat build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs/etc/os-version)" != "3.0.1.${AEC_BUILD_VERSION}d"; then
        bitbake -c cleansstate anki-version
      fi
      build-dev'
    ;;
  vendor)
    in_builder 'bash anki/victor/tools/audio/validate_aec_experiment.sh "$AEC_VALIDATION" vendor'
    ;;
  package)
    package="_build/aec-experiment-$workspace"
    ota="_build/vicos-3.0.1.${version}d.ota"
    images=poky/build/tmp-glibc/deploy/images/apq8009-robot-robot-perf
    test ! -e "$package"
    test ! -e "$ota"
    test "$(cat poky/build/tmp-glibc/work/apq8009_robot-oe-linux-gnueabi/machine-robot-image/1.0/rootfs/etc/os-version)" = "3.0.1.${version}d"
    test "$images/apq8009-robot-raw-sysfs.ext4" -nt anki/victor/_build/vicos/Release/bin/vic-anim
    # Only this exact generated image is stale after incremental image builds.
    rm -f "$images/apq8009-robot-sysfs.ext4"
    (cd ota && make devsign &&
      make BUILD="../$package" OTA_FILE="../$ota" ANKIDEV=1)
    python3 anki/victor/tools/audio/verify_aec_ota.py "$version" --workspace "$workspace"
    ;;
  *) echo "Unknown mode: $mode" >&2; exit 2 ;;
esac
