from __future__ import annotations

import json
import sys
from pathlib import Path

from . import frame_time, utils
from .video import Video


def save_subtitles_to_file(
        video_path: str, file_path: str = 'subtitle.srt', ocr_engine: str = 'paddleocr', lang: str = 'en',
        time_start: str = '0:00', time_end: str = '', conf_threshold: int = 75, sim_threshold: int = 80, max_merge_gap_sec: float = 0.1,
        use_fullframe: bool = False, use_gpu: bool = False, use_angle_cls: bool = False, use_server_model: bool = False,
        brightness_threshold: int | None = None, ssim_threshold: int = 94, subtitle_position: str = "center", frames_to_skip: int = 0,
        crop_zones: list[dict[str, int]] | None = None, ocr_image_max_width: int = 720, disable_stitching: bool = False, post_processing: bool = False,
        min_subtitle_duration_sec: float = 0.2, normalize_to_simplified_chinese: bool = True, subtitle_alignments: list[str | None] | None = None,
        paddleocr_path: str | None = None, support_files_path: str | None = None, temp_dir: str | None = None,
        frame_report_path: str | None = None, ass_output_path: str | None = None,
        write_ass: bool = True, color_styles: bool = True,
        color_style_budget: float = 360.0, color_report_path: str | None = None,
        subtitle_position_styles: bool = True, subtitle_font_size_styles: bool = True,
        subtitle_gap_1080p: int = 8) -> None:

    if crop_zones is None:
        crop_zones = []

    if subtitle_alignments is None:
        subtitle_alignments = [None, None]
    elif len(subtitle_alignments) == 1:
        subtitle_alignments.append(None)

    paddleocr_path = paddleocr_path or utils.find_executable("paddleocr")
    try:
        utils.perform_hardware_check(paddleocr_path, use_gpu)
    except SystemExit as e:
        print(e, flush=True)
        sys.exit(1)

    if ocr_engine == 'paddleocr':
        det_model_dir, rec_model_dir, cls_model_dir = utils.resolve_model_dirs(lang, use_server_model, support_files_path)
    else:
        # For the Text-Detection-Only Pass just the default detection model is needed
        det_model_dir, rec_model_dir, cls_model_dir = utils.resolve_model_dirs('en', use_server_model, support_files_path)

    # FKW: Google Lens CLI 未随 FKW 分发，强制禁用
    google_lens_path = None

    v = Video(video_path, paddleocr_path, det_model_dir, rec_model_dir, cls_model_dir, google_lens_path)
    v.color_style_budget = color_style_budget
    try:
        try:
            v.run_ocr(
                use_gpu, ocr_engine, lang, use_angle_cls, time_start, time_end, conf_threshold,
                use_fullframe, brightness_threshold, ssim_threshold, subtitle_position,
                frames_to_skip, crop_zones, ocr_image_max_width, disable_stitching,
                normalize_to_simplified_chinese, temp_dir
            )
        except Exception as e:
            print(f"Error: {e}", flush=True)
            sys.exit(1)
        subtitles = v.get_subtitles(sim_threshold, max_merge_gap_sec, lang, post_processing, min_subtitle_duration_sec, subtitle_alignments)

        with open(file_path, 'w+', encoding='utf-8') as f:
            f.write(subtitles)

        if write_ass:
            target_ass = ass_output_path or str(Path(file_path).with_suffix('.ass'))
            # Colour analysis can take minutes. Do not truncate an existing
            # ASS until it has completed successfully (including cancellation).
            ass_text = v.get_ass(
                color_styles=color_styles, position_styles=subtitle_position_styles,
                font_size_styles=subtitle_font_size_styles, gap_1080p=subtitle_gap_1080p,
            )
            with open(target_ass, 'w', encoding='utf-8', newline='\n') as f:
                f.write(ass_text)

            if color_report_path and color_styles:
                with open(color_report_path, 'w', encoding='utf-8') as f:
                    json.dump(v._color_report, f, ensure_ascii=False, indent=2)
                    f.write('\n')

        if frame_report_path:
            if v.fps is None:
                print("frame_report: skipped (VFR or insufficient timestamps)", flush=True)
            else:
                rows = frame_time.frame_rows(subtitles, v.fps)
                with open(frame_report_path, 'w', encoding='utf-8') as f:
                    json.dump(rows, f, ensure_ascii=False, indent=2)
                    f.write('\n')
    finally:
        v.cleanup()
