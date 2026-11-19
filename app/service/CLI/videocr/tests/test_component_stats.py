from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from videocr import utils


def brute_force_components(mask: np.ndarray, edges: np.ndarray):
    height, width = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    components = []
    for y in range(height):
        for x in range(width):
            if not mask[y, x] or seen[y, x]:
                continue
            stack = [(y, x)]
            seen[y, x] = True
            points = []
            border = False
            while stack:
                cy, cx = stack.pop()
                points.append((cy, cx))
                if cy == 0 or cx == 0 or cy == height - 1 or cx == width - 1:
                    border = True
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dy == 0 and dx == 0:
                            continue
                        ny, nx = cy + dy, cx + dx
                        if (
                            0 <= ny < height
                            and 0 <= nx < width
                            and mask[ny, nx]
                            and not seen[ny, nx]
                        ):
                            seen[ny, nx] = True
                            stack.append((ny, nx))
            density = float(sum(bool(edges[y, x]) for y, x in points)) / len(points)
            components.append((len(points), border, round(density, 12)))
    return sorted(components)


class ComponentStatsTest(unittest.TestCase):
    def test_matches_bruteforce_on_random_masks(self) -> None:
        rng = np.random.default_rng(20261117)
        for rows, cols in ((9, 13), (12, 7), (17, 19)):
            for density in (0.05, 0.2, 0.45, 0.7):
                mask = rng.random((rows, cols)) < density
                edges = rng.random((rows, cols)) < 0.35
                labels, areas, border, density_arr = utils._component_stats(mask, edges)
                expected = brute_force_components(mask, edges)
                actual = sorted(
                    (int(area), bool(is_border), round(float(d), 12))
                    for area, is_border, d in zip(areas, border, density_arr)
                )
                self.assertEqual(
                    actual,
                    expected,
                    msg=f"mask {rows}x{cols} density={density}",
                )

    def test_diagonal_pixels_are_one_eight_connected_component(self) -> None:
        mask = np.zeros((4, 4), dtype=bool)
        mask[1, 1] = True
        mask[2, 2] = True
        labels, areas, border, density = utils._component_stats(
            mask, np.zeros_like(mask, dtype=bool)
        )
        self.assertEqual(len(areas), 1)
        self.assertEqual(int(areas[0]), 2)
        self.assertFalse(bool(border[0]))

    def test_component_touching_edge_is_marked_border(self) -> None:
        mask = np.zeros((5, 6), dtype=bool)
        mask[0, 2] = True
        mask[0, 3] = True
        labels, areas, border, density = utils._component_stats(
            mask, np.ones_like(mask, dtype=bool)
        )
        self.assertEqual(len(areas), 1)
        self.assertTrue(bool(border[0]))

    def test_empty_mask_returns_no_components(self) -> None:
        mask = np.zeros((6, 6), dtype=bool)
        labels, areas, border, density = utils._component_stats(
            mask, np.zeros_like(mask, dtype=bool)
        )
        self.assertEqual(len(areas), 0)
        self.assertEqual(len(border), 0)
        self.assertEqual(len(density), 0)


class FillHolesTest(unittest.TestCase):
    def test_matches_scipy_binary_fill_holes_on_random_masks(self) -> None:
        rng = np.random.default_rng(20261118)
        for shape in ((7, 9), (15, 20), (31, 17)):
            for density in (0.1, 0.3, 0.5, 0.7):
                mask = rng.random(shape) < density
                np.testing.assert_array_equal(
                    utils._fill_holes(mask),
                    ndimage.binary_fill_holes(mask),
                    err_msg=f"shape={shape} density={density}",
                )

    def test_interior_hole_is_filled(self) -> None:
        mask = np.zeros((5, 5), dtype=bool)
        mask[1:4, 1:4] = True
        mask[2, 2] = False
        filled = utils._fill_holes(mask)
        self.assertTrue(bool(filled[2, 2]))

    def test_border_bay_is_not_filled(self) -> None:
        mask = np.zeros((5, 5), dtype=bool)
        mask[0, 2] = True
        mask[1, 1:4] = True
        filled = utils._fill_holes(mask)
        self.assertFalse(bool(filled[0, 1]))
        self.assertFalse(bool(filled[0, 3]))


if __name__ == "__main__":
    unittest.main()
