"""Compare ordered ink/stroke hypotheses directly against native-resolution glyphs."""
from functools import lru_cache

import numpy as np
from scipy import ndimage


# Colour swatches from the two manually corrected ASS palettes. Each is a
# hypothesis about the pixels in a crop, independent of cue text, time and file.
# Near-identical red/white swatches share one entry.
CALIBRATION = (
    ((254, 254, 254), (31, 71, 28)), ((237, 188, 132), (37, 49, 89)),
    ((253, 0, 0), (255, 255, 255)), ((255, 0, 1), (253, 232, 220)),
    ((253, 253, 0), (75, 63, 65)), ((127, 254, 255), (18, 34, 53)),
    ((238, 241, 183), (46, 21, 45)), ((250, 0, 2), (0, 11, 0)),
    ((0, 11, 0), (78, 207, 62)), ((255, 255, 255), (222, 58, 78)),
    ((194, 230, 28), (81, 122, 15)), ((255, 255, 255), (147, 70, 151)),
    ((244, 251, 12), (133, 143, 16)), ((0, 11, 0), (219, 231, 111)),
    ((246, 137, 0), (245, 222, 86)), ((129, 126, 217), (247, 226, 249)),
    ((13, 2, 132), (200, 59, 211)), ((15, 0, 248), (154, 11, 76)),
    ((243, 10, 237), (255, 104, 246)), ((13, 21, 232), (31, 99, 34)),
    ((234, 245, 56), (24, 25, 204)), ((224, 254, 207), (42, 124, 38)),
    ((255, 255, 255), (0, 11, 0)),
)


@lru_cache(maxsize=128)
def _likelihood(colour, neighbour):
    """Allow limited chroma spill between adjacent ink and outline in 4:2:0 video."""
    grid = np.indices((32, 32, 32), dtype=np.float32).reshape(3, -1).T * 8 + 3.5
    offset = grid - np.asarray(colour, dtype=np.float32)
    direction = np.subtract(neighbour, colour, dtype=np.float32)
    fraction = (np.einsum("ij,j->i", offset, direction) / max(1., float(direction @ direction))).clip(0, .43)
    residual = offset - fraction[:, None] * direction
    distance = (residual * residual).sum(axis=1) / (30. ** 2) + (fraction / .32) ** 2
    return np.exp(-.5 * distance)


def quantize(image):
    bins = image.astype(np.int32) >> 3
    return (bins[..., 0] * 32 + bins[..., 1]) * 32 + bins[..., 2]


def compatible(measured, fill, outline):
    """Check whether both measured colours can arise from the proposed pair."""
    keys = quantize(np.asarray(measured, dtype=np.uint8))
    return (_likelihood(fill, outline)[keys[0]] >= .08
            and _likelihood(outline, fill)[keys[1]] >= .08)


def stroke_score(keys, fill, outline):
    """Count ink supported by the specified outline on opposite sides of a stroke."""
    ink = _likelihood(fill, outline)[keys]
    edge = _likelihood(outline, fill)[keys]
    if ink.max() < .20 or edge.max() < .12:
        return 0., 0
    core = np.maximum(0, ink - edge)
    rim = np.maximum(0, edge - ink)
    radius = min(9, max(3, round(keys.shape[0] * .09)))
    enclosed = np.zeros_like(core)
    for axis in (0, 1):
        left = ndimage.maximum_filter1d(rim, radius, axis=axis, origin=(radius - 1) // 2, mode="constant")
        right = ndimage.maximum_filter1d(rim, radius, axis=axis, origin=-(radius // 2), mode="constant")
        # Do not wrap the two ends of a cropped word together.
        left = np.moveaxis(left, axis, 0)
        right = np.moveaxis(right, axis, 0)
        if len(left) > 2:
            target = np.moveaxis(enclosed, axis, 0)[1:-1]
            np.maximum(target, left[:-2] * right[2:], out=target)
    np.sqrt(enclosed, out=enclosed)
    strength = core * enclosed
    # Nearby blocks of scenery can contain both colours without forming a
    # glyph. Compare with the overlap after horizontally misaligning the rim.
    shift = min(core.shape[1] // 3, max(3, keys.shape[0] // 3))
    overlap = float(strength.sum())
    chance = float((core * np.roll(enclosed, shift, axis=1)).sum())
    lift = max(.45, 1 - .75 * chance / max(.001, overlap))
    # Cap the contribution of a local patch of similarly coloured scenery.
    counts = strength.sum(axis=0)
    blocks = min(12, len(counts))
    starts = np.arange(blocks) * len(counts) // blocks
    sums = np.add.reduceat(counts, starts)
    spatial_support = np.sqrt(sums).mean() ** 2 * blocks
    support = int(np.count_nonzero(sums > .12))
    return float(spatial_support * lift / keys.size), support


def discover_pair(image):
    """Find a repeated colour transition across short strokes, without swatches."""
    samples, weights, positions = [], [], []
    h, width = image.shape[:2]
    for axis, scan in enumerate((image, image.transpose(1, 0, 2))):
        pixels = scan.astype(np.float32)
        gradient = np.diff(pixels, axis=1)
        magnitude = np.linalg.norm(gradient, axis=2)
        peaks = (magnitude >= 18) & (magnitude == ndimage.maximum_filter1d(magnitude, 3, axis=1))
        row, edge = np.where(peaks)
        left, right = edge[:-1], edge[1:]
        valid = ((row[:-1] == row[1:]) & (right - left >= 2)
                 & (right - left <= max(8, h * .28)) & (left >= 3) & (right < scan.shape[1] - 4))
        row, left, right = row[:-1][valid], left[valid], right[valid]
        mid = (left + right + 1) // 2
        ink = pixels[row, mid]
        opposed = -np.sum(gradient[row, left] * gradient[row, right], axis=1)
        opposed /= magnitude[row, left] * magnitude[row, right]

        def flank(indices, direction):
            strip = pixels[row[:, None], indices[:, None] + direction * np.arange(3)]
            distance = np.linalg.norm(strip - ink[:, None], axis=2)
            reach = np.cumprod(np.diff(distance, axis=1) > 4, axis=1).sum(axis=1)
            return strip[np.arange(len(row)), reach]

        a, b = flank(left, -1), flank(right + 1, 1)
        rim = (a + b) / 2
        contrast = np.linalg.norm(ink - rim, axis=1)
        valid = (opposed > .5) & (contrast > 40)
        weight = opposed * np.minimum(1., contrast / 100) * np.exp(-np.linalg.norm(a - b, axis=1) / 28)
        samples.append(np.column_stack((ink, rim))[valid])
        weights.append(weight[valid])
        positions.append((row if axis else mid)[valid])
    colours = np.concatenate(samples)
    if len(colours) < 8:
        return None
    weight = np.concatenate(weights)
    block = np.concatenate(positions) * 8 // width
    keys = (colours.astype(np.int32) // 32) @ (8 ** np.arange(6))
    _, indices = np.unique(keys, return_inverse=True)
    histogram = np.bincount(indices, weights=weight)
    best_strength, best_pair = 0., None
    for index in np.argsort(histogram)[-8:]:
        group = indices == index
        seed = np.average(colours[group], weights=weight[group], axis=0)
        difference = np.abs(colours - seed).reshape(-1, 2, 3).max(axis=2)
        nearby = weight * np.exp(-.5 * ((difference / [18., 22.]) ** 2).sum(axis=1))
        counts = np.bincount(block, weights=nearby, minlength=8)
        strength = np.sqrt(counts).mean() ** 2 * 8
        if strength > max(4., best_strength) and np.count_nonzero(counts > .6) >= 3:
            colour = np.average(colours, weights=nearby, axis=0).round().astype(int)
            best_pair = tuple(map(int, colour[:3])), tuple(map(int, colour[3:]))
            best_strength = strength
    if best_pair is None:
        return None
    return best_pair, best_strength / max(1., weight.sum()), best_strength / width


def rank_pairs(keys, pairs):
    ranked = []
    for fill, outline in pairs:
        score, count = stroke_score(keys, fill, outline)
        if score > 0:
            ranked.append((score, count, fill, outline))
    return sorted(ranked, reverse=True)
