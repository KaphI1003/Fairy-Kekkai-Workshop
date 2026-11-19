import os
import json
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from videocr import paddle_recognition as rec
import paddle_stage_profile


class RecognitionTests(unittest.TestCase):
    def test_shards_keep_empty_images_and_predictions_together(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "input"
            source.mkdir()
            names = ["frame0.jpg", "字幕1.jpg", "frame2.jpg"]
            for name in names:
                (source / name).touch()
            script = Path(tmp) / "engine.py"
            script.write_text(
                "import pathlib, sys\n"
                "for image in pathlib.Path(sys.argv[-1]).iterdir():\n"
                "    print(f'ppocr INFO: **********{image}**********')\n"
                "    if image.stem != 'frame2':\n"
                "        print(f'ppocr INFO: [[[0, 0]], ({image.stem!r}, 0.97)]')\n",
                encoding="utf-8",
            )
            args = [sys.executable, str(script), "--input", str(source)]
            images = rec._run_pair(args, names, lambda: None)
            self.assertEqual(images.keys(), set(names))
            self.assertEqual(images[names[0]][0][1], ("frame0", 0.97))
            self.assertEqual(images[names[1]][0][1], ("字幕1", 0.97))
            self.assertEqual(images[names[2]], [])
            self.assertEqual({p.name for p in source.iterdir()}, set(names))
            self.assertFalse(list(Path(tmp).glob("gpu_rec_*")))

    def test_fallback_uses_the_original_input_and_engine(self):
        args = [os.path.join("PaddleOCR-GPU-test", "paddleocr.exe"), "ocr", "--input", "original"]
        names = [f"{i}.jpg" for i in range(256)]
        header = ["ppocr INFO: **********0.jpg**********"]
        with patch.object(rec, "_has_gpu_headroom", return_value=True), \
             patch.object(rec, "_run_pair", side_effect=OSError("links unavailable")), \
             patch.object(rec.utils, "stream_cli_process", return_value=header) as serial:
            self.assertEqual(rec.recognize(args, names, True), {"0.jpg": []})
            serial.assert_called_once_with(args, "paddleocr_error.log")

    def test_cpu_and_short_gpu_jobs_skip_the_gpu_probe(self):
        args = ["PaddleOCR-GPU-test/paddleocr.exe", "ocr", "--input", "original"]
        with patch.object(rec, "_has_gpu_headroom") as probe, \
             patch.object(rec.utils, "stream_cli_process", return_value=[]):
            rec.recognize(args, ["one.jpg"], True)
            rec.recognize(args, ["unused"] * 300, False)
            probe.assert_not_called()

    def test_disabling_parallel_detection_does_not_disable_parallel_recognition(self):
        engine = "PaddleOCR-GPU-test/paddleocr.exe"
        names = [f"frame{i}.jpg" for i in range(300)]
        with patch.dict(os.environ, {"FKW_OCR_PARALLEL_DETECTION": "0"}), \
             patch.object(rec, "_has_gpu_headroom", return_value=True), \
             patch.object(rec.utils, "stream_cli_process", return_value=[]) as serial, \
             patch.object(rec, "_run_pair", return_value={}) as parallel:
            rec.recognize([engine, "text_detection", "--input", "original"], names, True)
            rec.recognize([engine, "ocr", "--input", "original"], names, True)
            self.assertEqual(serial.call_count, 1)
            self.assertEqual(parallel.call_count, 1)
            self.assertEqual(parallel.call_args.args[0][1], "ocr")

    def test_failed_prefetch_retries_original_engine_before_single_worker(self):
        args = ["PaddleOCR-GPU-v3.7.0-CUDA-12.9/paddleocr.exe", "ocr", "--input", "original"]
        names = [f"frame{i}.jpg" for i in range(300)]
        with patch.dict(os.environ, {"FKW_OCR_RECOGNITION_PIPELINE": "prefetch"}), \
             patch.object(rec, "_has_gpu_headroom", return_value=True), \
             patch.object(rec, "_run_pair", side_effect=[OSError("missing runtime"), {}]) as runs, \
             patch.object(rec.utils, "stream_cli_process") as serial:
            self.assertEqual(rec.recognize(args, names, True), {})
            self.assertEqual(runs.call_args_list[0].kwargs["profile"], "prefetch")
            self.assertNotIn("profile", runs.call_args_list[1].kwargs)
            serial.assert_not_called()

    def test_cuda_error_only_resubmits_unpublished_images(self):
        pipeline = SimpleNamespace(img_reader=lambda items: items, text_det_model=None, text_rec_model=None)
        def predict(path):
            if Path(path).name == "2.jpg":
                raise RuntimeError("CUDA error(715), an illegal instruction was encountered")
            yield {"rec_polys": [], "rec_texts": [], "rec_scores": []}
        model = SimpleNamespace(paddlex_pipeline=SimpleNamespace(_pipeline=pipeline),
                                predict_iter=predict, close=lambda: None)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "input"
            source.mkdir()
            for i in range(5):
                (source / f"{i}.jpg").touch()
            timing = root / "timing.json"
            argv = ["profile", "--input", str(source), "--engine-dir", tmp,
                    "--timing", str(timing), "--prefetch", "--device", "gpu"]
            def native(command, check):
                directory = Path(command[command.index("--input") + 1])
                self.assertEqual(sorted(p.name for p in directory.iterdir()), ["2.jpg", "3.jpg", "4.jpg"])
                self.assertEqual(command[command.index("--device") + 1], "gpu")
                self.assertTrue(check)
            with patch.dict(sys.modules, {"paddleocr": SimpleNamespace(PaddleOCR=lambda **kw: model)}), \
                 patch.object(sys, "argv", argv), patch("subprocess.run", side_effect=native) as run:
                paddle_stage_profile.main()
            self.assertEqual(run.call_count, 1)
            stats = json.loads(timing.read_text(encoding="utf-8"))
            self.assertEqual((stats["profiled_images"], stats["native_images"]), (2, 3))
