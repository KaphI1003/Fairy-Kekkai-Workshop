# Use the same Windows PowerShell executable and arguments as the uninstaller.
$ErrorActionPreference = 'Stop'
$cleaner = Join-Path $PSScriptRoot 'uninstall-cleanup.ps1'
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$temp = [IO.DirectoryInfo]::new([IO.Path]::GetTempPath())
$fixture = $temp.CreateSubdirectory('fkw-cleanup-test-' + [guid]::NewGuid().ToString('N'))
$install = Join-Path $fixture.FullName 'Workshop Fork'
$data = Join-Path $fixture.FullName 'Local\Fairy-Oracle-Sanctuary\Fairy-Kekkai-Workshop'
$library = Join-Path $fixture.FullName 'Fairy-Kekkai-Workshop-Projects'
$junction = Join-Path $install 'tools\external'

function Put([string]$Path, [string]$Contents) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Contents)
}
function Run-Cleanup([switch]$Shared, [switch]$ExpectFailure) {
    $flags = @()
    if ($Shared) { $flags += '-KeepSharedData' }
    # Windows PowerShell reports native stderr as errors even when it is redirected.
    $ErrorActionPreference = 'Continue'
    $output = & $powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $cleaner -InstallDir "$install\." -UserDataDir $data @flags 2>&1
    if (($LASTEXITCODE -eq 0) -eq $ExpectFailure.IsPresent) { throw "Unexpected result ($LASTEXITCODE): $output" }
}
function Assert-Gone([string[]]$Paths) {
    foreach ($path in $Paths) {
        if (Test-Path -LiteralPath $path) { throw "Leftover: $path" }
    }
}

try {
    $cacheDirs = @('Log', 'ocr-temp', 'paddlex', 'InterpolationPreview', 'InterpolationTemp', 'SuperresPreview', 'SuperresTemp')
    $garbage = @("$install\tools\ocr\__pycache__\sample.pyc", "$install\platforms\plugin.dll",
                 "$install\Fairy-Kekkai-Workshop.exe", "$install\AppData\Log\old.log", "$install\config\config.json",
                 "$install\AppData\config.json", "$data\config.json")
    foreach ($cache in $cacheDirs) { $garbage += "$data\$cache\generated\sample.bin" }
    foreach ($path in $garbage) { Put $path 'discard' }

    # Place registered projects *inside* cleanup targets to exercise the exclusion.
    $root = "$data\paddlex\My collection"
    $linked = "$install\tools\custom [linked]"
    $ordered = "$data\SuperresTemp\Ordered"
    $index = "$data\project.json"
    Put $index (@{project_root=$root; project_link=@($linked); project_order=@($ordered)} | ConvertTo-Json)
    Put "$install\AppData\project.json" '{"project_link":["tools/relative"]}'
    $safe = @($index, "$install\AppData\project.json")
    foreach ($folder in @("$library\Series", "$install\project\Inside", "$data\Projects\Fallback",
                          "$root\Nested", $linked, $ordered, "$install\tools\relative",
                          "$install\tools\old project")) {
        foreach ($file in '1\source.mp4', '1\rendered.mp4', '1\subtitles.ass') {
            $path = Join-Path $folder $file
            Put $path ([guid]::NewGuid().ToString('N'))
            $safe += $path
        }
    }
    foreach ($path in @("$install\tools\old project\标题.txt", "$install\user-notes.txt")) {
        Put $path 'preserve'
        $safe += $path
    }
    [void](New-Item -ItemType Junction -Path $junction -Target $library)
    $emptyProject = [IO.Directory]::CreateDirectory("$install\tools\project")
    $hashes = @{}
    foreach ($path in $safe) { $hashes[$path] = (Get-FileHash -LiteralPath $path).Hash }

    Run-Cleanup -Shared
    Assert-Gone @($garbage | Where-Object { $_.StartsWith($install) })
    foreach ($path in $garbage | Where-Object { $_.StartsWith($data) }) {
        if (-not [IO.File]::Exists($path)) { throw "Removed shared edition data: $path" }
    }
    Run-Cleanup
    Run-Cleanup
    Assert-Gone $garbage
    Assert-Gone @("$install\platforms", "$install\config", "$data\Log", "$data\ocr-temp")
    foreach ($path in $safe) {
        if ((Get-FileHash -LiteralPath $path).Hash -ne $hashes[$path]) { throw "Changed project or user file: $path" }
    }
    if (-not ([IO.File]::GetAttributes($junction) -band [IO.FileAttributes]::ReparsePoint)) { throw 'Lost junction' }
    if (-not [IO.Directory]::Exists($emptyProject.FullName)) { throw 'Lost empty project directory' }
    'PASS: generated files removed, shared data respected, project contents and indexes identical after repeated cleanup.'

    Put $index '{corrupt'
    Put "$data\Log\new.log" 'retain'
    Run-Cleanup -ExpectFailure
    if (-not [IO.File]::Exists("$data\Log\new.log")) { throw 'Deleted data with an unreadable project index' }
    'PASS: unreadable project index prevents additional cleanup.'

    $install = Join-Path $fixture.FullName 'Clean install'
    $data = Join-Path $fixture.FullName 'Clean data'
    Put "$install\tools\cache.bin" 'discard'
    Put "$data\Log\last.log" 'discard'
    Run-Cleanup
    Assert-Gone @($install, $data)
    'PASS: empty installation and data directories removed.'
} finally {
    if ($fixture.Parent.FullName.TrimEnd('\') -ne $temp.FullName.TrimEnd('\') -or
        $fixture.Name -notmatch '^fkw-cleanup-test-[a-f0-9]{32}$') { throw 'Unexpected test directory' }
    if ([IO.Directory]::Exists($junction)) { [IO.Directory]::Delete($junction, $false) }
    $fixture.Delete($true)
}
