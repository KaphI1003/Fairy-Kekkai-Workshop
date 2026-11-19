param(
    [Parameter(Mandatory = $true)][string]$ToolsDir,
    [Parameter(Mandatory = $true)][string]$NoticesDir,
    [string]$DownloadDir = (Join-Path $PSScriptRoot '..\downloads\release-build'),
    [string]$VideoOcrExe,
    [string]$Python,
    [ValidateSet('ggml-small.bin', 'ggml-base.bin', 'ggml-base-q5_1.bin')][string]$WhisperModel = 'ggml-small.bin',
    [switch]$IncludeAniSd
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path $PSScriptRoot -Parent
foreach ($directory in @($ToolsDir, $NoticesDir, $DownloadDir)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$ToolsDir = (Resolve-Path -LiteralPath $ToolsDir).ProviderPath
$NoticesDir = (Resolve-Path -LiteralPath $NoticesDir).ProviderPath
$DownloadDir = (Resolve-Path -LiteralPath $DownloadDir).ProviderPath

function Get-ReleaseAsset([string]$Url, [string]$Path, [string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $partial = "$Path.partial"
        & curl.exe --fail --location --silent --show-error --retry 2 --max-time 420 --output $partial $Url
        if ($LASTEXITCODE -ne 0) { throw "下载失败：$Url" }
        if ($Sha256 -and (Get-FileHash -LiteralPath $partial).Hash -ne $Sha256) { throw "校验失败：$Url" }
        Move-Item -LiteralPath $partial -Destination $Path
    }
    if ($Sha256 -and (Get-FileHash -LiteralPath $Path).Hash -ne $Sha256) { throw "已有文件校验不通过：$Path" }
}

$subtitleRuntime = @(
    @{
        Url = 'https://github.com/AviSynth/AviSynthPlus/releases/download/v3.7.5/AviSynthPlus_3.7.5_20250420-filesonly.7z'
        Archive = 'AviSynthPlus_3.7.5_20250420-filesonly.7z'
        ArchiveHash = '4db18a1b3787b46ed99e810fa41cdc238f7f6ac82ffa6ed84d53fa8cefb5146f'
        Entry = 'AviSynthPlus_3.7.5_20250420-filesonly/x64/AviSynth.dll'
        Destination = 'AviSynth.dll'
        BinaryHash = '8f7d07917e4b364df5fd7f1b8b3c1e135465d73325f8d25bc4354dd6435afab4'
    }
    @{
        Url = 'https://github.com/Masaiki/VSFilterMod/releases/download/r5.3.1/VSFilterMod_bin.zip'
        Archive = 'VSFilterMod-r5.3.1.zip'
        ArchiveHash = '8dab86d7f165cdff5153468a9b783f61de73f6e32f58768414322222b3770906'
        Entry = 'x64/VSFilterMod.dll'
        Destination = 'vsfiltermod\VSFilterMod.dll'
        BinaryHash = 'd11b4e3df394a3faf6940213a6083cff13761b48908e182ec2baa50aa1868be7'
    }
)
foreach ($runtime in $subtitleRuntime) {
    $archive = Join-Path $DownloadDir $runtime.Archive
    Get-ReleaseAsset $runtime.Url $archive $runtime.ArchiveHash
    & tar.exe -xf $archive -C $DownloadDir $runtime.Entry
    if ($LASTEXITCODE -ne 0) { throw "解压失败：$($runtime.Archive)" }
    $extracted = Join-Path $DownloadDir $runtime.Entry
    if ((Get-FileHash -LiteralPath $extracted).Hash -ne $runtime.BinaryHash) {
        throw "运行库校验失败：$($runtime.Entry)"
    }
    $destination = Join-Path $ToolsDir $runtime.Destination
    New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $extracted -Destination $destination -Force
}
$subtitleNotices = Join-Path $repo 'tools\vsfiltermod'
$subtitleTools = Join-Path $ToolsDir 'vsfiltermod'
if ($subtitleNotices -ne $subtitleTools) {
    Get-ChildItem -LiteralPath $subtitleNotices -File -Filter '*.txt' |
        Copy-Item -Destination $subtitleTools -Force
}

$ffmpeg = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ffmpeg-windows-release.json') -Raw |
    ConvertFrom-Json
$ffmpegZip = Join-Path $DownloadDir $ffmpeg.asset
$denoZip = Join-Path $DownloadDir 'deno.zip'
Get-ReleaseAsset "https://github.com/$($ffmpeg.repository)/releases/download/$($ffmpeg.tag)/$($ffmpeg.asset)" `
    $ffmpegZip $ffmpeg.sha256
Get-ReleaseAsset 'https://github.com/denoland/deno/releases/download/v2.6.10/deno-x86_64-pc-windows-msvc.zip' `
    $denoZip '6b26d272c2107ca63474c5702c824132a4a3e4469463edf0f327b9b5a5f5f854'
$ffmpegExtract = Join-Path $DownloadDir ([IO.Path]::GetFileNameWithoutExtension($ffmpeg.asset))
$denoExtract = Join-Path $DownloadDir 'deno-2.6.10'
Expand-Archive -LiteralPath $ffmpegZip -DestinationPath $ffmpegExtract -Force
Expand-Archive -LiteralPath $denoZip -DestinationPath $denoExtract -Force
$ffmpegExe = @(Get-ChildItem -LiteralPath $ffmpegExtract -File -Recurse -Filter 'ffmpeg.exe')
if ($ffmpegExe.Count -ne 1) { throw 'FFmpeg 发行包必须包含唯一的 ffmpeg.exe。' }
$ffmpegBin = $ffmpegExe[0].DirectoryName
foreach ($name in $ffmpeg.runtimeFiles) {
    Copy-Item -LiteralPath (Join-Path $ffmpegBin $name) -Destination $ToolsDir -Force
}
$ffmpegNotices = Join-Path $NoticesDir 'FFmpeg-Windows'
New-Item -ItemType Directory -Path $ffmpegNotices -Force | Out-Null
Copy-Item -LiteralPath (Join-Path (Split-Path $ffmpegBin -Parent) 'LICENSE.txt') -Destination $ffmpegNotices -Force
$ffmpegConfiguration = & (Join-Path $ffmpegBin 'ffmpeg.exe') -hide_banner -buildconf 2>&1
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg 无法加载打包的运行库。' }
$ffmpegConfiguration | Set-Content -LiteralPath (Join-Path $ffmpegNotices 'buildconf.txt') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $denoExtract 'deno.exe') -Destination $ToolsDir -Force
Get-ReleaseAsset 'https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe' `
    (Join-Path $ToolsDir 'yt-dlp.exe') 'f5389d3698ecde4ef06f4e6b1ce0294fe9ac1396228c8990b8d3416d6d177a7d'

$whisperModels = Join-Path $ToolsDir 'Whisper.model'
New-Item -ItemType Directory -Path $whisperModels -Force | Out-Null
$modelHashes = @{
    'ggml-small.bin' = '1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b'
    'ggml-base.bin' = '60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe'
    'ggml-base-q5_1.bin' = '422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898'
}
Get-ReleaseAsset "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$WhisperModel" `
    (Join-Path $whisperModels $WhisperModel) $modelHashes[$WhisperModel]

$whisperCpu = Join-Path $ToolsDir 'whisper-cpu\whisper-cli.exe'
if (-not (Test-Path -LiteralPath $whisperCpu -PathType Leaf)) {
    $whisperSource = Join-Path $DownloadDir 'whisper.cpp'
    if (-not (Test-Path -LiteralPath (Join-Path $whisperSource '.git'))) {
        & git clone --depth 1 --branch v1.9.4 https://github.com/ggml-org/whisper.cpp.git $whisperSource
        if ($LASTEXITCODE -ne 0) { throw 'Whisper 源码下载失败。' }
    }
    $revision = & git -C $whisperSource rev-parse HEAD
    if ($revision -ne '927cfce34f31707e17f2bff35c349632fb9e2c3a') { throw 'Whisper 源码版本不符。' }
    $whisperBuild = Join-Path $DownloadDir 'whisper-cpu-build'
    & cmake -S $whisperSource -B $whisperBuild -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_SERVER=OFF -DGGML_NATIVE=OFF `
        -DGGML_AVX2=OFF -DGGML_AVX=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_OPENMP=OFF `
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    if ($LASTEXITCODE -ne 0) { throw 'Whisper CPU 配置失败。' }
    & cmake --build $whisperBuild --target whisper-cli --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Whisper CPU 构建失败。' }
    New-Item -ItemType Directory -Path (Split-Path $whisperCpu -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $whisperBuild 'bin\whisper-cli.exe') -Destination $whisperCpu
}

if (-not $VideoOcrExe) {
    if (-not $Python) { throw '请提供 VideOCR 构建所用的 Python 3.11，或新版 VideOCR 可执行文件。' }
    $output = Join-Path $DownloadDir 'videocr'
    & $Python -m nuitka --onefile --mingw64 --jobs=4 --assume-yes-for-downloads `
        "--output-dir=$output" "--report=$(Join-Path $DownloadDir 'videocr-report.xml')" `
        (Join-Path $repo 'app\service\CLI\videocr_cli.py')
    if ($LASTEXITCODE -ne 0) { throw 'VideOCR 构建失败。' }
    $VideoOcrExe = Join-Path $output 'videocr-cli.exe'
}
$videocrHelp = & $VideoOcrExe --help | Out-String
if ($LASTEXITCODE -ne 0 -or $videocrHelp -notmatch '--ass_output' -or $videocrHelp -notmatch '--no_subtitle_font_size') {
    throw 'VideOCR 缺少新版 ASS 输出选项。'
}
Copy-Item -LiteralPath $VideoOcrExe -Destination (Join-Path $ToolsDir 'videocr-cli.exe') -Force
if ($Python) {
    $pythonSite = & $Python -c 'import sysconfig; print(sysconfig.get_path("purelib"))'
    if ($LASTEXITCODE -ne 0) { throw '未能定位 VideOCR 依赖的许可。' }
    foreach ($package in Get-ChildItem -LiteralPath $pythonSite -Directory -Filter '*.dist-info') {
        $destination = Join-Path $NoticesDir "VideOCR-python\$($package.Name)"
        foreach ($notice in Get-ChildItem -LiteralPath $package.FullName -Recurse -File) {
            if ($notice.Name -notmatch '^(COPYING|COPYRIGHT|LICEN[CS]E|NOTICE)') { continue }
            $relative = $notice.FullName.Substring($package.FullName.Length + 1)
            $target = Join-Path $destination $relative
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $notice.FullName -Destination $target -Force
        }
    }
}

$noticeSources = [ordered]@{
    'VideOCR-MIT.txt' = 'https://raw.githubusercontent.com/timminator/VideOCR/master/LICENSE'
    'Qt-LGPL-3.0.txt' = 'https://raw.githubusercontent.com/qt/qtbase/v6.7.3/LICENSES/LGPL-3.0-only.txt'
    'FFmpeg-LGPL-3.0.txt' = 'https://raw.githubusercontent.com/FFmpeg/FFmpeg/n8.1.2/COPYING.LGPLv3'
    'yt-dlp.txt' = 'https://raw.githubusercontent.com/yt-dlp/yt-dlp/2026.10.26/THIRD_PARTY_LICENSES.txt'
    'Deno.txt' = 'https://raw.githubusercontent.com/denoland/deno/v2.6.10/LICENSE.md'
    'Whisper-cpp-MIT.txt' = 'https://raw.githubusercontent.com/ggml-org/whisper.cpp/927cfce34f31707e17f2bff35c349632fb9e2c3a/LICENSE'
    'Whisper-model-MIT.txt' = 'https://raw.githubusercontent.com/openai/whisper/v20250625/LICENSE'
    'Silero-VAD-MIT.txt' = 'https://raw.githubusercontent.com/snakers4/silero-vad/master/LICENSE'
    'PaddleOCR-Apache-2.0.txt' = 'https://raw.githubusercontent.com/timminator/PaddleOCR-Standalone/v3.7.0/LICENSE'
}
foreach ($filename in $noticeSources.Keys) {
    Get-ReleaseAsset $noticeSources[$filename] (Join-Path $NoticesDir $filename) ''
}
$intelZip = Join-Path $DownloadDir 'mklml.zip'
Get-ReleaseAsset 'https://paddlepaddledeps.bj.bcebos.com/mklml_win_2019.0.5.20190502.zip' `
    $intelZip '535857b17643d7f7546b58fc621244e7cfcc4fff2aa2ebd3fc5b4e126bfc36cf'
$intelNotices = Join-Path $NoticesDir 'Intel-MKLML'
New-Item -ItemType Directory -Path $intelNotices -Force | Out-Null
$intelArchive = [IO.Compression.ZipFile]::OpenRead($intelZip)
try {
    $license = $intelArchive.GetEntry('mklml_win_2019.0.5.20190502/license.txt')
    if (-not $license) { throw 'Intel MKLML 许可文件缺失。' }
    [IO.Compression.ZipFileExtensions]::ExtractToFile($license, (Join-Path $intelNotices 'license.txt'), $true)
} finally {
    $intelArchive.Dispose()
}

if ($IncludeAniSd) {
    $engine = Join-Path $ToolsDir 'realesrgan'
    Get-ReleaseAsset 'https://raw.githubusercontent.com/Sirosky/Upscale-Hub/AniSD/LICENSE.md' `
        (Join-Path $engine 'ANISD-LICENSE.md') ''
    @(
        'AniSD SPAN 2x_AniSD_G6i1_SPAN_215K by Sirosky',
        'Source: https://github.com/Sirosky/Upscale-Hub/releases/tag/AniSD',
        'License: CC BY-NC 4.0, see ANISD-LICENSE.md.',
        'Changes: FP32 ONNX converted to ncnn and input/output node names changed.'
    ) | Set-Content -LiteralPath (Join-Path $engine 'ANISD-NOTICE.txt') -Encoding utf8
}
