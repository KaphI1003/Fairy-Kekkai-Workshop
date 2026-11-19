<p align="center">
  <img src="docs\images\logo.png" alt="logo" width="200"/>
</p>

<h1 align="center">
  Fairy Kekkai Workshop ASS 适配功能增强版
</h1>

<p align="center">
  <a href="https://fkw.ora-san.org/">原项目官网</a>
</p>

<p align="center">
  <i>一站式烤肉自由软件</i>
</p>

<p align="center">
  项目管理、视频与播放列表下载、PaddleOCR 视频字幕识别、Whisper 语音识别、AI 字幕翻译、屏幕悬浮取词翻译，以及基于 FFmpeg 的视频压制。
</p>

<p align="center">
  <a style="text-decoration:none">
    <img src="https://img.shields.io/badge/LICENSE-GPL%20|%20CCBYSA-green" alt="LICENSE"/>
  </a>

  <a style="text-decoration:none">
    <img src="https://img.shields.io/badge/C%2B%2B-17-blue" alt="C++17"/>
  </a>

  <a style="text-decoration:none">
    <img src="https://img.shields.io/badge/Qt-6-green" alt="Qt 6"/>
  </a>

  <a style="text-decoration:none">
    <img src="https://img.shields.io/badge/Version-3.0.0-purple" alt="Version 3.0.0"/>
  </a>

  <a style="text-decoration:none">
    <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20macOS-orange" alt="Platform Windows | macOS"/>
  </a>

  <a style="text-decoration:none">
    <img src="https://img.shields.io/badge/Language-9%20Languages-cyan" alt="Language 9 Languages"/>
  </a>
</p>

<p align="center">
 <a href="README.md">English</a> | <a href="README.zh.md">简体中文</a>
</p>

![thumbnail](docs/images/zh/thumbnail_full_black.png)

<p align="center">
  <a href="#功能特性">功能特性</a> •
  <a href="#快速开始">快速开始</a> •
  <a href="#使用说明">使用说明</a> •
  <a href="#配置说明">配置说明</a> •
  <a href="#常见问题">常见问题</a>
</p>



## 相对原项目的改动

3.1.0 EX 基于天机阁的 [Fairy Kekkai Workshop v3.1.0](https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/tree/v3.1.0) 修改。

1. **字幕样式识别**。OCR 可自动识别字幕颜色、字号和画面位置，并写入 ASS 字幕。三项都可单独开启或关闭。
2. **ASS 时间轴适配**。按实际帧率处理字幕起止帧，减少导入 ASS 编辑器后因时间戳舍入造成的帧偏移。
3. **修复字幕压制**。视频压制可以将 ASS 或 SRT 字幕烧录到画面，并保留 ASS 字幕的样式。
4. **新增视频超分和补帧**。集成 Real-ESRGAN 和 RIFE。效果一般且因片源而异，可能出现过锐、重影或插帧伪影。建议先预览一小段，正式成片慎用。

## 功能特性

### 📁 项目管理
- 完整的项目文件系统管理
- 当前开发版本新增多项目标签页（下个版本发布）：支持切换、关闭、拖动排序，同时保留各项目详情页状态
- 支持导入/链接外部项目
- 项目进度自动追踪（封面、原视频、熟肉、原字幕、译文）
- 批量任务智能筛选和派发
- 支持从视频播放列表一键创建项目（支持所有 yt-dlp 支持的网站）
- 首次启动自动把散落在软件目录的项目迁移到独立数据目录

### 📥 视频下载
- 基于 yt-dlp，支持1800+视频网站
- 支持播放列表批量下载
- 自动下载视频封面
- 可配置并发下载数
- 支持自定义视频质量和格式

### 🔤 字幕提取（OCR）
- 基于 [VideOCR](https://github.com/timminator/VideOCR) 和 PaddleOCR
- 支持 200+ 种语言识别
- 可视化字幕区域选择
- 支持双区域OCR（上下字幕）
- 支持 GPU 加速（CUDA）
- 实时日志输出

### 🎙️ 语音识别
- 基于 [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp)
- 支持多语言语音转字幕（中文、日语、英语、韩语等）
- 实时进度显示
- 支持SRT、TXT、VTT输出格式
- 支持GPU加速

### 🌐 智能翻译
- 支持多个AI模型：Deepseek、腾讯混元、ERNIE、Gemini、书生等
- 可自定义翻译提示词模板
- Deepseek专属功能：模型切换（v4-flash/v4-pro）和深度思考模式
- 实时翻译进度显示
- 支持流式输出

### 🔍 屏幕悬浮取词翻译（新增）
- 屏幕任意区域框选后即可 OCR 并调用 AI 翻译，无需切换窗口
- 支持窗口绑定：框选区域可锁定到指定窗口，随窗口移动、缩放自动跟随
- 支持置顶、锁定、鼠标穿透、背景透明、圆角等显示模式
- 内置历史记录，可回看并复制此前的识别 / 翻译结果

### 🎬 视频压制
- 基于FFmpeg，支持自定义编码参数
- 支持硬件加速（CUDA、VideoToolbox）
- 自动嵌入字幕
- 实时输出日志

### 🔎 视频超分
- 「超分」入口包含视频超分、视频超分任务和高级设置
- 面向东方 MMD，支持 Real-ESRGAN 动漫视频、通用及动漫插图模型
- 主页面选择视频路径、目标分辨率、放大倍数或自定义尺寸，以及编码画质、格式和音频处理
- 默认复用完全相同帧的超分结果，支持跨批次及不相邻重复画面，保持原始帧数、帧率与时长
- 最多 5 秒的片段预览，显示渲染进度、处理帧数、速度和预计剩余时间，支持同步播放对比和逐帧查看
- 分批缓存、取消清理和输出校验；首版支持固定帧率 SDR 视频
- 接入、资源与验证说明见 [SUPERRES.md](cpp/SUPERRES.md)

### 🎞️ 视频补帧
- 「补帧」入口包含视频补帧、视频补帧任务和高级设置，沿用超分的布局与预览方式
- 面向东方 MMD，默认 RIFE 4.6，支持常用帧率、2/3/4 倍和自定义目标帧率，保持原视频时长与音画同步
- 默认启用静止画面复用、切镜保护；进度和速度按实际需要模型计算的新帧统计
- 拖动画面选择起点，最多预览 5 秒，原片与结果按相同播放时间同步比较
- 模型整项任务仅加载一次，预读、补帧和编码并行；与超分共用显卡排队，支持取消与失败清理
- 接入、资源与短测试说明见 [INTERPOLATION.md](cpp/INTERPOLATION.md)

### 🎨 界面特性
- 现代化UI设计（Qt 6 + Qt-Fluent-Widgets，原生 C++ 实现）
- 标题栏快捷主题切换（深色/浅色模式）
- 带进度条和状态文字的启动页
- 相邻文件快速导航
- 项目进度可视化展示
- 多语言支持（9种语言：简体中文、英语、日语、韩语、德语、西班牙语、法语、葡萄牙语、繁体中文）

---

## 系统要求

- **操作系统**：Windows 10/11（推荐）
  - OCR、语音识别与屏幕悬浮取词仅支持Windows
  - 其他功能支持macOS/Linux
- **运行方式**：
  - 直接使用安装包：**无需安装 Python**，安装包已包含全部运行依赖
  - 从源码构建：C++17 编译器（MSVC 2022）、CMake 3.21+、Qt 6（Widgets/Svg/Network）、OpenCV 4.12
- **硬件**：
  - GPU（可选）：用于OCR、Whisper、视频压制加速
  - 内存：建议8GB以上

> 3.0.0 起主程序已由 Python + PySide6 全量重写为 C++ + Qt 6 原生程序；仓库中的 `app/` 目录保留旧版 Python 实现，仅作参考，不再随安装包分发。

---

## 快速开始

### 方式一：直接安装（推荐）

从本仓库的 **Releases** 下载 3.1.0 EX 的完整安装包，按向导安装。安装包自带 OCR、Whisper、常规超分和补帧模型。

- CPU：适用于无独立显卡 / 不支持 CUDA 的设备
- GPU (CUDA 12.9)：适用于支持 CUDA 12.9 的 NVIDIA 显卡，含 CPU OCR 回退
- 视频超分和补帧需要可用的 Vulkan 驱动。在线下载和翻译需要网络，部分翻译服务需要自行填写密钥。

### 方式二：从源码构建（C++）

#### 1. 克隆仓库

```bash
git clone --recurse-submodules "<本增强版的仓库地址>"
cd Fairy-Kekkai-Workshop
```

#### 2. 准备依赖

- Qt 6（含 Widgets / Svg / Network 模块）
- OpenCV 4.12（core、videoio）
- CMake 3.21+ 与 Visual Studio 2022

#### 3. 构建

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --config Release
```

也可使用一键打包脚本完成「构建 + 依赖收集 + 生成安装包」：

```powershell
.\package-cpp-release.ps1
```

#### 4. 准备外部工具

从 [Releases](https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/releases) 页面下载 `tools.zip`，解压到项目根目录的 `tools/` 文件夹下即可，无需手动编译或单独安装各外部工具。

---

## 使用说明

### 首次运行

首次运行会按需执行项目迁移和旧版资源清理。当前开发版本已接入定位到具体组件的新手引导（计划随下个版本发布），维护结束后自动显示；可在设置页点击「新手引导」重播。引导会滚动并高亮目标按钮或输入控件，说明具体操作，支持上一步、下一步、跳过和完成，结束后返回主页。

### 主页功能

- **关于卡片**：显示应用版本信息，包含清空日志和重置设置按钮
- **项目管理**：创建和管理视频字幕项目
- **下载**：从YouTube等平台下载视频
- **OCR**：从视频中提取硬字幕（仅Windows）
- **语音识别**：从视频中提取语音转字幕（仅Windows）
- **翻译**：使用AI模型翻译字幕
- **屏幕悬浮取词**：在屏幕上框选区域直接识别并翻译（仅Windows）
- **压制**：使用FFmpeg压制视频
- **设置**：配置应用参数和外部工具路径

### 项目管理流程

1. **创建项目**：手动创建或从视频播放列表导入（支持所有 yt-dlp 支持的网站）
2. **添加剧集**：设置集数、标题、视频URL
3. **批量任务**：选择任务类型，智能筛选符合条件的剧集
4. **执行任务**：通过事件总线自动派发到对应功能界面
5. **进度追踪**：自动更新项目进度，可视化展示

### 屏幕悬浮取词

1. 在主页点击「屏幕悬浮取词」按钮，打开悬浮窗
2. 点击框选按钮，在屏幕上拖拽选择需要识别的区域
3. 识别完成后选择目标语言并触发翻译，结果直接显示在悬浮窗中
4. 可通过工具栏切换置顶、锁定、鼠标穿透、背景透明等模式，历史记录按钮可回看此前的识别结果

### 主题切换

点击标题栏最小化按钮左侧的主题切换按钮，可在深色/浅色模式之间快速切换。

### 日志管理

- 日志自动保存到 `AppData/Log/` 目录
- 在主页「关于」卡片中点击清空日志按钮可清空所有日志

### 重置设置

- 在主页「关于」卡片中点击重置设置按钮可恢复默认配置
- 重置后会自动重启应用

---

## 配置说明

主要配置项可在设置页面修改：

### 个性化
- 主题模式（深色/浅色）
- 主题色
- 界面缩放
- 背景图片
- 语言（9种语言）

### 项目
- 项目详情页数量

### 下载
- yt-dlp路径
- FFmpeg路径
- 视频格式
- 视频质量
- 最大并发下载数

### Whisper（仅 Windows）

当前引擎为官方 whisper.cpp v1.9.4，支持 Silero VAD 与停顿分段。源码准备、模型和编译说明见 [cpp/WHISPER.md](cpp/WHISPER.md)。

- CLI路径
- 模型路径
- 语言选择
- 输出格式

### AI 翻译
- 各AI模型的API Key配置
- Deepseek模型选择（v4-flash/v4-pro）
- Deepseek深度思考模式开关
- 翻译提示词模板

---

## 开发文档

- 通用开发文档：[DEVELOPMENT.md](DEVELOPMENT.md)
- C++ 版移植进度对照表：[cpp/PORTING_MATRIX.md](cpp/PORTING_MATRIX.md)

---

## 常见问题

### Q: 应用启动时显示 Shiboken 警告（仅旧版 Python 实现）

A: 这是 PySide6 的正常警告，不影响功能。可以安全忽略。3.0.0 起主程序已不再使用 Python。

### Q: 从源码构建 C++ 版失败

A:
1. 确认 CMake 能找到 Qt 6（必要时设置 `CMAKE_PREFIX_PATH` 指向 Qt 安装目录）
2. 确认 `OpenCV_DIR` 指向 OpenCV 的 `build` 目录，或把 OpenCV 解压到 `D:/CODE/opencv-4.12.0/build`
3. 确认使用 Visual Studio 2022 工具链并支持 C++17
4. 如缺少 `opencv_world4120.dll`，检查 OpenCV 版本是否为 4.12

### Q: 字幕提取失败

A:
1. 确保 `videocr-cli.exe` 存在于 `tools/` 目录
2. 确保 PaddleOCR 模型文件存在于 `tools/OCR.model/` 目录
3. 检查 VC++ 运行时是否已安装（需要 MSVCP140.dll 和 VCRUNTIME140.dll）
4. 检查 GPU 驱动是否支持 CUDA（如使用 GPU）

### Q: Whisper 语音识别失败

A:
1. 确保 whisper-cli.exe 存在于 `tools/whisper/` 目录
2. 确保所有依赖DLL（Whisper.dll、WhisperNet.dll、ComLight.dll）在同一目录
3. 确保Whisper模型文件存在于 `tools/Whisper.model/` 目录
4. 语言设置为 `auto` 时，CLI会自动检测语言
5. 检查GPU驱动是否支持DirectML（如使用GPU）

### Q: 屏幕悬浮取词无法框选

A:
1. 该功能仅支持 Windows，且需要以管理员权限运行（安装包启动时会自动请求提权）
2. 框选依赖 Win32 窗口接口，部分以管理员权限运行的窗口可能无法绑定
3. 若开启了鼠标穿透模式，请先点击工具栏对应按钮关闭后再框选

### Q: 翻译功能不可用

A:
- 确保已配置相应AI服务的API Key（在设置页面）
- C++ 版已接入 Spark、GLM 等服务，通过 HTTP API 调用；请确认密钥、网络连接及账户的模型访问权限
- 推荐使用Deepseek或腾讯混元（支持较好）
- Deepseek深度思考模式会增加推理时间，但翻译质量更高

### Q: 批量任务添加失败

A:
1. 检查项目文件结构是否完整（标题.txt、集文件夹）
2. 确保筛选条件正确（如下载任务需要视频URL）
3. 检查文件路径是否包含中文字符（某些工具不支持）

---

## 已知限制

| 功能 | 状态 | 备注 |
|------|------|------|
| 视频下载 | ✅ | 基于yt-dlp，支持1800+网站 |
| 字幕提取 | ✅ | 基于 VideOCR 和 PaddleOCR，仅 Windows |
| 语音识别 | ✅ | whisper.cpp，仅Windows，支持实时进度 |
| 翻译 | ✅ | 多 AI 服务通过 HTTP API 接入，需配置对应密钥 |
| 屏幕悬浮取词 | ✅ | 仅Windows，支持窗口绑定与跟随 |
| 视频压制 | ✅ | 基于FFmpeg，支持多种编码器 |
| B站上传 | ⚠️ | 功能已实现但因API问题未正式启用 |
| 批量处理 | ✅ | 支持批量任务，智能筛选 |

---

## 技术栈

- **主程序**：C++17 + Qt 6 + Qt-Fluent-Widgets（原生桌面程序）
- **图像处理**：OpenCV 4.12
- **视频处理**：FFmpeg + yt-dlp
- **字幕识别**：[VideOCR](https://github.com/timminator/VideOCR) 和 PaddleOCR
- **语音识别**：[ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp)
- **翻译**：多个云API（OpenAI、Deepseek、腾讯混元等）
- **JSON 处理**：nlohmann/json（header-only）
- **配置存储**：JSON
- **构建系统**：CMake 3.21+ / MSVC 2022
- **打包**：windeployqt + Inno Setup（`package-cpp-release.ps1`）；发布由 GitHub Actions 驱动，打 tag 即自动构建并发布四个变体（`.github/workflows/build-cpp.yml`）

> 仓库的 `app/` 目录保留了旧版 Python + PySide6 实现（VideOCR CLI 等外部工具的构建脚本仍在此处），仅供移植对照参考。

---

## 贡献指南

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

详细的开发指南请参阅 [DEVELOPMENT.md](DEVELOPMENT.md)

---

## 许可证

增强版程序源码沿用 [GPLv3](LICENSE)。原项目图标由天机阁提供，沿用 [CC BY-SA 4.0](LICENSE-TEXT)。安装包内的第三方组件与模型各自适用其原许可证，详见[组件与源码清单](docs/WINDOWS-RELEASE.md)。AniSD 权重另行获取，适用 CC BY-NC 4.0。

---

## 致谢

- OCR 引擎来自 [VideOCR](https://github.com/timminator/VideOCR)
- Whisper来自 [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp)

---

<a href="https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop/graphs/contributors"> <img src="https://contrib.rocks/image?repo=Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop" /> </a>

## Star History

<a href="https://www.star-history.com/?repos=Fairy-Oracle-Sanctuary%2FFairy-Kekkai-Workshop&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop&type=date&theme=light&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop&type=date&legend=top-left" />
 </picture>
</a>
