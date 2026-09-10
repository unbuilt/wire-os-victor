#!/usr/bin/env python3
"""Verify the selected KWS backend in an encrypted OTA without mounting or flashing."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def output(*command):
    return subprocess.check_output(command, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", type=int)
    parser.add_argument("--backend", choices=("picovoice", "sherpa_onnx"), default="picovoice")
    args = parser.parse_args()
    require(args.version > 0, "Version must be positive")
    root = Path(__file__).resolve().parents[5]
    version = "3.0.1.{}d".format(args.version)
    ota = root / "_build" / ("vicos-" + version + ".ota")
    package = root / "_build" / ("sherpa-kws-ota-{}".format(args.version))
    verified = root / "_build" / ("sherpa-kws-ota-{}-validation".format(args.version)) / "payloads"
    verified.mkdir(parents=True, exist_ok=True)
    compiled = root / "anki/victor/_build/vicos/Release"
    prepared = root / "_build/sherpa-kws"
    assets = "/anki/data/assets/cozmo_resources/assets/sherpaKws"
    inputs_path = package / "verification-inputs.json"
    recorded = inputs_path.is_file()
    inputs = json.loads(inputs_path.read_text()) if recorded else {
        "version": version, "backend": args.backend, "files": {}
    }
    require(inputs["version"] == version and inputs["backend"] == args.backend,
            "Recorded package inputs have a different version/backend")
    manifest = configparser.ConfigParser()
    with tarfile.open(ota) as archive:
        require(set(archive.getnames()) == {
            "manifest.ini", "apq8009-robot-boot.img.gz", "apq8009-robot-sysfs.img.gz"
        }, "Unexpected OTA archive contents")
        text = archive.extractfile("manifest.ini").read().decode()
    manifest.read_string(text)
    (verified / "manifest.ini").write_text(text)
    require(manifest["META"]["update_version"] == version, "Manifest version mismatch")
    require(manifest["META"]["ankidev"] == "1", "Not a development OTA")
    require(manifest["META"]["num_images"] == "2", "Unexpected image count")
    for section, stem in [("BOOT", "boot"), ("SYSTEM", "sysfs")]:
        require(manifest[section]["encryption"] == "1", "Payload is not encrypted")
        require(manifest[section]["compression"] == "gz", "Unexpected compression")
        name = "apq8009-robot-{}.img".format(stem)
        payload = verified / name
        subprocess.run([
            "bash", "-o", "pipefail", "-c",
            'tar -xOf "$1" "$2" | openssl aes-256-ctr -d -pass file:"$3" -md md5 | gzip -dc > "$4"',
            "verify", str(ota), name + ".gz", str(root / "ota/ota_test.pass"), str(payload)
        ], check=True)
        require(payload.stat().st_size == int(manifest[section]["bytes"]), section + " size mismatch")
        require(digest(payload) == manifest[section]["sha256"] == digest(package / name),
                section + " decrypted/manifest/package mismatch")
        print(section, payload.stat().st_size, digest(payload), "MATCH")

    # Verify the signature from the decrypted boot bytes, not merely a sidecar.
    unsigned_boot = package / "images/apq8009-robot-boot.img.nonsecure"
    boot_size = unsigned_boot.stat().st_size
    boot = (verified / "apq8009-robot-boot.img").read_bytes()
    require(hashlib.sha256(boot[:boot_size]).hexdigest() == digest(unsigned_boot),
            "Decrypted boot does not match the unsigned build input")
    boot_digest = verified / "boot.sha256"
    boot_signature = verified / "boot.signature"
    boot_digest.write_bytes(hashlib.sha256(boot[:boot_size]).digest())
    boot_signature.write_bytes(boot[boot_size:boot_size + 256])
    require(not any(boot[boot_size + 256:]), "Unexpected nonzero boot signature padding")
    subprocess.run([
        "openssl", "pkeyutl", "-verify", "-in", str(boot_digest),
        "-sigfile", str(boot_signature), "-inkey", str(root / "ota/vble-qti.key.pub"),
        "-pubin", "-pkeyopt", "digest:sha256", "-pkeyopt", "rsa_padding_mode:pkcs1"
    ], check=True)
    print("BOOT development signature from decrypted payload PASS")

    system = verified / "apq8009-robot-sysfs.img"

    def extract(remote):
        destination = verified / "rootfs" / remote.lstrip("/")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            destination.unlink()
        subprocess.run(["debugfs", "-R", 'dump "{}" "{}"'.format(remote, destination), str(system)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        require(destination.is_file(), "Missing from encrypted OTA: " + remote)
        return destination

    def matches(remote, source):
        extracted = extract(remote)
        expected = inputs["files"][remote] if recorded else digest(source)
        require(digest(extracted) == expected, remote + " differs from its build input")
        inputs["files"][remote] = expected
        print(remote, digest(extracted), "MATCH")
        return extracted

    def names(remote):
        listing = output("debugfs", "-R", 'ls -p "{}"'.format(remote), str(system))
        require(any(line.startswith("/") for line in listing.splitlines()),
                "Cannot list OTA directory: " + remote)
        return {line.split("/")[5] for line in listing.splitlines()
                if line.startswith("/") and len(line.split("/")) >= 7} - {".", ".."}

    def absent(remote):
        listing = subprocess.run(["debugfs", "-R", 'stat "{}"'.format(remote), str(system)],
                                 check=True, capture_output=True, text=True)
        require("File not found by ext2_lookup" in listing.stderr,
                "Unexpected OTA path (or failed absence check): " + remote)

    require(extract("/etc/os-version").read_text().strip() == version, "Rootfs version mismatch")
    print("/etc/os-version", version, "MATCH")
    binary = matches("/anki/bin/vic-anim", compiled / "bin/vic-anim")
    matches("/anki/bin/vic-robot", compiled / "bin/vic-robot")
    matches("/anki/bin/vic-engine", compiled / "bin/vic-engine")
    audio = matches("/anki/lib/libaudio_engine.so", compiled / "lib/libaudio_engine.so")
    environment = extract("/anki/etc/vic-anim.env").read_text().splitlines()
    settings = [line for line in environment if line.startswith("ANKI_KWS_BACKEND=")]
    expected_settings = ["ANKI_KWS_BACKEND=sherpa_onnx"] if args.backend == "sherpa_onnx" else []
    require(settings == expected_settings, "Wrong packaged backend environment")
    if not recorded:
        inputs["environment"] = [line for line in (compiled / "etc/vic-anim.env").read_text().splitlines()
                                 if line]
    require([line for line in environment if line and not line.startswith("ANKI_KWS_BACKEND=")] ==
            inputs["environment"],
            "Non-KWS vic-anim environment changed")
    print("/anki/etc/vic-anim.env", settings or "no override (Picovoice default)", "MATCH")
    service = matches("/usr/lib/systemd/system/vic-anim.service",
                      root / "poky/victor/meta-anki/recipes/anki-robot/files/vic-anim.service")
    require("EnvironmentFile=/anki/etc/vic-anim.env" in service.read_text(),
            "Service does not consume the packaged backend environment")
    require("ANKI_AEC_EXPERIMENT" not in "\n".join(environment) + service.read_text(),
            "AEC opt-in unexpectedly packaged")

    dynamic = output("readelf", "-d", str(binary))
    symbols = output("readelf", "--dyn-syms", "--wide", str(binary))
    require("/anki/lib" in dynamic, "vic-anim runtime library search path missing")
    for marker in (b"revision=109 mic_updates=%u", b"AecDiagnosticCapture",
                   b"unsupported vendor AEC reference filter/delay/history configuration"):
        require(marker in binary.read_bytes(), "Missing recognizer/AEC marker: " + repr(marker))
    require(b'"diagnostic_only":true' in audio.read_bytes(), "AEC diagnostics unexpectedly changed")

    require("libpv_porcupine_softfp.so" in dynamic and "pv_porcupine_init_softfp" in symbols,
            "vic-anim does not link/import Picovoice")
    matches("/anki/lib/libpv_porcupine_softfp.so", compiled / "lib/libpv_porcupine_softfp.so")
    for name in ("porcupine_params.pv", "hey_vector.ppn"):
        remote = "/anki/data/assets/cozmo_resources/assets/picovoice/" + name
        matches(remote, compiled / remote[len("/anki/"):])
    sherpa_libs = {name for name in names("/anki/lib") if "sherpa" in name or "onnxruntime" in name}
    if args.backend == "picovoice":
        require(not sherpa_libs, "Sherpa/ONNX runtime left in Picovoice OTA")
        require("sherpa" not in dynamic.lower() and "onnxruntime" not in dynamic.lower()
                and "SherpaOnnx" not in symbols, "Sherpa runtime remains linked/imported")
        absent(assets)
        absent("/anki/data/licenses/sherpaKws")
        for directory in ("/anki/lib", "/anki/data",
                          "/anki/data/assets/cozmo_resources/assets"):
            require(not any("sherpa" in name.lower() or "onnx" in name.lower() for name in names(directory)),
                    "Stale sherpa payload in " + directory)
        print("Picovoice linked; no sherpa/ONNX dependencies, models or notices PASS")
    else:
        model = prepared / "sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01"
        model_names = [part + "-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
                       for part in ("encoder", "decoder", "joiner")]
        require(names(assets) == set(model_names + ["tokens.txt", "keywords.txt"]),
                "KWS assets contain missing or unselected files")
        for name in model_names + ["tokens.txt"]:
            matches(assets + "/" + name, model / name)
        keywords = matches(assets + "/keywords.txt", Path(__file__).with_name("keywords.txt"))
        require(keywords.read_text().strip() == "n ǐ h ǎo x iǎo w éi @你好小维", "Wrong wake phrase")
        tokens = {line.rsplit(maxsplit=1)[0] for line in extract(assets + "/tokens.txt").read_text().splitlines()}
        require(set("n ǐ h ǎo x iǎo w éi".split()) <= tokens, "Keyword tokens absent from vocabulary")
        libraries = {"libsherpa-onnx-c-api.so", "libonnxruntime.so.1.17.1"}
        require(sherpa_libs == libraries, "Unexpected sherpa/ONNX libraries")
        for name in sorted(libraries):
            library = matches("/anki/lib/" + name, prepared / "target/lib" / name)
            if not recorded:
                require(digest(library) == digest(compiled / "lib" / name), "Stale compiled runtime copy")
            abi = output("readelf", "-h", "-A", str(library))
            require(re.search(r"Class:\s+ELF32", abi) and re.search(r"Machine:\s+ARM", abi)
                    and "soft-float ABI" in abi and "Tag_ABI_VFP_args: VFP registers" not in abi,
                    "Non-ARM-softfp runtime in OTA")
            lib_dynamic = output("readelf", "-d", str(library))
            require("$ORIGIN" in lib_dynamic, "Runtime cannot resolve colocated dependencies")
            require("libstdc++.so" not in lib_dynamic, "Unexpected libstdc++ ABI dependency")
        require("libsherpa-onnx-c-api.so" in dynamic, "vic-anim is not linked to sherpa KWS")
        require("SherpaOnnxCreateKeywordSpotter" in symbols, "vic-anim does not import the KWS API")
        for marker in (b"ANKI_KWS_BACKEND", b"sherpa_onnx", b"picovoice", b"sherpaKws"):
            require(marker in binary.read_bytes(), "Missing recognizer marker: " + repr(marker))
        license_names = {
            "SHERPA_ONNX_LICENSE", "ONNXRUNTIME_LICENSE",
            "ONNXRUNTIME_THIRD_PARTY_NOTICES", "SHERPA_ONNX_THIRD_PARTY_NOTICES"
        }
        require(names("/anki/data/licenses/sherpaKws") == license_names, "Missing runtime license notices")
        for name in sorted(license_names):
            matches("/anki/data/licenses/sherpaKws/" + name, prepared / "ota-licenses" / name)
    if not recorded:
        with inputs_path.open("x") as destination:
            json.dump(inputs, destination, indent=2, sort_keys=True)
            destination.write("\n")
    print("PASS: encrypted OTA backend={}; AEC environment and service unchanged".format(args.backend))
    print("OTA", ota, ota.stat().st_size, digest(ota))


if __name__ == "__main__":
    main()
