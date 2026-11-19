#!/usr/bin/env python
"""Normalize SRT cue times to the source video's frame grid.

The displayed frame of a cue is ceil(start_ms * fps / 1000).  This script
rewrites each time to the canonical millisecond value for that same frame,
so later comparisons do not depend on arbitrary 1-2 ms rounding.  Cue text,
numbering, line endings and BOM are preserved.  Originals are copied to a
backup directory before any file is replaced.

SRT end times are exclusive: the end is the first instant at which the next
cue may be shown.  For a cue whose last displayed frame is N, the end time is
the canonical time of frame N+1.  The change report therefore exposes both
``end_boundary_frame`` (N+1) and ``last_visible_frame`` (N).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

TIME_LINE_RE = re.compile(
    r"(?m)^(?P<start>\d+:\d{2}:\d{2}[,.]\d{1,3})"
    r"\s*-->\s*"
    r"(?P<end>\d+:\d{2}:\d{2}[,.]\d{1,3})"
    r"(?P<tail>[^\r\n]*)"
)
VIDEO_EXTS = (".mp4", ".mkv", ".webm", ".mov", ".avi", ".m2ts", ".ts", ".wmv", ".flv")
VIDEO_PREFERENCE = ("生肉.mp4", "熟肉.mp4")
EPS = 1e-6


def parse_time(value: str) -> int:
    hours, minutes, rest = value.split(":")
    seconds, millis = re.split(r"[,.]", rest)
    millis = (millis + "000")[:3]
    return ((int(hours) * 60 + int(minutes)) * 60 + int(seconds)) * 1000 + int(millis)


def format_time(value: int) -> str:
    hours, rest = divmod(value, 3_600_000)
    minutes, rest = divmod(rest, 60_000)
    seconds, millis = divmod(rest, 1000)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d},{millis:03d}"


def frame_at(ms: int, fps: float) -> int:
    return int(math.ceil(ms * fps / 1000.0 - EPS))


def canonical_ms(frame: int, fps: float) -> int:
    return int(math.floor(frame * 1000.0 / fps + EPS))


def canonical_time(ms: int, fps: float) -> tuple[int, int]:
    frame = frame_at(ms, fps)
    value = canonical_ms(frame, fps)
    while frame_at(value, fps) < frame:
        value += 1
    while frame_at(value, fps) > frame:
        value -= 1
    return value, frame


def snap_fps(fps: float) -> float:
    for nominal, exact in (
        (23.976, 24_000 / 1001),
        (29.97, 30_000 / 1001),
        (59.94, 60_000 / 1001),
    ):
        if abs(fps - nominal) < 0.01:
            return exact
    return fps


def detect_fps(video: Path, ffmpeg: Path) -> float | None:
    if not video.is_file():
        return None
    try:
        proc = subprocess.run(
            [str(ffmpeg), "-hide_banner", "-i", str(video)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError):
        return None
    match = re.search(r"(\d+(?:\.\d+)?)\s*fps", proc.stderr)
    if not match:
        return None
    try:
        return snap_fps(float(match.group(1)))
    except ValueError:
        return None


def find_video(srt: Path) -> Path | None:
    folder = srt.parent
    for name in VIDEO_PREFERENCE:
        candidate = folder / name
        if candidate.is_file():
            return candidate
    candidates = sorted(
        (p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in VIDEO_EXTS),
        key=lambda p: p.name,
    )
    return candidates[0] if candidates else None


def read_text(path: Path) -> tuple[str, bytes]:
    raw = path.read_bytes()
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    text = raw.decode("utf-8-sig" if bom else "utf-8")
    return text, bom


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def strip_time_lines(text: str) -> str:
    return TIME_LINE_RE.sub("", text)


def rewrite(text: str, fps: float) -> tuple[str, list[dict]]:
    changes: list[dict] = []
    counter = {"cue": 0}

    def replace(match: re.Match) -> str:
        counter["cue"] += 1
        cue = counter["cue"]
        old_start = parse_time(match.group("start"))
        old_end = parse_time(match.group("end"))
        new_start, start_frame = canonical_time(old_start, fps)
        new_end, end_frame = canonical_time(old_end, fps)
        if end_frame <= start_frame:
            end_frame = start_frame + 1
            new_end = canonical_ms(end_frame, fps)
            while new_end <= new_start:
                new_end += 1
        if (new_start, new_end) != (old_start, old_end):
            changes.append(
                {
                    "cue": cue,
                    "old_start_ms": old_start,
                    "new_start_ms": new_start,
                    "old_end_ms": old_end,
                    "new_end_ms": new_end,
                    "start_frame": start_frame,
                    "end_boundary_frame": end_frame,
                    "last_visible_frame": end_frame - 1,
                    "end_frame": end_frame,  # legacy alias for the boundary
                    "delta_start_ms": new_start - old_start,
                    "delta_end_ms": new_end - old_end,
                }
            )
        return f"{format_time(new_start)} --> {format_time(new_end)}{match.group('tail')}"

    return TIME_LINE_RE.sub(replace, text), changes


def count_cues(text: str) -> int:
    return len(TIME_LINE_RE.findall(text))


def verify(original: str, updated: str, fps: float) -> str | None:
    old_times = [
        (parse_time(m.group("start")), parse_time(m.group("end")))
        for m in TIME_LINE_RE.finditer(original)
    ]
    new_times = [
        (parse_time(m.group("start")), parse_time(m.group("end")))
        for m in TIME_LINE_RE.finditer(updated)
    ]
    if len(old_times) != len(new_times):
        return f"cue count changed: {len(old_times)} -> {len(new_times)}"
    if strip_time_lines(original) != strip_time_lines(updated):
        return "cue text or layout changed"
    for index, ((old_start, old_end), (new_start, new_end)) in enumerate(
        zip(old_times, new_times), 1
    ):
        if frame_at(old_start, fps) != frame_at(new_start, fps):
            return f"cue {index} start frame changed"
        if frame_at(old_end, fps) != frame_at(new_end, fps):
            return f"cue {index} end frame changed"
        if new_end <= new_start:
            return f"cue {index} has no duration"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=r"E:\Fairy-Kekkai-Workshop-Projects\sheshe")
    parser.add_argument("--apply", action="store_true", help="write changes; default is dry-run")
    parser.add_argument("--fps", type=float, default=None, help="override frame rate for every file")
    parser.add_argument("--video", default=None, help="force this video for fps detection")
    parser.add_argument("--ffmpeg", default=None)
    parser.add_argument("--work", default=r"E:\Fairy Kekkai Workshop\temp\frame_fix_runs")
    parser.add_argument("--no-backup", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if root.is_file():
        files = [root]
        base = root.parent
    elif root.is_dir():
        files = sorted(p for p in root.rglob("*.srt") if p.is_file())
        base = root
    else:
        print(f"root not found: {root}")
        return 2
    video_override = Path(args.video).resolve() if args.video else None
    ffmpeg = Path(args.ffmpeg) if args.ffmpeg else Path(__file__).resolve().parent / "ffmpeg.exe"
    if not ffmpeg.is_file():
        found = shutil.which("ffmpeg")
        if found:
            ffmpeg = Path(found)
    work = Path(args.work)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = work / stamp
    backup_dir = run_dir / "backup"
    run_dir.mkdir(parents=True, exist_ok=True)

    records: list[dict] = []
    failures = 0
    total_cues = 0
    total_changed_cues = 0
    total_changed_times = 0

    for srt in files:
        relative = srt.relative_to(base).as_posix()
        video = video_override if video_override is not None else find_video(srt)
        fps = args.fps
        fps_source = "argument"
        if fps is None and video is not None:
            fps = detect_fps(video, ffmpeg)
            fps_source = "ffmpeg"
        if fps is None:
            fps = 60.0
            fps_source = "default"
        try:
            original, bom = read_text(srt)
        except (OSError, UnicodeDecodeError) as exc:
            records.append({"path": relative, "status": "read_error", "error": str(exc)})
            failures += 1
            continue
        cues = count_cues(original)
        if cues == 0:
            records.append({"path": relative, "status": "no_cues", "fps": fps})
            failures += 1
            continue

        updated, changes = rewrite(original, fps)
        problem = verify(original, updated, fps)
        record = {
            "path": relative,
            "status": "ok",
            "fps": fps,
            "fps_source": fps_source,
            "video": (
                str(video)
                if video_override is not None
                else (video.relative_to(base).as_posix() if video else None)
            ),
            "cues": cues,
            "changed_cues": len(changes),
            "changed_times": len(changes) * 2,
            "max_delta_ms": max(
                (abs(c["delta_start_ms"]) for c in changes),
                default=0,
            )
            if changes
            else 0,
            "sha256_before": sha256(srt),
            "examples": changes[:5],
        }
        total_cues += cues
        total_changed_cues += len(changes)
        total_changed_times += len(changes) * 2

        if problem:
            record["status"] = "verification_failed"
            record["error"] = problem
            failures += 1
            records.append(record)
            continue

        if args.apply and changes:
            if not args.no_backup:
                target = backup_dir / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(srt, target)
                record["backup"] = str(target)
            encoded = (bom + updated.encode("utf-8")) if bom else updated.encode("utf-8")
            temporary = srt.with_name(srt.name + ".frame_fix.tmp")
            try:
                temporary.write_bytes(encoded)
                os.replace(temporary, srt)
                after, _ = read_text(srt)
                problem = verify(original, after, fps)
                if problem:
                    raise RuntimeError(problem)
                record["sha256_after"] = sha256(srt)
            except Exception as exc:
                if temporary.exists():
                    temporary.unlink(missing_ok=True)
                backup = backup_dir / relative
                if backup.is_file():
                    shutil.copy2(backup, srt)
                record["status"] = "write_failed"
                record["error"] = str(exc)
                failures += 1
        records.append(record)

    summary = {
        "root": str(root),
        "mode": "apply" if args.apply else "dry_run",
        "generated": datetime.now().isoformat(timespec="seconds"),
        "files": len(files),
        "cues": total_cues,
        "changed_cues": total_changed_cues,
        "changed_times": total_changed_times,
        "failures": failures,
        "backup_dir": str(backup_dir) if args.apply and not args.no_backup else None,
        "records": records,
    }
    (run_dir / "report.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    lines = [
        f"root: {root}",
        f"mode: {summary['mode']}",
        f"files: {len(files)}",
        f"cues: {total_cues}",
        f"changed_cues: {total_changed_cues}",
        f"changed_times: {total_changed_times}",
        f"failures: {failures}",
    ]
    for record in records:
        if record.get("changed_cues"):
            lines.append(
                f"{record['path']}: fps={record['fps']} cues={record['cues']} "
                f"changed={record['changed_cues']} max_delta={record['max_delta_ms']}ms"
            )
        elif record.get("status") != "ok":
            lines.append(f"{record['path']}: {record['status']} {record.get('error', '')}")
    (run_dir / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    print(f"report: {run_dir / 'report.json'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())