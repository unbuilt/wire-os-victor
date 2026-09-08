#!/usr/bin/env python3
"""Schedule the existing robot WAV capture and repeat one immutable SDK playback fixture."""

import argparse
import hashlib
import json
import re
import sys
import time
import urllib.parse
import urllib.request
import wave


def diagnostic_request(robot_ip, function, arguments=""):
    request = urllib.request.Request(
        "http://" + robot_ip + ":8889/consolefunccall",
        data=urllib.parse.urlencode({"func": function, "args": arguments}).encode("ascii"))
    with urllib.request.urlopen(request, timeout=10) as response:
        body = response.read().decode("utf-8")
    if "ERROR:" in body or "AEC_DIAGNOSTIC " not in body:
        raise RuntimeError("Diagnostic capability/request rejected: " + body)
    try:
        status, _ = json.JSONDecoder().raw_decode(body.split("AEC_DIAGNOSTIC ", 1)[1])
    except (ValueError, TypeError):
        raise RuntimeError("Invalid diagnostic status")
    version = re.fullmatch(r"3\.0\.1\.(\d+)d", status.get("os_version", ""))
    if (status.get("schema") != 1 or status.get("revision") != 110 or
            status.get("mode") != 1 or not version or int(version.group(1)) < 111):
        raise RuntimeError("Diagnostic requires compatible OTA111+ and REFERENCE mode; OTA110 has a corrupting WAV writer")
    return status


def wait_for_diagnostic(robot_ip, trial):
    for _ in range(100):
        status = diagnostic_request(robot_ip, "AecDiagnosticStatus")
        if status.get("trial") != trial:
            raise RuntimeError("Diagnostic trial changed; cannot associate artifacts")
        if status.get("state") == "failed":
            raise RuntimeError("Diagnostic artifact write failed: " + status.get("message", ""))
        if status.get("state") == "invalid":
            raise RuntimeError("Diagnostic artifact set is complete but unusable: " + str(status))
        if status.get("state") == "saved":
            if status.get("write_complete") is not True:
                raise RuntimeError("Diagnostic writer completion marker is absent")
            if status.get("errors") != 0:
                raise RuntimeError("Diagnostic artifacts have integrity errors: " + str(status))
            print("Firmware confirms complete diagnostic artifact set: " + status["path"] +
                  ". Retrieve all files and verify manifest offline. NOT an AEC comparison.", flush=True)
            return
        time.sleep(0.2)
    raise RuntimeError("Diagnostic writer completion was not confirmed; scheduling is not saved evidence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robot-ip", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--wav", required=True, help="Mono PCM16, 16 kHz; comparison 1..3 s, diagnostic 1..12 s (played once)")
    parser.add_argument("--condition", required=True, choices=("speaker", "near", "double"))
    parser.add_argument("--volume", type=int, default=50)
    parser.add_argument("--speech-prompt", default="Please count one two three four five.",
                        help="Human speech cue for near/double conditions; never played by the robot")
    parser.add_argument("--diagnostic", action="store_true",
                        help="OTA111+ reference-only PCM/clock diagnostics; timing-invalid allowed, NEVER an AEC comparison")
    parser.add_argument("--token-stdin", action="store_true",
                        help="Read a temporary SDK token from stdin; never save or print it")
    args = parser.parse_args()
    config = {}
    if args.token_stdin:
        token = sys.stdin.read().strip()
        if not token:
            parser.error("--token-stdin requires a nonempty token")
        config["guid"] = token
    if not 0 <= args.volume <= 100:
        parser.error("--volume must be 0..100")
    with wave.open(args.wav, "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getcomptype()) != (1, 2, 16000, "NONE"):
            parser.error("fixture must be uncompressed mono PCM16 at 16000 Hz")
        duration = source.getnframes() / source.getframerate()
        maximum = 12 if args.diagnostic else 3
        if not 1 <= duration <= maximum:
            parser.error("fixture must be 1..{} seconds for this capture mode".format(maximum))
    with open(args.wav, "rb") as source:
        print("fixture_sha256=" + hashlib.sha256(source.read()).hexdigest(), flush=True)
    if args.diagnostic:
        status = diagnostic_request(args.robot_ip, "AecDiagnosticStatus")
        if status.get("state") in ("active", "writing"):
            raise RuntimeError("Diagnostic capture already active")

    # Import only after validating arguments. Use the user's already-configured SDK;
    # never install dependencies, change robot configuration, or hide playback errors.
    import anki_vector

    # Audio-only trials do not need the SDK's animation-list startup RPCs.
    with anki_vector.Robot(serial=args.serial, ip=args.robot_ip, config=config,
                           cache_animation_lists=False) as robot:
        if args.diagnostic:
            status = diagnostic_request(args.robot_ip, "AecDiagnosticCapture", "15")
            if status.get("state") != "active" or not status.get("trial"):
                raise RuntimeError("Diagnostic capture was not scheduled")
            trial = status["trial"]
            print("Diagnostic trial " + trial + " scheduled; NOT saved yet. Cancellation/adaptation OFF.", flush=True)
        else:
            request = urllib.request.Request(
                "http://" + args.robot_ip + ":8889/consolefunccall",
                data=urllib.parse.urlencode({"func": "AecExperimentCapture", "args": "15"}).encode("ascii"),
            )
            with urllib.request.urlopen(request, timeout=10) as response:
                body = response.read().decode("utf-8")
            if "AEC capture scheduled:" not in body or "ERROR:" in body:
                raise RuntimeError("Robot did not accept capture: " + body)
            print(body, flush=True)
        start = time.monotonic()
        time.sleep(1)
        # One non-repeating diagnostic stimulus avoids manufacturing identical
        # correlation peaks. Historical comparisons keep their three repetitions.
        for repetition in range(1 if args.diagnostic else 3):
            print("repetition={} condition={}: {}".format(
                repetition + 1, args.condition,
                "SAY: " + args.speech_prompt if args.condition != "speaker" else "Stay silent."),
                flush=True)
            if args.condition == "near":
                time.sleep(duration)
            else:
                robot.audio.stream_wav_file(args.wav, args.volume)
            time.sleep(0.5)
        elapsed = time.monotonic() - start
        if elapsed > 14:
            raise RuntimeError("Playback schedule overran capture; discard this trial")
        time.sleep(16 - elapsed)
        if args.diagnostic:
            wait_for_diagnostic(args.robot_ip, trial)
        else:
            print("Capture interval finished; verify both saved WAVs and AEC_EXPERIMENT stats. "
                  "This message is not evidence of successful acoustic cancellation.", flush=True)


if __name__ == "__main__":
    main()
