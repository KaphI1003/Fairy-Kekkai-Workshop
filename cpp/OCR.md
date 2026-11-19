# OCR 的 CPU / GPU 切换

“启用 GPU 加速”关闭时调用独立 CPU 引擎，开启时调用 GPU 引擎。切换前会在后台执行一张小图的真实识别，验证成功后才保存；缺少组件、驱动不兼容或识别失败时保持原选择。检查最长 120 秒，不阻塞主界面。新启动的任务读取最新设备选择，正在运行的任务继续使用原设备。视频 OCR 与悬浮截图 OCR 共用引擎选择逻辑。

“使用高精度模型”独立控制模型：v6 支持的语言使用 medium（开）或 small（关），CPU 与 GPU 共用模型。其他语言沿用对应版本模型。

“启用字幕颜色识别”（`OCR.RecognizeSubtitleColors`）默认开启。关闭后，新建 OCR 任务传递 `--no_color_styles`，ASS 使用单一的 Default 样式。

“启用字幕位置识别”和“启用字幕字号识别”分别默认开启。位置识别把 ASS 字幕居中放在 OCR 字幕块的上方，间距可设为 0–100 像素（以 1080p 为单位，默认 8 像素）。字号识别根据单行框高估算字号。ASS 将常用位置、字号写入样式，个别条目可以覆盖样式值。同屏有相隔较远的两个字幕块时，位置识别会分别定位，空间不足的条目标记为“检查位置”。这三个开关会保存，作用于新建 OCR 任务的 ASS 导出。

## 引擎布局与设置

- `tools/PaddleOCR-CPU-v3.7.0/paddleocr.exe`
- `tools/PaddleOCR-GPU-v3.7.0-CUDA-12.9/paddleocr.exe`（或 CUDA 11.8）
- `tools/PaddleOCR.PP-OCRv6.support.files/`（共用）

高级设置分别提供 CPU 与 GPU 引擎路径，配置键为 `OCR.CpuPaddleocrPath`、`OCR.GpuPaddleocrPath`。已有 `OCR.PaddleocrPath` 仅用于向对应设备迁移；不会将 GPU exe 当成 CPU 引擎。`PADDLEOCR` 保留原含义，用于确定发行包类型、引擎代次和更新匹配，不再决定开关是否可操作。新安装 CPU 包默认关闭 GPU；GPU 包默认开启；已有选择会保留。

升级已有 GPU 安装时，需要补充 CPU 引擎（从相同代次的 PaddleOCR-Standalone CPU 压缩包解压至 `tools/`），或覆盖安装新的 GPU 完整包。Clear 包依旧不带 OCR 组件。CPU 安装可补充 GPU 组件后在高级设置指定路径；当前仅支持对应引擎适配的 NVIDIA CUDA 显卡。

## 打包与清理

`scripts/stage-ocr.ps1` 精确收录 CPU 引擎、所选 GPU 引擎及一份模型，避免将本机开发环境或其他 CUDA 包重复打入安装包。所有完整包必须含 CPU 引擎；GPU 包额外包含指定 CUDA 引擎。CI 下载并校验 CPU 备用组件。安装包命名与 Clear 更新规则保持兼容。

启动清理保留同代 CPU、CUDA 11.8、CUDA 12.9 引擎及用户通过新设置显式指定的引擎；旧代未使用资源仍按原规则清理。

## 视频 OCR 性能对照

GPU 文字检测在至少 84 张拼图、可用显存至少 4 GiB 时使用两个进程，各自按原尺寸推理独立图片。设置环境变量 `FKW_OCR_PARALLEL_DETECTION=0` 可以单独回到单进程检测。设置 `FKW_OCR_COLOR_FIRST_COMPLETED=0` 可以单独恢复取色队列按提交顺序等待。取色报告记录主进程等待结果的墙钟时间，以及各子进程相邻两条任务之间间隔的总和。

在配有 CUDA 12.9 版 PaddleOCR 3.7.0 和匹配的本地 Python GPU 运行环境时，将 `app/service/CLI/paddle_stage_profile.py` 放在 `tools/PaddleOCR-v6-runtime/`，其解释器应位于该目录的 `.venv/Scripts/python.exe`。设置 `FKW_OCR_RECOGNITION_PIPELINE=profile` 可在任务日志中查看每个进程的 JPEG 读取解码、单图文字检测和文字行识别耗时，设置为 `prefetch` 会在推理时预读下一张 JPEG，设置为 `native` 使用发行版引擎。检测和识字计时均包含对应模型的预处理与后处理，重叠时间和不同进程的时间不能相加作为总耗时。

本机可以在 `tools/videocr-performance.ini` 的 `[recognition]` 段设置 `pipeline=prefetch`，仅为至少 1,024 张待识字图且检测、识别都使用 v6 medium 的任务启用预读。环境变量优先。单图推理出错（例如 CUDA 或 cuDNN 错误）时，发行版引擎接手该进程尚未返回结果的图片，另一进程继续处理自己的图片。日志中的 `profiled_images` 和 `native_images` 分别记录两种引擎处理的图片数。可选运行环境缺失时直接使用发行版引擎，工作进程无法恢复时也会用发行版引擎重试。

## 验证

配置时添加 `-DFKW_BUILD_TESTS=ON`，构建后执行 `ctest --test-dir <构建目录> -C Release --output-on-failure`。

`ocr-device-tests --probe <paddleocr.exe> <cpu|gpu> <模型根目录> <small|medium>` 执行与界面相同的真实设备检查。分别验证 CPU/GPU × small/medium 四种组合；自动回归覆盖旧配置迁移、精度独立选择、设备设置持久化、双引擎清理保护及缺少引擎的错误处理。测试配置使用独立临时目录。
