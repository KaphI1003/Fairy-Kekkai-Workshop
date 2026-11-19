from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


VIDEOCR = Path(__file__).resolve().parents[1]
frame_time = _load("fkw_frame_time", VIDEOCR / "frame_time.py")
fixer = _load(
    "fkw_fix_srt_frames",
    Path(__file__).resolve().parents[2] / "fix_srt_frames.py",
)


class FrameSemanticsTest(unittest.TestCase):
    def test_60fps_boundary_is_exclusive(self) -> None:
        # Frame 1848 still shows cue 1; frame 1849 shows cue 2.
        self.assertEqual(frame_time.frame_at(30_816, 60.0), 1849)
        self.assertEqual(frame_time.last_visible_frame(30_816, 60.0), 1848)
        self.assertEqual(frame_time.canonical_ms(1848, 60.0), 30_800)
        self.assertEqual(frame_time.canonical_ms(1849, 60.0), 30_816)

    def test_seamless_cues_share_the_boundary(self) -> None:
        text = (
            "1\n00:00:28,283 --> 00:00:30,816\nfirst\n\n"
            "2\n00:00:30,816 --> 00:00:34,016\nsecond\n\n"
        )
        rows = frame_time.frame_rows(text, 60.0)
        self.assertEqual(rows[0]["start_frame"], 1697)
        self.assertEqual(rows[0]["end_boundary_frame"], 1849)
        self.assertEqual(rows[0]["last_visible_frame"], 1848)
        self.assertEqual(rows[1]["start_frame"], 1849)
        updated, changes = frame_time.canonicalize_srt(text, 60.0)
        self.assertEqual(updated, text)
        self.assertEqual(changes, [])

    def test_canonicalize_is_idempotent_on_test_project_ocr(self) -> None:
        path = Path(r"E:/Fairy-Kekkai-Workshop-Projects/test/1/原文_OCR.srt")
        if not path.is_file():
            self.skipTest("test project OCR SRT is not available")
        text = path.read_text(encoding="utf-8-sig")
        updated, changes = frame_time.canonicalize_srt(text, 60.0)
        self.assertEqual(changes, [])
        self.assertEqual(updated, text)
        rows = frame_time.frame_rows(text, 60.0)
        self.assertEqual(len(rows), 119)
        self.assertEqual(rows[0]["last_visible_frame"], 1848)

    def test_frame_cue_times_60fps(self) -> None:
        start_ms, end_ms = frame_time.frame_cue_times_ms(1697, 1849, fps=60.0)
        self.assertEqual((start_ms, end_ms), (28_283, 30_816))
        self.assertEqual(frame_time.frame_at(start_ms, 60.0), 1697)
        self.assertEqual(frame_time.frame_at(end_ms, 60.0), 1849)
        self.assertEqual(frame_time.last_visible_frame(end_ms, 60.0), 1848)

    def test_frame_cue_times_5994(self) -> None:
        fps = 60_000 / 1001
        start_ms, end_ms = frame_time.frame_cue_times_ms(1697, 1849, fps=fps)
        self.assertEqual(frame_time.frame_at(start_ms, fps), 1697)
        self.assertEqual(frame_time.frame_at(end_ms, fps), 1849)
        self.assertEqual(frame_time.last_visible_frame(end_ms, fps), 1848)

    def test_frame_cue_times_are_seamless(self) -> None:
        first_start, first_end = frame_time.frame_cue_times_ms(1697, 1849, fps=60.0)
        second_start, _ = frame_time.frame_cue_times_ms(1849, 2041, fps=60.0)
        self.assertEqual(first_end, second_start)
        self.assertGreater(second_start, first_start)

    def test_frame_cue_times_vfr_uses_pts_table(self) -> None:
        stamps = {1697: 28_283.4, 1849: 30_816.8}
        start_ms, end_ms = frame_time.frame_cue_times_ms(
            1697, 1849, fps=None, frame_timestamps=stamps
        )
        self.assertEqual((start_ms, end_ms), (28_283, 30_816))

    def test_frame_cue_times_vfr_falls_back_one_frame(self) -> None:
        stamps = {1697: 28_283.4, 1848: 30_800.2}
        start_ms, end_ms = frame_time.frame_cue_times_ms(
            1697, 1849, fps=None, frame_timestamps=stamps,
            fallback_frame_duration_ms=16.683,
        )
        self.assertEqual(start_ms, 28_283)
        self.assertEqual(end_ms, 30_816)

    def test_frame_cue_times_with_seek_base(self) -> None:
        # OCR started at 60 s; index 0 is absolute frame 3600 at 60 fps.
        start_ms, end_ms = frame_time.frame_cue_times_ms(
            0, 10, fps=60.0, frame_base=3600
        )
        self.assertEqual(start_ms, 60_000)
        self.assertEqual(end_ms, frame_time.canonical_ms(3610, 60.0))

    def test_frame_cue_times_with_seek_base_5994(self) -> None:
        fps = 60_000 / 1001
        start_ms, end_ms = frame_time.frame_cue_times_ms(
            0, 10, fps=fps, frame_base=3600
        )
        self.assertEqual(frame_time.frame_at(start_ms, fps), 3600)
        self.assertEqual(frame_time.frame_at(end_ms, fps), 3610)

    def test_frame_cue_times_bumps_zero_duration(self) -> None:
        start_ms, end_ms = frame_time.frame_cue_times_ms(1849, 1849, fps=60.0)
        self.assertGreater(end_ms, start_ms)
        self.assertEqual(frame_time.frame_at(end_ms, 60.0), 1850)

    def test_frame_cue_times_5994_round_trip_every_frame(self) -> None:
        fps = 60_000 / 1001
        for frame in range(1, 5_000):
            start_ms, end_ms = frame_time.frame_cue_times_ms(frame, frame + 1, fps=fps)
            self.assertEqual(frame_time.frame_at(start_ms, fps), frame)
            self.assertEqual(frame_time.frame_at(end_ms, fps), frame + 1)

    def test_serializer_round_trips_frames_60fps(self) -> None:
        fps = 60.0
        start_ms, end_ms = frame_time.frame_cue_times_ms(1697, 1849, fps=fps)
        text = (
            f"1\n{frame_time.format_time(start_ms)} --> {frame_time.format_time(end_ms)}\n"
            "first\n\n"
        )
        rows = frame_time.frame_rows(text, fps)
        self.assertEqual(rows[0]["start_frame"], 1697)
        self.assertEqual(rows[0]["end_boundary_frame"], 1849)
        self.assertEqual(rows[0]["last_visible_frame"], 1848)
        updated, changes = frame_time.canonicalize_srt(text, fps)
        self.assertEqual(updated, text)
        self.assertEqual(changes, [])

    def test_serializer_round_trips_frames_5994(self) -> None:
        fps = 60_000 / 1001
        start_ms, end_ms = frame_time.frame_cue_times_ms(1697, 1849, fps=fps)
        text = (
            f"1\n{frame_time.format_time(start_ms)} --> {frame_time.format_time(end_ms)}\n"
            "first\n\n"
        )
        rows = frame_time.frame_rows(text, fps)
        self.assertEqual(rows[0]["start_frame"], 1697)
        self.assertEqual(rows[0]["end_boundary_frame"], 1849)
        self.assertEqual(rows[0]["last_visible_frame"], 1848)

    def test_canonicalize_preserves_bom_and_crlf(self) -> None:
        text = "\ufeff1\r\n00:00:28,283 --> 00:00:30,816\r\nfirst\r\n\r\n"
        updated, changes = frame_time.canonicalize_srt(text, 60.0)
        self.assertEqual(updated, text)
        self.assertEqual(changes, [])

    def test_ass_cs_round_trip_60fps(self) -> None:
        start_cs, end_cs = frame_time.frame_cue_times_cs(1697, 1849, fps=60.0)
        self.assertEqual((start_cs, end_cs), (2828, 3081))
        self.assertEqual(frame_time.format_ass_time(start_cs), "0:00:28.28")
        self.assertEqual(frame_time.format_ass_time(end_cs), "0:00:30.81")
        # Aegisub START/END mapping is exactly our ceil / ceil-1 pair.
        self.assertEqual(frame_time.frame_at(start_cs * 10, 60.0), 1697)
        self.assertEqual(frame_time.last_visible_frame(end_cs * 10, 60.0), 1848)

    def test_ass_cs_round_trip_5994(self) -> None:
        fps = 60_000 / 1001
        start_cs, end_cs = frame_time.frame_cue_times_cs(1697, 1849, fps=fps)
        self.assertEqual(frame_time.frame_at(start_cs * 10, fps), 1697)
        self.assertEqual(frame_time.last_visible_frame(end_cs * 10, fps), 1848)

    def test_ass_one_frame_cue(self) -> None:
        start_cs, end_cs = frame_time.frame_cue_times_cs(1849, 1850, fps=60.0)
        self.assertEqual(frame_time.frame_at(start_cs * 10, 60.0), 1849)
        self.assertEqual(frame_time.last_visible_frame(end_cs * 10, 60.0), 1849)
        self.assertGreater(end_cs, start_cs)

    def test_snap_fps_exact_ntsc_rates(self) -> None:
        self.assertAlmostEqual(frame_time.snap_fps(59.94), 60_000 / 1001)
        self.assertAlmostEqual(frame_time.snap_fps(29.97), 30_000 / 1001)
        self.assertAlmostEqual(frame_time.snap_fps(23.976), 24_000 / 1001)
        self.assertEqual(frame_time.snap_fps(60.0), 60.0)

    def test_5994_round_trip_every_frame(self) -> None:
        fps = 60_000 / 1001
        for frame in range(1, 5_000):
            ms = frame_time.canonical_ms(frame, fps)
            self.assertEqual(frame_time.frame_at(ms, fps), frame, frame)

    def test_5994_rewrite_preserves_frame_index(self) -> None:
        fps = 60_000 / 1001
        frame = 1849
        ms = frame_time.canonical_ms(frame, fps)
        text = f"1\n{frame_time.format_time(ms)} --> {frame_time.format_time(ms + 1000)}\nx\n\n"
        updated, _ = frame_time.canonicalize_srt(text, fps)
        start = frame_time.parse_time(updated.split("\n")[1].split(" --> ")[0])
        self.assertEqual(frame_time.frame_at(start, fps), frame)

    def test_fix_srt_frames_reports_both_end_frames(self) -> None:
        # A non-canonical 59.94 time forces a change entry.
        text = "1\n00:00:30,848 --> 00:00:34,000\nx\n\n"
        updated, changes = fixer.rewrite(text, 60_000 / 1001)
        self.assertNotEqual(updated, text)
        self.assertEqual(len(changes), 1)
        change = changes[0]
        self.assertEqual(
            change["last_visible_frame"], change["end_boundary_frame"] - 1
        )
        self.assertEqual(change["end_frame"], change["end_boundary_frame"])

    def test_fix_srt_frames_keeps_canonical_60fps_seam(self) -> None:
        text = (
            "1\n00:00:28,283 --> 00:00:30,816\nfirst\n\n"
            "2\n00:00:30,816 --> 00:00:34,016\nsecond\n\n"
        )
        updated, changes = fixer.rewrite(text, 60.0)
        self.assertEqual(updated, text)
        self.assertEqual(changes, [])


if __name__ == "__main__":
    unittest.main()
