from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Any, Callable

import wordninja_enhanced as wordninja

from . import utils


@dataclass
class PredictedText:
    __slots__ = 'bounding_box', 'confidence', 'text'
    bounding_box: list[list[float]]
    confidence: float
    text: str


class PredictedFrames:
    start_index: int  # 0-based index of the frame
    end_index: int
    zone_index: int
    lines: list[list[PredictedText]]
    confidence: float  # total confidence of all words
    text: str
    fill_rgb: tuple[int, int, int] | None
    outline_rgb: tuple[int, int, int] | None
    color_confidence: float | None

    def __init__(self, ocr_engine: str, index: int, pred_data: list[list[Any]], conf_threshold: float,
                 zone_index: int, lang: str, normalize_to_simplified_chinese: bool) -> None:
        self.start_index = index
        self.end_index = index
        self.zone_index = zone_index
        self.lines: list[list[PredictedText]] = []
        self.fill_rgb = None
        self.outline_rgb = None
        self.color_confidence = None

        all_words: list[PredictedText] = []
        for word_pred in pred_data[0]:
            if len(word_pred) < 2:
                continue
            bounding_box = word_pred[0]
            text = word_pred[1][0]
            conf = word_pred[1][1]

            if conf >= conf_threshold:
                all_words.append(PredictedText(bounding_box, conf, text))

        if not all_words:
            self.confidence = 100 if not pred_data[0] else 0
            self.text = ''
            return

        lines_of_words: list[list[PredictedText]] = []
        for word in all_words:
            placed = False
            for line in lines_of_words:
                if utils.is_on_same_line(word, line[0]):
                    line.append(word)
                    placed = True
                    break
            if not placed:
                lines_of_words.append([word])

        lines_of_words.sort(key=lambda line: min(p[1] for p in line[0].bounding_box))

        for line in lines_of_words:
            line.sort(key=lambda word: word.bounding_box[0][0], reverse=utils.is_language_rtl(lang))

        self.lines = lines_of_words

        if self.lines:
            total_conf = sum(word.confidence for line in self.lines for word in line)
            word_count = sum(len(line) for line in self.lines)
            self.confidence = total_conf / word_count if word_count > 0 else 0
        else:
            self.confidence = 0

        if ocr_engine == "google_lens":
            self.text = '\n'.join(''.join(word.text for word in line) for line in self.lines)
        else:
            self.text = '\n'.join(' '.join(word.text for word in line) for line in self.lines)

        # FKW: OpenCC 繁转简已禁用（bug 太多），normalize_to_simplified_chinese 参数保留但不生效
        # if normalize_to_simplified_chinese and lang in ("ch", "zh-CN") and self.text:
        #     self.text = self._converter.convert(self.text)


def pick_color_frames(
    frames: list[PredictedFrames], limit: int
) -> list[PredictedFrames]:
    """Prefer interior OCR frames, with confidence as a secondary signal."""
    if limit <= 0 or not frames:
        return []
    start = min(f.start_index for f in frames)
    end = max(f.end_index for f in frames)
    middle = (start + end) / 2
    span = max(1, end - start)
    return sorted(frames, key=lambda f: (
        abs(f.start_index - middle) / span - .20 * min(1, f.confidence),
        f.start_index))[:limit]


def vote_frame_colors(
    frames: list[PredictedFrames], max_votes: int = 5
) -> tuple[
    tuple[int, int, int],
    tuple[int, int, int],
    float,
    float,
    int,
]:
    """Fuse ordered pairs by perceptual distance, not RGB bin boundaries."""
    from .color_styles import ColorObservation, fuse_observations, pair_distance

    valid = [frame for frame in frames if frame.fill_rgb is not None
             and frame.outline_rgb is not None and (frame.color_confidence or 0) > 0]
    valid.sort(key=lambda f: f.color_confidence or 0, reverse=True)
    observations = [ColorObservation(f.fill_rgb, f.outline_rgb,
                                    f.color_confidence, f.color_confidence,
                                    f.color_confidence, 1) for f in valid[:max_votes]]
    fused = fuse_observations(observations)
    if fused.fill is None or fused.outline is None:
        return (255, 255, 255), (0, 0, 0), 0.0, 0.0, 0
    agreement = sum(pair_distance(o, fused) <= 1 for o in observations) / len(observations)
    return fused.fill, fused.outline, fused.confidence, agreement, len(observations)


def sample_cue_colors(
    frames: list[PredictedFrames],
    sample: Callable[[PredictedFrames], None],
    min_samples: int = 2,
    max_samples: int = 5,
    confidence_floor: float = 0.5,
) -> list[PredictedFrames]:
    """Sample colors for one cue, expanding only when the first votes disagree.

    The first ``min_samples`` interior frames are sampled and voted. Confident
    agreement stops early; uncertain cues expand up to ``max_samples``.
    """
    candidates = [frame for frame in frames if frame.text.strip()]
    if not candidates:
        return []

    chosen = pick_color_frames(candidates, min(max_samples, len(candidates)))
    initial_count = min(min_samples, len(chosen))
    for frame in chosen[:initial_count]:
        sample(frame)

    _, _, confidence, agreement, votes = vote_frame_colors(chosen[:initial_count])
    if votes >= initial_count and agreement >= 1.0 and confidence >= confidence_floor:
        return chosen[:initial_count]

    for frame in chosen[initial_count:]:
        sample(frame)
    return chosen


class PredictedSubtitle:
    frames: list[PredictedFrames]
    zone_index: int
    sim_threshold: int
    text: str
    lang: str
    _language_model: wordninja.LanguageModel | None

    def __init__(self, frames: list[PredictedFrames], zone_index: int, sim_threshold: int, lang: str, language_model: wordninja.LanguageModel | None):
        self.frames = [f for f in frames if f.confidence > 0]
        self.frames.sort(key=lambda frame: frame.start_index)
        self.zone_index = zone_index
        self.sim_threshold = sim_threshold
        self.lang = lang
        self._language_model = language_model

        if self.frames:
            self.text = max(self.frames, key=lambda f: f.confidence).text
        else:
            self.text = ''

    @property
    def index_start(self) -> int:
        if self.frames:
            return self.frames[0].start_index
        return 0

    @property
    def index_end(self) -> int:
        if self.frames:
            return self.frames[-1].end_index
        return 0

    def color_pair(self) -> tuple[tuple[int, int, int], tuple[int, int, int], float]:
        """Return a temporally voted (fill, outline, confidence) for this cue."""
        fill, outline, confidence, _, _ = vote_frame_colors(self.frames)
        return fill, outline, confidence

    def is_similar_to(self, other: PredictedSubtitle) -> bool:
        return utils.levenshtein_ratio(self.text.replace(' ', ''), other.text.replace(' ', ''), self.sim_threshold) >= self.sim_threshold

    def finalize_text(self, post_processing: bool) -> None:
        text_counts: Counter[str] = Counter()
        text_confidences: dict[str, list[float]] = defaultdict(list)

        for frame in self.frames:
            text_counts[frame.text] += 1
            text_confidences[frame.text].append(frame.confidence)

        max_count = max(text_counts.values())
        candidates = [text for text, count in text_counts.items() if count == max_count]

        if len(candidates) == 1:
            final_text = candidates[0]
        else:
            final_text = max(
                candidates,
                key=lambda t: sum(text_confidences[t]) / len(text_confidences[t])
            )

        if post_processing:
            if self._language_model is not None and self.lang in ("en", "fr", "german", "it", "es", "pt"):
                final_text = self._language_model.rejoin(final_text)
            elif self.lang == "ch":
                segments = utils.extract_non_chinese_segments(final_text)
                rebuilt_text = ''

                for typ, seg in segments:
                    if typ == 'non_chinese':
                        rebuilt_text += wordninja.rejoin(seg)
                    else:
                        rebuilt_text += seg

                final_text = rebuilt_text

        self.text = final_text
