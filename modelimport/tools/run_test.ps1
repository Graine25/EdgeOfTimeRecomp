param(
  [string]$pak = "",                      # package to install as the test content item ("" = install nothing)
  [string]$name = "ReeotTest",            # content folder / header name
  [string]$display = "Spider-Man: EoT Reeot Test",
  [int]$settle = 20,                      # seconds after the front screen mounts before pressing Start
  [int]$after = 45,                       # seconds to watch after Start
  [switch]$keep                           # leave the game running
)
$ErrorActionPreference = 'Continue'
$root = 'D:\EdgeOfTimeRecompiled'
$exe = 'C:\Users\rieng\Documents\GitHub\reeot-dni\out\build\win-amd64-relwithdebinfo\reeot.exe'
$profileTitle = "$root\profiles\default\0000000000000000\415608B2"
$contentDir = "$profileTitle\00000002\$name"
$headerDir = "$profileTitle\Headers\00000002"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$logDir = Join-Path (Split-Path -Parent $exe) 'logs'   # the dev build logs beside the executable (since 2026-09-16)

Get-Process reeot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

if (Test-Path $contentDir) { Remove-Item -Recurse -Force $contentDir }
if (Test-Path "$headerDir\$name.header") { Remove-Item -Force "$headerDir\$name.header" }
if ($pak -ne "") {
  New-Item -ItemType Directory -Force $contentDir | Out-Null
  New-Item -ItemType Directory -Force $headerDir | Out-Null
  Copy-Item $pak "$contentDir\$(Split-Path -Leaf $pak)"
  $h = New-Object byte[] 0x14C
  [Array]::Copy([byte[]](0,0,0,1, 0,0,0,2), 0, $h, 0, 8)
  $dn = [System.Text.Encoding]::BigEndianUnicode.GetBytes($display); [Array]::Copy($dn, 0, $h, 8, [Math]::Min($dn.Length, 254))
  $fn = [System.Text.Encoding]::ASCII.GetBytes($name); [Array]::Copy($fn, 0, $h, 0x108, [Math]::Min($fn.Length, 42))
  [Array]::Copy([byte[]](0x41,0x56,0x08,0xB2), 0, $h, 0x140, 4)
  [Array]::Copy([byte[]](1,0,0,0), 0, $h, 0x148, 4)
  [IO.File]::WriteAllBytes("$headerDir\$name.header", $h)
  Write-Output "installed $(Split-Path -Leaf $pak) as content '$name'"
}

$before = (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
$p = Start-Process -FilePath $exe -ArgumentList '--eot_update_apply=false' -WorkingDirectory (Split-Path -Parent $exe) -PassThru
Write-Output "started pid $($p.Id)"

function LatestLog { (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
$deadline = (Get-Date).AddSeconds(150); $log = $null
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 3
  $log = LatestLog
  if ($log -ne $before -and (Select-String -Path $log -Pattern "mount id 3 'L:/FrontScreen.pak'" -Quiet)) { break }
  if ($p.HasExited) { break }
}
if ($p.HasExited) { Write-Output "exited before the front screen"; }
else {
  Start-Sleep -Seconds $settle
  powershell -ExecutionPolicy Bypass -File "$tools\key.ps1" -keys "ESC" | Out-Null
  $deadline = (Get-Date).AddSeconds($after)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if ($p.HasExited) { break }
    if (Select-String -Path $log -Pattern "reeot host crash|mount id 300[0-9] |\[dlc\] registered" -Quiet) { Start-Sleep -Seconds 6; break }
  }
}
Write-Output "log: $log"
Select-String -Path $log -Pattern 'GDLC|_DLC00|mount id 300|Bad Chunk|\[dlc\]|exception: |added \d+ items|\[crash\] (?!last)|guest fault|Fatal' |
  ForEach-Object { $_.Line.Substring(0, [Math]::Min(190, $_.Line.Length)) } | Where-Object { $_ -notmatch 'VFS: entry not found' } | Select-Object -Last 25
if (-not $keep) { Get-Process reeot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
