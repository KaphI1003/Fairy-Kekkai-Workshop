"""Inspect PP-OCRv6 recognition with the matching editable Paddle runtime.

The detector runs on each original recognition JPEG. Only reading the next
image overlaps inference. Model iteration times include their preprocessing
and postprocessing, and are measured at the host-visible result boundary.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def measure_predictor(predictor, stats, key):
    def predict(*args, **kwargs):
        started = time.perf_counter()
        items = list(predictor(*args, **kwargs))
        stats[key] += time.perf_counter() - started
        return items
    return predict


def finish_with_native(files, engine_dir, options):
    """Resume at the first image without a published result."""
    with tempfile.TemporaryDirectory(dir=files[0].parent.parent, prefix="native_tail_") as tmp:
        for path in files:
            os.link(path, Path(tmp) / path.name)
        command = [str(engine_dir / "paddleocr.exe"), "ocr", "--input", tmp]
        for name, value in options.items():
            if value is not None and name != "paddlex_config":
                command.extend(["--" + name, str(value).lower() if isinstance(value, bool) else str(value)])
        subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--engine-dir", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timing", required=True, type=Path)
    parser.add_argument("--prefetch", action="store_true")
    for name in ("device", "lang", "text_detection_model_dir", "text_detection_model_name",
                 "text_recognition_model_dir", "text_recognition_model_name",
                 "textline_orientation_model_dir", "textline_orientation_model_name"):
        parser.add_argument("--" + name)
    for name in ("use_textline_orientation", "use_doc_orientation_classify", "use_doc_unwarping"):
        parser.add_argument("--" + name, choices=["true", "false"], default="false")
    options = vars(parser.parse_args())
    source = options.pop("input")
    engine_dir = options.pop("engine_dir")
    output = options.pop("output")
    timing = options.pop("timing")
    prefetch = options.pop("prefetch")
    # The Python wheel installs and registers its own CUDA/cuDNN DLLs. Use the
    # standalone engine's pipeline config, without mixing its CUDA runtime in.
    config = engine_dir / "paddlex" / "configs" / "pipelines" / "OCR.yaml"
    if config.is_file():
        options["paddlex_config"] = str(config)
    os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
    started = time.perf_counter()
    from paddleocr import PaddleOCR

    for name in ("use_textline_orientation", "use_doc_orientation_classify", "use_doc_unwarping"):
        options[name] = options[name] == "true"
    model = PaddleOCR(**{k: v for k, v in options.items() if v is not None})
    pipeline = model.paddlex_pipeline._pipeline
    stats = {"model_startup_s": time.perf_counter() - started,
             "read_decode_s": 0., "detector_s": 0., "recognizer_s": 0.}
    pipeline.text_det_model = measure_predictor(pipeline.text_det_model, stats, "detector_s")
    pipeline.text_rec_model = measure_predictor(pipeline.text_rec_model, stats, "recognizer_s")
    reader = pipeline.img_reader

    def read(path):
        ts = time.perf_counter()
        image = reader([str(path)])[0]
        stats["read_decode_s"] += time.perf_counter() - ts
        return image

    files = sorted(source.glob("*.jpg"))
    predictions = {}
    native_tail = []
    began = time.perf_counter()
    with ThreadPoolExecutor(max_workers=1) as loader:
        next_image = loader.submit(read, files[0]) if prefetch and files else None
        for index, path in enumerate(files):
            image = next_image.result() if prefetch else read(path)
            if prefetch and index + 1 < len(files):
                next_image = loader.submit(read, files[index + 1])
            try:
                result, = model.predict_iter(image)
            except Exception as error:
                if output is not None:
                    raise RuntimeError(f"Inference failed on {path.name}: {error}") from error
                native_tail = files[index:]
                stats["first_native_image"] = path.name
                print(f"Inference failed on {path.name} ({error}); resuming {len(native_tail)} images with the bundled engine",
                      file=sys.stderr, flush=True)
                break
            items = [[poly.tolist(), [str(text), float(score)]] for poly, text, score in
                     zip(result["rec_polys"], result["rec_texts"], result["rec_scores"])]
            if output is not None:
                predictions[path.name] = items
            else:
                print(f"ppocr INFO: **********{path}**********", flush=True)
                for polygon, (text, score) in items:
                    print(f"ppocr INFO: {[polygon, (text, score)]!r}", flush=True)
    profile_wall = time.perf_counter() - began
    if native_tail:
        try:
            model.close()
        except Exception as error:
            print(f"Failed to release the inference model: {error}", file=sys.stderr, flush=True)
        del pipeline, model
        finish_with_native(native_tail, engine_dir, options)
    stats.update(images=len(files), profiled_images=len(files)-len(native_tail), native_images=len(native_tail),
                 prefetch=prefetch, profiled_wall_s=profile_wall, inference_wall_s=time.perf_counter()-began)
    if output is not None:
        output.write_text(json.dumps(predictions, ensure_ascii=False), encoding="utf-8")
    timing.write_text(json.dumps(stats, indent=2), encoding="utf-8")
    print("OCR stage timing: " + json.dumps(stats), flush=True)


if __name__ == "__main__":
    main()
