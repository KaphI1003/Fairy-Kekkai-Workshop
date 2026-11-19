# Invoked after Inno Setup removes its manifest. Never use project paths as
# deletion targets: they are exclusions, including when nested in a cache.
param(
    [Parameter(Mandatory)][string]$InstallDir,
    [Parameter(Mandatory)][string]$UserDataDir,
    [switch]$KeepSharedData
)
$ErrorActionPreference = 'Stop'

function Get-CleanPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -eq [IO.Path]::GetPathRoot($full)) { return $full }
    return $full.TrimEnd('\')
}

function Test-LinkedPath([string]$Path) {
    for ($dir = [IO.DirectoryInfo]::new($Path); $null -ne $dir; $dir = $dir.Parent) {
        if ($dir.Exists -and ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $true }
    }
    return $false
}

$install = Get-CleanPath $InstallDir
$userData = Get-CleanPath $UserDataDir
$legacyData = [IO.Path]::Combine($install, 'AppData')
$legacyConfig = [IO.Path]::Combine($install, 'config')
$protected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$projectNames = @('project', 'projects', 'Fairy-Kekkai-Workshop-Projects')
# ISPP FileRead uses the build machine's ANSI code page. Keep this source ASCII.
$titleFile = [string][char]0x6807 + [char]0x9898 + '.txt'

foreach ($folder in @($install, $legacyData, $legacyConfig, $userData)) {
    $index = [IO.Path]::Combine($folder, 'project.json')
    if (-not [IO.File]::Exists($index)) { continue }
    if ((Test-LinkedPath $folder) -or ([IO.File]::GetAttributes($index) -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Cannot determine protected project paths from linked index: $index"
    }
    $projects = [IO.File]::ReadAllText($index) | ConvertFrom-Json -ErrorAction Stop
    if ($projects -isnot [pscustomobject]) { throw "Invalid project index: $index" }
    foreach ($key in 'project_root', 'project_link', 'project_order') {
        foreach ($path in $projects.$key) {
            if ([string]::IsNullOrWhiteSpace($path)) { continue }
            if (-not [IO.Path]::IsPathRooted($path)) {
                $path = [IO.Path]::Combine($install, $path)
            }
            [void]$protected.Add((Get-CleanPath $path))
        }
    }
}

function Test-ProjectDirectory([IO.DirectoryInfo]$Dir) {
    return ($protected.Contains($Dir.FullName) -or $projectNames -contains $Dir.Name -or
        [IO.File]::Exists([IO.Path]::Combine($Dir.FullName, $titleFile)) -or
        [IO.File]::Exists([IO.Path]::Combine($Dir.FullName, 'icon.txt')))
}

function Remove-GeneratedEntry([IO.FileSystemInfo]$Entry, [string]$Boundary) {
    $prefix = $Boundary.TrimEnd('\') + '\'
    if (-not $Entry.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Entry outside cleanup directory: $($Entry.FullName)"
    }
    if ($Entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { return }
    if ($Entry -is [IO.DirectoryInfo]) {
        if (Test-ProjectDirectory $Entry) { return }
        foreach ($child in $Entry.GetFileSystemInfos()) { Remove-GeneratedEntry $child $Boundary }
        if ($Entry.GetFileSystemInfos().Length -eq 0) { $Entry.Delete() }
    } elseif ($Entry.Name -ne 'project.json') {
        $Entry.Delete()
    }
}

function Clear-OwnedEntries([string]$Folder, [string[]]$Directories, [string[]]$Files) {
    $dir = [IO.DirectoryInfo]::new($Folder)
    if (-not $dir.Exists -or -not $dir.Parent -or (Test-LinkedPath $Folder)) { return }
    for ($parent = $dir; $null -ne $parent; $parent = $parent.Parent) {
        if (Test-ProjectDirectory $parent) { return }
    }
    foreach ($entry in $dir.GetFileSystemInfos()) {
        if ($entry -is [IO.DirectoryInfo]) {
            if ($Directories -notcontains $entry.Name) { continue }
        } elseif ($Files -notcontains $entry.Name) { continue }
        try {
            Remove-GeneratedEntry $entry $Folder
        } catch {
            $script:failed = $true
            Write-Warning "Could not clean $($entry.FullName): $_"
        }
    }
    if ($dir.GetFileSystemInfos().Length -eq 0) { $dir.Delete() }
}

$failed = $false
$runtime = @('Log', 'ocr-temp', 'paddlex', 'InterpolationPreview', 'InterpolationTemp',
             'SuperresPreview', 'SuperresTemp')
$payload = @('tools', 'generic', 'iconengines', 'imageformats', 'licenses', 'networkinformation',
             'platforms', 'styles', 'tls', 'translations', 'temp', '__pycache__')
if ($install -ne [IO.Path]::GetPathRoot($install)) {
    Clear-OwnedEntries $legacyConfig @() @('config.json')
    Clear-OwnedEntries $legacyData $runtime @('config.json')
    Clear-OwnedEntries $install ($payload + $runtime) @('config.json', 'Fairy-Kekkai-Workshop.exe', 'Fairy-Kekkai-Workshop-Cpp.exe')
}
if (-not $KeepSharedData) { Clear-OwnedEntries $userData $runtime @('config.json') }
if ($failed) { exit 1 }
