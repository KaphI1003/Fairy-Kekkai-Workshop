"""Run independent subtitle images through the selected PaddleOCR model."""

from __future__ import annotations

import ast
import configparser
from collections.abc import Callable, Iterable
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
from typing import Any

from . import utils


def _read_images(lines: Iterable[str], progress: Callable[[], None]) -> dict[str, list[Any]]:
    images: dict[str, list[Any]] = {}
    name = None
    for line in lines:
        if line.startswith("OCR stage timing:"):
            print("\n" + line.strip(), flush=True)
        elif "ppocr INFO: **********" in line:
            header = re.search(r"\*+(.+?)\*+$", line.strip())
            if header:
                name = os.path.basename(header.group(1)).strip()
                images[name] = []
                progress()
        elif name and "[[" in line:
            payload = re.search(r"ppocr INFO:\s*(\[.+\])", line)
            if payload:
                try:
                    images[name].append(ast.literal_eval(payload.group(1)))
                except (ValueError, SyntaxError) as exc:
                    print(f"Error parsing OCR for {name}: {exc}", flush=True)
    return images


def _has_gpu_headroom() -> bool:
    try:
        output = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
            text=True, stderr=subprocess.DEVNULL, timeout=3,
        )
        return min(int(value) for value in output.splitlines()) >= 4096
    except (OSError, ValueError, subprocess.SubprocessError):
        return False


def _stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.kill()
    process.wait()


def _profile_command(args: list[str], shard: Path, timing: Path, prefetch: bool) -> list[str]:
    engine_dir = Path(args[0]).resolve().parent
    runtime = engine_dir.parent / "PaddleOCR-v6-runtime"
    python = runtime / ".venv" / "Scripts" / "python.exe"
    script = runtime / "paddle_stage_profile.py"
    if not python.is_file() or not script.is_file():
        raise OSError("The matching PaddleOCR profiling runtime is unavailable")
    command = [str(python), str(script), "--engine-dir", str(engine_dir),
               "--timing", str(timing), *args[2:]]
    command[command.index("--input") + 1] = str(shard)
    if prefetch:
        command.append("--prefetch")
    return command


def _recognition_pipeline(args: list[str], count: int) -> str:
    engine_dir = Path(args[0]).parent
    if engine_dir.name != "PaddleOCR-GPU-v3.7.0-CUDA-12.9":
        return ""
    mode = os.getenv("FKW_OCR_RECOGNITION_PIPELINE")
    if mode is None and count >= 1024 and "PP-OCRv6_medium_rec" in args and "PP-OCRv6_medium_det" in args:
        # Per-install opt-in for long jobs, only for the tested model/runtime.
        config = configparser.ConfigParser(interpolation=None)
        try:
            config.read(engine_dir.parent / "videocr-performance.ini", encoding="utf-8-sig")
            mode = config.get("recognition", "pipeline", fallback="native")
        except (OSError, ValueError, configparser.Error):
            return ""
    return mode if mode in ("profile", "prefetch") else ""


def _run_pair(args: list[str], names: list[str], progress: Callable[[], None],
              *, worker_count: int = 2, profile: str = "") -> dict[str, list[Any]]:
    input_index = args.index("--input") + 1
    source = Path(args[input_index])
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
    with tempfile.TemporaryDirectory(dir=source.parent, prefix="gpu_rec_") as tmp:
        with ThreadPoolExecutor(max_workers=2) as readers, ExitStack() as resources:
            workers = []
            for index in range(worker_count):
                shard = Path(tmp) / str(index)
                shard.mkdir()
                expected = names[index::worker_count]
                for name in expected:
                    os.link(source / name, shard / name)
                command = args.copy()
                command[input_index] = str(shard)
                if profile:
                    command = _profile_command(command, shard, Path(tmp) / f"timing_{index}.json",
                                               prefetch=profile == "prefetch")
                errors = resources.enter_context(tempfile.TemporaryFile())
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors,
                                           env=env, text=True, encoding="utf-8")
                assert process.stdout is not None
                resources.callback(process.stdout.close)
                # Kill children before joining their pipe readers on failure or interrupt.
                resources.callback(_stop, process)
                future = readers.submit(_read_images, process.stdout, progress)
                workers.append((process, future, errors, expected))

            results = {}
            for process, future, errors, expected in workers:
                images = future.result()
                if process.wait():
                    errors.seek(max(0, errors.tell() - 1000))
                    raise RuntimeError(errors.read().decode("utf-8", "replace").strip())
                if images.keys() != set(expected):
                    raise RuntimeError("Incomplete image set returned by PaddleOCR")
                results.update(images)
            return results


def recognize(args: list[str], names: list[str], use_gpu: bool) -> dict[str, list[Any]]:
    count = 0
    lock = threading.Lock()
    detection = args[1] == "text_detection"
    step = "Step 2/3: Performing Text-Detection" if detection else "Step 3/3: Performing OCR"

    def progress() -> None:
        nonlocal count
        with lock:
            count += 1
            print(f"\r{step} on image {count} of {len(names)}", end="", flush=True)

    # Model startup outweighs overlap on short jobs. Only shard the bundled GPU engine.
    bundled_gpu = use_gpu and Path(args[0]).parent.name.lower().startswith("paddleocr-gpu-")
    parallel = (bundled_gpu and len(names) >= (84 if detection else 256)
                and (not detection or os.getenv("FKW_OCR_PARALLEL_DETECTION", "1") != "0")
                and _has_gpu_headroom())
    profile = _recognition_pipeline(args, len(names)) if bundled_gpu and not detection else ""
    if parallel or profile:
        workers = 2 if parallel else 1
        phase = "detection" if detection else "recognition"
        print(f"GPU OCR: {phase} with {workers} worker(s).", flush=True)
        try:
            return _run_pair(args, names, progress, worker_count=workers, profile=profile)
        except (OSError, RuntimeError, ValueError) as exc:
            print(f"\nGPU OCR: worker failure ({exc}); retrying bundled engine.", flush=True)
            count = 0
        if parallel and profile:
            try:
                return _run_pair(args, names, progress)
            except (OSError, RuntimeError, ValueError) as exc:
                print(f"\nGPU OCR: retrying with one worker ({exc}).", flush=True)
                count = 0
    return _read_images(utils.stream_cli_process(args, "paddleocr_error.log"), progress)
