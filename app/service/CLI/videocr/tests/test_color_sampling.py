from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from videocr import models


def make_frame(index: int, confidence: float) -> SimpleNamespace:
    return SimpleNamespace(
        start_index=index,
        end_index=index,
        zone_index=0,
        confidence=confidence,
        text=f"subtitle-{index}",
        lines=[],
        fill_rgb=None,
        outline_rgb=None,
        color_confidence=None,
    )


def paint(frame, fill, outline, confidence):
    frame.fill_rgb = fill
    frame.outline_rgb = outline
    frame.color_confidence = confidence


class PickColorFramesTest(unittest.TestCase):
    def test_interior_frames_are_preferred_over_fading_boundaries(self) -> None:
        frames = [
            make_frame(0, 40.0),
            make_frame(1, 55.0),
            make_frame(2, 99.0),
            make_frame(3, 70.0),
            make_frame(4, 60.0),
        ]
        picked = models.pick_color_frames(frames, 3)
        self.assertEqual(len(picked), 3)
        self.assertEqual({frame.start_index for frame in picked}, {1, 2, 3})

    def test_high_confidence_boundary_does_not_displace_all_interior_frames(self) -> None:
        frames = [make_frame(0, 99.0), make_frame(1, 60.0), make_frame(2, 55.0), make_frame(3, 50.0)]
        picked = models.pick_color_frames(frames, 3)
        self.assertEqual(len(picked), 3)
        self.assertEqual({frame.start_index for frame in picked}, {0, 1, 2})

    def test_fewer_frames_than_limit_returns_all(self) -> None:
        frames = [make_frame(0, 90.0), make_frame(1, 80.0)]
        self.assertEqual(len(models.pick_color_frames(frames, 3)), 2)

    def test_five_frame_selection_extends_the_three_frame_prefix(self) -> None:
        frames = [make_frame(index, 50.0 + index) for index in range(6)]
        three = models.pick_color_frames(frames, 3)
        five = models.pick_color_frames(frames, 5)
        self.assertEqual(three, five[:3])
        self.assertEqual(len(five), 5)


class VoteFrameColorsTest(unittest.TestCase):
    def test_identical_samples_agree_fully(self) -> None:
        frames = [make_frame(index, 90.0) for index in range(3)]
        for frame in frames:
            paint(frame, (200, 20, 20), (0, 0, 0), 0.8)
        fill, outline, confidence, agreement, votes = models.vote_frame_colors(frames)
        self.assertEqual(fill, (200, 20, 20))
        self.assertEqual(outline, (0, 0, 0))
        self.assertAlmostEqual(confidence, 0.8)
        self.assertAlmostEqual(agreement, 1.0)
        self.assertEqual(votes, 3)

    def test_disagreeing_sample_reduces_agreement(self) -> None:
        frames = [make_frame(index, 90.0) for index in range(3)]
        paint(frames[0], (200, 20, 20), (0, 0, 0), 0.6)
        paint(frames[1], (204, 24, 24), (0, 0, 0), 0.6)
        paint(frames[2], (20, 200, 20), (0, 0, 0), 0.6)
        fill, _, confidence, agreement, votes = models.vote_frame_colors(frames)
        self.assertIn(fill, [(200, 20, 20), (204, 24, 24)])
        self.assertAlmostEqual(agreement, 2 / 3)
        self.assertLess(confidence, 0.6)
        self.assertGreater(confidence, 0.3)
        self.assertEqual(votes, 3)

    def test_uncolored_frames_are_ignored(self) -> None:
        frames = [make_frame(index, 90.0) for index in range(2)]
        paint(frames[0], (10, 20, 30), (0, 0, 0), 0.7)
        fill, outline, confidence, agreement, votes = models.vote_frame_colors(frames)
        self.assertEqual(fill, (10, 20, 30))
        self.assertEqual(outline, (0, 0, 0))
        self.assertAlmostEqual(confidence, 0.7)
        self.assertAlmostEqual(agreement, 1.0)
        self.assertEqual(votes, 1)


class SampleCueColorsTest(unittest.TestCase):
    def _sampler(self, calls, fill=(30, 40, 50), confidence=0.9, colors=None):
        def sample(frame):
            calls.append(frame.start_index)
            frame_fill = colors(frame) if colors else fill
            paint(frame, frame_fill, (0, 0, 0), confidence)
        return sample

    def test_consistent_two_frame_sample_stops_early(self) -> None:
        frames = [make_frame(index, 50.0 + index) for index in range(6)]
        calls: list[int] = []
        chosen = models.sample_cue_colors(frames, self._sampler(calls))
        self.assertEqual(len(calls), 2)
        self.assertEqual(len(chosen), 2)

    def test_color_conflict_expands_to_five_frames(self) -> None:
        frames = [make_frame(index, 50.0 + index) for index in range(8)]
        calls: list[int] = []

        def colors(frame):
            return (200, 0, 0) if frame.start_index % 2 == 0 else (0, 0, 200)

        chosen = models.sample_cue_colors(frames, self._sampler(calls, colors=colors))
        self.assertEqual(len(calls), 5)
        self.assertEqual(len(chosen), 5)

    def test_low_confidence_expands_to_five_frames(self) -> None:
        frames = [make_frame(index, 50.0 + index) for index in range(8)]
        calls: list[int] = []
        chosen = models.sample_cue_colors(
            frames, self._sampler(calls, confidence=0.3)
        )
        self.assertEqual(len(calls), 5)
        self.assertEqual(len(chosen), 5)

    def test_failed_detection_counts_as_missing_vote_and_expands(self) -> None:
        frames = [make_frame(index, 50.0 + index) for index in range(8)]
        calls: list[int] = []

        def sample(frame):
            calls.append(frame.start_index)
            if len(calls) == 2:
                return  # detection failed, frame stays uncolored
            paint(frame, (30, 40, 50), (0, 0, 0), 0.9)

        chosen = models.sample_cue_colors(frames, sample)
        self.assertEqual(len(calls), 5)
        self.assertEqual(len(chosen), 5)

    def test_two_frame_cue_is_not_forced_to_five(self) -> None:
        frames = [make_frame(0, 90.0), make_frame(1, 80.0)]
        calls: list[int] = []
        chosen = models.sample_cue_colors(frames, self._sampler(calls))
        self.assertEqual(len(calls), 2)
        self.assertEqual(len(chosen), 2)


if __name__ == "__main__":
    unittest.main()
