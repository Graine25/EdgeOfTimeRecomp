param(
  [string]$Build = "$PSScriptRoot\..\..\out\build\win-amd64-relwithdebinfo",
  [int]$EmptyFrames = 10
)
$Build = (Resolve-Path $Build).Path
$dumper = Join-Path $PSScriptRoot "dump_stacks.py"
$logDir = Join-Path $Build "logs"
Write-Host "watching $logDir for the hang signature (Ctrl+C to stop)"
$lastCaptured = ""
while ($true) {
  $proc = Get-Process reeot -ErrorAction SilentlyContinue
  if (-not $proc) { Start-Sleep -Seconds 2; continue }
  $log = Get-ChildItem "$logDir\reeot_*.log" -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $log) { Start-Sleep -Seconds 1; continue }
  $tail = Get-Content $log.FullName -Tail 6000 -ErrorAction SilentlyContinue
  if (-not $tail) { Start-Sleep -Seconds 1; continue }
  $played = ($tail | Select-String -Pattern '\[d3d-frame\] \d+: draw=[1-9]\d\d' -Quiet)
  $recent = $tail | Select-Object -Last 60
  $empty = ($recent | Select-String -Pattern '\[d3d-frame\] \d+:\s*$' | Measure-Object).Count
  if ($played -and $empty -ge $EmptyFrames -and $log.FullName -ne $lastCaptured) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $out = Join-Path $logDir "hang_$stamp.txt"
    "hang signature in $($log.Name): $empty empty frames; pid $($proc.Id)" | Out-File $out -Encoding utf8
    "== threads ==" | Out-File $out -Append -Encoding utf8
    & python $dumper $proc.Id 40 2>&1 | Out-File $out -Append -Encoding utf8
    "== last 300 log lines ==" | Out-File $out -Append -Encoding utf8
    Get-Content $log.FullName -Tail 300 | Out-File $out -Append -Encoding utf8
    Write-Host "captured: $out"
    $lastCaptured = $log.FullName
  }
  Start-Sleep -Seconds 1
}
