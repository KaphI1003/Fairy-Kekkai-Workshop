param(
    [Parameter(Mandatory = $true)][string]$SourceTools,
    [Parameter(Mandatory = $true)][string]$DestinationTools,
    [switch]$Download,
    [switch]$IncludeAniSd,
    [string]$WorkerPath,
    [string]$DownloadCache = (Join-Path $PSScriptRoot '..\downloads\superres')
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$engine = Join-Path $SourceTools 'realesrgan'
$modelNames = @('realesr-animevideov3-x2', 'realesr-animevideov3-x3', 'realesr-animevideov3-x4', 'realesrgan-x4plus', 'realesrgan-x4plus-anime')
$payload = @('realesrgan-ncnn-vulkan.exe', 'vcomp140.dll', 'README_windows.md', 'LICENSE.txt', 'MODEL-LICENSE.txt')
foreach ($model in $modelNames) { $payload += "models\$model.bin", "models\$model.param" }

if ($Download -and -not (Test-Path -LiteralPath (Join-Path $engine 'realesrgan-ncnn-vulkan.exe'))) {
    New-Item -ItemType Directory -Path $DownloadCache -Force | Out-Null
    $archive = Join-Path $DownloadCache 'realesrgan-ncnn-vulkan-20220424-windows.zip'
    $expectedHash = 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D'
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip' -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) { throw '超分引擎压缩包校验失败。' }
    $extracted = Join-Path $DownloadCache 'realesrgan-extracted'
    Expand-Archive -LiteralPath $archive -DestinationPath $extracted -Force
    New-Item -ItemType Directory -Path $engine -Force | Out-Null
    foreach ($relative in $payload | Where-Object { $_ -notin @('LICENSE.txt', 'MODEL-LICENSE.txt') }) {
        $target = Join-Path $engine $relative
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $extracted $relative) -Destination $target -Force
    }
}
if ($Download -and -not (Test-Path -LiteralPath (Join-Path $engine 'LICENSE.txt'))) {
    Invoke-WebRequest 'https://raw.githubusercontent.com/xinntao/Real-ESRGAN-ncnn-vulkan/master/LICENSE' -OutFile (Join-Path $engine 'LICENSE.txt')
}
if ($Download -and -not (Test-Path -LiteralPath (Join-Path $engine 'MODEL-LICENSE.txt'))) {
    Invoke-WebRequest 'https://raw.githubusercontent.com/xinntao/Real-ESRGAN/master/LICENSE' -OutFile (Join-Path $engine 'MODEL-LICENSE.txt')
}
if ($Download -and -not $WorkerPath -and -not (Test-Path -LiteralPath (Join-Path $engine 'fkw-superres-worker.exe'))) {
    & (Join-Path $PSScriptRoot 'build-superres-worker.ps1') | Out-Host
    $WorkerPath = Join-Path $PSScriptRoot '..\downloads\interpolation\superres-worker-build\fkw-superres-worker.exe'
}
if ($WorkerPath) {
    Copy-Item -LiteralPath $WorkerPath -Destination (Join-Path $engine 'fkw-superres-worker.exe') -Force
    Copy-Item -LiteralPath (Join-Path (Split-Path $WorkerPath -Parent) 'WORKER-LICENSES.txt') -Destination (Join-Path $engine 'WORKER-LICENSES.txt') -Force
}
if (Test-Path -LiteralPath (Join-Path $engine 'fkw-superres-worker.exe')) {
    $payload += 'fkw-superres-worker.exe', 'WORKER-LICENSES.txt'
}
if ($IncludeAniSd) {
    $payload += 'fkw-superres-worker.exe', 'ANISD-LICENSE.md', 'ANISD-NOTICE.txt',
        'models\2x_AniSD_G6i1_SPAN_215K.bin', 'models\2x_AniSD_G6i1_SPAN_215K.param'
    $payload = @($payload | Select-Object -Unique)
}
foreach ($relative in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $engine $relative) -PathType Leaf)) { throw "缺少超分发布文件: $relative" }
}
$probe = Join-Path $SourceTools 'ffprobe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) { throw '缺少 ffprobe.exe，请从 FFmpeg 发行包的 bin 目录收录。' }
New-Item -ItemType Directory -Path $DestinationTools -Force | Out-Null
if ([IO.Path]::GetFullPath($SourceTools) -ne [IO.Path]::GetFullPath($DestinationTools)) {
    foreach ($relative in $payload) {
        $destination = Join-Path $DestinationTools "realesrgan\$relative"
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $engine $relative) -Destination $destination -Force
    }
    Copy-Item -LiteralPath $probe -Destination (Join-Path $DestinationTools 'ffprobe.exe') -Force
}
Write-Host '超分引擎、模型、许可证和 ffprobe 已收录（不含演示视频与下载缓存）。'
