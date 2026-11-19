import unittest

from videocr.models import PredictedFrames, PredictedSubtitle
from videocr.video import Video


def subtitle(index, rows, *, zone=0, end=None, advance=None):
    predictions = []
    for top, bottom, text in rows:
        width_per_character = advance if advance is not None else bottom - top
        right = 100 + len(text) * width_per_character
        predictions.append([[[100, top], [right, top], [right, bottom], [100, bottom]], (text, .98)])
    frame = PredictedFrames("paddleocr", index, [predictions], .75, zone, "ja", False)
    frame.end_index = index + 14 if end is None else end
    return PredictedSubtitle([frame], zone, 80, "ja", None)


def make_video(cues, height=1080):
    video = Video.__new__(Video)
    video.width, video.height = round(height * 16 / 9), height
    video.validated_zones = [{"crop_str": f"{video.width}:{height}:0:0",
                              "h": 1080, "w": 1920, "midpoint_y": height / 2}]
    video.pred_subs = cues
    video._merged_ass_sources = {}
    video.fps = 30
    video.frame_base = 0
    video.start_time_offset_ms = 0
    video.avg_frame_duration_ms = 1000 / 30
    video.frame_timestamps = {}
    return video


def ass_fields(text, prefix):
    return [line.removeprefix(prefix).split(",", 9 if prefix == "Dialogue: " else -1)
            for line in text.splitlines() if line.startswith(prefix)]


class SubtitleLayoutTest(unittest.TestCase):
    def test_dominant_style_with_multiline_position_and_large_font_overrides(self):
        ordinary = [subtitle(i * 30, [(900 + i % 2, 948 + i % 2, "字幕")]) for i in range(5)]
        multiline = subtitle(150, [(840, 888, "一行目"), (900, 948, "二行目")])
        large = subtitle(180, [(900, 977, "大きな字幕")])
        clipped = subtitle(210, [(1020, 1080, "字幕")])
        video = make_video([*ordinary, multiline, large, clipped])
        ass = video.get_ass(color_styles=False)
        style = ass_fields(ass, "Style: ")[0]
        events = ass_fields(ass, "Dialogue: ")

        self.assertEqual(int(style[2]), 53)
        self.assertTrue(183 <= int(style[21]) <= 189)
        self.assertTrue(all(event[7] == "0" and not event[9].startswith("{")
                            for event in events[:5]))
        self.assertGreater(int(events[5][7]), int(style[21]) + 50)
        self.assertEqual(events[5][9], r"一行目\N二行目")
        self.assertIn(r"\fs91", events[6][9])
        self.assertEqual((events[7][7], events[7][9]), ("0", "字幕"))

    def test_independent_switches(self):
        video = make_video([subtitle(0, [(900, 948, "字幕")])])
        for position in (False, True):
            for fontsize in (False, True):
                with self.subTest(position=position, fontsize=fontsize):
                    ass = video.get_ass(color_styles=False, position_styles=position,
                                        font_size_styles=fontsize)
                    style = ass_fields(ass, "Style: ")[0]
                    self.assertEqual(float(style[2]), 53 if fontsize else 80)
                    self.assertEqual(int(style[21]) == 72, not position)
                    self.assertEqual(ass_fields(ass, "Dialogue: ")[0][9], "字幕")

    def test_height_noise_inherits_style_but_quiet_and_moved_dialogue_keep_overrides(self):
        # The same baseline and glyph advance can produce taller OCR boxes
        # around short words. These 720p samples are scaled to the fixture's coordinates.
        ordinary = [(636, 686, 48), (637, 684, 48), (627, 694, 50),
                    (628, 694, 47), (625, 693, 51), (629, 705, 58)]
        subtitles = [
            subtitle(i * 30, [(top * 1.5, bottom * 1.5, "普通字幕")], advance=advance * 1.5)
            for i, (top, bottom, advance) in enumerate(ordinary)
        ]
        subtitles += [
            subtitle(180, [(652 * 1.5, 687 * 1.5, "静かにしてね")], advance=27 * 1.5),
            subtitle(210, [(596 * 1.5, 646 * 1.5, "位置が違う")], advance=48 * 1.5),
        ]
        events = ass_fields(make_video(subtitles, 720).get_ass(color_styles=False), "Dialogue: ")
        self.assertTrue(all(event[7] == "0" and r"\fs" not in event[9] for event in events[:6]))
        self.assertNotEqual(events[6][7], "0")
        self.assertIn(r"\fs", events[6][9])
        self.assertNotEqual(events[7][7], "0")
        self.assertNotIn(r"\fs", events[7][9])

    def test_repeated_larger_color_style_gets_its_own_default(self):
        from videocr import subtitle_layout

        cues = []
        for i in range(12):
            height = 48 + i % 2 if i < 8 else 75 + i % 3
            sub = subtitle(i * 30, [(900, 900 + height, "字幕の例")], advance=height * .9)
            cues.append(subtitle_layout.AssCue(sub, (subtitle_layout.LineBox(900, 900 + height,
                                                                            height, height * .9),)))
        styles = ["A"] * 4 + ["B"] * 4 + ["C"] * 4
        defaults, events = subtitle_layout.estimate_layout(
            cues, styles, video_height=1080, base_font=80, base_margin=72, outline=3.5,
            shadow=1, position=True, font_size=True, gap_1080p=8,
        )
        self.assertEqual(defaults["A"].font_size, defaults["B"].font_size)
        self.assertGreater(defaults["C"].font_size, defaults["A"].font_size + 20)
        self.assertTrue(all(event.font_size is None for event in events))

    def test_proportional_change_in_height_and_advance_preserves_a_smaller_font(self):
        ordinary = [subtitle(i * 30, [(900, 960, "普通字幕")], advance=52) for i in range(5)]
        smaller = subtitle(150, [(912, 961, "静かな字幕")], advance=42)
        padded = subtitle(180, [(882, 961, "普通字幕")], advance=55)
        ass = make_video([*ordinary, smaller, padded]).get_ass(color_styles=False)
        events = ass_fields(ass, "Dialogue: ")
        self.assertIn(r"\fs55", events[5][9])
        self.assertNotIn(r"\fs", events[6][9])

    def test_gap_scales_and_supports_both_endpoints(self):
        for height in (720, 1080, 2160):
            with self.subTest(height=height):
                video = make_video([subtitle(0, [(900, 948, "字幕")])], height)
                margins = []
                for gap in (0, 8, 100):
                    ass = video.get_ass(color_styles=False, gap_1080p=gap)
                    margins.append(int(ass_fields(ass, "Style: ")[0][21]))
                self.assertAlmostEqual(margins[1] - margins[0], 8 * height / 1080, delta=1)
                self.assertAlmostEqual(margins[2] - margins[0], 100 * height / 1080, delta=1)
        with self.assertRaises(ValueError):
            video.get_ass(color_styles=False, gap_1080p=101)

    def test_two_distant_blocks_in_one_zone_are_separately_anchored(self):
        video = make_video([subtitle(0, [(420, 468, "上"), (900, 948, "下")])])
        ass = video.get_ass(color_styles=False)
        events = ass_fields(ass, "Dialogue: ")
        self.assertEqual(len(events), 2)
        self.assertIn(r"\pos(960,4", events[0][9])
        self.assertIn(r"\pos(960,8", events[1][9])
        self.assertEqual([event[9].split("}")[1] for event in events], ["上", "下"])
        self.assertEqual(video.pred_subs[0].text, "上\n下")

    def test_dual_zone_split_preserves_each_source_time_and_merged_srt_text(self):
        upper = subtitle(0, [(200, 248, "先に出る")], end=29)
        lower = subtitle(10, [(300, 348, "後に出る")], zone=1, end=39)
        video = make_video([])
        video.validated_zones.extend([{"crop_str": "1920:480:0:600", "h": 480,
                                       "w": 1920, "midpoint_y": 840}])
        video.pred_subs = video._merge_dual_zone_subtitles([upper], [lower])
        self.assertEqual(video.pred_subs[0].text, "先に出る\n後に出る")
        self.assertEqual(upper.text, "先に出る")
        events = ass_fields(video.get_ass(color_styles=False), "Dialogue: ")
        self.assertEqual([event[1:3] for event in events],
                         [["0:00:00.00", "0:00:01.00"], ["0:00:00.33", "0:00:01.33"]])
        self.assertEqual([event[9].split("}")[1] for event in events], ["先に出る", "後に出る"])

    def test_position_that_cannot_fit_is_marked_for_review(self):
        video = make_video([subtitle(0, [(10, 58, "画面上端")])])
        ass = video.get_ass(color_styles=False)
        event = ass_fields(ass, "Dialogue: ")[0]
        self.assertIn("检查位置", event[4])
        self.assertEqual(event[7], "72")

    def test_overlapping_events_can_be_stacked_by_the_renderer(self):
        video = make_video([subtitle(0, [(900, 948, "上")]),
                            subtitle(0, [(917, 965, "下")])])
        events = ass_fields(video.get_ass(color_styles=False), "Dialogue: ")
        self.assertEqual(len(events), 2)
        self.assertTrue(all("检查位置" in event[4] and r"\pos" not in event[9] for event in events))


if __name__ == "__main__":
    unittest.main()
