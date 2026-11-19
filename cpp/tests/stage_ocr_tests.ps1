$ErrorActionPreference = 'Stop'
$stageScript = Join-Path $PSScriptRoot '..\..\scripts\stage-ocr.ps1'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('fkw-ocr-stage-' + [guid]::NewGuid().ToString('N'))
$sourceTools = Join-Path $fixture 'source'
foreach ($name in @('PaddleOCR-CPU-v3.7.0', 'PaddleOCR-GPU-v3.7.0-CUDA-11.8',
                     'PaddleOCR-GPU-v3.7.0-CUDA-12.9', 'PaddleOCR.PP-OCRv6.support.files')) {
    $directory = Join-Path $sourceTools $name
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    Set-Content -LiteralPath (Join-Path $directory 'paddleocr.exe') -Value 'fixture'
    Set-Content -LiteralPath (Join-Path $directory 'portable_mode.txt') -Value ''
    New-Item -ItemType Directory -Path (Join-Path $directory '.paddlex') | Out-Null
}
foreach ($tag in @('PaddleOCR-CPU-v3.7.0', 'PaddleOCR-GPU-v3.7.0-CUDA-11.8', 'PaddleOCR-GPU-v3.7.0-CUDA-12.9')) {
    $destination = Join-Path $fixture $tag
    & $stageScript -SourceTools $sourceTools -DestinationTools $destination -PaddleOcrTag $tag
    if (-not (Test-Path -LiteralPath (Join-Path $destination 'PaddleOCR-CPU-v3.7.0/paddleocr.exe'))) {
        throw "CPU engine missing from $tag"
    }
    $expected = if ($tag -like '*-GPU-*') { 3 } else { 2 }
    if (@(Get-ChildItem -LiteralPath $destination).Count -ne $expected) {
        throw "Wrong payload count for $tag"
    }
    if (Get-ChildItem -LiteralPath $destination -Force -Recurse |
        Where-Object Name -in @('portable_mode.txt', '.paddlex')) {
        throw "Developer cache or portable-mode marker shipped in $tag"
    }
}
# GPU 引擎存在但 CPU 缺失时，打包必须失败。
$gpuOnly = Join-Path $fixture 'gpu-only'
New-Item -ItemType Directory -Force -Path $gpuOnly | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceTools 'PaddleOCR-GPU-v3.7.0-CUDA-12.9') -Destination $gpuOnly -Recurse
$rejected = $false
try {
    & $stageScript -SourceTools $gpuOnly -DestinationTools (Join-Path $fixture 'invalid') `
        -PaddleOcrTag 'PaddleOCR-GPU-v3.7.0-CUDA-12.9'
} catch { $rejected = $true }
if (-not $rejected) { throw 'GPU installer without CPU was accepted' }
Write-Host 'OCR staging: CPU, both CUDA variants, shared models and missing-CPU rejection passed.'
