param(
    # Qt 安装根目录（需包含 bin\windeployqt.exe）
    [string]$QtRoot = 'D:\Qt\6.7.3\msvc2019_64',
    # OpenCV 预编译包目录（需包含 OpenCVConfig.cmake），本机为 world 版
    [string]$OpenCVDir = 'D:\CODE\opencv-4.12.0\build',
    # CMake 可执行文件；留空则先试默认路径，再退回 PATH
    [string]$CMakePath = '',
    # Visual Studio 生成器；CI 的 windows-latest 用 'Visual Studio 17 2022'
    [string]$Generator = 'Visual Studio 16 2019',
    # CI 用 Ninja + sccache；本机不指定则保持原有构建方式。
    [string]$CompilerLauncher = '',
    [string]$Arch = 'x64',
    # 编译缓存目录（相对仓库根）
    [string]$BuildDir = 'build-cpp-release',
    # 待打包目录（相对仓库根）；安装包整体收录该目录
    [string]$StageDir = 'dist-cpp\Fairy-Kekkai-Workshop.dist',
    [string]$ToolsDir = (Join-Path $PSScriptRoot 'tools'),
    [string]$ThirdPartyNoticesDir = '',
    [string]$ReleaseRepository = '',
    [ValidateSet('ggml-small.bin', 'ggml-base.bin', 'ggml-base-q5_1.bin')][string]$WhisperModel = 'ggml-small.bin',
    # 额外用 Inno Setup 生成安装包
    [switch]$Setup,
    [string]$OutputDir = 'Output',
    # ISCC.exe 路径；留空则自动查找
    [string]$ISCCPath = '',
    # 安装包文件名中的 OCR 变体，如 CPU-v3.7.0；留空则按 PADDLEOCR 标识推导
    [string]$Variant = '',
    # 写入待打包目录的 OCR 版本标识（即 PADDLEOCR 文件内容）；
    # 留空则沿用仓库根同名文件，-NoTools 时不写
    [string]$PaddleOcrTag = '',
    # 收录完整运行工具与默认转录模型（-Setup 时自动开启）
    [switch]$FullTools,
    # 不收录普通工具与 PADDLEOCR 标识
    [switch]$NoTools,
    # Clear 引擎升级：即使 -NoTools，也收录 Whisper 与 Silero VAD
    [switch]$IncludeWhisper,
    # 可选非商业模型，需要提供原许可和转换说明。
    [switch]$IncludeAniSd,
    [switch]$BuildTests,
    [switch]$IncludeProjects
)

$ErrorActionPreference = 'Stop'

# C++ 主程序打包：编译 Release -> 铺 Qt/OpenCV 运行库与 tools -> 可选用 Inno Setup 出安装包。
# 安装包复用仓库根 Fairy-Kekkai-Workshop.iss，用 /D 覆盖主程序名与待打包目录，
# 因此 AppId / 安装目录 / 快捷方式与 Python 版保持一致，可直接覆盖升级旧版。
$repo = $PSScriptRoot
$source = Join-Path $repo 'cpp'
# CMake target 名（--target 用）；产物名由 CMake 的 OUTPUT_NAME 指定为 Fairy-Kekkai-Workshop.exe，
# 与 Python 版同名，覆盖升级时安装包能直接替换。
$target = 'Fairy-Kekkai-Workshop-Cpp'
$exeName = 'Fairy-Kekkai-Workshop.exe'
$issName = 'Fairy-Kekkai-Workshop.iss'
$complete = -not $NoTools -and ($FullTools -or $Setup)
$ToolsDir = [IO.Path]::GetFullPath($ToolsDir)
if ($ReleaseRepository -and $ReleaseRepository -notmatch '^[a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+$') {
    throw 'ReleaseRepository 必须为 GitHub owner/repository。'
}

if (-not (Test-Path (Join-Path $OpenCVDir 'OpenCVConfig.cmake'))) {
    throw "未找到 OpenCV 配置: $OpenCVDir\OpenCVConfig.cmake（可用 -OpenCVDir 指定）"
}
$opencvBin = Join-Path $OpenCVDir 'x64\vc16\bin'

if (-not $CMakePath) {
    $preferred = 'D:\Qt\Tools\CMake_64\bin\cmake.exe'
    if (Test-Path $preferred) { $CMakePath = $preferred }
}
if (-not $CMakePath) {
    $found = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($found) { $CMakePath = $found.Source }
}
if (-not $CMakePath) { throw '未找到 cmake.exe，请安装 CMake 或显式指定 -CMakePath。' }

$windeployqt = Join-Path $QtRoot 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) { throw "未找到 windeployqt: $windeployqt（可用 -QtRoot 指定）" }

$buildPath = Join-Path $repo $BuildDir
$stagePath = Join-Path $repo $StageDir
$exe = Join-Path $buildPath ('Release\' + $exeName)

Write-Host '[1/4] 配置 Release...'
# 旧构建目录里可能残留 vcpkg 工具链和 FFMPEG 缓存（会去找 vcpkg 版 OpenCV），这里清掉。
$configureArgs = @(
    "-DCMAKE_PREFIX_PATH=$QtRoot"
    "-DOpenCV_DIR=$OpenCVDir"
    '-UCMAKE_TOOLCHAIN_FILE'
    '-UFFMPEG_DIR'
    '-UCMAKE_MODULE_PATH'
    "-DFKW_BUILD_TESTS=$($BuildTests.IsPresent.ToString().ToUpperInvariant())"
)
$generatorArgs = @('-G', $Generator)
if ($Generator -eq 'Ninja') {
    $configureArgs += @('-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$buildPath/Release")
} else {
    $generatorArgs += @('-A', $Arch)
}
if ($CompilerLauncher) {
    if ($Generator -ne 'Ninja') { throw 'CompilerLauncher 目前仅支持 Ninja 生成器。' }
    $configureArgs += @("-DCMAKE_C_COMPILER_LAUNCHER=$CompilerLauncher", "-DCMAKE_CXX_COMPILER_LAUNCHER=$CompilerLauncher")
} else {
    $configureArgs += @('-UCMAKE_C_COMPILER_LAUNCHER', '-UCMAKE_CXX_COMPILER_LAUNCHER')
}
& $CMakePath -S $source -B $buildPath @generatorArgs @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }

Write-Host '[2/4] 编译 Release...'
$buildTargets = @($target)
if ($BuildTests) {
    $buildTargets += @('ocr-device-tests', 'superres-tests', 'interpolation-tests',
        'subtitle-encode-tests', 'subtitle-translation-tests')
}
& $CMakePath --build $buildPath --config Release --target @buildTargets --parallel
if ($LASTEXITCODE -ne 0) { throw 'Release 编译失败。' }
if (-not (Test-Path $exe)) { throw "找不到编译产物: $exe" }
if ($BuildTests) {
    $ctest = Join-Path (Split-Path $CMakePath -Parent) 'ctest.exe'
    & $CMakePath -E env "PATH=$QtRoot\bin;$opencvBin;$env:PATH" "FKW_SUBTITLE_TEST_TOOLS=$ToolsDir" `
        $ctest --test-dir $buildPath -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw '回归检查失败。' }
}

Write-Host '[3/4] 收集运行库与配套文件...'
$stagePath = [IO.Path]::GetFullPath($stagePath)
$repoPrefix = [IO.Path]::GetFullPath($repo).TrimEnd('\') + '\'
if (-not $stagePath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '暂存目录必须位于源码仓库内。'
}
if (Test-Path -LiteralPath $stagePath) {
    $existingStage = Get-Item -LiteralPath $stagePath -Force
    if ($existingStage.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '暂存目录不能是重解析点。' }
    if (-not (Test-Path -LiteralPath (Join-Path $stagePath '.fkw-package-stage') -PathType Leaf)) {
        throw "暂存目录不是本脚本创建的：$stagePath。请指定新的 StageDir。"
    }
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
New-Item -ItemType Directory -Path $stagePath -Force | Out-Null
Set-Content -LiteralPath (Join-Path $stagePath '.fkw-package-stage') -Value 'Fairy Kekkai Workshop'
Copy-Item -LiteralPath $exe -Destination $stagePath
$stageExe = Join-Path $stagePath $exeName

& $windeployqt --release --compiler-runtime --dir $stagePath $stageExe
if ($LASTEXITCODE -ne 0) { throw 'windeployqt 失败。' }

# OpenCV Release 运行库（world 版 + videoio 的 ffmpeg 后端）
foreach ($dll in @('opencv_world4120.dll', 'opencv_videoio_ffmpeg4120_64.dll')) {
    $path = Join-Path $opencvBin $dll
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "缺少 OpenCV 运行库：$path" }
    Copy-Item -LiteralPath $path -Destination $stagePath
}

$licenseDir = Join-Path $stagePath 'licenses'
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE'), (Join-Path $repo 'LICENSE-TEXT') -Destination $licenseDir
$opencvNotices = Join-Path $licenseDir 'OpenCV'
New-Item -ItemType Directory -Path $opencvNotices -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $OpenCVDir 'LICENSE') -Destination $opencvNotices
Copy-Item -LiteralPath (Join-Path $OpenCVDir 'etc\licenses') -Destination $opencvNotices -Recurse
if ($ThirdPartyNoticesDir) {
    $notices = Get-ChildItem -LiteralPath $ThirdPartyNoticesDir -Force
    if (-not $notices) { throw '第三方许可目录为空。' }
    foreach ($notice in $notices) { Copy-Item -LiteralPath $notice.FullName -Destination $licenseDir -Recurse }
}
Copy-Item -LiteralPath (Join-Path $repo 'docs\WINDOWS-RELEASE.md') -Destination $licenseDir -Force
if ($Setup -or $ReleaseRepository) {
    Set-Content -LiteralPath (Join-Path $stagePath 'release-repository.txt') -Value $ReleaseRepository -Encoding ascii
}
if ($Setup) {
    $redist = Join-Path $ToolsDir 'vc_redist.x64.exe'
    if (-not (Test-Path -LiteralPath $redist -PathType Leaf)) {
        $downloads = Join-Path $repo 'downloads'
        New-Item -ItemType Directory -Path $downloads -Force | Out-Null
        $redist = Join-Path $downloads 'vc_redist.x64.exe'
        if (-not (Test-Path -LiteralPath $redist -PathType Leaf)) {
            Invoke-WebRequest 'https://aka.ms/vc14/vc_redist.x64.exe' -OutFile $redist
        }
    }
    $signature = Get-AuthenticodeSignature -FilePath $redist
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation(?:,|$)') {
        throw "Microsoft VC++ Redistributable 签名无效：$redist"
    }
    Copy-Item -LiteralPath $redist -Destination $stagePath
}

if (-not $NoTools) {
    $toolSource = $ToolsDir
    if (-not (Test-Path $toolSource)) { throw "未找到 tools 目录: $toolSource" }
    $toolDest = Join-Path $stagePath 'tools'
    New-Item -ItemType Directory -Path $toolDest -Force | Out-Null
    $ffmpeg = Get-Content -LiteralPath (Join-Path $repo 'scripts\ffmpeg-windows-release.json') -Raw |
        ConvertFrom-Json
    $runtimeFiles = @('AviSynth.dll', 'yt-dlp.exe', 'videocr-cli.exe',
        'vsfiltermod\VSFilterMod.dll', 'vsfiltermod\SOURCES.txt', 'vsfiltermod\AviSynth-GPL.txt',
        'vsfiltermod\VSFilterMod-LICENSE.txt', 'vsfiltermod\OFL.TXT', 'vsfiltermod\lgpl_for_used_libs.txt')
    $runtimeFiles += $ffmpeg.runtimeFiles
    if ($complete) { $runtimeFiles += 'deno.exe' }
    foreach ($file in $runtimeFiles) {
        $path = Join-Path $toolSource $file
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "缺少发布工具：$path" }
        $destination = Join-Path $toolDest $file
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $path -Destination $destination
    }
    & (Join-Path $repo 'scripts\stage-superres.ps1') -SourceTools $toolSource -DestinationTools $toolDest -IncludeAniSd:$IncludeAniSd
    & (Join-Path $repo 'scripts\stage-interpolation.ps1') -SourceTools $toolSource -DestinationTools $toolDest
    # OCR 版本标识：程序运行期从 exe 同级目录读取第一行，决定 tools 下的引擎子目录
    $markerDest = Join-Path $stagePath 'PADDLEOCR'
    if ($PaddleOcrTag) {
        Set-Content -LiteralPath $markerDest -Value $PaddleOcrTag -Encoding ascii -NoNewline
    } elseif (Test-Path (Join-Path $repo 'PADDLEOCR')) {
        Copy-Item -LiteralPath (Join-Path $repo 'PADDLEOCR') -Destination $markerDest
    }
    if ($complete) {
        if (-not (Test-Path -LiteralPath $markerDest -PathType Leaf)) { throw '完整安装包需要指定 PaddleOcrTag。' }
        $engineTag = (Get-Content -LiteralPath $markerDest -TotalCount 1).Trim()
        & (Join-Path $repo 'scripts\stage-ocr.ps1') `
            -SourceTools $toolSource -DestinationTools $toolDest -PaddleOcrTag $engineTag
    }
}

if (-not $NoTools -or $IncludeWhisper) {
    & (Join-Path $repo 'scripts\stage-whisper.ps1') `
        -SourceTools $ToolsDir -DestinationTools (Join-Path $stagePath 'tools') `
        -IncludeTranscriptionModel:$complete -TranscriptionModel $WhisperModel
}

$background = Join-Path $repo 'background.jpg'
if (Test-Path $background) { Copy-Item -LiteralPath $background -Destination $stagePath }

if ($IncludeProjects) {
    Get-ChildItem -LiteralPath $repo -Directory | ForEach-Object {
        if (Test-Path (Join-Path $_.FullName '标题.txt')) {
            Copy-Item -LiteralPath $_.FullName -Destination $stagePath -Recurse
        }
    }
}

Write-Host '[4/4] 汇总...'
$stageFiles = Get-ChildItem -LiteralPath $stagePath -Recurse -File
foreach ($file in $stageFiles) {
    if ($file.Extension -ieq '.json' -and
        [IO.File]::ReadAllText($file.FullName) -match '"DeepseekApiKey"\s*:\s*"[^"]+') {
        $relative = [IO.Path]::GetRelativePath($stagePath, $file.FullName)
        throw "待打包目录包含 DeepSeek 用户密钥：$relative"
    }
}
$stageTotal = ($stageFiles | Measure-Object -Property Length -Sum).Sum
Write-Host ''
Write-Host "待打包目录: $stagePath"
Write-Host "文件数: $($stageFiles.Count)，体积: $([math]::Round($stageTotal / 1MB, 1)) MB"
Write-Host "可执行文件: $stageExe"
Write-Host '用户配置保存在 Local AppData；待打包目录不包含本机配置。'

if (-not $NoTools -and -not ($FullTools -or $Setup)) {
    Write-Host 'OCR 模型与 Whisper 转录模型未复制；需要完整包时加 -FullTools（-Setup 会自动包含）。'
}
if (-not $IncludeProjects) { Write-Host '本地项目目录未复制；需要时加 -IncludeProjects。' }

if (-not $Setup) {
    Write-Host '未生成安装包；加 -Setup 会用 Inno Setup 打包成 Setup。'
    return
}

# ---- Inno Setup：生成 Setup ----
# 变体名决定安装包文件名后缀，默认由 PADDLEOCR 标识推导（PaddleOCR-GPU-v3.7.0-CUDA-12.9 -> GPU-v3.7.0-CUDA-12.9）
if (-not $Variant) {
    $markerPath = Join-Path $stagePath 'PADDLEOCR'
    if (Test-Path $markerPath) {
        $marker = (Get-Content -LiteralPath $markerPath -TotalCount 1).Trim()
        if ($marker.StartsWith('PaddleOCR-')) { $Variant = $marker.Substring('PaddleOCR-'.Length) }
    }
    if (-not $Variant) {
        throw '无法推导安装包变体名，请用 -Variant 指定（如 CPU-v3.7.0，Clear 包请直接写 Clear）。'
    }
}

# 版本号取自 C++ 运行期读取的 setting_data.json，保证安装包文件名与程序内显示一致
$releaseSettings = Get-Content -LiteralPath (Join-Path $source 'resources\setting_data.json') -Raw |
    ConvertFrom-Json
$version = $releaseSettings.VERSION
if (-not $version) { throw '无法从 cpp\resources\setting_data.json 读取 VERSION。' }

if (-not $ISCCPath) {
    # 1) 常见的 Program Files 位置（本机 Inno Setup 装在 D 盘，故一并探测）
    $candidates = @()
    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles,
                        'D:\Program Files (x86)', 'D:\Program Files')) {
        if ($root) { $candidates += (Join-Path $root 'Inno Setup 6\ISCC.exe') }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { $ISCCPath = $candidate; break }
    }
}
if (-not $ISCCPath) {
    # 2) 注册表登记的安装位置
    foreach ($key in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1'
    )) {
        if (-not (Test-Path $key)) { continue }
        $location = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).InstallLocation
        if ($location) {
            $candidate = Join-Path $location 'ISCC.exe'
            if (Test-Path $candidate) { $ISCCPath = $candidate; break }
        }
    }
}
if (-not $ISCCPath) {
    $found = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($found) { $ISCCPath = $found.Source }
}
if (-not $ISCCPath) { throw '未找到 ISCC.exe，请安装 Inno Setup 6 或显式指定 -ISCCPath。' }
$innoLicense = Join-Path (Split-Path $ISCCPath -Parent) 'license.txt'
if (Test-Path -LiteralPath $innoLicense -PathType Leaf) {
    Copy-Item -LiteralPath $innoLicense -Destination (Join-Path $licenseDir 'Inno-Setup.txt') -Force
}

Write-Host ''
Write-Host "生成安装包：版本 $version，变体 $Variant"
$outputPath = $OutputDir
if (-not [IO.Path]::IsPathRooted($outputPath)) {
    $outputPath = Join-Path $repo $outputPath
}
$outputPath = [IO.Path]::GetFullPath($outputPath)
Push-Location $repo
try {
    $installerArgs = @("/DMyAppVersion=$version", "/DMyDisplayVersion=$($releaseSettings.DISPLAY_VERSION)", "/DMyVariant=$Variant", "/DMyAppExeName=$exeName", "/DMySourceDir=$stagePath", '/DMyFork=1', "/O$outputPath")
    if ($ReleaseRepository) { $installerArgs += "/DMyAppURL=https://github.com/$ReleaseRepository" }
    if ($complete -and $engineTag -match '^PaddleOCR-GPU-(v[0-9.]+)-CUDA-[0-9.]+$') {
        $cpuRoot = Join-Path $stagePath "tools\PaddleOCR-CPU-$($Matches[1])"
        $gpuRoot = Join-Path $stagePath "tools\$engineTag"
        $sharedFiles = 0
        $entries = foreach ($file in Get-ChildItem -LiteralPath $gpuRoot -Recurse -File) {
            $relative = $file.FullName.Substring($gpuRoot.Length + 1)
            $cpuFile = Get-Item -LiteralPath (Join-Path $cpuRoot $relative) -ErrorAction SilentlyContinue
            $source = $file.FullName
            if ($cpuFile -and $cpuFile.Length -eq $file.Length -and
                (Get-FileHash -LiteralPath $cpuFile.FullName).Hash -eq (Get-FileHash -LiteralPath $source).Hash) {
                $source = $cpuFile.FullName
                $sharedFiles++
            }
            $folder = $file.DirectoryName.Substring($stagePath.Length + 1)
            'Source: "{0}"; DestDir: "{{app}}\{1}"; Flags: ignoreversion' -f $source, $folder
        }
        $include = Join-Path $stagePath '.fkw-ocr-files.iss'
        $entries | Set-Content -LiteralPath $include -Encoding utf8BOM
        $installerArgs += "/DMyOcrFiles=$include"
        Write-Host "安装包中只存储一份 $sharedFiles 个内容相同的 CPU / CUDA OCR 文件。"
    }
    & $ISCCPath @installerArgs $issName
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup 编译失败（退出码 $LASTEXITCODE）。" }
} finally {
    Pop-Location
}

# 注意：局部变量不要与上面的参数同名——PowerShell 变量名大小写不敏感，
# 例如 $setup 会命中 [switch]$Setup，把字符串赋给 [switch] 会抛类型转换错误，故用 $setupPath。
$setupPath = Join-Path $outputPath "Fairy-Kekkai-Workshop-v$version-$Variant-Windows-x86_64-Setup.exe"
if (-not (Test-Path $setupPath)) { throw "未找到安装包产物: $setupPath" }
$size = (Get-Item -LiteralPath $setupPath).Length
if ($size -ge 2GB) { throw "安装包超过 GitHub Release 的单文件大小限制：$setupPath" }
Write-Host ''
Write-Host "安装包: $setupPath"
Write-Host "体积: $([math]::Round($size / 1MB, 1)) MB"
