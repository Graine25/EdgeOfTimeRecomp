param(
    [Parameter(Mandatory = $true)][string]$Idb,
    [string]$Script = "headless_dump.py",
    [string]$Mode = "",
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Arg = "",
    [string]$OutDir = (Join-Path $env:TEMP 'ida_headless'),
    [string]$Idat = "C:\Program Files\IDA Professional 9.3\idat.exe"
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$copy = Join-Path $OutDir (Split-Path -Leaf $Idb)
if (-not (Test-Path $copy)) { Copy-Item $Idb $copy }
$scriptPath = Join-Path $PSScriptRoot $Script
$outPath = Join-Path $OutDir $Out
$args = if ($Mode -ne "") { "$scriptPath $Mode $outPath" } else { "$scriptPath $outPath" }
if ($Arg -ne "") { $args += " $Arg" }
$env:IDALOG = Join-Path $OutDir 'idat.log'
& $Idat -A "-S`"$args`"" $copy | Out-Null
Get-Content $env:IDALOG -Tail 4 | Select-String -Pattern 'done|exported|Error|error|Traceback'
Write-Host "output: $outPath"
