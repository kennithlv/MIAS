#!/usr/bin/env python3
"""Decode a MIAS session and compare actual video frames with CSV/manifest counts.

This verifies saved-file consistency only. It cannot prove that every exposure
reached the host, that no USB data were lost, or that cameras exposed in sync.
"""

import argparse
import csv
import json
import sys
from pathlib import Path


def verify_session(session):
    try:
        import cv2
    except ImportError as exc:
        raise RuntimeError("Python OpenCV is required; on Ubuntu install python3-opencv") from exc
    session = Path(session).resolve()
    manifest = json.loads((session / "manifest.json").read_text(encoding="utf-8"))
    result = {"session": str(session), "ok": False, "saved_files_consistent": False,
              "cameras": [], "errors": [], "warnings": []}
    result["warnings"].extend(str(item) for item in manifest.get("integrity_warnings", []))
    errors = result["errors"]
    if manifest.get("schema_version") != 2:
        errors.append("Expected MIAS schema_version 2")
    if manifest.get("status") not in ("completed", "interrupted"):
        errors.append(f"Session did not finish cleanly: status={manifest.get('status')!r}")
    if not manifest.get("config", {}).get("record", False):
        errors.append("This was an acquisition-only session: no recorded videos to verify")
    cameras = manifest.get("cameras", [])
    if not cameras:
        errors.append("Manifest has no cameras")
    names = set()
    for camera in cameras:
        name = camera.get("name", "")
        if not isinstance(name, str) or not name or Path(name).name != name or name in (".", ".."):
            errors.append(f"Unsafe or missing camera directory name: {name!r}")
            continue
        if name in names:
            errors.append(f"Duplicate camera name: {name}")
            continue
        names.add(name)
        report = {"name": name, "decoded_frames": 0, "csv_frames": 0, "segments": []}
        result["cameras"].append(report)
        directory = session / name
        try:
            with (directory / "frames.csv").open(newline="", encoding="utf-8") as handle:
                reader = csv.DictReader(handle)
                required = {"video_file", "video_frame_index", "capture_index", "frame_id",
                            "frame_id_valid", "host_monotonic_ns"}
                if not required.issubset(reader.fieldnames or []):
                    raise ValueError("CSV is missing required columns")
                rows = list(reader)
            report["csv_frames"] = len(rows)
            if not rows:
                errors.append(f"{name}: no frame metadata")
            if len(rows) != camera.get("frames_submitted"):
                errors.append(f"{name}: CSV count {len(rows)} differs from manifest frames_submitted {camera.get('frames_submitted')}")
            if camera.get("captured") != camera.get("frames_submitted"):
                errors.append(f"{name}: captured and submitted counts differ")
            if camera.get("error"):
                errors.append(f"{name}: acquisition reported {camera['error']}")
            if camera.get("frame_id_gaps", 0):
                result["warnings"].append(f"{name}: acquisition observed {camera['frame_id_gaps']} missing frame IDs; file checks do not undo these losses")
            if camera.get("frame_id_discontinuities", 0):
                result["warnings"].append(f"{name}: acquisition observed {camera['frame_id_discontinuities']} duplicate/reset/wrapped frame IDs")

            by_file = {}
            previous_time = None
            previous_file = None
            previous_id = None
            closed_files = set()
            for index, row in enumerate(rows):
                if int(row["capture_index"]) != index:
                    raise ValueError(f"capture_index is not contiguous at CSV row {index + 2}")
                timestamp = int(row["host_monotonic_ns"])
                if previous_time is not None and timestamp < previous_time:
                    raise ValueError(f"host timestamp went backward at CSV row {index + 2}")
                previous_time = timestamp
                if row["frame_id_valid"] not in ("0", "1", "false", "true"):
                    raise ValueError(f"invalid frame_id_valid at CSV row {index + 2}")
                if row["frame_id_valid"] in ("1", "true"):
                    frame_id = int(row["frame_id"])
                    if previous_id is not None and frame_id != previous_id + 1:
                        result["warnings"].append(f"{name}: CSV frame IDs are not consecutive at row {index + 2}: {previous_id} -> {frame_id}")
                    previous_id = frame_id
                else:
                    previous_id = None
                filename = row["video_file"]
                if not filename or Path(filename).name != filename or filename in (".", ".."):
                    raise ValueError(f"unsafe video filename: {filename!r}")
                if previous_file is not None and filename != previous_file:
                    closed_files.add(previous_file)
                if filename in closed_files:
                    raise ValueError(f"CSV returns to an already-closed segment: {filename}")
                previous_file = filename
                segment_rows = by_file.setdefault(filename, [])
                if int(row["video_frame_index"]) != len(segment_rows):
                    raise ValueError(f"video_frame_index is not contiguous in {filename}")
                segment_rows.append(row)

            listed_segments = camera.get("segments", [])
            manifest_segments = {item["file"]: item for item in listed_segments}
            if len(manifest_segments) != len(listed_segments):
                errors.append(f"{name}: duplicate manifest segment names")
            if set(manifest_segments) != set(by_file):
                errors.append(f"{name}: manifest and CSV segment lists differ")
            actual_files = {path.name for suffix in ("*.avi", "*.mkv") for path in directory.glob(suffix)}
            if actual_files != set(by_file):
                errors.append(f"{name}: video files and CSV segment lists differ")
            for filename, segment_rows in by_file.items():
                path = directory / filename
                if not path.is_file():
                    errors.append(f"{name}: missing video {filename}")
                    continue
                video = cv2.VideoCapture(str(path))
                if not video.isOpened():
                    errors.append(f"{name}: cannot decode {filename}")
                    video.release()
                    continue
                count = 0
                dimensions = None
                try:
                    # Do not trust CAP_PROP_FRAME_COUNT: decode until EOF.
                    while True:
                        ok, image = video.read()
                        if not ok:
                            break
                        shape = [int(image.shape[1]), int(image.shape[0])]
                        if dimensions is None:
                            dimensions = shape
                        elif shape != dimensions:
                            errors.append(f"{name}/{filename}: dimensions changed during decoding")
                        count += 1
                finally:
                    video.release()
                report["decoded_frames"] += count
                report["segments"].append({"file": filename, "decoded_frames": count, "dimensions": dimensions})
                device = camera.get("device_info", {})
                if dimensions and device.get("width") and device.get("height"):
                    if dimensions != [device["width"], device["height"]]:
                        errors.append(f"{name}/{filename}: decoded dimensions differ from device metadata")
                if count != len(segment_rows):
                    errors.append(f"{name}/{filename}: decoded {count} frames but CSV has {len(segment_rows)}")
                listed = manifest_segments.get(filename, {})
                if listed.get("frames_submitted") != count:
                    errors.append(f"{name}/{filename}: decoded count differs from manifest")
                if listed.get("bytes") != path.stat().st_size:
                    errors.append(f"{name}/{filename}: file size differs from manifest")
        except (OSError, ValueError, KeyError, TypeError) as exc:
            errors.append(f"{name}: {exc}")
    result["saved_files_consistent"] = not errors
    result["ok"] = not errors and not result["warnings"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path, help="A session-... directory containing manifest.json")
    parser.add_argument("--json", action="store_true", help="Print machine-readable results")
    args = parser.parse_args()
    try:
        result = verify_session(args.session)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as exc:
        print(f"Verification could not run: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for camera in result["cameras"]:
            print(f"{camera['name']}: decoded {camera['decoded_frames']} frames in {len(camera['segments'])} segments; CSV has {camera['csv_frames']} rows")
        for warning in result["warnings"]:
            print("WARNING: " + warning)
        for error in result["errors"]:
            print("ERROR: " + error)
        print("PASS: saved video/metadata counts agree, no recorded integrity warnings" if result["ok"]
              else "FAIL: recording consistency checks or acquisition integrity warnings require review")
        print("This does not verify USB completeness or exposure synchronization.")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
