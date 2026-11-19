"""CPU-only subtitle colour estimation and whole-video palette assignment.

OCR supplies geometry, never colour.  Observations retain missing components;
only supported, ordered (fill, outline) pairs may create an ASS style.
"""
from __future__ import annotations

from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from concurrent.futures.process import BrokenProcessPool
from contextlib import ExitStack
from dataclasses import dataclass, field
import math
import multiprocessing
import os
import time
from typing import Callable

import numpy as np
from scipy import ndimage
from scipy.cluster.hierarchy import fcluster, linkage
from scipy.spatial.distance import squareform

from . import stroke_colors

RGB = tuple[int, int, int]
_CONNECTED = np.ones((3, 3), dtype=bool)


def rgb_to_lab(rgb) -> np.ndarray:
    rgb = np.asarray(rgb, dtype=np.float64) / 255.0
    linear = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    xyz = linear @ np.array([[.4124564, .2126729, .0193339],
                             [.3575761, .7151522, .1191920],
                             [.1804375, .0721750, .9503041]])
    xyz /= np.array([.95047, 1.0, 1.08883])
    f = np.where(xyz > (6 / 29) ** 3, np.cbrt(xyz), xyz / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack((116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]),
                     200 * (f[..., 1] - f[..., 2])), axis=-1)


def delta_e(left, right) -> np.ndarray:
    """Vectorised CIEDE2000, with Lab on its conventional 0..100 scale."""
    l1, a1, b1 = np.moveaxis(np.asarray(left, dtype=float), -1, 0)
    l2, a2, b2 = np.moveaxis(np.asarray(right, dtype=float), -1, 0)
    c1, c2 = np.hypot(a1, b1), np.hypot(a2, b2)
    c = (c1 + c2) / 2
    g = .5 * (1 - np.sqrt(c ** 7 / (c ** 7 + 25 ** 7)))
    ap1, ap2 = (1 + g) * a1, (1 + g) * a2
    cp1, cp2 = np.hypot(ap1, b1), np.hypot(ap2, b2)
    h1 = np.mod(np.degrees(np.arctan2(b1, ap1)), 360)
    h2 = np.mod(np.degrees(np.arctan2(b2, ap2)), 360)
    dl, dc = l2 - l1, cp2 - cp1
    dh = h2 - h1
    dh = np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh))
    dh = np.where(cp1 * cp2 == 0, 0, dh)
    d_h = 2 * np.sqrt(cp1 * cp2) * np.sin(np.radians(dh / 2))
    lm, cm = (l1 + l2) / 2, (cp1 + cp2) / 2
    hm = np.where(np.abs(h1 - h2) <= 180, (h1 + h2) / 2,
                  np.where(h1 + h2 < 360, (h1 + h2 + 360) / 2, (h1 + h2 - 360) / 2))
    hm = np.where(cp1 * cp2 == 0, h1 + h2, hm)
    t = (1 - .17 * np.cos(np.radians(hm - 30)) + .24 * np.cos(np.radians(2 * hm))
         + .32 * np.cos(np.radians(3 * hm + 6)) - .20 * np.cos(np.radians(4 * hm - 63)))
    sl = 1 + .015 * (lm - 50) ** 2 / np.sqrt(20 + (lm - 50) ** 2)
    sc, sh = 1 + .045 * cm, 1 + .015 * cm * t
    rt = -2 * np.sqrt(cm ** 7 / (cm ** 7 + 25 ** 7)) * np.sin(
        np.radians(60 * np.exp(-((hm - 275) / 25) ** 2)))
    return np.sqrt(np.maximum(0, (dl / sl) ** 2 + (dc / sc) ** 2 + (d_h / sh) ** 2
                              + rt * (dc / sc) * (d_h / sh)))


@dataclass
class ColorObservation:
    fill: RGB | None = None
    outline: RGB | None = None
    fill_confidence: float = 0.0
    outline_confidence: float = 0.0
    confidence: float = 0.0
    support: int = 0
    frame_count: int = 1
    reason: str = "no_text_colour"
    alternatives: list[ColorObservation] = field(default_factory=list, repr=False)


def _canonical_outline(rgb: RGB) -> RGB:
    # Compression and blending tint nominal white/black strokes. Keep visibly
    # coloured outlines (including lavender) outside these narrow neutral caps.
    if min(rgb) >= 220 and max(rgb) - min(rgb) <= 32:
        return (255, 255, 255)
    if max(rgb) <= 18:
        return (0, 0, 0)
    return rgb


def pair_distance(left: ColorObservation, right: ColorObservation,
                  fill_limit: float = 7.0, outline_limit: float = 9.0) -> float:
    if left.fill is None or right.fill is None or left.outline is None or right.outline is None:
        return math.inf
    return max(float(delta_e(rgb_to_lab(left.fill), rgb_to_lab(right.fill))) / fill_limit,
               float(delta_e(rgb_to_lab(left.outline), rgb_to_lab(right.outline))) / outline_limit)


def _palette(image: np.ndarray, limit: int = 16) -> tuple[np.ndarray, np.ndarray]:
    """Histogram in C; retain actual pixel medians, not quantised RGB values."""
    pixels = image.reshape(-1, 3)
    q = pixels.astype(np.int32) // 16
    keys = q[:, 0] * 256 + q[:, 1] * 16 + q[:, 2]
    counts = np.bincount(keys, minlength=4096)
    sums = [np.bincount(keys, weights=pixels[:, channel], minlength=4096) for channel in range(3)]
    candidates = np.argsort(counts)[::-1]
    colours: list[np.ndarray] = []
    labs: list[np.ndarray] = []
    for key in candidates[:48]:
        if counts[key] < max(3, len(pixels) * .0005):
            break
        rgb = np.array([channel[key] / counts[key] for channel in sums])
        lab = rgb_to_lab(rgb)
        if any(np.linalg.norm(lab - other) < 7 for other in labs):
            continue
        colours.append(rgb)
        labs.append(lab)
        if len(colours) >= limit:
            break
    return np.asarray(colours), np.asarray(labs)


def _glyph_mask(mask: np.ndarray, height: int) -> tuple[np.ndarray, int]:
    labels, count = ndimage.label(mask, structure=_CONNECTED)
    if not count:
        return mask, 0
    areas = np.bincount(labels.ravel(), minlength=count + 1)
    valid = np.zeros(count + 1, dtype=bool)
    objects = ndimage.find_objects(labels)
    for label, obj in enumerate(objects, 1):
        if obj is None:
            continue
        sy, sx = obj
        h, w = sy.stop - sy.start, sx.stop - sx.start
        at_edge = sy.start == 0 or sx.start == 0 or sy.stop == mask.shape[0] or sx.stop == mask.shape[1]
        valid[label] = (areas[label] >= max(3, height * height * .0015)
                        and areas[label] <= height * height * 1.5
                        and h >= max(2, height * .10) and w <= height * 2.5
                        and not at_edge)
    return valid[labels], int(valid.sum())


def _enclosed_pair(image: np.ndarray) -> ColorObservation:
    h, w = image.shape[:2]
    if h < 5 or w < 5:
        return ColorObservation()
    # Native colour crops preserve thin outlines. Bound work on very large text.
    if h > 110:
        step = max(1, h // 80)
        image = image[::step, ::step]
        h, w = image.shape[:2]
    colours, labs = _palette(image)
    if len(colours) < 2:
        return ColorObservation()
    pixel_labs = rgb_to_lab(image).astype(np.float32)
    distances = np.stack([np.linalg.norm(pixel_labs - lab.astype(np.float32), axis=2) for lab in labs])
    colour_distances = delta_e(labs[:, None, :], labs[None, :, :])
    nearest = distances.argmin(axis=0)
    supported = distances.min(axis=0) < 13
    masks = [(nearest == index) & supported for index in range(len(labs))]
    # Thin JPEG outlines can straddle several histogram bins. A wider mask is
    # safe for the outline test provided overlap with the fill is excluded.
    outline_masks = [distance < 20 for distance in distances]
    glyphs = [_glyph_mask(mask, h) for mask in masks]
    # Closing repairs anti-aliasing gaps; hole filling is used as a directional
    # enclosure test, never as an unconditional replacement for the text mask.
    enclosed = [ndimage.binary_fill_holes(ndimage.binary_closing(mask, structure=_CONNECTED))
                for mask in outline_masks]
    best = ColorObservation()
    radius = max(1, min(4, round(h / 22)))
    def shape_stats(mask, count):
        area = int(mask.sum())
        ring = ndimage.binary_dilation(mask, structure=_CONNECTED, iterations=radius) & ~mask
        filled = ndimage.binary_fill_holes(ndimage.binary_closing(mask, structure=_CONNECTED))
        hollow = min(.45, .22 * float(np.count_nonzero(filled & ~mask)) / max(1, area))
        repeat = min(1.0, count / max(2.0, w / h * .55))
        return area, ring, max(1, int(ring.sum())), filled, hollow, repeat

    for i, (base_mask, base_components) in enumerate(glyphs):
        base_area = int(base_mask.sum())
        base_shape = shape_stats(base_mask, base_components) if base_components else None
        needs_rescue = base_area < w * h * .015 or base_components < max(2, w / h * .1)
        for j, outline_mask in enumerate(outline_masks):
            if i == j or colour_distances[i, j] < 9:
                continue
            fill_mask, components = base_mask, base_components
            shape = base_shape
            # Fill may share the background colour (orange letters on an
            # orange shirt). Isolate interiors enclosed by the proposed stroke
            # before rejecting background-connected components.
            if needs_rescue:
                interior = masks[i] & enclosed[j] & ~outline_mask
                inside_area = np.count_nonzero(interior)
                if max(8, base_area * 1.25) < inside_area <= max(w * h * .35, h * h * 6):
                    rescued, count = _glyph_mask(interior, h)
                    if np.count_nonzero(rescued) > base_area:
                        fill_mask, components = rescued, count
                        shape = shape_stats(fill_mask, components)
            area = shape[0] if shape is not None else 0
            if components == 0 or area < max(8, h * h * .006):
                continue
            _, ring, ring_area, fill_enclosed, hollow_penalty, repeat = shape
            surround = float(np.count_nonzero(ring & outline_mask)) / ring_area
            inside = float(np.count_nonzero(fill_mask & enclosed[j] & ~outline_mask)) / area
            if surround < .18 or inside < .30:
                continue
            other_mask = glyphs[j][0]
            reverse = float(np.count_nonzero(other_mask & fill_enclosed)) / max(1, int(other_mask.sum()))
            contrast = min(1.0, float(colour_distances[i, j]) / 50)
            score = (.42 * inside + .25 * min(1, surround / .7) + .18 * repeat
                     + .15 * contrast - .20 * reverse - hollow_penalty)
            # A few tiny enclosed patches in textured scenery are not enough
            # evidence for a subtitle colour, even when their contrast is high.
            coverage = min(1.0, area / max(1.0, h * w * .06))
            score *= .4 + .6 * coverage
            if score <= best.confidence:
                continue
            # Estimate the flat interiors; mask medians resist JPEG fringes.
            fill = tuple(int(x) for x in np.median(image[fill_mask], axis=0))
            outline_pixels = image[ring & outline_mask]
            outline_median = np.median(outline_pixels, axis=0)
            variation = float(np.median(np.abs(outline_pixels.astype(float) - outline_median).max(axis=1)))
            score *= .4 + .6 * math.exp(-max(0.0, variation - 8) / 8)
            outline = _canonical_outline(tuple(int(x) for x in outline_median))
            separation = float(delta_e(rgb_to_lab(fill), rgb_to_lab(outline)))
            score *= .5 + .5 * min(1.0, separation / 25)
            if score <= best.confidence:
                continue
            best = ColorObservation(fill, outline, score, min(score, inside),
                                    max(0.0, min(1.0, score)), components, reason="spatial_pair")
    return best


def _estimate_line(image: np.ndarray) -> ColorObservation:
    if min(image.shape[:2]) < 5:
        return ColorObservation()
    measured = _enclosed_pair(image)
    if measured.confidence >= .85:
        return measured

    if image.shape[0] > 160:
        step = max(2, image.shape[0] // 90)
        image = image[::step, ::step]
    keys = stroke_colors.quantize(image)
    ranked = stroke_colors.rank_pairs(keys, stroke_colors.CALIBRATION)
    # Confirm orientation with enclosed glyphs if both directions match. A
    # narrow tint next to saturated ink is often chroma spill onto white.
    def evidence(item):
        score, _, fill, outline = item
        if outline is None:
            return 0.
        if min(outline) >= 250:
            score *= 1.08
        if measured.fill is not None and measured.outline is not None:
            df = math.dist(fill, measured.fill)
            do = math.dist(outline, measured.outline)
            score *= 1 + measured.confidence * math.exp(-.5 * ((df / 50) ** 2 + (do / 85) ** 2))
        return score

    match = max(ranked, key=evidence, default=(0., 0, None, None))
    score, support, fill, outline = match
    if measured.fill is not None and measured.outline is not None and measured.confidence >= .65:
        local_score, _ = stroke_colors.stroke_score(keys, measured.fill, measured.outline)
        if local_score > 1.45 * evidence(match):
            return measured
    discovery = stroke_colors.discover_pair(image)
    if discovery is not None:
        proposed, agreement, density = discovery
        # Repeated opposing transitions across the line can identify an ink
        # and a broken one-pixel rim even if neither colour is in the palette.
        strong_profile = agreement >= .18 and density >= .18
        if strong_profile or score < .0035:
            local_score, count = stroke_colors.stroke_score(keys, *proposed)
            near_swatch = False
            if fill is not None:
                df, do = delta_e(rgb_to_lab(proposed), rgb_to_lab([fill, outline]))
                near_swatch = ((df < 9 and do < 15)
                               or (min(df, do) < 9 and stroke_colors.compatible(proposed, fill, outline)))
            if not near_swatch and count >= 3 and (
                    (strong_profile and local_score > max(.003, evidence(match) * 1.25))
                    or local_score > max(.009, evidence(match) * 2.5)):
                score, support = max(local_score, min(.05, .12 * agreement)), count
                fill, outline = proposed
    if score >= .0018 and support >= 3:
        confidence = .48 + .50 * (1 - math.exp(-score / .008))
        return ColorObservation(fill, outline, confidence, confidence, confidence,
                                support, reason="stroke_pair")
    return measured


def estimate_colors(image: np.ndarray, boxes: list[list[list[float]]]) -> ColorObservation:
    """Estimate an ordered colour pair independently in each OCR line/word."""
    observations = []
    height, width = image.shape[:2]
    for box in boxes:
        if not box:
            continue
        points = np.asarray(box)
        if len(points) == 4:
            top_edge, left_edge = points[1] - points[0], points[3] - points[0]
            line_width, line_height = np.linalg.norm(top_edge), np.linalg.norm(left_edge)
            angle = abs(math.degrees(math.atan2(top_edge[1], top_edge[0])))
            if 3 < angle < 45 and line_width > line_height * 1.5 and line_height >= 5:
                # Rectify sloping lines rather than treating their tall bounding
                # rectangle (mostly scenery) as the character height.
                padding = max(3, round(line_height * .08))
                yy, xx = np.mgrid[-padding:int(math.ceil(line_height)) + padding + 1,
                                  -padding:int(math.ceil(line_width)) + padding + 1]
                u, v = xx / line_width, yy / line_height
                coordinates = ((1 - u)[..., None] * (1 - v)[..., None] * points[0]
                               + u[..., None] * (1 - v)[..., None] * points[1]
                               + u[..., None] * v[..., None] * points[2]
                               + (1 - u)[..., None] * v[..., None] * points[3])
                crop = np.stack([ndimage.map_coordinates(image[..., channel],
                                 [coordinates[..., 1], coordinates[..., 0]], order=1, mode="nearest")
                                 for channel in range(3)], axis=-1)
                observations.append(_estimate_line(crop))
                continue
        left, top = np.floor(points.min(axis=0)).astype(int)
        right, bottom = np.ceil(points.max(axis=0)).astype(int)
        padding = max(3, round((bottom - top) * .08))
        crop = image[max(0, top - padding):min(height, bottom + padding + 1),
                     max(0, left - padding):min(width, right + padding + 1)]
        if crop.size:
            observations.append(_estimate_line(crop))
    return fuse_observations(observations, within_frame=True)


def fuse_observations(observations: list[ColorObservation], *, within_frame: bool = False) -> ColorObservation:
    valid = [o for o in observations if o.fill is not None and o.outline is not None and o.confidence > 0]
    if not valid:
        return ColorObservation()
    groups: list[list[ColorObservation]] = []
    for obs in sorted(valid, key=lambda item: (-item.confidence, item.fill, item.outline)):
        nearest = min(groups, key=lambda group: max(pair_distance(obs, x) for x in group), default=None)
        if nearest is not None and max(pair_distance(obs, x) for x in nearest) <= 1:
            nearest.append(obs)
        else:
            groups.append([obs])
    winning = max(groups, key=lambda group: sum(o.confidence ** 2 * (min(o.support, 15) if within_frame else 1)
                                              for o in group))
    # A representative is a real supported pair, rather than an average of
    # faded frames which may never have appeared in the video.
    representative = min(winning, key=lambda o: sum(pair_distance(o, p) * p.confidence for p in winning))
    agreement = sum(o.confidence ** 2 for o in winning) / sum(o.confidence ** 2 for o in valid)
    confidence = representative.confidence * (.65 + .35 * agreement)
    fused = ColorObservation(representative.fill, representative.outline,
                             representative.fill_confidence, representative.outline_confidence,
                             confidence, max(o.support for o in winning),
                             1 if within_frame else len(winning), "spatial_pair" if within_frame else "temporal_pair")
    if not within_frame:
        fused.alternatives = valid
    return fused


@dataclass
class StylePalette:
    fill: RGB
    outline: RGB
    first_cue: int
    members: list[int] = field(default_factory=list)
    name: str = ""


def _cluster_pairs(observations: list[ColorObservation], indices: list[int]) -> list[list[int]]:
    if len(indices) < 2:
        return [indices] if indices else []
    fills = rgb_to_lab([observations[i].fill for i in indices])
    outlines = rgb_to_lab([observations[i].outline for i in indices])
    distances = np.maximum(delta_e(fills[:, None, :], fills[None, :, :]) / 7,
                           delta_e(outlines[:, None, :], outlines[None, :, :]) / 9)
    np.fill_diagonal(distances, 0)
    labels = fcluster(linkage(squareform(distances, checks=False), method="complete"),
                      1.0, criterion="distance")
    groups = {}
    for index, label in zip(indices, labels):
        groups.setdefault(label, []).append(index)
    return list(groups.values())


def _stable_pair(observation: ColorObservation) -> bool:
    return (observation.fill is not None and observation.outline is not None
            and observation.reason == "temporal_pair"
            and observation.frame_count >= 3 and observation.support >= 4)


def build_palettes(observations: list[ColorObservation]) -> tuple[list[StylePalette], list[int | None]]:
    """Complete-link clustering prevents colour bridges and order dependence."""
    assignments: list[int | None] = [None] * len(observations)
    seeds = [i for i, o in enumerate(observations)
             if o.fill is not None and o.outline is not None and o.confidence >= .76
             and (o.frame_count >= 2 or (o.confidence >= .93 and o.support >= 6))]
    groups = _cluster_pairs(observations, seeds)
    # Compression can weaken each frame's spatial evidence for a thin outline.
    # A moderate pair can seed a style if it recurs across frames and distinct cues.
    stable = {i for i, o in enumerate(observations) if _stable_pair(o)}
    repeated = [i for i, o in enumerate(observations) if i in stable
                and .35 <= o.confidence < .76
                and not any(pair_distance(o, observations[j]) <= 1 for j in seeds)]
    groups.extend(members for members in _cluster_pairs(observations, repeated)
                  if len(members) >= 3 and any(observations[i].confidence >= .40 for i in members))
    palettes: list[StylePalette] = []
    for members in groups:
        representative = min(members, key=lambda i: sum(pair_distance(observations[i], observations[j])
                                                       * observations[j].confidence for j in members))
        obs = observations[representative]
        palettes.append(StylePalette(obs.fill, obs.outline, min(members), members))
    palettes.sort(key=lambda p: p.first_cue)
    for p_index, palette in enumerate(palettes):
        palette.name = f"Color_{p_index + 1:02d}"
        for member in palette.members:
            assignments[member] = p_index
    for index, obs in enumerate(observations):
        if assignments[index] is not None or obs.fill is None or obs.confidence < .30:
            continue
        ranked = []
        for p_index, p in enumerate(palettes):
            scores = []
            for candidate in [obs] + obs.alternatives:
                minimum = .30 if candidate is obs and index in stable else .40
                if candidate.fill is None or candidate.outline is None or candidate.confidence < minimum:
                    continue
                df = float(delta_e(rgb_to_lab(candidate.fill), rgb_to_lab(p.fill)))
                do = float(delta_e(rgb_to_lab(candidate.outline), rgb_to_lab(p.outline)))
                distance = max(df / 11, do / 15)
                scores.append(distance + max(0.0, obs.confidence - candidate.confidence) * .5)
            score = min(scores, default=math.inf)
            ranked.append((score, p_index))
        ranked.sort()
        if ranked and ranked[0][0] <= 1 and (len(ranked) == 1 or ranked[1][0] - ranked[0][0] >= .20):
            p_index = ranked[0][1]
            assignments[index] = p_index
            palettes[p_index].members.append(index)
    return palettes, assignments


def assign_by_fill(observations: list[ColorObservation], assignments: list[int | None],
                   palettes: list[StylePalette]) -> set[int]:
    """Reuse an unambiguous, nearby fill only when the outline is missing."""
    matched: set[int] = set()
    if not palettes:
        return matched
    candidates = [i for i, obs in enumerate(observations)
                  if obs.fill is not None and obs.outline is None and obs.fill_confidence >= .27
                  and obs.frame_count >= 3 and obs.support >= 4]
    if not candidates:
        return matched
    fills = rgb_to_lab([observations[i].fill for i in candidates])
    known = rgb_to_lab([palette.fill for palette in palettes])
    distances = delta_e(fills[:, None], known[None, :])
    agreement = delta_e(fills[:, None], fills[None, :]) <= 5
    anchored = np.array([observations[i].fill_confidence >= .40 for i in candidates])
    for row, index in enumerate(candidates):
        if assignments[index] is not None:
            continue
        matches = agreement[row]
        if not anchored[row] and not (np.count_nonzero(matches) >= 4 and np.any(matches & anchored)):
            continue
        order = np.argsort(distances[row])
        palette_index = int(order[0])
        nearest = distances[row, palette_index]
        if nearest > 8 or (len(order) > 1 and distances[row, order[1]] - nearest < 3):
            continue
        assignments[index] = palette_index
        palettes[palette_index].members.append(index)
        matched.add(index)
    return matched


@dataclass
class ColorRequest:
    start_ms: float
    end_ms: float
    boxes: list[list[list[float]]]


def _visibility(image: np.ndarray, boxes: list[list[list[float]]]) -> float:
    scores = []
    for box in boxes:
        points = np.asarray(box)
        left, top = np.maximum(0, np.floor(points.min(axis=0)).astype(int))
        right, bottom = np.ceil(points.max(axis=0)).astype(int)
        crop = image[top:bottom + 1:2, left:right + 1:2].astype(np.float32)
        if min(crop.shape[:2], default=0) < 3:
            continue
        dx = np.abs(crop[1:, 1:] - crop[1:, :-1]).max(axis=2)
        dy = np.abs(crop[1:, 1:] - crop[:-1, 1:]).max(axis=2)
        edge = np.maximum(dx, dy)
        scores.append(float(np.percentile(edge, 90)) + float(np.mean(edge > 70)) * 100)
    return float(np.median(scores)) if scores else 0.0


def _estimate_cue(images, boxes):
    started = time.perf_counter()
    estimates = []
    for image in images:
        estimates.append(estimate_colors(image, boxes))
        if (len(estimates) >= 2 and all(o.confidence >= .72 for o in estimates[:2])
                and pair_distance(estimates[0], estimates[1]) <= .75):
            break
    observation = fuse_observations(estimates)
    return observation, len(estimates), os.getpid(), started, time.perf_counter()


def sample_video_colors(video_path: str, requests: list[ColorRequest], *,
                        budget_seconds: float = 360.0,
                        progress: Callable[[int, int], None] | None = None
                        ) -> tuple[list[ColorObservation], dict]:
    """Read small, ordered windows with one decoder; never run extra OCR.

    This independent raw-colour pass also works when OCR discarded the useful
    middle frames or applied a brightness threshold. The budget is soft: reduce
    retries near it, but never silently drop the unprocessed end of the video.
    """
    import av

    if not math.isfinite(budget_seconds) or budget_seconds <= 0:
        raise ValueError("Colour analysis budget must be a finite positive number")
    started = time.perf_counter()
    observations = [ColorObservation(reason="missing_geometry") for _ in requests]
    stats = {"sampled_frames": 0, "analysed_frames": 0, "decode_seconds": 0.0,
             "estimate_seconds": 0.0, "errors": [], "budget_seconds": budget_seconds}
    workers = min(4, os.cpu_count() or 1)
    pending = {}
    first_completed = os.getenv("FKW_OCR_COLOR_FIRST_COMPLETED", "1") != "0"
    stats.update(queue_wait_seconds=0.0, worker_gap_seconds=0.0)
    last_worker_end = {}
    completed = 0
    pool = None

    def report_completed():
        nonlocal completed
        completed += 1
        if progress and (completed % 25 == 0 or completed == len(requests)):
            progress(completed, len(requests))

    def collect(index, images, boxes, future=None):
        nonlocal pool
        try:
            if future is not None:
                try:
                    result = future.result()
                except BrokenProcessPool:
                    pool = None
                    future = None
            if future is None:
                result = _estimate_cue(images, boxes)
            observations[index], count, pid, began, ended = result
            stats["analysed_frames"] += count
            # Cumulative worker time; the overall "seconds" field is wall time.
            stats["estimate_seconds"] += ended - began
            if pid != os.getpid():
                stats["worker_gap_seconds"] += max(0.0, began - last_worker_end.get(pid, began))
                last_worker_end[pid] = max(last_worker_end.get(pid, ended), ended)
        except (OSError, ValueError) as error:
            observations[index] = ColorObservation(reason="sampling_error")
            stats["errors"].append({"cue": index, "error": str(error)})
        report_completed()

    def collect_ready():
        blocked = time.perf_counter()
        candidates = pending if first_completed else [next(iter(pending))]
        ready, _ = wait(candidates, return_when=FIRST_COMPLETED)
        stats["queue_wait_seconds"] += time.perf_counter() - blocked
        # Submission order makes per-worker timing and diagnostics deterministic.
        for future in list(pending):
            if future in ready:
                collect(*pending.pop(future), future)

    with ExitStack() as resources:
        container = resources.enter_context(av.open(video_path))
        if workers > 1 and sum(bool(request.boxes) for request in requests) >= 4:
            try:
                pool = resources.enter_context(ProcessPoolExecutor(
                    max_workers=workers, mp_context=multiprocessing.get_context("spawn")))
            except (OSError, RuntimeError):
                pool = None
        stream = container.streams.video[0]
        stream.thread_type = "AUTO"
        stream.codec_context.thread_count = 2
        offset_ms = float(container.start_time or 0) / 1000
        iterator = None
        current_ms = -math.inf
        previous_frame = None
        for index in sorted(range(len(requests)), key=lambda i: requests[i].start_ms):
            request = requests[index]
            if not request.boxes:
                report_completed()
                continue
            try:
                elapsed = time.perf_counter() - started
                fractions = (.25, .40, .55, .70, .82) if elapsed < budget_seconds * .85 else (.35, .60)
                duration = max(0.0, request.end_ms - request.start_ms)
                targets = sorted(set(request.start_ms + duration * f + offset_ms for f in fractions))
                points = np.asarray([point for box in request.boxes for point in box])
                x0, y0 = np.maximum(0, np.floor(points.min(axis=0)).astype(int) - 10)
                x1, y1 = np.ceil(points.max(axis=0)).astype(int) + 11
                boxes = [[[x - int(x0), y - int(y0)] for x, y in box] for box in request.boxes]
                candidates = []
                decoded_pts = set()
                decode_started = time.perf_counter()
                for target in targets:
                    if iterator is None or current_ms > target or target - current_ms > 1600:
                        container.seek(max(0, int(target / 1000 / stream.time_base)), stream=stream, backward=True)
                        iterator = iter(container.decode(stream))
                        previous_frame = None
                    selected = None
                    for frame in iterator:
                        if frame.pts is None:
                            continue
                        current_ms = float(frame.pts * stream.time_base) * 1000
                        if current_ms + .01 >= target:
                            selected = frame
                            if previous_frame is not None:
                                previous_ms = float(previous_frame.pts * stream.time_base) * 1000
                                if (previous_ms >= request.start_ms + offset_ms - .01
                                        and (current_ms >= request.end_ms + offset_ms - .01
                                             or target - previous_ms < current_ms - target)):
                                    selected = previous_frame
                            previous_frame = frame
                            break
                        previous_frame = frame
                    if selected is None:
                        selected = previous_frame
                    if selected is None or selected.pts in decoded_pts:
                        continue
                    # Seek rounding can move beyond a very short cue. Never
                    # borrow the next speaker's pixels to colour this one.
                    selected_ms = float(selected.pts * stream.time_base) * 1000
                    if (selected_ms >= request.end_ms + offset_ms - .01
                            or selected_ms < request.start_ms + offset_ms - .01):
                        continue
                    decoded_pts.add(selected.pts)
                    image = selected.to_ndarray(format="rgb24")[y0:y1, x0:x1].copy()
                    if image.size:
                        quality = _visibility(image, boxes)
                        candidates.append((quality, abs(target - (request.start_ms + duration / 2 + offset_ms)), image))
                        stats["sampled_frames"] += 1
                stats["decode_seconds"] += time.perf_counter() - decode_started
                candidates.sort(key=lambda c: (-c[0], c[1]))
            except (av.error.FFmpegError, OSError, ValueError) as error:
                observations[index] = ColorObservation(reason="sampling_error")
                stats["errors"].append({"cue": index, "error": str(error)})
                iterator = None
                report_completed()
                continue

            images = [item[2] for item in candidates]
            future = None
            if pool is not None:
                try:
                    future = pool.submit(_estimate_cue, images, boxes)
                except (OSError, RuntimeError):
                    pool = None
            if future is None:
                collect(index, images, boxes)
                continue
            pending[future] = (index, images, boxes)
            # Keep decoding sequential; bound the number of RGB crops held for IPC/retry.
            if len(pending) >= workers + 1:
                collect_ready()
        while pending:
            collect_ready()
    stats["seconds"] = time.perf_counter() - started
    stats["over_budget"] = stats["seconds"] > budget_seconds
    return observations, stats
