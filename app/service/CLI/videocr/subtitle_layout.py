from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, replace
from math import ceil, isfinite
from statistics import median
from typing import TYPE_CHECKING
from unicodedata import east_asian_width

if TYPE_CHECKING:
    from .models import PredictedFrames, PredictedSubtitle


FONT_NAME = "Yu Gothic UI"
# Measured with CJK text and the bundled VSFilterMod, before adding the outline.
GLYPH_HEIGHT_RATIO = 0.77
BOTTOM_INSET_RATIO = 0.12


@dataclass(frozen=True)
class LineBox:
    top: float
    bottom: float
    height: float
    advance: float | None = None


@dataclass(frozen=True)
class AssCue:
    subtitle: PredictedSubtitle
    rows: tuple[LineBox, ...] = ()
    line_span: tuple[int, int] | None = None

    @property
    def text(self) -> str:
        if self.line_span is None:
            return self.subtitle.text
        start, end = self.line_span
        return "\n".join(self.subtitle.text.splitlines()[start:end])


def _frame_rows(frame: PredictedFrames, zone: dict) -> tuple[LineBox, ...]:
    crop_width, crop_height, _, crop_y = map(int, zone["crop_str"].split(":"))
    ratio = crop_height / zone["h"]
    x_ratio = crop_width / zone["w"]
    rows = []
    for line in frame.lines:
        bounds = []
        advances = []
        for word in line:
            if not word.bounding_box or not all(isfinite(x) and isfinite(y) for x, y in word.bounding_box):
                continue
            xs, ys = zip(*word.bounding_box)
            top, bottom = min(ys), max(ys)
            width = max(xs) - min(xs)
            if bottom <= top or width <= 0:
                continue
            bounds.append((top, bottom))
            chars = [char for char in word.text if not char.isspace()]
            wide = sum(east_asian_width(char) in ("W", "F", "A") for char in chars)
            if wide >= 2 and 2 * wide >= len(chars):
                # CJK glyph advance is much less sensitive to the shape of a
                # particular line than the OCR polygon's height.
                cells = wide + .6 * (len(chars) - wide)
                advances.append(width * x_ratio / cells)
        if not bounds:
            return ()
        top = min(b[0] for b in bounds)
        bottom = max(b[1] for b in bounds)
        if top <= .5 or bottom >= zone["h"] - .5:
            return ()
        rows.append(LineBox(crop_y + top * ratio, crop_y + bottom * ratio,
                            median(b - t for t, b in bounds) * ratio,
                            median(advances) if advances else None))
    return tuple(rows)


def _measure_rows(sub: PredictedSubtitle, zones: list[dict]) -> tuple[LineBox, ...]:
    if not 0 <= sub.zone_index < len(zones):
        return ()
    text_lines = sub.text.splitlines()
    candidates = [frame for frame in sub.frames
                  if frame.zone_index == sub.zone_index and len(frame.lines) == len(text_lines)]
    normalized = ["".join(line.split()) for line in text_lines]
    matching = [frame for frame in candidates
                if ["".join(line.split()) for line in frame.text.splitlines()] == normalized]
    frames = matching or candidates
    if not frames:
        return ()
    if len(frames) > 9:
        frames = [frames[round(i * (len(frames) - 1) / 8)] for i in range(9)]
    samples = [_frame_rows(frame, zones[sub.zone_index]) for frame in frames]
    samples = [rows for rows in samples if rows]
    return tuple(LineBox(median(row.top for row in line), median(row.bottom for row in line),
                         median(row.height for row in line),
                         _median_advance(line)) for line in zip(*samples))


def _separate_blocks(upper: LineBox, lower: LineBox) -> bool:
    return lower.top - upper.bottom > 1.25 * max(upper.height, lower.height)


def prepare_cues(subtitles: list[PredictedSubtitle], zones: list[dict],
                 merged_sources: dict[PredictedSubtitle, list[PredictedSubtitle]],
                 *, position: bool, font_size: bool) -> list[AssCue]:
    if not (position or font_size):
        return [AssCue(sub) for sub in subtitles]

    cues = []
    for sub in subtitles:
        sources = merged_sources.get(sub)
        if sources:
            parts = [AssCue(source, _measure_rows(source, zones)) for source in sources]
            by_zone = defaultdict(list)
            for part in parts:
                by_zone[part.subtitle.zone_index].extend(part.rows)
            bands = sorted(by_zone.values(), key=lambda rows: min(row.top for row in rows)
                           if rows else float("inf"))
            if (position and len(bands) == 2 and all(bands)
                    and _separate_blocks(max(bands[0], key=lambda row: row.bottom),
                                         min(bands[1], key=lambda row: row.top))):
                cues.extend(parts)
                continue
            rows = tuple(sorted((row for part in parts for row in part.rows),
                                key=lambda row: row.top)) if all(part.rows for part in parts) else ()
            cues.append(AssCue(sub, rows))
            continue

        rows = _measure_rows(sub, zones)
        cuts = [i for i in range(1, len(rows)) if _separate_blocks(rows[i - 1], rows[i])] if position else []
        if cuts:
            boundaries = [0, *cuts, len(rows)]
            cues.extend(AssCue(sub, rows[start:end], (start, end))
                        for start, end in zip(boundaries, boundaries[1:]))
        else:
            cues.append(AssCue(sub, rows))
    return sorted(cues, key=lambda cue: cue.subtitle.index_start)


@dataclass(frozen=True)
class StyleLayout:
    font_size: float
    margin_v: int


@dataclass(frozen=True)
class EventLayout:
    font_size: int | None = None
    margin_v: int = 0
    anchor_y: int | None = None
    review: bool = False


def _dominant_band(ordered: list[float], tolerance: float) -> slice:
    left = best_left = best_right = 0
    for right, value in enumerate(ordered):
        while value - ordered[left] > 2 * tolerance:
            left += 1
        if right - left > best_right - best_left:
            best_left, best_right = left, right
    return slice(best_left, best_right + 1)


def _median_advance(rows: tuple[LineBox, ...]) -> float | None:
    values = [row.advance for row in rows if row.advance is not None]
    return median(values) if values else None


@dataclass(frozen=True)
class FontObservation:
    height: float
    advance: float | None


def _font_reference(observations: list[FontObservation], scale: float) -> tuple[FontObservation, int]:
    with_advance = sorted((value for value in observations if value.advance is not None),
                          key=lambda value: value.advance)
    if len(with_advance) >= max(3, .5 * len(observations)):
        advances = [value.advance for value in with_advance]
        band = _dominant_band(advances, max(3 * scale, .10 * median(advances)))
        values = with_advance[band]
        if len(values) >= 3:
            height = median(value.height for value in values)
            support = len(values) + sum(value.advance is None and abs(value.height - height) <= .14 * height
                                        for value in observations)
            return FontObservation(height, median(advances[band])), support
    ordered = sorted(observations, key=lambda value: value.height)
    heights = [value.height for value in ordered]
    dominant = _dominant_band(heights, max(3 * scale, .10 * median(heights)))
    values = ordered[dominant]
    advances = [value.advance for value in values if value.advance is not None]
    return FontObservation(median(heights[dominant]), median(advances) if len(advances) >= 2 else None), len(values)


def _different_size(value: FontObservation, reference: FontObservation, scale: float) -> bool:
    delta = value.height - reference.height
    if abs(delta) <= max(4 * scale, .14 * reference.height):
        return False
    if value.advance is None or reference.advance is None:
        return abs(delta) > max(6 * scale, .27 * reference.height)
    advance_delta = (value.advance - reference.advance) / reference.advance
    # A short word or punctuation can also throw off the width estimate. Require
    # close agreement between height and advance for smaller changes in size.
    height_delta = delta / reference.height
    return advance_delta * height_delta > 0 and (
        abs(advance_delta) > max(.23, .45 * abs(height_delta))
        or (abs(advance_delta) > .16 and abs(advance_delta - height_delta) <= .05)
    )


def _typical_top(cue: AssCue, reference: FontObservation | None, size_exception: bool) -> float:
    row = cue.rows[0]
    if len(cue.rows) != 1 or reference is None or size_exception:
        return row.top
    # Detector padding often grows in both directions on a short line.
    # Keep the measured vertical center and recover the top of a typical line.
    return (row.top + row.bottom - reference.height) / 2


def _simultaneous_cues(cues: list[AssCue]) -> tuple[list[tuple[int, int]], list[float]]:
    """Keep overlapping ASS events anchored and account for Japanese text above them."""
    pairs = []
    floors = [0.0] * len(cues)
    active = []
    for i, cue in enumerate(cues):
        active = [j for j in active if cues[j].subtitle.index_end >= cue.subtitle.index_start]
        if not cue.rows:
            continue
        for j in active:
            pairs.append((i, j))
            other = cues[j]
            if other.rows[0].top < cue.rows[0].top:
                floors[i] = max(floors[i], max(row.bottom for row in other.rows))
            elif cue.rows[0].top < other.rows[0].top:
                floors[j] = max(floors[j], max(row.bottom for row in cue.rows))
        active.append(i)
    return pairs, floors


def estimate_layout(cues: list[AssCue], styles: list[str], *, video_height: int,
                    base_font: float, base_margin: int, outline: float, shadow: float,
                    position: bool, font_size: bool, gap_1080p: int
                    ) -> tuple[dict[str, StyleLayout], list[EventLayout]]:
    scale = video_height / 1080
    gap = gap_1080p * scale
    fonts_by_style = defaultdict(list)
    observations = []
    for cue, style in zip(cues, styles):
        value = None
        if (position or font_size) and cue.rows:
            value = FontObservation(median(row.height for row in cue.rows), _median_advance(cue.rows))
            fonts_by_style[style].append(value)
        observations.append(value)

    # Different colors often belong to speakers using the same typography.
    # Require several consistent observations before assigning a distinct size to a color.
    available = [value for value in observations if value is not None]
    common = _font_reference(available, scale)[0] if available else None
    references = {}
    style_fonts = {}
    tolerances = {}

    def size_for(height: float) -> int:
        return max(1, round(12 * scale), round((height - 2 * outline) / GLYPH_HEIGHT_RATIO))

    for style in set(styles):
        values = fonts_by_style[style]
        reference = common
        if common and len(values) >= 3:
            local, support = _font_reference(values, scale)
            if support >= max(3, .6 * len(values)) and _different_size(local, common, scale):
                reference = local
        references[style] = reference
        style_fonts[style] = size_for(reference.height) if font_size and reference else base_font
        tolerances[style] = max(3 * scale, min(14 * scale, .14 * reference.height)) if reference else 3 * scale

    pairs, floors = _simultaneous_cues(cues) if position else ([], [0.0] * len(cues))
    pinned = {index for pair in pairs for index in pair}
    margins_by_style = defaultdict(list)
    event_fields = []
    for cue, style, value, floor in zip(cues, styles, observations, floors):
        style_font = style_fonts[style]
        reference = references[style]
        size_exception = bool(value and reference and _different_size(value, reference, scale))
        override = size_for(value.height) if font_size and size_exception else None
        fs = style_font if override is None else override
        inset = BOTTOM_INSET_RATIO * fs - outline - shadow
        ink_height = fs * (GLYPH_HEIGHT_RATIO + max(0, len(cue.rows) - 1)) + 2 * outline + shadow
        margin = None
        top = None
        review = False
        if position and cue.rows:
            top = _typical_top(cue, reference, size_exception)
            room_for_gap = top - floor - ink_height
            used_gap = min(gap, max(0, room_for_gap))
            margin = max(1, ceil(video_height - top + used_gap - inset))
            review = room_for_gap < 0
            if top < ink_height:
                margin = base_margin
            elif used_gap == gap and not review:
                margins_by_style[style].append(margin)
        event_fields.append((override, margin, review, inset, ink_height, top))

    defaults = {}
    for style, fs in style_fonts.items():
        margins = sorted(margins_by_style[style])
        typical = margins[_dominant_band(margins, tolerances[style])] if margins else []
        margin = round(median(typical)) if typical else base_margin
        defaults[style] = StyleLayout(fs, margin)
    events = []
    ink_bounds = []
    for i, (style, fields) in enumerate(zip(styles, event_fields)):
        override, margin, review, inset, ink_height, top = fields
        default_margin = defaults[style].margin_v
        tolerance = tolerances[style]
        if (margin is not None and i not in pinned and not review
                and abs(margin - default_margin) <= tolerance
                and video_height - default_margin - inset <= top + scale):
            margin = None
        anchor = video_height - (margin or default_margin)
        bottom = anchor - inset
        ink_bounds.append((bottom - ink_height, bottom))
        fixed = i in pinned and not review
        events.append(EventLayout(override, 0 if fixed else (margin or 0),
                                  anchor if fixed else None, review))
    for i, j in pairs:
        top = max(ink_bounds[i][0], ink_bounds[j][0])
        bottom = min(ink_bounds[i][1], ink_bounds[j][1])
        if bottom > top:
            for index in (i, j):
                event = events[index]
                margin = event.margin_v if event.anchor_y is None else video_height - event.anchor_y
                # Let the renderer stack these events when their intended positions collide.
                events[index] = replace(event, margin_v=margin, anchor_y=None, review=True)
    return defaults, events
