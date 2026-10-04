param([string]$Preset = 'win-amd64-release')

$ErrorActionPreference = 'Stop'

$repo      = Split-Path -Parent $PSScriptRoot
$build     = Join-Path $repo "out\build\$Preset"
$downloads = Join-Path $env:USERPROFILE 'Downloads'
$dis       = Join-Path $downloads 'EdgeOfTimeRecomp-dis'
$list      = Join-Path $build 'program_files.txt'

if (-not (Test-Path (Join-Path $build 'EdgeOfTimeRecomp.exe'))) {
    Write-Host "[FAILED] No build at $build." -ForegroundColor Red
    Write-Host "         Build first (scripts\build.bat), then run this."
    exit 1
}
if (-not (Test-Path $list)) {
    Write-Host "[FAILED] $list is missing; cannot tell what to package." -ForegroundColor Red
    exit 1
}

$ver = 'unknown'; $stamp = 'unknown'
$info = Join-Path $build 'generated\core\build_info.h'
if (Test-Path $info) {
    $text = Get-Content $info -Raw
    if ($text -match 'REEOT_VERSION_STRING\s+"([^"]+)"')  { $ver   = $Matches[1] }
    if ($text -match 'REEOT_BUILD_TIMESTAMP\s+"([^"]+)"') { $stamp = $Matches[1] }
}
Write-Host "EdgeOfTimeRecomp package  v$ver  build $stamp"
Write-Host ""

Write-Host "[1/4] Clearing old remnants..."
if (Test-Path $dis) {
    Get-ChildItem $dis -Force | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $dis | Out-Null
}
Get-ChildItem $downloads -Filter 'EdgeOfTimeRecomp-v*.zip' -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "[2/4] Copying the latest build's program files..."
$missing = @()
$binaries = @()
foreach ($raw in (Get-Content $list)) {
    $name = $raw.Trim()
    if ($name -eq '') { continue }
    $src = Join-Path $build $name
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $dis $name) -Force
        Write-Host "        + $name"
        if ($name -match '\.(exe|dll)$') { $binaries += $name }
    } else {
        Write-Host "        [WARN] missing $name" -ForegroundColor Yellow
        $missing += $name
    }
}

Write-Host "[3/4] Copying the symbols..."
foreach ($name in $binaries) {
    $pdb = [System.IO.Path]::ChangeExtension($name, '.pdb')
    $src = Join-Path $build $pdb
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $dis $pdb) -Force
        Write-Host "        + $pdb"
    }
}

Write-Host "[4/4] Zipping..."
$zip = Join-Path $downloads "EdgeOfTimeRecomp-v$ver-$stamp.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $dis -DestinationPath $zip -Force

$size = '{0:N1} MB' -f ((Get-Item $zip).Length / 1MB)
Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "    folder: $dis"
Write-Host "    zip:    $zip  ($size)"
if ($missing.Count) {
    Write-Host "    NOTE: missing $($missing -join ', '); the package may be incomplete." -ForegroundColor Yellow
}
