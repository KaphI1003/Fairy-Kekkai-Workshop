from __future__ import annotations

import concurrent.futures
import json
import os
import queue
import shutil
import statistics
import threading
import time
from copy import copy
from typing import Any, cast

import av
import fast_ssim
import wordninja_enhanced as wordninja

from . import color_styles as colors
from . import frame_time, paddle_recognition, subtitle_layout, utils
from .models import PredictedFrames, PredictedSubtitle
from .pyav_adapter import Capture, get_video_properties


class Video:
    path: str
    lang: str
    use_fullframe: bool
    paddleocr_path: str
    google_lens_path: str
    post_processing: bool
    det_model_dir: str
    rec_model_dir: str
    cls_model_dir: str
    duration_ms: int
    height: int
    width: int
    coded_width: int
    sample_aspect_ratio: float
    pred_frames_zone1: list[PredictedFrames]
    pred_frames_zone2: list[PredictedFrames]
    pred_subs: list[PredictedSubtitle]
    validated_zones: list[dict[str, Any]]
    frame_timestamps: dict[int, float]
    start_time_offset_ms: float
    avg_frame_duration_ms: float
    fps: float | None
    frame_base: int
    soft_events: list[dict]

    def __init__(self, path: str, paddleocr_path: str, det_model_dir: str, rec_model_dir: str, cls_model_dir: str, google_lens_path: str) -> None:
        self.path = path
        self.paddleocr_path = paddleocr_path
        self.det_model_dir = det_model_dir
        self.rec_model_dir = rec_model_dir
        self.cls_model_dir = cls_model_dir
        self.google_lens_path = google_lens_path
        self.frame_timestamps = {}
        self.start_time_offset_ms = 0.0
        self.avg_frame_duration_ms = 0.0
        self.fps = None
        self.frame_base = 0
        self.soft_events = []

        props = get_video_properties(self.path)
        self.height = props['height']
        self.width = props['width']
        self.coded_width = props['coded_width']
        self.sample_aspect_ratio = props['sample_aspect_ratio']
        self.duration_ms = props['duration_ms']
        self.start_time_offset_ms = props['start_time_offset_ms']

    def _detect_cfr_fps(self) -> float | None:
        """Return the constant frame rate, or None for VFR/too little data."""
        if len(self.frame_timestamps) < 3:
            return None
        items = sorted(self.frame_timestamps.items())
        deltas: list[float] = []
        for (left_index, left_ms), (right_index, right_ms) in zip(items, items[1:]):
            step = right_index - left_index
            if step <= 0:
                continue
            delta = (right_ms - left_ms) / step
            if delta > 0:
                deltas.append(delta)
        if len(deltas) < 2:
            return None
        median = statistics.median(deltas)
        if median <= 0:
            return None
        # A tiny tolerance absorbs 59.94 fps PTS rounding; a real VFR file
        # (mixed 23.976/29.97/60) exceeds it and is left untouched.
        if any(abs(delta - median) > 0.5 for delta in deltas):
            return None
        return frame_time.snap_fps(1000.0 / median)

    def run_ocr(self, use_gpu: bool, ocr_engine: str, lang: str, use_angle_cls: bool, time_start: str, time_end: str, conf_threshold: int,
                use_fullframe: bool, brightness_threshold: int | None, ssim_threshold: int, subtitle_position: str, frames_to_skip: int,
                crop_zones: list[dict[str, int]], ocr_image_max_width: int, disable_stitching: bool, normalize_to_simplified_chinese: bool,
                temp_dir: str | None = None) -> None:
        conf_threshold_ratio = conf_threshold / 100
        ssim_threshold_ratio = ssim_threshold / 100
        # Colours are sampled from raw video, independently of filtered JPEGs.
        self._hold_temp_for_colors = False
        self._frame_image_paths: dict[tuple[int, int], str] = {}
        self.lang = lang
        self.soft_events = utils.extract_soft_subtitle_events(self.path)
        if self.soft_events:
            print(f"soft_subs: {len(self.soft_events)} styled events", flush=True)
        self.use_fullframe = use_fullframe
        self.validated_zones = []
        self.pred_frames_zone1 = []
        self.pred_frames_zone2 = []

        user_start_ms = 0.0
        if time_start:
            user_start_ms = utils.get_ms_from_time_str(time_start)

        target_start_ms = user_start_ms + self.start_time_offset_ms

        target_end_ms = None
        target_end_str = "Unknown"

        if time_end:
            user_end_ms = utils.get_ms_from_time_str(time_end)
            target_end_ms = user_end_ms + self.start_time_offset_ms
            target_end_str = utils.get_srt_timestamp_from_ms(user_end_ms).split(',')[0]
        elif self.duration_ms > 0:
            target_end_str = utils.get_srt_timestamp_from_ms(self.duration_ms).split(',')[0]

        for zone in crop_zones:
            if zone['y'] >= self.height:
                raise ValueError(f"Crop Y position ({zone['y']}) is outside video height ({self.height}).")
            if zone['x'] >= self.width:
                raise ValueError(f"Crop X position ({zone['x']}) is outside video width ({self.width}).")

            if zone['y'] + zone['height'] > self.height:
                print(f"Warning: Crop area extends out of bounds (crop_y + crop_height > video height ({self.height})). The crop area will be clipped.", flush=True)
            if zone['x'] + zone['width'] > self.width:
                print(f"Warning: Crop area extends out of bounds (crop_x + crop_width > video width ({self.width})). The crop area will be clipped.", flush=True)

            self.validated_zones.append({
                'x_start': zone['x'],
                'y_start': zone['y'],
                'x_end': zone['x'] + zone['width'],
                'y_end': zone['y'] + zone['height'],
                'midpoint_y': zone['y'] + (zone['height'] / 2)
            })

        # Handle full frame and fallback scenarios by injecting them as standard zones
        if self.use_fullframe:
            # Overwrite any user zones with a single full-frame zone
            self.validated_zones = [{
                'x_start': 0,
                'y_start': 0,
                'x_end': self.width,
                'y_end': self.height,
                'midpoint_y': self.height / 2
            }]
        elif not self.validated_zones:
            # Default to bottom third if no zones were provided
            self.validated_zones.append({
                'x_start': 0,
                'y_start': 2 * self.height // 3,
                'x_end': self.width,
                'y_end': self.height,
                'midpoint_y': (2 * self.height // 3) + (self.height // 6)
            })

        # Pre-calculate FFmpeg crop and scale strings for all zones
        for val_zone in self.validated_zones:
            crop_w = max(2, (min(self.width, val_zone['x_end']) - max(0, val_zone['x_start'])) & ~1)
            crop_h = max(2, (min(self.height, val_zone['y_end']) - max(0, val_zone['y_start'])) & ~1)
            crop_x = max(0, val_zone['x_start']) & ~1
            crop_y = max(0, val_zone['y_start']) & ~1

            # Resize image
            MIN_SIDE = 64
            scale_ratio = 1.0
            if ocr_image_max_width and crop_w > ocr_image_max_width:
                scale_ratio = ocr_image_max_width / crop_w

            min_required_ratio = MIN_SIDE / min(crop_w, crop_h)
            if scale_ratio < min_required_ratio:
                scale_ratio = min_required_ratio

            target_w = max(2, int(crop_w * scale_ratio) & ~1)
            target_h = max(2, int(crop_h * scale_ratio) & ~1)

            if self.sample_aspect_ratio and self.sample_aspect_ratio != 1:
                crop_x_coded = int(crop_x / self.sample_aspect_ratio) & ~1
                crop_w_coded = max(2, int(crop_w / self.sample_aspect_ratio) & ~1)
                crop_w_coded = min(crop_w_coded, (self.coded_width - crop_x_coded) & ~1)
            else:
                crop_x_coded = crop_x
                crop_w_coded = crop_w

            val_zone['w'] = target_w
            val_zone['h'] = target_h
            val_zone['crop_str'] = f"{crop_w_coded}:{crop_h}:{crop_x_coded}:{crop_y}"
            val_zone['scale_str'] = f"{target_w}:{target_h}:flags=area:threads=1"

        temp_dir = utils.create_clean_temp_dir(temp_dir)
        self._temp_dir = temp_dir

        try:
            raw_queue: queue.Queue[Any] = queue.Queue(maxsize=100)
            processed_queue: queue.Queue[Any] = queue.Queue(maxsize=100)
            write_queue: queue.Queue[Any] = queue.Queue(maxsize=200)
            start_index_queue: queue.Queue[Any] = queue.Queue()
            stop_event = threading.Event()
            drain_event = threading.Event()
            error_list: list[Exception] = []

            MAX_STITCH_WIDTH = 1500
            MAX_STITCH_HEIGHT = 1500
            GRID_SPACING = 10
            MAX_STITCH_COLS = 3
            MAX_STITCH_ROWS = 10
            FILENAME_ZERO_PADDING = 8

            def producer_thread() -> None:
                try:
                    with Capture(self.path) as v:
                        is_seeking = user_start_ms > 0

                        if is_seeking:
                            v.seek(target_start_ms)

                        current_index = 0
                        modulo = frames_to_skip + 1
                        first_queued = False

                        while not stop_event.is_set():
                            success, raw_frame, timestamp_ms = v.read()

                            if not success:
                                break

                            curr_str = utils.get_srt_timestamp_from_ms(timestamp_ms - self.start_time_offset_ms).split(',')[0]

                            # Check Start Time
                            if is_seeking:
                                if timestamp_ms < target_start_ms:
                                    continue
                                else:
                                    is_seeking = False

                            # Check End Time
                            if target_end_ms is not None and timestamp_ms > target_end_ms:
                                break

                            if not first_queued:
                                start_index_queue.put(current_index)
                                first_queued = True

                            should_process_frame = (current_index % modulo == 0)
                            if should_process_frame:
                                raw_queue.put((current_index, timestamp_ms, raw_frame, curr_str))
                            else:
                                raw_queue.put((current_index, timestamp_ms, None, curr_str))

                            current_index += 1

                except Exception as e:
                    error_list.append(e)
                    stop_event.set()

                finally:
                    if not first_queued:
                        start_index_queue.put(None)
                    raw_queue.put(None)

            def worker_thread() -> None:
                graph = None
                sinks: list[Any] = []

                try:
                    while not stop_event.is_set():
                        try:
                            item = raw_queue.get(timeout=0.5)
                        except queue.Empty:
                            continue

                        if item is None:
                            raw_queue.put(None)
                            break

                        current_index, timestamp_ms, raw_frame, curr_str = item

                        if raw_frame is None:
                            processed_queue.put((current_index, timestamp_ms, None, curr_str))
                            continue

                        images_to_process: list[dict[str, Any]] = []

                        # Filter Graph
                        if graph is None:
                            graph = av.filter.Graph()
                            buffer_node = graph.add_buffer(template=raw_frame)
                            num_zones = len(self.validated_zones)

                            if num_zones == 2:
                                main_split = graph.add("split", "2")
                                buffer_node.link_to(main_split)

                            for i, z in enumerate(self.validated_zones):
                                crop_node = graph.add("crop", z['crop_str'])

                                if num_zones == 2:
                                    main_split.link_to(crop_node, output_idx=i)
                                else:
                                    buffer_node.link_to(crop_node)

                                last = graph.add("scale", z['scale_str'])
                                crop_node.link_to(last)

                                fmt_base_node = graph.add("format", "rgb24")
                                last.link_to(fmt_base_node)
                                last = fmt_base_node

                                if brightness_threshold is not None:
                                    thresh_split = graph.add("split", "2")
                                    last.link_to(thresh_split)

                                    gray_node = graph.add("format", "gray")
                                    thresh_split.link_to(gray_node, output_idx=0)

                                    lut_node = graph.add("lut", f"c0='255*gt(val,{brightness_threshold})'")
                                    gray_node.link_to(lut_node)

                                    mask_rgb_node = graph.add("format", "rgb24")
                                    lut_node.link_to(mask_rgb_node)

                                    blend_node = graph.add("blend", "all_mode=multiply")
                                    thresh_split.link_to(blend_node, output_idx=1)
                                    mask_rgb_node.link_to(blend_node, input_idx=1)
                                    last = blend_node

                                    fmt_out_node = graph.add("format", "rgb24")
                                    last.link_to(fmt_out_node)
                                    last = fmt_out_node

                                sink_node = graph.add("buffersink")
                                last.link_to(sink_node)
                                sinks.append(sink_node)

                            graph.configure()

                        graph.push(raw_frame)

                        for idx, sink in enumerate(sinks):
                            processed_raw_frame = cast(av.VideoFrame, sink.pull())

                            img = utils.video_frame_to_frame(processed_raw_frame)
                            zone_idx = idx

                            sample = None
                            if ssim_threshold_ratio < 1:
                                w = img.width
                                if subtitle_position == "center":
                                    w_margin = int(w * 0.35)
                                    sample = img.crop_columns(w_margin, w - w_margin)
                                elif subtitle_position == "left":
                                    sample = img.crop_columns(0, int(w * 0.3))
                                elif subtitle_position == "right":
                                    sample = img.crop_columns(int(w * 0.7), w)
                                elif subtitle_position == "any":
                                    sample = img
                                else:
                                    raise ValueError(f"Invalid subtitle_position: {subtitle_position}")

                            images_to_process.append({
                                'zone_idx': zone_idx,
                                'img': img,
                                'ssim_sample': sample
                            })

                        processed_queue.put((current_index, timestamp_ms, images_to_process, curr_str))

                except Exception as e:
                    error_list.append(e)
                    stop_event.set()

            def writer_thread() -> None:
                max_canvas = bytearray(MAX_STITCH_WIDTH * MAX_STITCH_HEIGHT * 3)

                try:
                    while not stop_event.is_set():
                        try:
                            item = write_queue.get(timeout=0.5)
                        except queue.Empty:
                            if drain_event.is_set():
                                break
                            continue

                        frame_path, canvas_w, canvas_h, draw_instructions = item

                        active_bytes = canvas_w * canvas_h * 3
                        canvas_view = memoryview(max_canvas)[:active_bytes]

                        for img, x, y in draw_instructions:
                            utils.blit(canvas_view, canvas_w, img, x, y)

                        jpeg_bytes = utils.encode_frame_to_jpeg(canvas_view, canvas_w, canvas_h, quality=80)
                        with open(frame_path, 'wb') as f:
                            f.write(jpeg_bytes)

                except Exception as e:
                    error_list.append(e)
                    stop_event.set()

            # Start Threads
            producer = threading.Thread(target=producer_thread)
            producer.start()

            num_workers = num_writers = (os.cpu_count() or 1) // 4 + 1
            workers: list[threading.Thread] = []
            for _ in range(num_workers):
                t = threading.Thread(target=worker_thread)
                t.start()
                workers.append(t)

            writers: list[threading.Thread] = []
            for _ in range(num_writers):
                t = threading.Thread(target=writer_thread)
                t.start()
                writers.append(t)

            batch_limits: dict[int, int] = {}
            zone_cols: dict[int, int] = {}
            for z_idx, z in enumerate(self.validated_zones):
                if disable_stitching:
                    batch_limits[z_idx] = 1
                    zone_cols[z_idx] = 1
                else:
                    cols = utils.grid_cols(z['w'], MAX_STITCH_WIDTH, GRID_SPACING, MAX_STITCH_COLS)
                    zone_cols[z_idx] = cols
                    batch_limits[z_idx] = utils.get_batch_limit(cols, z['h'], MAX_STITCH_HEIGHT, GRID_SPACING, MAX_STITCH_ROWS)

            def flush_batch(batch: list[Any], counter: int, zone_idx: int, prefix: str, out_dir: str, target_map: dict[str, Any]) -> int:
                queue_args = utils.prepare_stitch_batch(batch, counter, zone_idx, prefix, out_dir, target_map, zone_cols[zone_idx], GRID_SPACING, FILENAME_ZERO_PADDING)
                write_queue.put(queue_args)
                return counter + 1

            # Consumer Logic
            det_stitched_dir = os.path.join(temp_dir, "detection_input")
            utils.make_clean_ocr_image_dir(det_stitched_dir)

            det_stitch_map: dict[str, list[dict[str, Any]]] = {}
            det_counter = 0
            det_batches: dict[int, list[Any]] = {0: [], 1: []}

            prev_samples = [None] * len(self.validated_zones) if self.validated_zones else [None]
            expected_index = None
            success = False

            try:
                while expected_index is None and not stop_event.is_set():
                    if error_list:
                        break
                    try:
                        expected_index = start_index_queue.get(timeout=0.1)
                    except queue.Empty:
                        if not producer.is_alive():
                            break
                        continue

                if expected_index is not None:
                    buffer: dict[int, tuple[float, Any, str]] = {}
                    while not stop_event.is_set():
                        if error_list:
                            break

                        try:
                            item = processed_queue.get(timeout=0.1)
                        except queue.Empty:
                            if not producer.is_alive() and all(not w.is_alive() for w in workers) and processed_queue.empty():
                                break
                            continue

                        current_index, timestamp_ms, images_to_process, curr_str = item
                        buffer[current_index] = (timestamp_ms, images_to_process, curr_str)

                        # Process buffer sequentially
                        while expected_index in buffer:
                            timestamp_ms, images_to_process, curr_str = buffer.pop(expected_index)
                            self.frame_timestamps[expected_index] = timestamp_ms

                            if current_index % 15 == 0:
                                print(f"\rStep 1/3: Processing video... Current: {curr_str} / {target_end_str}, Frame: {expected_index + 1}", end="", flush=True)

                            if images_to_process is not None:
                                for zone_data in images_to_process:
                                    zone_idx = zone_data['zone_idx']
                                    img = zone_data['img']
                                    sample = zone_data['ssim_sample']

                                    if ssim_threshold_ratio < 1:
                                        if prev_samples[zone_idx] is not None:
                                            score = fast_ssim.ssim(prev_samples[zone_idx].data, sample.data, sample.width, sample.height, channels=3, data_range=255)
                                            if score > ssim_threshold_ratio:
                                                prev_samples[zone_idx] = sample
                                                continue
                                        prev_samples[zone_idx] = sample

                                    det_batches[zone_idx].append({
                                        "img": img,
                                        "frame_idx": expected_index
                                    })

                                    if len(det_batches[zone_idx]) >= batch_limits[zone_idx]:
                                        det_counter = flush_batch(det_batches[zone_idx], det_counter, zone_idx, "det_stitched", det_stitched_dir, det_stitch_map)
                                        det_batches[zone_idx] = []

                            expected_index += 1

                    for z_idx in [0, 1]:
                        if det_batches[z_idx]:
                            det_counter = flush_batch(det_batches[z_idx], det_counter, z_idx, "det_stitched", det_stitched_dir, det_stitch_map)

                    if not error_list and expected_index > 0:
                        last_idx = expected_index - 1
                        final_ms = self.frame_timestamps.get(last_idx, 0)
                        final_str = utils.get_srt_timestamp_from_ms(final_ms - self.start_time_offset_ms).split(',')[0]

                        if target_end_ms is not None:
                            print(f"\rStep 1/3: Processing video... Current: {target_end_str} / {target_end_str}, Frame: {expected_index}", end="")
                            print("\nReached end time. Stopping.", flush=True)
                        else:
                            print(f"\rStep 1/3: Processing video... Current: {final_str} / {target_end_str}, Frame: {expected_index}", flush=True)

                success = True

            except KeyboardInterrupt:
                raise

            finally:
                is_aborting = not success or len(error_list) > 0

                if is_aborting:
                    stop_event.set()
                else:
                    drain_event.set()

                while not raw_queue.empty():
                    try:
                        raw_queue.get_nowait()
                    except queue.Empty:
                        break

                while not processed_queue.empty():
                    try:
                        processed_queue.get_nowait()
                    except queue.Empty:
                        break

                if is_aborting:
                    while not write_queue.empty():
                        try:
                            write_queue.get_nowait()
                        except queue.Empty:
                            break

                producer.join()
                for w in workers:
                    w.join()
                for w in writers:
                    w.join()

                if error_list:
                    raise error_list[0]

            ocr_end = expected_index if expected_index is not None else 0

            if len(self.frame_timestamps) > 1:
                min_idx = min(self.frame_timestamps.keys())
                max_idx = max(self.frame_timestamps.keys())
                if max_idx > min_idx:
                    total_duration = self.frame_timestamps[max_idx] - self.frame_timestamps[min_idx]
                    self.avg_frame_duration_ms = total_duration / (max_idx - min_idx)
            self.fps = self._detect_cfr_fps()
            if self.fps is not None and self.frame_timestamps:
                first_index = min(self.frame_timestamps)
                first_ms = self.frame_timestamps[first_index] - self.start_time_offset_ms
                self.frame_base = max(
                    0,
                    frame_time.frame_at(int(round(first_ms)), self.fps) - first_index,
                )

            if det_counter == 0:
                return

            # --------------------------------------------------------
            # Detection pass and SSIM filtering on detected text boxes
            # --------------------------------------------------------
            TIGHT_BOX_SSIM_THRESHOLD = 0.85
            total_stitched_frames = sum(len(mappings) for mappings in det_stitch_map.values())
            print(f"Running Text-Detection-Only pass on {total_stitched_frames} filtered frame(s) stitched into {det_counter} image grid(s)...", flush=True)

            args = [
                self.paddleocr_path,
                "text_detection",
                "--input", det_stitched_dir,
                "--model_dir", self.det_model_dir,
                "--model_name", os.path.basename(self.det_model_dir)
            ]

            print("Starting PaddleOCR...", flush=True)

            det_ocr_outputs = paddle_recognition.recognize(args, list(det_stitch_map), use_gpu)
            print()

            parsed_detections: dict[int, list[Any]] = {0: [], 1: []}

            for stitched_filename, det_results in det_ocr_outputs.items():
                if stitched_filename not in det_stitch_map:
                    continue

                mapping = det_stitch_map[stitched_filename]
                zone_idx = mapping[0]["zone_idx"]

                temp_polys_dict: dict[int, list[Any]] = {m["frame_idx"]: [] for m in mapping}

                for item in det_results:
                    poly = item[0]
                    score = item[1]

                    for adjusted_poly, m in utils.unstitch_polygon(poly, mapping):
                        temp_polys_dict[m["frame_idx"]].append({"poly": adjusted_poly, "score": score})

                for m in mapping:
                    polys_data = temp_polys_dict[m["frame_idx"]]
                    frame_score = sum(p["score"] for p in polys_data) / len(polys_data) if polys_data else 0.0
                    extracted_polygons = [p["poly"] for p in polys_data]

                    parsed_detections[zone_idx].append((m["frame_idx"], extracted_polygons, frame_score, m))

            for z_idx in parsed_detections:
                parsed_detections[z_idx].sort(key=lambda x: x[0])

            frames_processed = 0
            frames_deleted_count = 0
            next_print_target = 15

            rec_images_dir = os.path.join(temp_dir, "recognition_input")
            utils.make_clean_ocr_image_dir(rec_images_dir)

            empty_frames_meta: set[tuple[int, int]] = set()
            surviving_frames_meta: set[tuple[int, int]] = set()
            rec_image_map: dict[str, dict[str, int]] = {}
            rec_counter = 0

            drain_event.clear()
            rec_writers: list[threading.Thread] = []
            for _ in range(max(1, (os.cpu_count() or 1) - 1)):
                t = threading.Thread(target=writer_thread)
                t.start()
                rec_writers.append(t)

            success = False

            # Process Zones
            try:
                for z_idx, zone_data in parsed_detections.items():
                    if not zone_data:
                        continue

                    groups: list[Any] = []
                    current_group: list[Any] = []
                    current_union_rects: list[list[float]] = []

                    for _, polys, frame_score, m in zone_data:
                        if not polys or len(polys) == 0:
                            frames_deleted_count += 1
                            frames_processed += 1

                            coord = (m["frame_idx"], z_idx)
                            if coord not in empty_frames_meta:
                                empty_frames_meta.add(coord)

                            if frames_processed >= next_print_target:
                                print(f"\rAnalyzing frame {frames_processed} of {total_stitched_frames}", end="", flush=True)
                                next_print_target = frames_processed + 15

                            continue

                        line_rects = utils.get_line_rects(polys)

                        if not current_group:
                            current_group = [(m["frame_idx"], line_rects, frame_score, m)]
                            current_union_rects = line_rects
                        else:
                            if utils.are_rect_lists_similar(current_union_rects, line_rects, tolerance=0.05):
                                current_group.append((m["frame_idx"], line_rects, frame_score, m))
                                new_unions: list[list[float]] = []
                                for u_rect, l_rect in zip(current_union_rects, line_rects):
                                    new_unions.append([
                                        min(u_rect[0], l_rect[0]), min(u_rect[1], l_rect[1]),
                                        max(u_rect[2], l_rect[2]), max(u_rect[3], l_rect[3])
                                    ])
                                current_union_rects = new_unions
                            else:
                                groups.append((current_union_rects, current_group))
                                current_group = [(m["frame_idx"], line_rects, frame_score, m)]
                                current_union_rects = line_rects

                    if current_group:
                        groups.append((current_union_rects, current_group))

                    # SSIM & Repacking
                    MAX_GRIDS_PER_CHUNK = 30

                    chunks: list[tuple[list[Any], set[str]]] = []
                    current_chunk_groups: list[Any] = []
                    current_chunk_grids: set[str] = set()

                    for union_rects, group_frames in groups:
                        group_grids = set()
                        for _, _, _, m in group_frames:
                            group_grids.add(m["grid_file"])

                        if len(current_chunk_grids | group_grids) > MAX_GRIDS_PER_CHUNK and current_chunk_groups:
                            chunks.append((current_chunk_groups, current_chunk_grids))
                            current_chunk_groups = []
                            current_chunk_grids = set()

                        current_chunk_groups.append((union_rects, group_frames))
                        current_chunk_grids.update(group_grids)

                    if current_chunk_groups:
                        chunks.append((current_chunk_groups, current_chunk_grids))

                    with concurrent.futures.ThreadPoolExecutor() as executor:
                        for chunk_groups, chunk_grids in chunks:
                            loaded_grids: dict[str, Any] = {}

                            for g_file, img_array in executor.map(utils.decode_jpeg_to_frame, chunk_grids):
                                loaded_grids[g_file] = img_array

                            group_args = [
                                (union_rects, group_frames, loaded_grids, TIGHT_BOX_SSIM_THRESHOLD)
                                for union_rects, group_frames in chunk_groups
                            ]

                            for ssim_args in group_args:
                                surviving_items, local_deleted = utils.process_ssim_group(*ssim_args)
                                frames_deleted_count += local_deleted

                                for item in surviving_items:
                                    surviving_frames_meta.add((item["frame_idx"], z_idx))

                                    filename = f"rec_image_{rec_counter:0{FILENAME_ZERO_PADDING}d}_zone{z_idx}.jpg"
                                    filepath = os.path.join(rec_images_dir, filename)
                                    h, w = item["img"].height, item["img"].width
                                    write_queue.put((filepath, w, h, [(item["img"], 0, 0)]))

                                    rec_image_map[filename] = {
                                        "frame_idx": item["frame_idx"],
                                        "zone_idx": z_idx
                                    }
                                    rec_counter += 1

                                frames_processed += len(ssim_args[1])

                                if frames_processed >= next_print_target:
                                    print(f"\rAnalyzing frame {frames_processed} of {total_stitched_frames}", end="", flush=True)
                                    next_print_target = frames_processed + 15

                            loaded_grids.clear()

                print(f"\rAnalyzing frame {frames_processed} of {total_stitched_frames}", end="", flush=True)
                success = True

            except KeyboardInterrupt:
                raise

            finally:
                is_aborting = not success or len(error_list) > 0

                if is_aborting:
                    stop_event.set()
                else:
                    drain_event.set()

                if is_aborting:
                    while not write_queue.empty():
                        try:
                            write_queue.get_nowait()
                        except queue.Empty:
                            break

                for w in rec_writers:
                    w.join()

                if error_list:
                    raise error_list[0]

            print(f"\nFiltered out {frames_deleted_count} redundant frame(s) via Text-Detection and tight-box SSIM analysis.", flush=True)

            if rec_counter == 0:
                return

            # --------------------------------------------------------
            self._frame_image_paths = {
                (meta["zone_idx"], meta["frame_idx"]): os.path.join(
                    rec_images_dir, filename
                )
                for filename, meta in rec_image_map.items()
            }

            # Recognition Pass
            # --------------------------------------------------------
            rec_ocr_outputs: dict[str, list[Any]] = {}
            ocr_image_index = 0

            if ocr_engine == "google_lens":
                args = [
                    self.google_lens_path,
                    rec_images_dir,
                    self.lang,
                    "--get-coords",
                    "--oneline",
                    "-q",
                ]

                print("Starting Google Lens CLI...", flush=True)

                for line in utils.stream_cli_process(args, "google_lens_error.log"):
                    line = line.strip()
                    if not line or not line.startswith('{') or '"file"' not in line:
                        continue

                    data = json.loads(line)
                    stitched_filename = data["file"]

                    if stitched_filename not in rec_image_map:
                        continue

                    grid_w = data["dimensions"]["original_width"]
                    grid_h = data["dimensions"]["original_height"]

                    results: list[list[Any]] = []
                    for word_item in data["words"]:
                        text = word_item["text"]
                        separator = word_item["separator"]
                        geom = word_item["geometry"]

                        if not geom:
                            continue

                        cx = geom["center_x"] * grid_w
                        cy = geom["center_y"] * grid_h
                        w = geom["width"] * grid_w
                        h = geom["height"] * grid_h

                        x1, x2 = cx - w / 2.0, cx + w / 2.0
                        y1, y2 = cy - h / 2.0, cy + h / 2.0
                        box = [[x1, y1], [x2, y1], [x2, y2], [x1, y2]]

                        combined_text = text + separator
                        results.append([box, (combined_text, 1.0)])

                    rec_ocr_outputs[stitched_filename] = results
                    ocr_image_index += 1
                    print(f"\rStep 3/3: Performing OCR on image {ocr_image_index} of {len(rec_image_map)}", end="", flush=True)
                print()

            elif ocr_engine == "paddleocr":
                args = [
                    self.paddleocr_path,
                    "ocr",
                    "--input", rec_images_dir,
                    "--device", "gpu" if use_gpu else "cpu",
                    "--use_textline_orientation", "true" if use_angle_cls else "false",
                    "--use_doc_orientation_classify", "false",
                    "--use_doc_unwarping", "false",
                    "--lang", self.lang,
                    "--text_detection_model_dir", self.det_model_dir,
                    "--text_detection_model_name", os.path.basename(self.det_model_dir),
                    "--text_recognition_model_dir", self.rec_model_dir,
                    "--text_recognition_model_name", os.path.basename(self.rec_model_dir),
                ]

                if use_angle_cls:
                    args += ["--textline_orientation_model_dir", self.cls_model_dir]
                    args += ["--textline_orientation_model_name", os.path.basename(self.cls_model_dir)]

                print("Starting PaddleOCR...", flush=True)

                rec_ocr_outputs = paddle_recognition.recognize(args, list(rec_image_map), use_gpu)
                print()

            # Map 2D coordinates
            ocr_outputs: dict[tuple[int, int], list[Any]] = {}

            for filename, results in rec_ocr_outputs.items():
                if filename not in rec_image_map:
                    continue

                m = rec_image_map[filename]
                coord_key = (m["frame_idx"], m["zone_idx"])

                if coord_key not in ocr_outputs:
                    ocr_outputs[coord_key] = []

                for word_pred in results:
                    ocr_outputs[coord_key].append([word_pred[0], word_pred[1]])

            active_frame_coords = surviving_frames_meta | empty_frames_meta

            frame_predictions_dict: dict[int, dict[int, PredictedFrames]] = {0: {}, 1: {}}

            for frame_index, zone_index in active_frame_coords:
                ocr_result = ocr_outputs.get((frame_index, zone_index), [])
                pred_data = [ocr_result] if ocr_result else [[]]

                predicted_frame = PredictedFrames(ocr_engine, frame_index, pred_data, conf_threshold_ratio, zone_index, lang, normalize_to_simplified_chinese)
                frame_predictions_dict[zone_index][frame_index] = predicted_frame

            frame_predictions_list: dict[int, list[PredictedFrames]] = {}

            for zone_idx in frame_predictions_dict:
                frames = sorted(frame_predictions_dict[zone_idx].values(), key=lambda f: f.start_index)

                if not frames:
                    continue

                for i in range(len(frames) - 1):
                    current_pred = frames[i]
                    next_pred = frames[i + 1]

                    current_pred.end_index = next_pred.start_index - 1

                if frames:
                    frames[-1].end_index = ocr_end - 1

                frame_predictions_list[zone_idx] = frames

            self.pred_frames_zone1 = frame_predictions_list.get(0, [])
            self.pred_frames_zone2 = frame_predictions_list.get(1, [])

        finally:
            # Style sampling reads raw video; OCR JPEGs need not outlive OCR.
            if not getattr(self, "_hold_temp_for_colors", False):
                shutil.rmtree(temp_dir, ignore_errors=True)

    def _sample_cue_colors(self, cues: list[subtitle_layout.AssCue]) -> None:
        """Select visible middle frames from raw video without additional OCR."""
        key = [(cue.subtitle, cue.line_span) for cue in cues]
        if getattr(self, "_color_cue_key", None) == key:
            return
        requests = []
        for cue in cues:
            sub = cue.subtitle
            start_ms, end_ms = self._get_subtitle_ms_times(sub)
            boxes = []
            by_zone = {}
            line_count = len(sub.text.splitlines()) if cue.line_span is not None else None
            for frame in sub.frames:
                if (frame.text.strip() and frame.lines
                        and (line_count is None or len(frame.lines) == line_count)):
                    by_zone.setdefault(frame.zone_index, []).append(frame)
            for zone_index, frames in by_zone.items():
                zone = self.validated_zones[zone_index]
                crop_w, crop_h, crop_x, crop_y = map(int, zone["crop_str"].split(":"))
                reference = min(frames, key=lambda frame: (
                    abs(self.frame_timestamps.get(frame.start_index, start_ms + self.start_time_offset_ms)
                        - self.start_time_offset_ms - (start_ms + end_ms) / 2)
                    / max(1, end_ms - start_ms) - .2 * min(1, frame.confidence)))
                first, last = cue.line_span or (0, len(reference.lines))
                for line in reference.lines[first:last]:
                    for word in line:
                        boxes.append([[crop_x + x * crop_w / zone["w"],
                                       crop_y + y * crop_h / zone["h"]]
                                      for x, y in word.bounding_box])
            requests.append(colors.ColorRequest(start_ms, end_ms, boxes))
        print("colors: analysing raw colour frames on CPU...", flush=True)
        observations, stats = colors.sample_video_colors(
            self.path, requests, budget_seconds=getattr(self, "color_style_budget", 360.0),
            progress=lambda done, total: print(f"colors: sampling cue {done} of {total}", flush=True))
        self._color_observations = observations
        self._color_stats = stats
        self._color_requests = requests
        self._color_cue_key = key
        print(f"colors: sampling took {stats['seconds']:.2f}s, "
              f"analysed {stats['analysed_frames']}/{stats['sampled_frames']} candidate frames", flush=True)
        print(f"colors: decode and selection {stats['decode_seconds']:.2f}s, "
              f"parent waiting for results {stats.get('queue_wait_seconds', 0):.2f}s, "
              f"gaps between jobs summed across workers {stats.get('worker_gap_seconds', 0):.2f}s",
              flush=True)

    def _dump_debug_frames(self) -> None:
        debug_dump = os.environ.get("FKW_OCR_DEBUG_DUMP")
        if not debug_dump:
            return
        payload = {
            "fps": self.fps,
            "frame_base": self.frame_base,
            "zones": {
                1: [
                    {
                        "frame": frame.start_index,
                        "text": frame.text,
                        "confidence": frame.confidence,
                        "fill_rgb": frame.fill_rgb,
                        "outline_rgb": frame.outline_rgb,
                        "color_confidence": frame.color_confidence,
                        "lines": [
                            {
                                "box": line[0].bounding_box if line else [],
                                "text": "".join(word.text for word in line),
                            }
                            for line in frame.lines
                        ],
                    }
                    for frame in self.pred_frames_zone1
                ],
                2: [
                    {
                        "frame": frame.start_index,
                        "text": frame.text,
                        "confidence": frame.confidence,
                    }
                    for frame in self.pred_frames_zone2
                ],
            },
        }
        with open(debug_dump, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)

    def _release_temp(self) -> None:
        temp_dir = getattr(self, "_temp_dir", None)
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)
        self._temp_dir = None
        self._frame_image_paths = {}

    def cleanup(self) -> None:
        """Release recognition images kept for per-cue color sampling."""
        self._release_temp()

    def get_subtitles(self, sim_threshold: int, max_merge_gap_sec: float, lang: str, post_processing: bool, min_subtitle_duration_sec: float, subtitle_alignments: list[str | None]) -> str:
        self._generate_subtitles(sim_threshold, max_merge_gap_sec, lang, post_processing, min_subtitle_duration_sec, subtitle_alignments)

        srt_lines: list[str] = []
        for i, sub in enumerate(self.pred_subs, 1):
            start_time, end_time = self._get_srt_timestamps(sub)

            text = sub.text
            tag = subtitle_alignments[sub.zone_index]
            if tag:
                text = f"{{\\{tag}}}{sub.text}"

            srt_lines.append(f'{i}\n{start_time} --> {end_time}\n{text}\n\n')

        return ''.join(srt_lines)

    def get_ass(self, color_styles: bool = True, *, position_styles: bool = True,
                font_size_styles: bool = True, gap_1080p: int = 8) -> str:
        """Serialize frame-level cues as ASS, ready for Aegisub.

        ASS stores centiseconds.  Both boundaries are floored so Aegisub's
        START/END frame mapping (which biases by one millisecond) lands on the
        intended start frame and the intended last visible frame.  When
        ``color_styles`` is enabled, cues with the same detected fill/outline
        color pair share one ASS style.
        """
        if not hasattr(self, "pred_subs"):
            raise RuntimeError("get_subtitles() must run before get_ass()")
        if not 0 <= gap_1080p <= 100:
            raise ValueError("The 1080p subtitle gap must be between 0 and 100 pixels")

        width = self.width or 1920
        height = self.height or 1080
        scale = height / 1080.0
        font_size = max(16, round(80 * scale, 1))
        margin_v = max(10, round(72 * scale))
        outline = round(3.5 * scale, 2)
        shadow = 1
        outline_text = f"{outline:g}"
        cues = subtitle_layout.prepare_cues(
            self.pred_subs, getattr(self, "validated_zones", []),
            getattr(self, "_merged_ass_sources", {}),
            position=position_styles, font_size=font_size_styles,
        )

        if color_styles:
            self._sample_cue_colors(cues)
        palette_started = time.perf_counter()
        observations = list(getattr(self, "_color_observations", [])) if color_styles else []
        soft_exact = 0
        for index, cue in enumerate(cues):
            if color_styles and self.soft_events:
                start_ms, end_ms = self._get_subtitle_ms_times(cue.subtitle)
                event = utils.match_soft_event(self.soft_events, start_ms, end_ms, cue.text)
                if event:
                    observations[index] = colors.ColorObservation(
                        event["fill"], event["outline"], 1.0, 1.0, 1.0, 10, 2, "soft_subtitle")
                    soft_exact += 1
        if color_styles:
            palette_objects, assignments = colors.build_palettes(observations)
            fill_matches = colors.assign_by_fill(observations, assignments, palette_objects)
        else:
            palette_objects, assignments = [], [None] * len(cues)
            fill_matches = set()
        palettes = [{"fill": p.fill, "outline": p.outline, "name": p.name, "count": len(p.members)}
                    for p in palette_objects]
        sub_palettes = [palettes[index] if index is not None else None for index in assignments]
        defaulted = sum(index is None for index in assignments)
        if color_styles:
            stats = dict(getattr(self, "_color_stats", {}))
            stats["palette_seconds"] = time.perf_counter() - palette_started
            stats["total_seconds"] = stats.get("seconds", 0) + max(0, stats["palette_seconds"])
            stats["styles"] = len(palettes)
            stats["unresolved"] = defaulted
            stats["fill_only"] = len(fill_matches)
            stats["soft_exact"] = soft_exact
            stats["confirmed_white_black"] = sum(
                p["count"] for p in palettes
                if float(colors.delta_e(colors.rgb_to_lab(p["fill"]), colors.rgb_to_lab((255, 255, 255)))) < 4
                and float(colors.delta_e(colors.rgb_to_lab(p["outline"]), colors.rgb_to_lab((0, 0, 0)))) < 4)
            self._color_report = {"version": 2, "summary": stats, "palettes": palettes, "cues": [
                {"index": index, "start_ms": request.start_ms, "end_ms": request.end_ms,
                 "text": cue.text, "fill": obs.fill, "outline": obs.outline,
                 "fill_confidence": obs.fill_confidence, "outline_confidence": obs.outline_confidence,
                 "confidence": obs.confidence, "support": obs.support, "frames": obs.frame_count,
                 "reason": obs.reason, "boxes": request.boxes,
                 "style": p["name"] if p else "Default",
                 "fill_only_match": index in fill_matches}
                 for index, (cue, obs, p, request) in enumerate(zip(
                    cues, observations, sub_palettes, self._color_requests))]}
            print(f"colors: {len(palettes)} ASS styles, fill_only={stats['fill_only']}, unresolved={defaulted}, "
                  f"total={stats['total_seconds']:.2f}s (CPU)", flush=True)
            if stats["total_seconds"] > getattr(self, "color_style_budget", 360.0):
                print("colors: soft time budget exceeded; all cues were still processed", flush=True)

        def ass_color(rgb) -> str:
            return f"&H00{rgb[2]:02X}{rgb[1]:02X}{rgb[0]:02X}"

        style_names = [p["name"] if p else "Default" for p in sub_palettes]
        style_layouts, event_layouts = subtitle_layout.estimate_layout(
            cues, style_names, video_height=height, base_font=font_size, base_margin=margin_v,
            outline=outline, shadow=shadow, position=position_styles, font_size=font_size_styles,
            gap_1080p=gap_1080p,
        )
        fallback = subtitle_layout.StyleLayout(font_size, margin_v)
        default_layout = style_layouts.get("Default", fallback)
        font_name = subtitle_layout.FONT_NAME

        lines = [
            "[Script Info]",
            "; Script generated by Fairy Kekkai Workshop",
            "ScriptType: v4.00+",
            "WrapStyle: 0",
            "ScaledBorderAndShadow: yes",
            "YCbCr Matrix: TV.709",
            f"PlayResX: {width}",
            f"PlayResY: {height}",
        ]
        for palette in palettes:
            fill = palette["fill"]
            outline = palette["outline"]
            lines.append(
                f"; {palette['name']} fill=#{fill[0]:02X}{fill[1]:02X}{fill[2]:02X} "
                f"outline=#{outline[0]:02X}{outline[1]:02X}{outline[2]:02X} "
                f"cues={palette['count']}"
            )
        lines.extend(
            [
                "",
                "[V4+ Styles]",
                "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
                "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
                "Alignment, MarginL, MarginR, MarginV, Encoding",
                f"Style: Default,{font_name},{default_layout.font_size:g},&H00FFFFFF,&H000000FF,"
                f"&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,{outline_text},{shadow},2,10,10,"
                f"{default_layout.margin_v},1",
            ]
        )
        for palette in palettes:
            layout = style_layouts.get(palette["name"], fallback)
            lines.append(
                f"Style: {palette['name']},{font_name},{layout.font_size:g},"
                f"{ass_color(palette['fill'])},{ass_color(palette['fill'])},"
                f"{ass_color(palette['outline'])},&H64000000,0,0,0,0,100,100,0,0,"
                f"1,{outline_text},{shadow},2,10,10,{layout.margin_v},1"
            )
        lines.extend(
            [
                "",
                "[Events]",
                "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
                "MarginV, Effect, Text",
            ]
        )
        for cue, style, layout in zip(cues, style_names, event_layouts):
            if not cue.text.strip():
                continue
            sub = cue.subtitle
            start_cs, end_cs = frame_time.frame_cue_times_cs(
                sub.index_start,
                sub.index_end + 1,
                fps=self.fps,
                frame_timestamps=self.frame_timestamps,
                start_time_offset_ms=self.start_time_offset_ms,
                fallback_frame_duration_ms=self.avg_frame_duration_ms,
                frame_base=self.frame_base,
            )
            text = cue.text.replace("\n", "\\N")
            tags = ""
            if layout.anchor_y is not None:
                tags += f"\\pos({width // 2},{layout.anchor_y})"
            if layout.font_size is not None:
                tags += f"\\fs{layout.font_size}"
            if tags:
                text = "{" + tags + "}" + text
            name = "OCR: 检查位置" if layout.review else ""
            lines.append(
                f"Dialogue: 0,{frame_time.format_ass_time(start_cs)},"
                f"{frame_time.format_ass_time(end_cs)},{style},{name},0,0,{layout.margin_v},,{text}"
            )
        return "\n".join(lines) + "\n"

    def _generate_subtitles(self, sim_threshold: int, max_merge_gap_sec: float, lang: str, post_processing: bool, min_subtitle_duration_sec: float, subtitle_alignments: list[str | None]) -> None:
        print("Generating subtitles...", flush=True)
        self._color_cue_key = None
        self._merged_ass_sources = {}

        subs_zone1 = self._process_single_zone(self.pred_frames_zone1, sim_threshold, max_merge_gap_sec, lang, post_processing, min_subtitle_duration_sec)
        subs_zone2 = self._process_single_zone(self.pred_frames_zone2, sim_threshold, max_merge_gap_sec, lang, post_processing, min_subtitle_duration_sec)

        if subs_zone1 and not subs_zone2:
            self.pred_subs = subs_zone1
        elif not subs_zone1 and subs_zone2:
            self.pred_subs = subs_zone2
        elif subs_zone1 and subs_zone2:
            if subtitle_alignments[0] != subtitle_alignments[1]:
                self.pred_subs = sorted(subs_zone1 + subs_zone2, key=lambda s: s.index_start)
            else:
                self.pred_subs = self._merge_dual_zone_subtitles(subs_zone1, subs_zone2)
        else:
            self.pred_subs = []

        self._dump_debug_frames()
        self._release_temp()

    def _process_single_zone(self, pred_frames: list[PredictedFrames], sim_threshold: int, max_merge_gap_sec: float, lang: str,
                             post_processing: bool, min_subtitle_duration_sec: float) -> list[PredictedSubtitle]:
        if not pred_frames:
            return []

        language_mapping = {
            "en": "en",
            "fr": "fr",
            "german": "de",
            "it": "it",
            "es": "es",
            "pt": "pt"
        }

        language_model = None
        if post_processing:
            if lang in language_mapping:
                language_model = wordninja.LanguageModel(language=language_mapping[lang])

        subs: list[PredictedSubtitle] = []
        for frame in sorted(pred_frames, key=lambda f: f.start_index):
            new_sub = PredictedSubtitle([frame], frame.zone_index, sim_threshold, lang, language_model)
            if not new_sub.text:
                continue

            if subs:
                last_sub = subs[-1]
                if self._is_gap_mergeable(last_sub, new_sub, max_merge_gap_sec) and last_sub.is_similar_to(new_sub):
                    last_sub.frames.extend(new_sub.frames)
                    last_sub.frames.sort(key=lambda f: f.start_index)
                else:
                    subs.append(new_sub)
            else:
                subs.append(new_sub)

        for sub in subs:
            sub.finalize_text(post_processing)

        # Filter out subs that are too short
        filtered_subs = [
            sub for sub in subs if self._get_subtitle_duration_sec(sub) >= min_subtitle_duration_sec
        ]

        if not filtered_subs:
            return []

        # Re-merge the cleaned-up list of subtitles if applicable
        cleaned_subs = [filtered_subs[0]]
        for next_sub in filtered_subs[1:]:
            last_sub = cleaned_subs[-1]
            if self._is_gap_mergeable(last_sub, next_sub, max_merge_gap_sec) and last_sub.is_similar_to(next_sub):
                last_sub.frames.extend(next_sub.frames)
                last_sub.frames.sort(key=lambda f: f.start_index)
                last_sub.finalize_text(post_processing)
            else:
                cleaned_subs.append(next_sub)

        return cleaned_subs

    def _merge_dual_zone_subtitles(self, subs1: list[PredictedSubtitle], subs2: list[PredictedSubtitle]) -> list[PredictedSubtitle]:
        all_subs = sorted(subs1 + subs2, key=lambda s: s.index_start)

        if not all_subs:
            return []

        merged_subs = [all_subs[0]]
        for current_sub in all_subs[1:]:
            last_sub = merged_subs[-1]

            if current_sub.index_start <= last_sub.index_end:
                if last_sub not in self._merged_ass_sources:
                    combined = copy(last_sub)
                    combined.frames = list(last_sub.frames)
                    self._merged_ass_sources[combined] = [last_sub]
                    merged_subs[-1] = last_sub = combined
                self._merged_ass_sources[last_sub].append(current_sub)
                last_zone_info = self.validated_zones[last_sub.zone_index]
                current_zone_info = self.validated_zones[current_sub.zone_index]

                if current_zone_info['midpoint_y'] < last_zone_info['midpoint_y']:
                    last_sub.text = f"{current_sub.text}\n{last_sub.text}"
                else:
                    last_sub.text = f"{last_sub.text}\n{current_sub.text}"

                last_sub.frames.extend(current_sub.frames)
                last_sub.frames.sort(key=lambda f: f.start_index)
            else:
                merged_subs.append(current_sub)

        return merged_subs

    def _get_srt_timestamps(self, sub: PredictedSubtitle) -> tuple[str, str]:
        start_ms, end_ms = self._get_subtitle_ms_times(sub)
        start_time = utils.get_srt_timestamp_from_ms(start_ms)
        end_time = utils.get_srt_timestamp_from_ms(end_ms)
        return start_time, end_time

    def _get_subtitle_ms_times(self, sub: PredictedSubtitle) -> tuple[int, int]:
        # Frame-level cue: the end is the first frame of the next cue.  CFR
        # uses the frame index directly; VFR uses the per-frame PTS table.
        return frame_time.frame_cue_times_ms(
            sub.index_start,
            sub.index_end + 1,
            fps=self.fps,
            frame_timestamps=self.frame_timestamps,
            start_time_offset_ms=self.start_time_offset_ms,
            fallback_frame_duration_ms=self.avg_frame_duration_ms,
            frame_base=self.frame_base,
        )

    def _get_subtitle_duration_sec(self, sub: PredictedSubtitle) -> float:
        start_ms, end_ms = self._get_subtitle_ms_times(sub)
        return (end_ms - start_ms) / 1000

    def _is_gap_mergeable(self, last_sub: PredictedSubtitle, next_sub: PredictedSubtitle, max_merge_gap_sec: float) -> bool:
        _, last_end_ms = self._get_subtitle_ms_times(last_sub)
        next_start_ms, _ = self._get_subtitle_ms_times(next_sub)
        gap_ms = next_start_ms - last_end_ms
        return gap_ms <= (max_merge_gap_sec * 1000)
