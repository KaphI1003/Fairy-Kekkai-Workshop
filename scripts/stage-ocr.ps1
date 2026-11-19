param(
    [Parameter(Mandatory = $true)][string]$SourceTools,
    [Parameter(Mandatory = $true)][string]$DestinationTools,
    [Parameter(Mandatory = $true)][string]$PaddleOcrTag
)
$ErrorActionPreference = 'Stop'
if ($PaddleOcrTag -notmatch '^PaddleOCR-(CPU|GPU)-(v[0-9.]+)(-CUDA-[0-9.]+)?$') {
    throw "无效的 OCR 引擎标识: $PaddleOcrTag"
}
$cpuTag = "PaddleOCR-CPU-$($Matches[2])"
$engines = @($cpuTag)
if ($PaddleOcrTag -ne $cpuTag) { $engines += $PaddleOcrTag }
# 所有完整包必须有 CPU；GPU 完整包额外带对应 CUDA 引擎。模型只复制一份。
$payload = $engines + @('PaddleOCR.PP-OCRv6.support.files')
foreach ($name in $payload) {
    $source = Join-Path $SourceTools $name
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "缺少 OCR 发布组件: $source（GPU 包也必须包含独立 CPU 引擎）"
    }
    if ($name -in $engines -and -not (Test-Path -LiteralPath (Join-Path $source 'paddleocr.exe'))) {
        throw "缺少 OCR 引擎程序: $source\paddleocr.exe"
    }
}
New-Item -ItemType Directory -Force -Path $DestinationTools | Out-Null
foreach ($name in $payload) {
    $destination = Join-Path $DestinationTools $name
    if (Test-Path -LiteralPath $destination) { throw "OCR 暂存目标应为空: $destination" }
    New-Item -ItemType Directory -Path $destination | Out-Null
    $source = Join-Path $SourceTools $name
    Get-ChildItem -LiteralPath $source -Force | Where-Object {
        $_.Name -notin @('.paddlex', '.cache', '__pycache__', 'portable_mode.txt')
    } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $destination -Recurse }
}
Write-Host "OCR 已收录: $($payload -join ', ')"
