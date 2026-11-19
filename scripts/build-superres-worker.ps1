param(
    [string]$Cache = (Join-Path $PSScriptRoot '..\downloads\interpolation'),
    [string]$CMakePath = 'cmake',
    [string]$NinjaPath = 'ninja'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path $Cache -Force | Out-Null
$Cache = (Resolve-Path -LiteralPath $Cache).Path
function Get-VerifiedArchive($Name, $Url, $Hash, $Directory) {
    $archive = Join-Path $Cache $Name
    if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest $Url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Hash) { throw "超分构建依赖校验失败: $Name" }
    if (-not (Test-Path -LiteralPath (Join-Path $Cache $Directory))) {
        Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $Cache $Directory) -Force
    }
}
Get-VerifiedArchive 'ncnn-20221128-windows-vs2019.zip' 'https://github.com/Tencent/ncnn/releases/download/20221128/ncnn-20221128-windows-vs2019.zip' '13568F20F7C13ABC720B279712373D3C22142BAF07269284537A6D420FD1C4E9' 'sdk'
Get-VerifiedArchive 'vulkan-headers.zip' 'https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/tags/v1.3.231' '68FA5E19776D0E371CD86878DB882A5AA9A4D6E3D91ED9363F8EA14E3C356CC6' 'vulkan-headers'
$exports = Join-Path $Cache 'vulkan-1.def'
if (-not (Test-Path -LiteralPath $exports)) { Invoke-WebRequest 'https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/v1.3.231/loader/vulkan-1.def' -OutFile $exports }
if ((Get-FileHash -LiteralPath $exports).Hash -ne '166014C0B24E6D6B728A05FED1EA4B6A123ED2AC5D98F63F3A176DA4E93ED75D') { throw 'Vulkan 导出表校验失败。' }
$build = Join-Path $Cache 'superres-worker-build'
& $CMakePath -S (Join-Path $repo 'cpp\superres_engine') -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$NinjaPath" -DCMAKE_BUILD_TYPE=Release `
    "-DNCNN_ROOT=$Cache/sdk/ncnn-20221128-windows-vs2019/x64" "-DVULKAN_INCLUDE=$Cache/vulkan-headers/Vulkan-Headers-1.3.231/include" "-DVULKAN_DEF=$exports"
if ($LASTEXITCODE -ne 0) { throw '超分引擎配置失败，请在 x64 MSVC 开发环境中运行。' }
& $CMakePath --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw '超分引擎构建失败。' }
$vendor = Join-Path $repo 'third_party\realesrgan-ncnn-vulkan'
$licenseText = "FKW persistent super-resolution worker`n`n" +
    (Get-Content -LiteralPath (Join-Path $vendor 'LICENSE') -Raw) + "`n`nNCNN 20221128`n" +
    (Get-Content -LiteralPath (Join-Path $vendor 'NCNN-LICENSE.txt') -Raw) + "`n`nGLSLANG`n" +
    (Get-Content -LiteralPath (Join-Path $vendor 'GLSLANG-LICENSE.txt') -Raw) + "`n`nSTB IMAGE WRITE`n" +
    (Get-Content -LiteralPath (Join-Path $vendor 'STB-LICENSE.txt') -Raw)
Set-Content -LiteralPath (Join-Path $build 'WORKER-LICENSES.txt') -Value $licenseText -Encoding UTF8
Write-Output (Join-Path $build 'fkw-superres-worker.exe')


