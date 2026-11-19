from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import sys

import numpy as np
import simplejpeg

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

try:
    from videocr import utils
except Exception:  # pragma: no cover - optional OCR build dependencies
    utils = None


def _distance(left, right) -> float:
    return sum(
        (int(left[index]) - int(right[index])) ** 2 for index in range(3)
    ) ** 0.5


def _write_styled_image(
    path: Path,
    *,
    outline: tuple[int, int, int],
    fill: tuple[int, int, int],
    background: tuple[int, int, int],
    width: int = 240,
    height: int = 80,
    border: int = 4,
) -> tuple[int, int, int, int]:
    image = np.full((height, width, 3), background, dtype=np.uint8)
    left, right = 20, width - 20
    top, bottom = 20, height - 20
    gap = 8
    block_width = max(4, (right - left - gap * 3) // 4)
    for block in range(4):
        x0 = left + block * (block_width + gap)
        x1 = min(right, x0 + block_width)
        if x1 <= x0:
            continue
        image[top:bottom, x0:x1] = outline
        inner_x0 = min(x1, x0 + border)
        inner_x1 = max(inner_x0, x1 - border)
        inner_y0 = min(bottom, top + border)
        inner_y1 = max(inner_y0, bottom - border)
        image[inner_y0:inner_y1, inner_x0:inner_x1] = fill
    data = simplejpeg.encode_jpeg(image, quality=90, colorspace="RGB")
    path.write_bytes(data)
    return left, top, right, bottom


@unittest.skipIf(utils is None, "OCR build dependencies are unavailable")
class ColorDetectionTest(unittest.TestCase):
    def _detect(self, outline, fill, background, **kwargs):
        with tempfile.TemporaryDirectory(prefix="fkw-color-test-") as temporary:
            path = Path(temporary) / "frame.jpg"
            x0, y0, x1, y1 = _write_styled_image(
                path,
                outline=outline,
                fill=fill,
                background=background,
                **kwargs,
            )
            detected_fill, detected_outline, confidence = utils.detect_subtitle_colors(
                str(path),
                [[[x0, y0], [x1, y0], [x1, y1], [x0, y1]]],
            )
        return detected_fill, detected_outline, confidence

    def test_magenta_fill_deep_purple_outline(self) -> None:
        fill, outline, confidence = self._detect(
            outline=(112, 0, 224),
            fill=(240, 112, 240),
            background=(208, 176, 208),
        )
        self.assertLess(_distance(fill, (240, 112, 240)), 30)
        self.assertLess(_distance(outline, (112, 0, 224)), 30)
        self.assertGreater(confidence, 0.4)

    def test_yellow_fill_red_outline(self) -> None:
        fill, outline, confidence = self._detect(
            outline=(224, 0, 0),
            fill=(240, 240, 112),
            background=(232, 176, 192),
        )
        self.assertLess(_distance(fill, (240, 240, 112)), 30)
        self.assertLess(_distance(outline, (224, 0, 0)), 30)
        self.assertGreater(confidence, 0.4)

    def test_white_fill_red_outline_is_not_swapped(self) -> None:
        fill, outline, confidence = self._detect(
            outline=(224, 0, 0),
            fill=(240, 240, 240),
            background=(40, 40, 40),
        )
        self.assertLess(_distance(fill, (240, 240, 240)), 30)
        self.assertLess(_distance(outline, (224, 0, 0)), 30)
        self.assertGreater(confidence, 0.4)

    def test_small_text_uses_adaptive_parameters(self) -> None:
        fill, outline, confidence = self._detect(
            outline=(224, 0, 0),
            fill=(240, 240, 240),
            background=(40, 40, 40),
            width=200,
            height=60,
            border=1,
        )
        self.assertLess(_distance(fill, (240, 240, 240)), 30)
        self.assertLess(_distance(outline, (224, 0, 0)), 30)
        self.assertGreater(confidence, 0.3)

    def test_soft_subtitle_ass_styles(self) -> None:
        ass = (
            "[Script Info]\nScriptType: v4.00+\n\n"
            "[V4+ Styles]\n"
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
            "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
            "Alignment, MarginL, MarginR, MarginV, Encoding\n"
            "Style: A,Yu Gothic UI,60,&H00F070F0,&H000000FF,&H007000E0,"
            "&H64000000,0,0,0,0,100,100,0,0,1,2,1,2,10,10,43,1\n\n"
            "[Events]\n"
            "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
            "MarginV, Effect, Text\n"
            "Dialogue: 0,0:00:28.28,0:00:30.81,A,,0,0,0,,テスト\n"
        )
        events = utils.parse_ass_events(ass)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["fill"], (240, 112, 240))
        self.assertEqual(events[0]["outline"], (112, 0, 224))
        matched = utils.match_soft_event(events, 28_283, 30_816, "テスト")
        self.assertIsNotNone(matched)
        self.assertIsNone(utils.match_soft_event(events, 40_000, 41_000, "別の台詞"))


if __name__ == "__main__":
    unittest.main()
