from __future__ import annotations

import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from collections.abc import Iterator
from dataclasses import dataclass
from typing import IO, Any

import av
import fast_ssim
import numpy as np
import simplejpeg
from scipy import ndimage

try:
    from cpuid import cpuid, xgetbv  # type: ignore
except ImportError:  # pragma: no cover - optional build dependency
    cpuid = None
    xgetbv = None

from .lang_dictionaries import PADDLEOCR_LANGS
from .models import PredictedText

ALIGNMENT_MAP = {
    'bottom-left': 'an1', 'bottom-center': 'an2', 'bottom-right': 'an3',
    'middle-left': 'an4', 'middle-center': 'an5', 'middle-right': 'an6',
    'top-left': 'an7', 'top-center': 'an8', 'top-right': 'an9',
}
VALID_ALIGNMENT_NAMES = set(ALIGNMENT_MAP.keys())


@dataclass
class Frame:
    """Thin wrapper around a packed HxWx3 uint8 buffer (flat, row-major:
    len(data) == height * width * 3)."""
    data: bytes | bytearray | memoryview
    height: int
    width: int

    def _row_bytes(self) -> int:
        """Calculates the total number of bytes in a single row."""
        return self.width * 3

    def row(self, r: int) -> memoryview:
        """Extracts a single row of pixels as a memoryview."""
        rb = self._row_bytes()
        return memoryview(self.data)[r * rb:(r + 1) * rb]

    def crop_columns(self, x0: int, x1: int) -> Frame:
        """Crops the image vertically, returning a new Frame containing columns from x0 to x1."""
        rb = self._row_bytes()
        crop_w_bytes = (x1 - x0) * 3
        mv = memoryview(self.data)
        out = bytearray(crop_w_bytes * self.height)

        for r in range(self.height):
            src_off = r * rb + x0 * 3
            dst_off = r * crop_w_bytes
            out[dst_off:dst_off + crop_w_bytes] = mv[src_off:src_off + crop_w_bytes]

        return Frame(out, self.height, x1 - x0)

    def crop_rect(self, x0: int, y0: int, x1: int, y1: int) -> Frame:
        """Extracts a specific rectangular region and returns it as a new Frame."""
        rb = self._row_bytes()
        crop_w = max(0, x1 - x0)
        crop_h = max(0, y1 - y0)
        crop_w_bytes = crop_w * 3
        mv = memoryview(self.data)
        out = bytearray(crop_w_bytes * crop_h)

        for row_idx, r in enumerate(range(y0, y1)):
            src_off = r * rb + x0 * 3
            dst_off = row_idx * crop_w_bytes
            out[dst_off:dst_off + crop_w_bytes] = mv[src_off:src_off + crop_w_bytes]

        return Frame(out, crop_h, crop_w)

    def copy(self) -> Frame:
        """Returns a deep copy of the frame's buffer."""
        return Frame(bytes(self.data), self.height, self.width)


def video_frame_to_frame(frame: av.VideoFrame) -> Frame:
    """Converts an RGB24 frame to a contiguous Frame, removing any row-stride padding."""
    plane = frame.planes[0]
    stride = plane.line_size
    width_bytes = frame.width * 3
    mv = memoryview(plane)

    if stride == width_bytes:
        data = bytes(mv)
    else:
        rows = [mv[r * stride: r * stride + width_bytes] for r in range(frame.height)]
        data = b''.join(rows)

    return Frame(data, frame.height, frame.width)


def blit(canvas: memoryview, canvas_w: int, img: Frame, x: int, y: int) -> None:
    """Copies an RGB frame into the canvas at (x, y) in place."""
    row_bytes = img.width * 3
    canvas_stride = canvas_w * 3
    src_mv = memoryview(img.data)

    for row in range(img.height):
        dst_off = (y + row) * canvas_stride + x * 3
        src_off = row * row_bytes
        canvas[dst_off:dst_off + row_bytes] = src_mv[src_off:src_off + row_bytes]


def encode_frame_to_jpeg(canvas: memoryview, canvas_w: int, canvas_h: int, quality: int) -> bytes:
    """Encodes a flat memoryview buffer to JPEG."""
    mv = canvas.cast('B', shape=(canvas_h, canvas_w, 3))
    return simplejpeg.encode_jpeg(mv, quality=quality, colorspace='RGB')


def decode_jpeg_to_frame(g_file: str) -> tuple[str, Frame]:
    """Decodes an RGB JPEG file into a Frame."""
    with open(g_file, 'rb') as f:
        data, h, w, _ = simplejpeg.decode_jpeg(f.read(), colorspace='RGB', output='bytes')
    return g_file, Frame(data, h, w)


def get_ms_from_time_str(time_str: str) -> float:
    """Convert time string to milliseconds."""
    t = [float(x) for x in time_str.split(":")]
    if len(t) == 3:
        td = datetime.timedelta(hours=t[0], minutes=t[1], seconds=t[2])
    elif len(t) == 2:
        td = datetime.timedelta(minutes=t[0], seconds=t[1])
    else:
        raise ValueError(f'Time data "{time_str}" does not match format "%H:%M:%S"')
    return td.total_seconds() * 1000


def get_srt_timestamp(frame_index: int, fps: float, offset_ms: float = 0.0) -> str:
    """Convert frame index into SRT timestamp."""
    td = datetime.timedelta(milliseconds=(frame_index / fps * 1000 + offset_ms))
    ms = td.microseconds // 1000
    m, s = divmod(td.seconds, 60)
    h, m = divmod(m, 60)
    return f'{h:02d}:{m:02d}:{s:02d},{ms:03d}'


def get_srt_timestamp_from_ms(ms: float) -> str:
    """Convert milliseconds into SRT timestamp."""
    td = datetime.timedelta(milliseconds=ms)
    minutes, seconds = divmod(td.seconds, 60)
    hours, minutes = divmod(minutes, 60)
    milliseconds = td.microseconds // 1000
    return f'{hours:02d}:{minutes:02d}:{seconds:02d},{milliseconds:03d}'


def _load_rgb_image(path: str) -> np.ndarray:
    with open(path, 'rb') as handle:
        data, height, width, _ = simplejpeg.decode_jpeg(
            handle.read(), colorspace='RGB', output='bytes'
        )
    return np.frombuffer(data, dtype=np.uint8).reshape(height, width, 3)


def _union_box(boxes: list[list[list[float]]]) -> tuple[int, int, int, int] | None:
    xs: list[float] = []
    ys: list[float] = []
    for box in boxes:
        for point in box:
            xs.append(float(point[0]))
            ys.append(float(point[1]))
    if not xs or not ys:
        return None
    return int(min(xs)), int(min(ys)), int(max(xs)), int(max(ys))


_EIGHT_CONNECTED = np.ones((3, 3), dtype=bool)


def _component_stats(
    mask: np.ndarray, edges: np.ndarray
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Label 8-connected components and return (labels, areas, border, density).

    ``border[i]`` is True when component ``i`` touches the crop edge and
    ``density[i]`` is the mean edge-map value over that component.  Labels 0
    (background) are excluded from the returned statistics.
    """
    labels, count = ndimage.label(mask, structure=_EIGHT_CONNECTED)
    if count == 0:
        return (
            labels,
            np.zeros(0, dtype=np.int64),
            np.zeros(0, dtype=bool),
            np.zeros(0, dtype=np.float64),
        )

    flat = labels.ravel()
    areas = np.bincount(flat, minlength=count + 1)[1:].astype(np.int64)
    edge_sums = np.bincount(
        flat, weights=edges.ravel(), minlength=count + 1
    )[1:].astype(np.float64)
    density = edge_sums / np.maximum(areas, 1)

    border_ids = np.unique(
        np.concatenate((labels[0, :], labels[-1, :], labels[:, 0], labels[:, -1]))
    )
    border = np.zeros(count + 1, dtype=bool)
    border[border_ids[border_ids > 0]] = True
    return labels, areas, border[1:], density


def _dilate(mask: np.ndarray, iterations: int = 1) -> np.ndarray:
    out = mask
    for _ in range(iterations):
        padded = np.pad(out, 1)
        out = (
            padded[1:-1, 1:-1] | padded[:-2, 1:-1] | padded[2:, 1:-1]
            | padded[1:-1, :-2] | padded[1:-1, 2:]
            | padded[:-2, :-2] | padded[:-2, 2:]
            | padded[2:, :-2] | padded[2:, 2:]
        )
    return out


def _erode(mask: np.ndarray, iterations: int = 1) -> np.ndarray:
    out = mask
    for _ in range(iterations):
        padded = np.pad(out, 1)
        out = (
            padded[1:-1, 1:-1] & padded[:-2, 1:-1] & padded[2:, 1:-1]
            & padded[1:-1, :-2] & padded[1:-1, 2:]
        )
    return out


def _fill_holes(mask: np.ndarray) -> np.ndarray:
    """Fill non-mask regions that are not reachable from the crop border."""
    # binary_fill_holes uses the same 4-connected flood fill from the border
    # as the previous Python implementation, in C.
    return ndimage.binary_fill_holes(mask)


def _edge_map(gray: np.ndarray) -> np.ndarray:
    dx = np.zeros_like(gray)
    dy = np.zeros_like(gray)
    dx[:, 1:] = np.abs(np.diff(gray, axis=1))
    dy[1:, :] = np.abs(np.diff(gray, axis=0))
    magnitude = dx + dy
    threshold = max(0.06, float(np.percentile(magnitude, 85)) * 0.6)
    return magnitude >= threshold


def detect_subtitle_colors(
    image_path: str, boxes: list[list[list[float]]]
) -> tuple[tuple[int, int, int], tuple[int, int, int], float]:
    """Compatibility API; unknown components carry zero confidence."""
    from .color_styles import estimate_colors

    observation = estimate_colors(_load_rgb_image(image_path), boxes)
    return (observation.fill or (255, 255, 255),
            observation.outline or (0, 0, 0), observation.confidence)


def is_on_same_line(word1: PredictedText, word2: PredictedText) -> bool:
    """Checks if two words are on the same line based on vertical overlap."""
    y_min1 = min(p[1] for p in word1.bounding_box)
    y_max1 = max(p[1] for p in word1.bounding_box)
    y_min2 = min(p[1] for p in word2.bounding_box)
    y_max2 = max(p[1] for p in word2.bounding_box)

    midpoint1 = (y_min1 + y_max1) / 2
    midpoint2 = (y_min2 + y_max2) / 2

    return (y_min1 < midpoint2 < y_max1) or (y_min2 < midpoint1 < y_max2)


def is_language_rtl(lang: str) -> bool:
    """Checks if a given language code is written Right-to-Left (RTL)."""
    google_lens_extra_rtl = {"iw", "yi", "dv", "syr"}

    return lang in PADDLEOCR_LANGS["arabic"] or lang in google_lens_extra_rtl


def extract_non_chinese_segments(text: str) -> list[tuple[str, str]]:
    """Extracts non chinese segments out of the detected text for post processing."""
    segments: list[tuple[str, str]] = []
    current_segment = ''

    def is_chinese(char: str) -> bool:
        return '\u4e00' <= char <= '\u9fff'

    for char in text:
        if is_chinese(char):
            if current_segment:
                segments.append(('non_chinese', current_segment))
                current_segment = ''
            segments.append(('chinese', char))
        else:
            current_segment += char

    if current_segment:
        segments.append(('non_chinese', current_segment))

    return segments


def _parse_ass_color(value: str) -> tuple[int, int, int] | None:
    text = value.strip().rstrip('&')
    if text.startswith('&H') or text.startswith('&h'):
        text = text[2:]
    text = text.lstrip('Hh')
    try:
        packed = int(text, 16)
    except ValueError:
        return None
    return (packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF


def _parse_ass_time(value: str) -> int:
    hours, minutes, rest = value.strip().split(':')
    seconds, centiseconds = rest.split('.')
    return (
        (int(hours) * 60 + int(minutes)) * 60 + int(seconds)
    ) * 1000 + int(centiseconds) * 10


def _ass_plain_text(value: str) -> str:
    return re.sub(r'\{[^}]*\}', '', value).replace('\\N', ' ').strip()


def parse_ass_events(text: str) -> list[dict]:
    """Parse ASS styles and dialogue events, keeping fill/outline colors."""
    style_fields: list[str] = []
    styles: dict[str, dict] = {}
    event_fields: list[str] = []
    events: list[dict] = []
    section = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith('['):
            section = line.lower()
            continue
        if 'styles' in section:
            if line.startswith('Format:'):
                style_fields = [item.strip().lower() for item in line[7:].split(',')]
                continue
            if line.startswith('Style:') and style_fields:
                parts = line[6:].split(',', len(style_fields) - 1)
                data = dict(zip(style_fields, parts))
                name = data.get('name', '').strip()
                fill = _parse_ass_color(data.get('primarycolour', ''))
                outline = _parse_ass_color(data.get('outlinecolour', ''))
                if name and fill and outline:
                    styles[name] = {"fill": fill, "outline": outline}
            continue
        if 'events' in section:
            if line.startswith('Format:'):
                event_fields = [item.strip().lower() for item in line[7:].split(',')]
                continue
            if line.startswith('Dialogue:') and event_fields:
                parts = line[9:].split(',', len(event_fields) - 1)
                data = dict(zip(event_fields, parts))
                style_name = data.get('style', '').strip()
                style = styles.get(style_name)
                if not style:
                    continue
                try:
                    start_ms = _parse_ass_time(data.get('start', ''))
                    end_ms = _parse_ass_time(data.get('end', ''))
                except (ValueError, TypeError):
                    continue
                text_value = _ass_plain_text(data.get('text', ''))
                if text_value and end_ms > start_ms:
                    events.append(
                        {
                            "start_ms": start_ms,
                            "end_ms": end_ms,
                            "text": text_value,
                            "fill": style["fill"],
                            "outline": style["outline"],
                        }
                    )
    return events


def _find_ffmpeg() -> str | None:
    env = os.environ.get("FKW_FFMPEG")
    if env and os.path.isfile(env):
        return env
    candidates = [
        os.path.join(os.getcwd(), "ffmpeg.exe"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffmpeg.exe"),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return shutil.which("ffmpeg")


def extract_soft_subtitle_events(video_path: str) -> list[dict]:
    """Extract the first text subtitle stream as ASS and parse its styles."""
    ffmpeg = _find_ffmpeg()
    if not ffmpeg:
        return []
    handle, target = tempfile.mkstemp(prefix="fkw-soft-subs-", suffix=".ass")
    os.close(handle)
    try:
        result = subprocess.run(
            [
                ffmpeg,
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                video_path,
                "-map",
                "0:s:0",
                "-c:s",
                "ass",
                target,
            ],
            capture_output=True,
            timeout=60,
        )
        if result.returncode != 0 or not os.path.isfile(target):
            return []
        with open(target, encoding="utf-8-sig", errors="replace") as stream:
            return parse_ass_events(stream.read())
    except (OSError, subprocess.SubprocessError):
        return []
    finally:
        try:
            os.unlink(target)
        except OSError:
            pass


def _normalize_subtitle_text(value: str) -> str:
    return re.sub(r'[\s、。！？…「」『』・,.!?\-]', '', value)


def match_soft_event(
    events: list[dict], start_ms: int, end_ms: int, text: str
) -> dict | None:
    """Find a soft-subtitle event with enough time overlap and text similarity."""
    if not events:
        return None
    normalized = _normalize_subtitle_text(text)
    duration = max(1, end_ms - start_ms)
    best = None
    best_score = -1.0
    for event in events:
        overlap = min(end_ms, event["end_ms"]) - max(start_ms, event["start_ms"])
        if overlap <= 0:
            continue
        ratio = levenshtein_ratio(
            normalized, _normalize_subtitle_text(event["text"])
        )
        if ratio < 30 and overlap / duration < 0.7:
            continue
        score = overlap + ratio * 10.0
        if score > best_score:
            best = event
            best_score = score
    return best


def find_executable(program_name: str) -> str:
    """Finds an executable inside a directory starting with the program name."""
    program_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
    ext = ".exe" if sys.platform == "win32" else ".bin"
    executable_name = f"{program_name}{ext}"

    for entry in os.listdir(program_dir):
        if entry.lower().startswith(f"{program_name.lower()}-"):
            path = os.path.join(program_dir, entry, executable_name)
            if os.path.isfile(path):
                return path

    raise FileNotFoundError(f"Could not find {executable_name} in any folder starting with '{program_name}'")


def resolve_model_dirs(lang: str, use_server_model: bool, support_files_path: str | None = None) -> tuple[str, str, str]:
    """Resolves the model directory for the specified language and mode."""
    program_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
    base_path = support_files_path or os.path.join(program_dir, "PaddleOCR.PP-OCRv6.support.files")

    det_path = os.path.join(base_path, "det")
    rec_path = os.path.join(base_path, "rec")
    cls_path = os.path.join(base_path, "cls", "PP-LCNet_x1_0_textline_ori")

    v6_mode = "medium" if use_server_model else "small"
    legacy_mode = "server" if use_server_model else "mobile"

    is_v6_supported = lang in ("ch", "chinese_cht", "en", "japan") or (lang in PADDLEOCR_LANGS["latin"] and lang != "pi")

    if is_v6_supported:
        det_sub = f"PP-OCRv6_{v6_mode}_det"
        rec_sub = f"PP-OCRv6_{v6_mode}_rec"
    else:
        if lang == "ka":
            det_sub = "PP-OCRv3_mobile_det"
        else:
            det_sub = f"PP-OCRv5_{legacy_mode}_det"

        if lang in PADDLEOCR_LANGS["latin"]:
            rec_sub = "latin_PP-OCRv5_mobile_rec"
        elif lang in PADDLEOCR_LANGS["arabic"]:
            rec_sub = "arabic_PP-OCRv5_mobile_rec"
        elif lang in PADDLEOCR_LANGS["eslav"]:
            rec_sub = "eslav_PP-OCRv5_mobile_rec"
        elif lang in PADDLEOCR_LANGS["cyrillic"]:
            rec_sub = "cyrillic_PP-OCRv5_mobile_rec"
        elif lang in PADDLEOCR_LANGS["devanagari"]:
            rec_sub = "devanagari_PP-OCRv5_mobile_rec"
        elif lang in ("korean", "th", "el", "te", "ta"):
            rec_sub = f"{lang}_PP-OCRv5_mobile_rec"
        elif lang == "ka":
            rec_sub = "ka_PP-OCRv3_mobile_rec"

    return (
        os.path.join(det_path, det_sub),
        os.path.join(rec_path, rec_sub),
        cls_path
    )


def perform_hardware_check(paddleocr_path: str, use_gpu: bool) -> None:
    """Checks if the current system supports the hardware requirements."""
    error_prefix = "Unsupported Hardware Error:"
    warning_prefix = "Hardware Check Warning:"

    def has_avx() -> bool:
        if cpuid is None or xgetbv is None:
            if sys.platform == "win32":
                import ctypes

                # PF_AVX_INSTRUCTIONS_AVAILABLE = 39
                return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(39))
            try:
                with open("/proc/cpuinfo", encoding="utf-8") as handle:
                    return " avx " in handle.read()
            except OSError:
                return False
        # CPUID leaf 1: Check AVX and OSXSAVE flags
        _, _, ecx, _ = cpuid(1)
        osxsave = bool(ecx & (1 << 27))
        avx = bool(ecx & (1 << 28))

        # OS support check: XGETBV for YMM registers
        ymm_supported = False
        if osxsave:
            try:
                xcr0 = xgetbv(0)
                ymm_supported = (xcr0 & 0b110) == 0b110
            except Exception:
                ymm_supported = False

        return avx and ymm_supported

    def check_cpu() -> None:
        try:
            if not has_avx():
                raise SystemExit(f"{error_prefix} CPU or Operating System does not support the AVX instruction set, which is required.")
        except Exception as e:
            print(f"{warning_prefix} Could not determine CPU AVX support due to an error: {e}. Functionality is uncertain.", flush=True)

    CUDA_COMPATIBILITY_MAP = {
        "CUDA-11.8": (6.1, 8.9) if sys.platform == "win32" else (6.0, 8.9),
        "CUDA-12.9": (7.5, 12.0),
    }

    CUDA_DRIVER_MAP = {
        "CUDA-11.8": "451.22" if sys.platform == "win32" else "450.36.06",
        "CUDA-12.9": "527.41" if sys.platform == "win32" else "525.60.13",
    }

    def parse_version(v_str: str) -> tuple[int, ...]:
        return tuple(map(int, v_str.split('.')))

    def check_gpu() -> None:
        try:
            command = ["nvidia-smi", "--query-gpu=driver_version,compute_cap", "--format=csv,noheader"]
            result = subprocess.run(command, capture_output=True, text=True, check=True, encoding='utf-8')

            first_gpu_info = result.stdout.strip().split('\n')[0]
            if not first_gpu_info:
                raise SystemExit(f"{error_prefix} GPU mode enabled, but 'nvidia-smi' returned no GPU info.")

            driver_version_str, compute_cap_str = [item.strip() for item in first_gpu_info.split(',')]
            compute_capability = float(compute_cap_str)

            detected_cuda_version = next((v for v in CUDA_COMPATIBILITY_MAP if v in paddleocr_path), None)

            if detected_cuda_version:
                # Check Compute Capability
                min_cc, max_cc = CUDA_COMPATIBILITY_MAP[detected_cuda_version]
                if not (min_cc <= compute_capability <= max_cc):
                    raise SystemExit(
                        f"{error_prefix} GPU compute capability is {compute_capability}, but this build "
                        f"({detected_cuda_version}) requires a value between {min_cc} and {max_cc}."
                    )

                # Check NVIDIA Driver Version
                required_driver = CUDA_DRIVER_MAP[detected_cuda_version]
                if parse_version(driver_version_str) < parse_version(required_driver):
                    raise SystemExit(
                        f"{error_prefix} NVIDIA driver version is {driver_version_str}, but this build "
                        f"({detected_cuda_version}) requires version {required_driver} or newer."
                    )

        except Exception as e:
            print(f"{warning_prefix} Could not determine GPU support due to an error: {e}. Functionality is uncertain.", flush=True)

    check_cpu()

    build_folder_name = os.path.basename(os.path.dirname(paddleocr_path))

    if use_gpu and 'GPU' in build_folder_name.upper():
        check_gpu()


def read_pipe(pipe: IO[str], output_list: list[str]) -> None:
    """Reads lines from a pipe and appends them to a list."""
    try:
        for line in iter(pipe.readline, ''):
            output_list.append(line)
    finally:
        pipe.close()


def is_process_running(pid: int) -> bool:
    """Check if a process with given PID is still running."""
    try:
        if sys.platform == "win32":
            result = subprocess.run(
                ["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                capture_output=True, text=True, timeout=5
            )
            return str(pid) in result.stdout
        else:
            if os.path.exists(f"/proc/{pid}"):
                return True
    except (OSError, ProcessLookupError, subprocess.TimeoutExpired, FileNotFoundError):
        return False
    return False


def is_running_in_container() -> bool:
    """Check if the app is running inside a Docker container."""
    return os.path.exists('/.dockerenv')


def create_clean_temp_dir(base_temp: str | None = None) -> str:
    """Cleans up orphaned temporary directories from previous crashed runs and creates a fresh one for the current process."""
    current_pid = os.getpid()
    temp_prefix = f"videocr_temp_{current_pid}_"
    base_temp = base_temp or tempfile.gettempdir()
    os.makedirs(base_temp, exist_ok=True)

    for name in os.listdir(base_temp):
        if name.startswith("videocr_temp_"):
            temp_path = os.path.join(base_temp, name)
            try:
                match = re.match(r"videocr_temp_(\d+)_", name)
                if match:
                    dir_pid = int(match.group(1))

                    if dir_pid == current_pid:
                        continue

                    if os.path.isdir(temp_path):
                        if not is_process_running(dir_pid):
                            shutil.rmtree(temp_path, ignore_errors=True)
            except Exception as e:
                print(f"Could not remove leftover temp dir '{name}': {e}", flush=True)

    return tempfile.mkdtemp(prefix=temp_prefix, dir=base_temp)


def make_clean_ocr_image_dir(path: str) -> None:
    """(Re)creates an empty directory for OCR image output, clearing any existing content first."""
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)


def log_error(message: str, log_name: str = "error_log.txt") -> str:
    """Saves errors to a log file."""
    if sys.platform == "win32":
        log_dir = os.path.join(os.getenv('LOCALAPPDATA') or os.path.expanduser('~'), "VideOCR")
    else:
        log_dir = os.path.join(os.path.expanduser('~'), ".config", "VideOCR")

    os.makedirs(log_dir, exist_ok=True)

    log_path = os.path.join(log_dir, log_name)
    timestamp = datetime.datetime.now().strftime("[%Y-%m-%d %H:%M:%S]")
    with open(log_path, "a", encoding="utf-8") as f:
        f.write(f"{timestamp} {message}\n")

    return log_path


def grid_cols(item_w: int, max_width: int, padding: int, max_cols: int) -> int:
    """Calculates the maximum number of frames that can fit in one row."""
    return min(max_cols, max(1, (max_width + padding) // (item_w + padding)))


def prepare_stitch_batch(batch: list[Any], counter: int, zone_idx: int, prefix: str, out_dir: str, target_map: dict[str, Any],
                         cols: int, grid_spacing: int, zero_pad_length: int) -> tuple[str, int, int, list[tuple[Any, int, int]]]:
    """Calculates grid dimensions and maps coordinates for a batch. Returns queue arguments."""
    h, w = batch[0]["img"].height, batch[0]["img"].width

    actual_cols = min(len(batch), cols)
    actual_rows = (len(batch) + cols - 1) // cols
    canvas_w = actual_cols * w + (actual_cols - 1) * grid_spacing
    canvas_h = actual_rows * h + (actual_rows - 1) * grid_spacing

    mapping: list[dict[str, Any]] = []
    draw_instructions: list[tuple[Any, int, int]] = []

    filename = f"{prefix}_{counter:0{zero_pad_length}d}_zone{zone_idx}.jpg"
    filepath = os.path.join(out_dir, filename)

    for i, item in enumerate(batch):
        row_idx = i // cols
        col_idx = i % cols
        x_offset = col_idx * (w + grid_spacing)
        y_offset = row_idx * (h + grid_spacing)

        draw_instructions.append((item["img"], x_offset, y_offset))

        mapping.append({
            "grid_file": filepath,
            "frame_idx": item["frame_idx"],
            "zone_idx": zone_idx,
            "x": x_offset,
            "y": y_offset,
            "w": w,
            "h": h
        })

    target_map[filename] = mapping

    return filepath, canvas_w, canvas_h, draw_instructions


def get_batch_limit(cols: int, h: int, max_height: int, padding: int, max_rows: int) -> int:
    """Calculates the maximum number of frames that can fit in a stitched grid, for a given column count."""
    rows = min(max_rows, max(1, (max_height + padding) // (h + padding)))

    return cols * rows


def unstitch_polygon(poly: list[list[float]], mapping: list[dict[str, Any]]) -> list[tuple[list[list[float]], dict[str, Any]]]:
    """Finds all grid frames a polygon intersects and clips coordinates to their local space."""
    poly_min_x = min(pt[0] for pt in poly)
    poly_max_x = max(pt[0] for pt in poly)
    poly_min_y = min(pt[1] for pt in poly)
    poly_max_y = max(pt[1] for pt in poly)

    intersections = []

    for m in mapping:
        cell_min_x = m["x"]
        cell_max_x = m["x"] + m["w"]
        cell_min_y = m["y"]
        cell_max_y = m["y"] + m["h"]

        if (poly_min_x < cell_max_x and poly_max_x > cell_min_x and
            poly_min_y < cell_max_y and poly_max_y > cell_min_y):

            inter_min_x = max(poly_min_x, cell_min_x)
            inter_max_x = min(poly_max_x, cell_max_x)
            inter_min_y = max(poly_min_y, cell_min_y)
            inter_max_y = min(poly_max_y, cell_max_y)

            if inter_max_x - inter_min_x < 5 or inter_max_y - inter_min_y < 5:
                continue

            local_min_x = inter_min_x - m["x"]
            local_max_x = inter_max_x - m["x"]
            local_min_y = inter_min_y - m["y"]
            local_max_y = inter_max_y - m["y"]

            adjusted_poly = [
                [local_min_x, local_min_y],
                [local_max_x, local_min_y],
                [local_max_x, local_max_y],
                [local_min_x, local_max_y]
            ]
            intersections.append((adjusted_poly, m))

    if not intersections:
        cx = sum(pt[0] for pt in poly) / 4.0
        cy = sum(pt[1] for pt in poly) / 4.0

        best_m = min(mapping, key=lambda m:
            (cx - (m["x"] + m["w"] / 2.0))**2 +
            (cy - (m["y"] + m["h"] / 2.0))**2
        )

        adjusted_poly = [[pt[0] - best_m["x"], pt[1] - best_m["y"]] for pt in poly]
        intersections.append((adjusted_poly, best_m))

    return intersections


def stream_cli_process(args: list[str], log_name: str, extra_env: dict[str, str] | None = None) -> Iterator[str]:
    """Executes a CLI process, yields its stdout lines, and handles errors/logging."""
    cli_env = os.environ.copy()
    cli_env["PYTHONIOENCODING"] = "utf-8"
    cli_env["PYTHONUNBUFFERED"] = "1"
    if extra_env:
        cli_env.update(extra_env)

    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", env=cli_env, bufsize=1)

    stderr_lines: list[str] = []
    stderr_thread = threading.Thread(target=read_pipe, args=(process.stderr, stderr_lines))
    stderr_thread.start()

    stdout_lines: list[str] = []
    is_interrupted = False

    try:
        assert process.stdout is not None
        for line in iter(process.stdout.readline, ''):
            stdout_lines.append(line)
            yield line
    except KeyboardInterrupt:
        is_interrupted = True
        if process.poll() is None:
            process.terminate()
            process.wait()
        raise
    finally:
        if process.stdout:
            process.stdout.close()
        exit_code = process.wait()
        stderr_thread.join()

        if exit_code != 0 and not is_interrupted:
            full_stdout = "".join(stdout_lines)
            full_stderr = "".join(stderr_lines)
            command_str = ' '.join(args)
            log_message = (
                f"Process failed with exit code {exit_code}.\n"
                f"Command: {command_str}\n\n"
                f"--- STDOUT ---\n{full_stdout}\n\n"
                f"--- STDERR ---\n{full_stderr}\n"
            )
            log_file_path = log_error(log_message, log_name=log_name)
            print(f"\nError: Process failed. See the log file for technical details:\n{log_file_path}", flush=True)
            sys.exit(1)


def get_line_rects(polys: list[list[list[float]]]) -> list[list[float]]:
    """Converts a list of polygons into merged line bounding boxes [min_x, min_y, max_x, max_y]."""
    if not polys:
        return []

    rects: list[list[float]] = []
    for poly in polys:
        xs = [pt[0] for pt in poly]
        ys = [pt[1] for pt in poly]
        rects.append([min(xs), min(ys), max(xs), max(ys)])

    rects.sort(key=lambda r: r[1])

    merged_lines: list[list[float]] = []
    for r in rects:
        if not merged_lines:
            merged_lines.append(r)
        else:
            last = merged_lines[-1]
            overlap_top = max(last[1], r[1])
            overlap_bottom = min(last[3], r[3])

            if overlap_top < overlap_bottom:
                merged_lines[-1] = [
                    min(last[0], r[0]), min(last[1], r[1]),
                    max(last[2], r[2]), max(last[3], r[3])
                ]
            else:
                merged_lines.append(r)

    return merged_lines


def are_rect_lists_similar(rects1: list[list[float]], rects2: list[list[float]], tolerance: float) -> bool:
    """Compares two lists of bounding boxes to see if they are spatially similar within a tolerance."""
    if len(rects1) != len(rects2):
        return False

    for r1, r2 in zip(rects1, rects2):
        w1, h1 = r1[2] - r1[0], r1[3] - r1[1]
        w2, h2 = r2[2] - r2[0], r2[3] - r2[1]
        cx1, cy1 = r1[0] + w1 / 2, r1[1] + h1 / 2
        cx2, cy2 = r2[0] + w2 / 2, r2[1] + h2 / 2

        max_w, max_h = max(w1, w2, 1), max(h1, h2, 1)
        if not (abs(w1 - w2) / max_w <= tolerance and
                abs(h1 - h2) / max_h <= tolerance and
                abs(cx1 - cx2) / max_w <= tolerance and
                abs(cy1 - cy2) / max_h <= tolerance):
            return False

    return True


def process_ssim_group(union_rects: list[list[float]], group_frames: list[tuple[int, list[list[float]], float, dict[str, Any]]],
                       loaded_grids: dict[str, Frame], ssim_threshold: float) -> tuple[list[dict[str, Any]], int]:
    """Processes a group for SSIM, keeping the frame with the highest detection score per contiguous block."""
    local_surviving_items: list[dict[str, Any]] = []
    current_similar_batch: list[dict[str, Any]] = []
    prev_crops: list[Any] = []

    for i, (_, _, det_score, m) in enumerate(group_frames):
        grid_img = loaded_grids[m["grid_file"]]
        img = grid_img.crop_rect(m["x"], m["y"], m["x"] + m["w"], m["y"] + m["h"])
        h, w = img.height, img.width

        current_crops: list[Any] = []
        for rect in union_rects:
            cx1, cy1 = max(0, int(rect[0])), max(0, int(rect[1]))
            cx2, cy2 = min(w, int(rect[2])), min(h, int(rect[3]))
            current_crops.append(img.crop_rect(cx1, cy1, cx2, cy2))

        item_dict = {
            "img": img.copy(),
            "frame_idx": m["frame_idx"],
            "det_score": det_score
        }

        if i == 0:
            prev_crops = current_crops
            current_similar_batch.append(item_dict)
            continue

        all_lines_match = True
        for prev_c, curr_c in zip(prev_crops, current_crops):
            if prev_c.width == 0 or prev_c.height == 0 or curr_c.width == 0 or curr_c.height == 0:
                all_lines_match = False
                break
            score = fast_ssim.ssim(prev_c.data, curr_c.data, curr_c.width, curr_c.height, channels=3, data_range=255)
            if score <= ssim_threshold:
                all_lines_match = False
                break

        if all_lines_match:
            current_similar_batch.append(item_dict)
        else:
            best_item = max(current_similar_batch, key=lambda x: x["det_score"])
            best_item["frame_idx"] = current_similar_batch[0]["frame_idx"]
            local_surviving_items.append(best_item)

            current_similar_batch = [item_dict]
            prev_crops = current_crops

    if current_similar_batch:
        best_item = max(current_similar_batch, key=lambda x: x["det_score"])
        best_item["frame_idx"] = current_similar_batch[0]["frame_idx"]
        local_surviving_items.append(best_item)

    local_deleted = len(group_frames) - len(local_surviving_items)

    return local_surviving_items, local_deleted


def levenshtein_ratio(s1: str, s2: str, score_cutoff: int = 0) -> int:
    """Calculates a 0-100 Levenshtein similarity ratio."""
    if s1 == s2:
        return 100

    len1, len2 = len(s1), len(s2)
    if len1 == 0 or len2 == 0:
        return 0

    total_len = len1 + len2

    if score_cutoff > 0:
        best_possible_distance = abs(len1 - len2)
        best_possible_score = ((total_len - best_possible_distance) / total_len) * 100
        if best_possible_score < score_cutoff:
            return 0

    if len2 > len1:
        s1, s2 = s2, s1
        len1, len2 = len2, len1

    prev_row = list(range(len2 + 1))
    curr_row = [0] * (len2 + 1)

    for i in range(len1):
        curr_row[0] = i + 1
        c1 = s1[i]
        for j in range(len2):
            cost = 0 if c1 == s2[j] else 2
            del_cost = prev_row[j + 1] + 1
            ins_cost = curr_row[j] + 1
            sub_cost = prev_row[j] + cost
            best = del_cost
            if ins_cost < best:
                best = ins_cost
            if sub_cost < best:
                best = sub_cost
            curr_row[j + 1] = best
        prev_row, curr_row = curr_row, prev_row

    distance = prev_row[len2]
    return round(((total_len - distance) / total_len) * 100)
