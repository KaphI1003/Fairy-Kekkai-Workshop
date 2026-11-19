from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import av
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from videocr import color_styles as colors
from videocr import api
from videocr.video import Video


def observation(fill, outline=(255, 255, 255), confidence=.95, frames=2):
    return colors.ColorObservation(fill, outline, confidence, confidence, confidence, 8, frames)


def lettering(fill, outline, background=(40, 40, 40), border=3):
    image = np.full((64, 260, 3), background, dtype=np.uint8)
    for left in range(16, 240, 45):
        image[17:47, left:left + 26] = outline
        image[17 + border:47 - border, left + border:left + 26 - border] = fill
    return image, [[[13, 14], [245, 14], [245, 50], [13, 50]]]


class PerceptualDistanceTest(unittest.TestCase):
    def test_ciede2000_published_reference_pairs(self):
        # Sharma, Wu & Dalal CIEDE2000 supplementary test data.
        for left, right, expected in [
            ([50, 2.6772, -79.7751], [50, 0, -82.7485], 2.0425),
            ([50, 3.1571, -77.2803], [50, 0, -82.7485], 2.8615),
            ([50, 0, 0], [50, -1, 2], 2.3669),
        ]:
            self.assertAlmostEqual(float(colors.delta_e(left, right)), expected, places=4)
            self.assertAlmostEqual(float(colors.delta_e(right, left)), expected, places=4)

    def test_rgb_black_white_and_vectorisation(self):
        labs = colors.rgb_to_lab([[0, 0, 0], [255, 255, 255]])
        np.testing.assert_allclose(labs[:, 0], [0, 100], atol=.001)
        np.testing.assert_allclose(colors.delta_e(labs, labs), [0, 0], atol=1e-10)


class SpatialColourTest(unittest.TestCase):
    def test_dark_green_fill_is_not_rejected_by_brightness(self):
        image, boxes = lettering((8, 67, 10), (255, 255, 255))
        obs = colors.estimate_colors(image, boxes)
        self.assertEqual(obs.fill, (8, 67, 10))
        self.assertEqual(obs.outline, (255, 255, 255))

    def test_fill_and_outline_are_directional(self):
        for fill, outline in [((255, 255, 255), (220, 0, 0)),
                              ((220, 0, 0), (255, 255, 255))]:
            image, boxes = lettering(fill, outline)
            obs = colors.estimate_colors(image, boxes)
            self.assertEqual((obs.fill, obs.outline), (fill, outline))

    def test_same_hue_outline_is_allowed(self):
        image, boxes = lettering((10, 0, 250), (125, 125, 255))
        obs = colors.estimate_colors(image, boxes)
        self.assertEqual(obs.fill, (10, 0, 250))
        self.assertEqual(obs.outline, (125, 125, 255))

    def test_background_matching_fill_does_not_reverse_the_pair(self):
        image, boxes = lettering((250, 175, 0), (255, 255, 255), background=(250, 175, 0))
        obs = colors.estimate_colors(image, boxes)
        self.assertEqual(obs.fill, (250, 175, 0))
        self.assertEqual(obs.outline, (255, 255, 255))

    def test_plain_background_has_no_invented_outline(self):
        image = np.full((60, 200, 3), (90, 110, 120), dtype=np.uint8)
        obs = colors.estimate_colors(image, [[[0, 0], [199, 0], [199, 59], [0, 59]]])
        self.assertIsNone(obs.fill)
        self.assertIsNone(obs.outline)
        self.assertEqual(obs.confidence, 0)

    def test_textured_background_cannot_outvote_yellow_letter_interiors(self):
        image = np.repeat(np.array([95, 145, 170, 120], dtype=np.uint8), 80)
        image = np.tile(image, (90, 1))
        image = np.repeat(image[..., None], 3, axis=2)
        mask = np.zeros(image.shape[:2], dtype=bool)
        for left in (20, 110, 200):
            mask[20:70, left:left + 5] = True
            mask[20:70, left + 40:left + 45] = True
            for top in (20, 40, 65):
                mask[top:top + 5, left:left + 45] = True
        stroke = colors.ndimage.binary_dilation(mask, structure=np.ones((3, 3)), iterations=4)
        image[stroke] = (0, 0, 0)
        image[mask] = (250, 250, 0)
        obs = colors.estimate_colors(image, [[[10, 10], [260, 10], [260, 80], [10, 80]]])
        self.assertEqual(obs.fill, (250, 250, 0))
        self.assertEqual(obs.outline, (0, 0, 0))

    def test_one_pixel_strokes_survive_chroma_subsampling(self):
        h, w = 62, 255
        y, x = np.mgrid[:h, :w]
        background = np.stack((70 + x % 60, 90 + y // 2, 115 + x % 70), axis=2).astype(np.uint8)
        glyph = np.zeros((h, w), dtype=bool)
        for left in range(12, 230, 31):
            glyph[12:46, left:left + 4] = True
            glyph[16:20, left:left + 20] = True
            glyph[32:36, left:left + 24] = True
            glyph[12:45, left + 17:left + 21] = True
        for fill, outline in [((241, 8, 11), (0, 4, 0)), ((241, 8, 11), (252, 252, 252)),
                              ((42, 200, 168), (99, 32, 140))]:
            with self.subTest(fill=fill, outline=outline):
                image = background.copy()
                image[colors.ndimage.binary_dilation(glyph)] = outline
                image[glyph] = fill
                frame = av.VideoFrame.from_ndarray(image, format="rgb24").reformat(format="yuv420p")
                sample = colors.estimate_colors(frame.to_ndarray(format="rgb24"),
                                                [[[2, 4], [252, 4], [252, 55], [2, 55]]])
                error = colors.delta_e(colors.rgb_to_lab([sample.fill, sample.outline]),
                                       colors.rgb_to_lab([fill, outline]))
                self.assertLess(error[0], 10)
                self.assertLess(error[1], 18)

    def test_sloping_line_uses_its_polygon_instead_of_scenery(self):
        letters, boxes = lettering((8, 130, 130), (255, 255, 255))
        image = np.full((200, 400, 3), 40, dtype=np.uint8)
        image[68:132, 70:330] = letters
        rotated = colors.ndimage.rotate(image, -12, reshape=False, order=0)
        theta = np.radians(12)
        rotation = np.array([[np.cos(theta), -np.sin(theta)], [np.sin(theta), np.cos(theta)]])
        polygon = (np.asarray(boxes[0]) + [70, 68] - [200, 100]) @ rotation.T + [200, 100]
        obs = colors.estimate_colors(rotated, [polygon.tolist()])
        np.testing.assert_allclose(obs.fill, (8, 130, 130), atol=8)
        self.assertEqual(obs.outline, (255, 255, 255))


class PaletteTest(unittest.TestCase):
    def test_rgb_bin_boundary_does_not_split_the_same_style(self):
        palettes, assigned = colors.build_palettes([
            observation((127, 30, 90)), observation((129, 31, 91)), observation((128, 32, 90))])
        self.assertEqual(len(palettes), 1)
        self.assertEqual(assigned, [0, 0, 0])

    def test_same_fill_with_different_outlines_remains_distinct(self):
        palettes, assigned = colors.build_palettes([
            observation((230, 120, 220)), observation((230, 120, 220), (0, 0, 0))])
        self.assertEqual(len(palettes), 2)
        self.assertNotEqual(*assigned)

    def test_swapped_pairs_are_not_equivalent(self):
        palettes, _ = colors.build_palettes([
            observation((255, 255, 255), (220, 0, 0)),
            observation((220, 0, 0), (255, 255, 255))])
        self.assertEqual(len(palettes), 2)

    def test_low_confidence_cannot_create_or_move_a_style(self):
        good = observation((20, 130, 130))
        noisy = observation((29, 143, 132), (236, 242, 239), .4)
        palettes, assigned = colors.build_palettes([good, noisy, observation((200, 20, 20), confidence=.2)])
        self.assertEqual(len(palettes), 1)
        self.assertEqual((palettes[0].fill, palettes[0].outline), (good.fill, good.outline))
        self.assertIsNone(assigned[2])

    def test_single_cue_with_two_good_frames_keeps_rare_style(self):
        palettes, assigned = colors.build_palettes([observation((120, 230, 240), (50, 60, 70))])
        self.assertEqual(len(palettes), 1)
        self.assertEqual(assigned, [0])

    def test_thin_outline_can_seed_a_style_when_it_recurs_across_cues(self):
        samples = [
            colors.ColorObservation((253, 254, 253), (30, 64 + i, 28),
                                    .48, .46, .45, 10, 4, "temporal_pair")
            for i in range(3)
        ]
        self.assertFalse(colors.build_palettes(samples[:2])[0])
        matching = colors.ColorObservation((253, 254, 253), (31, 65, 28),
                                           .35, .31, .32, 12, 5, "temporal_pair")
        one_frame = colors.ColorObservation((253, 254, 253), (31, 65, 28),
                                            .35, .31, .32, 12, 1, "temporal_pair")
        palettes, assigned = colors.build_palettes([*samples, matching, one_frame])
        self.assertEqual(len(palettes), 1)
        self.assertEqual(assigned, [0, 0, 0, 0, None])

    def test_complete_link_does_not_merge_a_colour_bridge(self):
        samples = [observation((v, v, v), (0, 0, 0)) for v in (140, 160, 180)]
        self.assertLess(colors.pair_distance(samples[0], samples[1]), 1)
        self.assertLess(colors.pair_distance(samples[1], samples[2]), 1)
        self.assertGreater(colors.pair_distance(samples[0], samples[2]), 1)
        palettes, assigned = colors.build_palettes(samples)
        self.assertEqual(len(palettes), 2)
        self.assertNotEqual(assigned[0], assigned[2])

    def test_known_fill_without_an_outline_uses_the_closest_existing_style(self):
        samples = [observation((34, 100, 180)), observation((215, 166, 107)),
                   colors.ColorObservation((236, 188, 133), None, .65, 0, .32, 10, 4),
                   colors.ColorObservation((236, 188, 133), None, .22, 0, .22, 10, 4)]
        palettes, assigned = colors.build_palettes(samples)
        self.assertEqual(colors.assign_by_fill(samples, assigned, palettes), {2})
        self.assertEqual(assigned, [0, 1, 1, None])
        self.assertEqual(palettes[1].members, [1, 2])
        self.assertEqual(colors.assign_by_fill(samples, assigned, []), set())

    def test_repeated_fill_can_use_an_existing_style_without_a_reliable_outline(self):
        samples = [observation((215, 166, 107))]
        samples += [colors.ColorObservation((236, 188, 133), None, score, 0, score, 10, 4)
                    for score in (.28, .32, .37, .43)]
        palettes, assigned = colors.build_palettes(samples)
        self.assertEqual(colors.assign_by_fill(samples, assigned, palettes), {1, 2, 3, 4})

    def test_fill_fallback_rejects_ambiguous_far_and_contradictory_pairs(self):
        red = (250, 0, 0)
        partial = colors.ColorObservation(red, None, .9, 0, .4, 12, 5)
        distant = colors.ColorObservation((0, 250, 0), None, .9, 0, .4, 12, 5)
        conflicting = observation(red, (0, 0, 220), confidence=.65, frames=5)
        samples = [observation(red), observation(red, (0, 0, 0)), partial, distant, conflicting]
        palettes, assigned = colors.build_palettes(samples)
        self.assertEqual(colors.assign_by_fill(samples, assigned, palettes), set())
        self.assertEqual(assigned[2:], [None, None, None])

    def test_fade_samples_do_not_average_down_the_supported_pair(self):
        opaque = observation((250, 250, 0), (0, 0, 0))
        faded = observation((170, 170, 50), (50, 50, 50), .45)
        result = colors.fuse_observations([opaque, opaque, faded])
        self.assertEqual((result.fill, result.outline), (opaque.fill, opaque.outline))
        self.assertEqual(result.frame_count, 2)

    def test_missing_outline_does_not_guess_between_equal_fills(self):
        partial = colors.ColorObservation((230, 120, 220), None, .8, 0, .4)
        _, assigned = colors.build_palettes([
            observation((230, 120, 220)), observation((230, 120, 220), (0, 0, 0)), partial])
        self.assertIsNone(assigned[2])


class RawSamplingTest(unittest.TestCase):
    def test_invalid_budget_is_rejected_before_opening_video(self):
        for value in (0, -1, float('nan'), float('inf')):
            with self.assertRaises(ValueError):
                colors.sample_video_colors('unused.mp4', [], budget_seconds=value)

    def test_final_frame_is_available_when_there_is_no_later_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'last.mp4')
            with av.open(path, 'w') as container:
                stream = container.add_stream('mpeg4', rate=30)
                stream.width, stream.height = 64, 64
                stream.pix_fmt = 'yuv420p'
                frame = av.VideoFrame.from_ndarray(np.full((64, 64, 3), 120, dtype=np.uint8), format='rgb24')
                for packet in stream.encode(frame):
                    container.mux(packet)
                for packet in stream.encode():
                    container.mux(packet)
            request = colors.ColorRequest(0, 33.333, [[[5, 5], [55, 5], [55, 55], [5, 55]]])
            with patch.object(colors, 'estimate_colors', return_value=observation((20, 130, 130))) as estimate:
                result, stats = colors.sample_video_colors(path, [request])
            self.assertEqual(stats['sampled_frames'], 1)
            estimate.assert_called_once()

    def test_short_cue_never_borrows_the_next_speakers_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'short.mp4')
            with av.open(path, 'w') as container:
                stream = container.add_stream('mpeg4', rate=30)
                stream.width, stream.height = 64, 64
                stream.pix_fmt = 'yuv420p'
                for index in range(3):
                    rgb = (230, 20, 20) if index == 0 else (20, 20, 230)
                    frame = av.VideoFrame.from_ndarray(np.full((64, 64, 3), rgb, dtype=np.uint8), format='rgb24')
                    for packet in stream.encode(frame):
                        container.mux(packet)
                for packet in stream.encode():
                    container.mux(packet)
            seen = []
            def estimate(image, boxes):
                seen.append(image[30, 30].copy())
                return observation((230, 20, 20))
            request = colors.ColorRequest(0, 33.333, [[[5, 5], [55, 5], [55, 55], [5, 55]]])
            with patch.object(colors, 'estimate_colors', side_effect=estimate):
                result, stats = colors.sample_video_colors(path, [request])
            self.assertFalse(stats['errors'])
            self.assertGreater(len(seen), 0)
            for pixel in seen:
                self.assertGreater(int(pixel[0]), int(pixel[2]))

    def test_disabling_colour_styles_does_not_sample_video(self):
        video = Video.__new__(Video)
        video.pred_subs = []
        video.width, video.height = 1280, 720
        with patch.object(video, '_sample_cue_colors') as sample:
            ass = video.get_ass(color_styles=False)
        sample.assert_not_called()
        self.assertIn('Style: Default', ass)

    def test_failed_style_analysis_preserves_an_existing_ass(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'ocr.srt'
            ass = output.with_suffix('.ass')
            ass.write_text('existing reviewed styles', encoding='utf-8')
            with (patch.object(api.utils, 'perform_hardware_check'),
                  patch.object(api.utils, 'resolve_model_dirs', return_value=('', '', '')),
                  patch.object(api, 'Video') as video_class):
                video = video_class.return_value
                video.get_subtitles.return_value = '1\n00:00:00,000 --> 00:00:01,000\ntext\n'
                video.get_ass.side_effect = RuntimeError('style analysis interrupted')
                with self.assertRaisesRegex(RuntimeError, 'interrupted'):
                    api.save_subtitles_to_file('video.mp4', str(output), paddleocr_path='ocr.exe')
                video.cleanup.assert_called_once()
            self.assertEqual(ass.read_text(encoding='utf-8'), 'existing reviewed styles')

    def test_no_ass_skips_the_style_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            with (patch.object(api.utils, 'perform_hardware_check'),
                  patch.object(api.utils, 'resolve_model_dirs', return_value=('', '', '')),
                  patch.object(api, 'Video') as video_class):
                video = video_class.return_value
                video.get_subtitles.return_value = ''
                api.save_subtitles_to_file('video.mp4', str(Path(directory) / 'ocr.srt'),
                                           paddleocr_path='ocr.exe', write_ass=False)
                video.get_ass.assert_not_called()


if __name__ == '__main__':
    unittest.main()
