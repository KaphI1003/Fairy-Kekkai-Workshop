param(
    [Parameter(Mandatory = $true)][string]$SourceTools,
    [Parameter(Mandatory = $true)][string]$DestinationTools,
    [switch]$IncludeTranscriptionModel,
    [ValidateSet('ggml-small.bin', 'ggml-base.bin', 'ggml-base-q5_1.bin')][string]$TranscriptionModel = 'ggml-small.bin'
)
$ErrorActionPreference = 'Stop'
# 精确收录运行文件，增量包沿用已安装的转录模型。
$payload = @(
    'whisper\whisper-cli.exe', 'whisper\whisper.dll',
    'whisper\ggml.dll', 'whisper\ggml-base.dll',
    'whisper\ggml-cpu.dll', 'whisper\ggml-vulkan.dll',
    'Whisper.model\ggml-silero-v6.2.0.bin'
)
if ($IncludeTranscriptionModel) {
    $payload += "Whisper.model\$TranscriptionModel"
}
if ($IncludeTranscriptionModel -and (Test-Path -LiteralPath (Join-Path $SourceTools 'whisper-cpu\whisper-cli.exe') -PathType Leaf)) {
    $payload += 'whisper-cpu\whisper-cli.exe'
}
foreach ($relative in $payload) {
    $source = Join-Path $SourceTools $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "缺少 Whisper/VAD 发布文件: $source"
    }
}
foreach ($relative in $payload) {
    $destination = Join-Path $DestinationTools $relative
    New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $SourceTools $relative) -Destination $destination -Force
}
Write-Host "Whisper 已收录，是否包含转录模型：$($IncludeTranscriptionModel.IsPresent)"
