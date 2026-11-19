"""Frame/time conversion shared by the OCR pipeline.

SRT stores an *exclusive* end: the end timestamp is the first instant at
which the next cue may be shown.  If a cue is visible on frame N and the
next cue starts on frame N+1, the end timestamp is the canonical time of
frame N+1, not of frame N.  Consumers that need the last displayed frame
must use ``last_visible_frame(end_ms, fps)``.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

EPS = 1e-6

TIME_LINE_RE = re.compile(
    r"(?m)^(?P<start>\d+:\d{2}:\d{2}[,.]\d{1,3})"
    r"\s*-->\s*"
    r"(?P<end>\d+:\d{2}:\d{2}[,.]\d{1,3})"
    r"(?P<tail>[^\r\n]*)"
)


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


def format_ass_time(centiseconds: int) -> str:
    hours, rest = divmod(max(0, centiseconds), 360_000)
    minutes, rest = divmod(rest, 6_000)
    seconds, cs = divmod(rest, 100)
    return f"{hours}:{minutes:02d}:{seconds:02d}.{cs:02d}"


def frame_at(ms: int, fps: float) -> int:
    """First frame whose presentation starts at or after ``ms``."""
    return int(math.ceil(ms * fps / 1000.0 - EPS))


def snap_fps(fps: float) -> float:
    """Map the decimal NTSC rates reported by ffmpeg to exact rationals."""
    for nominal, exact in (
        (23.976, 24_000 / 1001),
        (29.97, 30_000 / 1001),
        (59.94, 60_000 / 1001),
    ):
        if abs(fps - nominal) < 0.01:
            return exact
    return fps


def canonical_ms(frame: int, fps: float) -> int:
    """Canonical millisecond value for a frame's presentation start."""
    return int(math.floor(frame * 1000.0 / fps + EPS))


def canonical_time(ms: int, fps: float) -> tuple[int, int]:
    frame = frame_at(ms, fps)
    value = canonical_ms(frame, fps)
    while frame_at(value, fps) < frame:
        value += 1
    while frame_at(value, fps) > frame:
        value -= 1
    return value, frame


def last_visible_frame(end_ms: int, fps: float) -> int:
    """Last frame displayed by a cue whose SRT end is ``end_ms``."""
    return frame_at(end_ms, fps) - 1


def frame_cue_times_ms(
    start_frame: int,
    end_frame_exclusive: int,
    *,
    fps: float | None,
    frame_timestamps: dict[int, float] | None = None,
    start_time_offset_ms: float = 0.0,
    fallback_frame_duration_ms: float = 0.0,
    frame_base: int = 0,
) -> tuple[int, int]:
    """Serialize a frame-level cue to SRT milliseconds.

    CFR uses the frame number directly, so 59.94 fps and 60 fps never drift.
    ``frame_base`` is the absolute frame number of index 0 (non-zero when OCR
    starts from a seek point).  VFR falls back to the per-frame PTS table.
    The end is always the first instant after the cue
    (``end_frame_exclusive``), never the last frame.
    """
    if end_frame_exclusive <= start_frame:
        end_frame_exclusive = start_frame + 1

    if fps is not None:
        start_ms = canonical_ms(frame_base + start_frame, fps)
        end_ms = canonical_ms(frame_base + end_frame_exclusive, fps)
    else:
        if not frame_timestamps:
            raise ValueError("VFR timing requires frame_timestamps")
        start_raw = frame_timestamps.get(start_frame, 0.0)
        end_raw = frame_timestamps.get(end_frame_exclusive)
        if end_raw is None:
            last_raw = frame_timestamps.get(end_frame_exclusive - 1, start_raw)
            end_raw = last_raw + fallback_frame_duration_ms
        start_ms = int(start_raw - start_time_offset_ms)
        end_ms = int(end_raw - start_time_offset_ms)

    if end_ms <= start_ms:
        step = int(fallback_frame_duration_ms) if fallback_frame_duration_ms > 0 else 1
        end_ms = start_ms + max(1, step)
    return start_ms, end_ms


def frame_cue_times_cs(
    start_frame: int,
    end_frame_exclusive: int,
    *,
    fps: float | None,
    frame_timestamps: dict[int, float] | None = None,
    start_time_offset_ms: float = 0.0,
    fallback_frame_duration_ms: float = 0.0,
    frame_base: int = 0,
) -> tuple[int, int]:
    """Serialize a frame-level cue to ASS centiseconds.

    ASS stores 10 ms ticks, and Aegisub maps a line's START/END back to frames
    with a one-millisecond bias.  Flooring both boundaries keeps that mapping
    identical to the intended frame: no occasional +1 at 59.94/60 fps.
    """
    start_ms, end_ms = frame_cue_times_ms(
        start_frame,
        end_frame_exclusive,
        fps=fps,
        frame_timestamps=frame_timestamps,
        start_time_offset_ms=start_time_offset_ms,
        fallback_frame_duration_ms=fallback_frame_duration_ms,
        frame_base=frame_base,
    )
    start_cs = start_ms // 10
    end_cs = end_ms // 10
    if end_cs <= start_cs:
        end_cs = start_cs + 1
    return start_cs, end_cs


def canonicalize_srt(text: str, fps: float) -> tuple[str, list[dict]]:
    """Snap every cue time to the frame grid without changing its frame.

    The end remains exclusive: it is still written as the first frame after
    the cue.  Text, numbering, line endings and a leading BOM are preserved.
    """
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
                }
            )
        return f"{format_time(new_start)} --> {format_time(new_end)}{match.group('tail')}"

    return TIME_LINE_RE.sub(replace, text), changes


def frame_rows(text: str, fps: float) -> list[dict]:
    """Return explicit frame semantics for every cue in an SRT string."""
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    rows: list[dict] = []
    for block in re.split(r"\n\s*\n", normalized.strip()):
        lines = [line for line in block.split("\n") if line.strip()]
        if len(lines) < 2:
            continue
        time_index = next((i for i, line in enumerate(lines) if "-->" in line), None)
        if time_index is None:
            continue
        match = TIME_LINE_RE.search(lines[time_index])
        if not match:
            continue
        start_ms = parse_time(match.group("start"))
        end_ms = parse_time(match.group("end"))
        start_frame = frame_at(start_ms, fps)
        end_boundary = frame_at(end_ms, fps)
        rows.append(
            {
                "index": len(rows) + 1,
                "start_ms": start_ms,
                "end_ms": end_ms,
                "start_frame": start_frame,
                "end_boundary_frame": end_boundary,
                "last_visible_frame": end_boundary - 1,
                "text": "\n".join(lines[time_index + 1 :]).strip(),
            }
        )
    return rows


def _main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--srt", required=True)
    parser.add_argument("--fps", type=float, required=True)
    parser.add_argument("--json", default="")
    args = parser.parse_args(argv)
    args.fps = snap_fps(args.fps)

    raw = Path(args.srt).read_bytes()
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    text = raw[len(bom) :].decode("utf-8")
    rows = frame_rows(text, args.fps)
    if args.json:
        Path(args.json).write_text(
            json.dumps(rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
    for row in rows:
        print(
            f"{row['index']:03d} {row['start_ms'] / 1000:8.3f}-{row['end_ms'] / 1000:8.3f} "
            f"start={row['start_frame']} end_boundary={row['end_boundary_frame']} "
            f"last_visible={row['last_visible_frame']} {row['text']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
