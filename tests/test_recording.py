#!/usr/bin/env python3
"""End-to-end checks using generated images and actual encoded/decoded video.

This does not emulate vendor SDKs and cannot establish real-camera compatibility
or USB throughput. All outputs live in a temporary directory.
"""

import argparse
import csv
import json
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import cv2
import numpy as np


def camera(index, pixel_format="Mono8"):
    return {
        "backend": "simulated",
        "serial": f"sim-{index:02d}",
        "name": f"cam{index:02d}",
        "pixel_format": pixel_format,
        "width": 160,
        "height": 120,
        "fps": 30.0,
        "trigger": "off",
    }


def config(output, count=5, frame_limit=20):
    return {
        "schema_version": 2,
        "output_dir": str(output),
        "record": True,
        "preview": False,
        "codec": "FFV1",
        "container": "avi",
        "segment_seconds": 0.2,
        "queue_frames": 32,
        "queue_megabytes": 16,
        "duration_seconds": 0,
        "frame_limit": frame_limit,
        "no_frame_timeout_seconds": 5,
        "cameras": [camera(i) for i in range(1, count + 1)],
    }


def invoke(executable, root, label, settings, expect_success=True, extra=()):
    path = root / f"{label}.json"
    path.write_text(json.dumps(settings), encoding="utf-8")
    result = subprocess.run(
        [str(executable), "--config", str(path), *extra],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=30,
    )
    if expect_success and result.returncode != 0:
        raise AssertionError(f"{label}: exit {result.returncode}\n{result.stdout}")
    if not expect_success and result.returncode == 0:
        raise AssertionError(f"{label}: unexpectedly accepted invalid input\n{result.stdout}")
    return result


def get_session(output):
    manifests = list(output.glob("session-*/manifest.json"))
    assert len(manifests) == 1, f"Expected one session under {output}: {manifests}"
    return manifests[0].parent, json.loads(manifests[0].read_text(encoding="utf-8"))


def check_recording(output, expected_cameras, expected_frames=None, status="completed", segments=True):
    session, manifest = get_session(output)
    verify = subprocess.run(
        [sys.executable, str(Path(__file__).resolve().parents[1] / "tools" / "verify_recording.py"),
         str(session), "--json"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
    )
    assert verify.returncode == 0, (verify.stdout, verify.stderr)
    assert json.loads(verify.stdout)["ok"]
    assert manifest["status"] == status, manifest
    cameras = manifest["cameras"]
    assert len(cameras) == expected_cameras, cameras
    for entry in cameras:
        directory = session / entry["name"]
        with (directory / "frames.csv").open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        assert rows, f"No frame metadata in {directory}"
        if expected_frames is not None:
            assert len(rows) == expected_frames, (entry["name"], len(rows), expected_frames)
        assert len(rows) == entry["frames_submitted"], entry
        assert entry["captured"] == entry["frames_submitted"], entry
        assert entry["frame_id_gaps"] == 0, entry
        assert [int(row["capture_index"]) for row in rows] == list(range(len(rows)))
        assert [int(row["frame_id"]) for row in rows] == list(range(1, len(rows) + 1))
        assert all(row["frame_id_valid"] in ("1", "true") for row in rows)
        timestamps = [int(row["host_monotonic_ns"]) for row in rows]
        assert all(a < b for a, b in zip(timestamps, timestamps[1:])), timestamps
        assert all(int(row["host_unix_ns"]) > 1_000_000_000_000_000_000 for row in rows)

        by_file = {}
        for row in rows:
            by_file.setdefault(row["video_file"], []).append(row)
        if segments:
            assert len(by_file) >= 2, f"Segment rotation did not occur: {directory}"
        videos = sorted([*directory.glob("*.avi"), *directory.glob("*.mkv")])
        assert {path.name for path in videos} == set(by_file), (videos, by_file.keys())
        decoded = 0
        first = None
        changed = False
        for path in videos:
            video = cv2.VideoCapture(str(path))
            assert video.isOpened(), f"Cannot open recorded video: {path}"
            index = 0
            while True:
                ok, image = video.read()
                if not ok:
                    break
                assert image.shape[:2] == (120, 160), image.shape
                if first is None:
                    first = image.copy()
                elif not np.array_equal(first, image):
                    changed = True
                index += 1
            video.release()
            expected = by_file[path.name]
            assert index == len(expected), (path, index, len(expected))
            assert [int(row["video_frame_index"]) for row in expected] == list(range(index))
            decoded += index
        assert decoded == len(rows), (decoded, len(rows))
        assert changed, f"Recorded images did not change: {directory}"
    return manifest


def run_tests(executable):
    with tempfile.TemporaryDirectory(prefix="mias-recording-test-") as temp:
        root = Path(temp)
        output = root / "five-cameras"
        settings = config(output)
        invoke(executable, root, "five", settings)
        check_recording(output, 5, 20)
        print("PASS: five cameras, segmented videos, frame metadata and decoder counts")

        # Make a real inconsistency to verify that the user-facing checker fails.
        session, _ = get_session(output)
        first_video = next(session.glob("*/*.avi"))
        missing_video = first_video.with_suffix(".hidden")
        first_video.rename(missing_video)
        try:
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve().parents[1] / "tools" / "verify_recording.py"), str(session)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            assert result.returncode == 1, result.stdout
            assert "missing video" in result.stdout, result.stdout
        finally:
            missing_video.rename(first_video)
        print("PASS: verification tool detects a missing segment")

        manifest_path = session / "manifest.json"
        original_manifest = manifest_path.read_text(encoding="utf-8")
        modified_manifest = json.loads(original_manifest)
        modified_manifest["integrity_warnings"] = ["Injected frame-ID warning for verifier regression test"]
        manifest_path.write_text(json.dumps(modified_manifest), encoding="utf-8")
        try:
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve().parents[1] / "tools" / "verify_recording.py"), str(session), "--json"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
            )
            report = json.loads(result.stdout)
            assert result.returncode == 1 and not report["ok"] and report["saved_files_consistent"], result.stdout
        finally:
            manifest_path.write_text(original_manifest, encoding="utf-8")
        print("PASS: verification tool treats acquisition integrity warnings as a nonzero result")

        output = root / "color"
        settings = config(output, count=1)
        settings["cameras"][0]["pixel_format"] = "BGR8"
        invoke(executable, root, "color", settings)
        check_recording(output, 1, 20)
        print("PASS: color video recording")

        for label, codec, container, pixel_format in (
            ("ffv1-mkv", "FFV1", "mkv", "Mono8"),
            ("mjpg-mono", "MJPG", "avi", "Mono8"),
            ("mjpg-color", "MJPG", "avi", "BGR8"),
        ):
            output = root / label
            settings = config(output, count=1, frame_limit=12)
            settings["codec"] = codec
            settings["container"] = container
            settings["cameras"][0]["pixel_format"] = pixel_format
            invoke(executable, root, label, settings)
            check_recording(output, 1, 12)
            print(f"PASS: {codec}/{container}/{pixel_format} encoded video and metadata")

        settings = config(root / "bayer-lossy", count=1, frame_limit=4)
        settings["codec"] = "MJPG"
        settings["cameras"][0]["pixel_format"] = "BayerGR8"
        invoke(executable, root, "bayer-lossy", settings, expect_success=False)
        print("PASS: lossy raw-Bayer recording rejected")

        output = root / "no-record"
        settings = config(output, count=2, frame_limit=12)
        invoke(executable, root, "no-record", settings, extra=("--no-record",))
        _, manifest = get_session(output)
        assert manifest["status"] == "completed", manifest
        assert all(cam["captured"] == 12 for cam in manifest["cameras"]), manifest
        assert not list(output.rglob("*.avi")), "--no-record wrote video"
        print("PASS: acquisition-only mode")

        output = root / "bad-codec"
        settings = config(output, count=1, frame_limit=4)
        settings["codec"] = "ZZZZ"
        invoke(executable, root, "bad-codec", settings, expect_success=False)
        print("PASS: unsupported video encoder rejected")

        settings = config(root / "duplicates", count=2)
        settings["cameras"][1]["name"] = settings["cameras"][0]["name"]
        invoke(executable, root, "duplicate-name", settings, expect_success=False)
        print("PASS: duplicate camera output names rejected")

        output_file = root / "existing-file"
        output_file.write_text("do not overwrite", encoding="utf-8")
        invoke(executable, root, "invalid-output", config(output_file, count=1), expect_success=False)
        assert output_file.read_text(encoding="utf-8") == "do not overwrite"
        print("PASS: invalid destination rejected without overwriting existing content")

        output = root / "interrupted"
        settings = config(output, count=2, frame_limit=0)
        path = root / "interrupted.json"
        path.write_text(json.dumps(settings), encoding="utf-8")
        process = subprocess.Popen(
            [str(executable), "--config", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while not list(output.glob("session-*/*/frames.csv")):
                if process.poll() is not None:
                    raise AssertionError(f"Capture exited before signal:\n{process.communicate()[0]}")
                if time.monotonic() > deadline:
                    raise AssertionError("Timed out waiting for acquisition startup")
                time.sleep(0.05)
            time.sleep(0.5)
            process.send_signal(signal.SIGINT)
            stdout, _ = process.communicate(timeout=15)
            assert process.returncode in (0, 130), (process.returncode, stdout)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        check_recording(output, 2, status="interrupted", segments=False)
        print("PASS: interrupt closes playable files and drains accepted frames")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    args = parser.parse_args()
    run_tests(args.executable.resolve())
