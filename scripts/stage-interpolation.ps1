param(
    [Parameter(Mandatory = $true)][string]$SourceTools,
    [Parameter(Mandatory = $true)][string]$DestinationTools,
    [switch]$Download,
    [string]$BridgePath,
    [string]$DownloadCache = (Join-Path $PSScriptRoot '..\downloads\interpolation')
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$engine = Join-Path $SourceTools 'rife'
$models = @('rife-v4.6', 'rife-v4')
$payload = @('fkw-rife-bridge.exe', 'vcomp140.dll', 'LICENSE.txt', 'NCNN-LICENSE.txt', 'GLSLANG-LICENSE.txt', 'MODEL-LICENSE.txt', 'STB-LICENSE.txt')
foreach ($model in $models) { $payload += "models\$model\flownet.bin", "models\$model\flownet.param" }
if ($Download) {
    New-Item -ItemType Directory -Path $DownloadCache -Force | Out-Null
    New-Item -ItemType Directory -Path $engine -Force | Out-Null
    $archive = Join-Path $DownloadCache 'rife-ncnn-vulkan-20221029-windows.zip'
    if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest 'https://github.com/nihui/rife-ncnn-vulkan/releases/download/20221029/rife-ncnn-vulkan-20221029-windows.zip' -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive).Hash -ne 'D8E4D772D26CD8006EF0AD0BC82EB191B53C68677D1AE2F42506D74CBBBEA606') { throw 'RIFE 模型压缩包校验失败。' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($relative in @('vcomp140.dll', 'LICENSE') + @($models | ForEach-Object { "$_/flownet.bin"; "$_/flownet.param" })) {
            $entry = $zip.GetEntry("rife-ncnn-vulkan-20221029-windows/$relative")
            if (-not $entry) { throw "RIFE 发布文件缺失: $relative" }
            $target = if ($relative -eq 'LICENSE') { Join-Path $engine 'LICENSE.txt' } elseif ($relative -eq 'vcomp140.dll') { Join-Path $engine $relative } else { Join-Path $engine "models\$relative" }
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    } finally { $zip.Dispose() }
    foreach ($license in @(
        @('NCNN-LICENSE.txt', 'https://raw.githubusercontent.com/Tencent/ncnn/20221128/LICENSE.txt'),
        @('GLSLANG-LICENSE.txt', 'https://raw.githubusercontent.com/KhronosGroup/glslang/11.12.0/LICENSE.txt'),
        @('MODEL-LICENSE.txt', 'https://raw.githubusercontent.com/hzwer/ECCV2022-RIFE/main/LICENSE'))) {
        $destination = Join-Path $engine $license[0]
        if (-not (Test-Path -LiteralPath $destination)) { Invoke-WebRequest $license[1] -OutFile $destination }
    }
    if (-not $BridgePath) {
        & (Join-Path $PSScriptRoot 'build-rife-bridge.ps1') -Cache $DownloadCache | Out-Host
        $BridgePath = Join-Path $DownloadCache 'bridge-build\fkw-rife-bridge.exe'
    }
}
if ($BridgePath) { Copy-Item -LiteralPath $BridgePath -Destination (Join-Path $engine 'fkw-rife-bridge.exe') -Force }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\third_party\realesrgan-ncnn-vulkan\STB-LICENSE.txt') -Destination (Join-Path $engine 'STB-LICENSE.txt') -Force
foreach ($relative in $payload) { if (-not (Test-Path -LiteralPath (Join-Path $engine $relative) -PathType Leaf)) { throw "缺少补帧发布文件: $relative" } }
if ([IO.Path]::GetFullPath($SourceTools) -ne [IO.Path]::GetFullPath($DestinationTools)) {
    foreach ($relative in $payload) {
        $destination = Join-Path $DestinationTools "rife\$relative"
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $engine $relative) -Destination $destination -Force
    }
}
Write-Host '补帧引擎、RIFE v4.6/v4 模型与许可证已收录。'
