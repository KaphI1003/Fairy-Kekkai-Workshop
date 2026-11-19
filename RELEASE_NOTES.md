## 更新日志

### 新增 / Added
- OCR 导出 ASS 时可自动识别字幕颜色、字号和位置，各项可单独开启或关闭。
  ASS export can detect subtitle color, font size and position, with independent switches.
- 新增基于 Real-ESRGAN 的视频超分和基于 RIFE 的视频补帧。效果因片源而异，正式成片慎用。
  Added Real-ESRGAN super-resolution and RIFE interpolation. Preview the result before using it in a final export.
- 项目管理支持标签页，可同时打开多个项目，并分别切换和关闭。
  Project tabs allow multiple projects to stay open, with independent switching and closing.
- 新增组件定位的新手引导，说明具体按钮、输入框和操作步骤；可在设置中重新播放。
  Added a guided tutorial anchored to actual controls, with detailed steps and replay from Settings.
- Whisper 切换到 whisper.cpp，支持 Vulkan GPU 推理与 CPU 推理；新增 Silero VAD 语音检测及相关设置，减少静音片段干扰。
  Migrated to whisper.cpp with Vulkan GPU and CPU inference, adding Silero VAD and related settings to reduce interference from silent sections.

### 改进 / Improved
- Windows 安装包改用固定版本的 FFmpeg-Builds GPL 共享库构建，并提供对应源码和构建材料的索引。
  Windows installers use a pinned FFmpeg-Builds GPL shared build with an index to the corresponding source and build materials.
- 项目详情页改用可折叠内容和分页，减少页面拥挤，保留批量操作。
  Project details use collapsible content and pagination to reduce clutter while retaining batch actions.
- 视频预览支持拖动过程中持续更新，并合并待处理取帧请求；松手后定位最终位置。复杂编码视频的更新速度仍受解码耗时影响。
  Video previews update during dragging and coalesce pending frame requests, then seek to the final position on release. Complex codecs remain limited by decoding speed.
- 完整安装包包含 Whisper 多语言转录模型（CPU 包为 small，CUDA 包为 base Q5_1）、Silero VAD、支持 ASS 的新版 VideOCR、补帧和超分引擎。Clear 增量包沿用当前 Fork 安装中的转录及 OCR 模型。
  Full installers include a multilingual Whisper model (small for CPU and base Q5_1 for CUDA), Silero VAD, updated VideOCR with ASS output, interpolation and super-resolution engines. The Clear update reuses the models installed by this fork.
- 发布工作流增加依赖缓存与编译缓存，减少后续构建的重复下载和编译耗时。
  Release workflows cache dependencies and compilation results to reduce repeated work in subsequent builds.

### 修复 / Fixed
- 调整 ASS 时间戳的帧对齐，减少在字幕编辑器中出现的起止帧偏移。
  Align ASS timestamps to video frames to reduce subtitle boundary offsets in editors.
- 修复视频压制中的 ASS 与 SRT 字幕烧录。
  Fixed ASS and SRT subtitle burn-in during video encoding.
- 启动时迁移软件目录中的项目，也包含文件不完整的项目，减少损坏项目遗漏。
  Startup migration also relocates incomplete projects from the installation directory.
- 新版 Whisper 文件齐全时，启动维护自动清理旧版 Whisper 可执行文件及旧依赖。
  Startup maintenance removes obsolete Whisper executables and dependencies when the new runtime is complete.
- 缩短主页 OCR 版本显示，完整信息保留在提示中，避免文字过长导致布局溢出。
  Shortened the home OCR version label, retaining full details in its tooltip to prevent layout overflow.

## 下载提示

| 平台 / Platform | 类型 / Type | 安装包 / Installer |
| --- | --- | --- |
| Windows 10/11 | CPU | [Fairy-Kekkai-Workshop-v3.1.0-CPU-v3.7.0-Windows-x86_64-Setup.exe](https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/releases/download/v3.1.0/Fairy-Kekkai-Workshop-v3.1.0-CPU-v3.7.0-Windows-x86_64-Setup.exe) |
| Windows 10/11 | GPU (CUDA 11.8) | [Fairy-Kekkai-Workshop-v3.1.0-GPU-v3.7.0-CUDA-11.8-Windows-x86_64-Setup.exe](https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/releases/download/v3.1.0/Fairy-Kekkai-Workshop-v3.1.0-GPU-v3.7.0-CUDA-11.8-Windows-x86_64-Setup.exe) |
| Windows 10/11 | GPU (CUDA 12.9) | [Fairy-Kekkai-Workshop-v3.1.0-GPU-v3.7.0-CUDA-12.9-Windows-x86_64-Setup.exe](https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/releases/download/v3.1.0/Fairy-Kekkai-Workshop-v3.1.0-GPU-v3.7.0-CUDA-12.9-Windows-x86_64-Setup.exe) |
| Windows 10/11 | Clear 增量包 | [Fairy-Kekkai-Workshop-v3.1.0-Clear-Windows-x86_64-Setup.exe](https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/releases/download/v3.1.0/Fairy-Kekkai-Workshop-v3.1.0-Clear-Windows-x86_64-Setup.exe) |

- 第一次安装此 Fork 时请选择完整安装包。已有相同 OCR 引擎代次的 Fork 安装可以使用 Clear 增量包。
  Select a full installer for your first installation of this fork. The Clear update is for an existing installation with the same OCR engine generation.
- Whisper 的 Vulkan 版本需要可用的 Vulkan 驱动与运行库；CPU 模式可在设置中关闭 GPU 推理。
  The Vulkan Whisper runtime requires a working Vulkan driver and loader. Disable GPU inference in Settings to use CPU mode.
- macOS 版本无变动，请使用上一版对应安装包。
  macOS is unchanged; use the previous macOS package.

## 使用说明 / Usage

- Windows：根据 PaddleOCR 所需的 CPU / CUDA 版本选择安装包，升级安装前关闭正在运行的软件。
  Windows: select the appropriate PaddleOCR CPU/CUDA package and close the application before upgrading.
- 新手引导可在设置中重新播放。完整安装包的 Whisper 默认模型无需手动配置。
  Replay the tutorial from Settings. The full installer includes and selects the default Whisper model.
