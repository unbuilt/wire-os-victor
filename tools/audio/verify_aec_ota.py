#!/usr/bin/env python3
"""Verify the actual encrypted development OTA, not just packaging inputs."""
import argparse
import configparser
import hashlib
import re
from pathlib import Path
import subprocess
import tarfile


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", type=int)
    parser.add_argument("--workspace", help="Fresh build workspace, e.g. 107final (default: version)")
    args = parser.parse_args()
    root = Path.cwd()
    workspace = args.workspace or str(args.version)
    if not re.fullmatch(str(args.version) + r"[a-zA-Z0-9-]*", workspace):
        parser.error("workspace must start with the version and contain only letters, digits or hyphens")
    version = "3.0.1.{}d".format(args.version)
    ota = root / "_build" / ("vicos-" + version + ".ota")
    package = root / "_build" / ("aec-experiment-{}".format(workspace))
    verified = root / "_build" / ("aec-experiment-{}-validation".format(workspace)) / "payloads"
    verified.mkdir(parents=True, exist_ok=True)
    manifest = configparser.ConfigParser()
    with tarfile.open(ota) as archive:
        text = archive.extractfile("manifest.ini").read().decode()
    manifest.read_string(text)
    (verified / "manifest.ini").write_text(text)
    assert manifest["META"]["update_version"] == version
    assert manifest["META"]["ankidev"] == "1"
    for section, stem in [("BOOT", "boot"), ("SYSTEM", "sysfs")]:
        name = "apq8009-robot-{}.img".format(stem)
        payload = verified / name
        subprocess.run(
            ["bash", "-o", "pipefail", "-c",
             'tar -xOf "$1" "$2" | openssl aes-256-ctr -d -pass file:ota/ota_test.pass -md md5 | gzip -dc > "$3"',
             "verify", str(ota), name + ".gz", str(payload)], check=True)
        assert payload.stat().st_size == int(manifest[section]["bytes"])
        assert digest(payload) == manifest[section]["sha256"] == digest(package / name)
        print(section, payload.stat().st_size, digest(payload), "manifest/decrypted/package MATCH")
    system = verified / "apq8009-robot-sysfs.img"
    for remote, built in [
        ("/etc/os-version", None),
        ("/anki/bin/vic-anim", "bin/vic-anim"),
        ("/anki/bin/vic-robot", "bin/vic-robot"),
        ("/anki/bin/vic-engine", "bin/vic-engine"),
        ("/anki/lib/libaudio_engine.so", "lib/libaudio_engine.so"),
    ]:
        extracted = verified / Path(remote).name
        if extracted.exists():
            extracted.unlink()
        subprocess.run(["debugfs", "-R", "dump {} {}".format(remote, extracted), str(system)], check=True)
        assert extracted.exists()
        if built:
            assert digest(extracted) == digest(root / "anki/victor/_build/vicos/Release" / built)
            print(remote, digest(extracted), "decrypted OTA / compiled MATCH")
        else:
            assert extracted.read_text().strip() == version
            print(remote, version, "MATCH")
    binary = (verified / "vic-anim").read_bytes()
    for marker in [b"max_se_cpu_us=%u", b"mic_clock_fault=%u", b"last_update_bypass=%d",
                   b"vendor AEC adaptation control unavailable",
                   b"unsupported vendor AEC reference filter/delay/history configuration",
                   b"prior_td_samples=%d", b"history_remaining=%d",
                   b"mic_rate_ppb=%d", b"source_errors=%u", b"ref_fit_error_us=%d"]:
        assert marker in binary, marker
    if args.version >= 109:
        for marker in [b"revision=109 mic_updates=%u", b"mic_fault_observed_ns=%llu",
                       b"source_expected=%u", b"ref_fault_reason=%u"]:
            assert marker in binary, marker
    if args.version >= 110:
        for marker in [b"AecDiagnosticCapture", b"AecDiagnosticStatus", b"AEC_DIAGNOSTIC"]:
            assert marker in binary, marker
        audio_binary = (verified / "libaudio_engine.so").read_bytes()
        for marker in [b"\"revision\":110", b"\"diagnostic_only\":true", b"predicted_before_ns"]:
            assert marker in audio_binary, marker
    for remote in ["/usr/lib/systemd/system/vic-anim.service", "/anki/etc/vic-anim.env"]:
        extracted = verified / Path(remote).name
        if extracted.exists():
            extracted.unlink()
        subprocess.run(["debugfs", "-R", "dump {} {}".format(remote, extracted), str(system)], check=True)
        assert extracted.exists()
        assert b"ANKI_AEC_EXPERIMENT" not in extracted.read_bytes()
        print(remote, "no packaged AEC opt-in")
    print("OTA", ota.stat().st_size, digest(ota))


if __name__ == "__main__":
    main()
