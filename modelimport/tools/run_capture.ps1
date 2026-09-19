param(
  [int]$frame = 40000,                   # guest frame to capture with the in-process RenderDoc API
  [string]$out = "D:/reeot_caps/tmp/s99",  # capture path template
  [int]$enterAfter = 45                  # seconds after launch to press Start + A A A (save select, continue)
)
$ErrorActionPreference = 'Continue'
$exe = 'C:\Users\rieng\Documents\GitHub\reeot-dni\out\build\win-amd64-relwithdebinfo\reeot.exe'
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$logDir = Join-Path (Split-Path -Parent $exe) 'logs'
Get-Process reeot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$before = (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
$p = Start-Process -FilePath $exe -ArgumentList "--eot_update_apply=false --eot_rdc_frame=$frame --eot_rdc_path=$out" -WorkingDirectory (Split-Path -Parent $exe) -PassThru
Write-Output "started pid $($p.Id), capture armed for guest frame $frame"
function LatestLog { (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
$deadline = (Get-Date).AddSeconds(150); $log = $null
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 3
  $log = LatestLog
  if ($log -ne $before -and (Select-String -Path $log -Pattern "mount id 3 'L:/FrontScreen.pak'" -Quiet)) { break }
  if ($p.HasExited) { Write-Output "exited early"; exit 1 }
}
Start-Sleep -Seconds 20
$tries = 0
while ($tries -lt 8 -and -not (Select-String -Path $log -Pattern "mount id 200 " -Quiet)) {
  foreach ($k in 'ESC', 'SPACE', 'SPACE') {
    powershell -ExecutionPolicy Bypass -File "$tools\key.ps1" -keys $k | Out-Null
    Start-Sleep -Seconds 5
  }
  Start-Sleep -Seconds 10
  $tries++
}
Write-Output "in the level after $tries tries; waiting for the capture"
$deadline = (Get-Date).AddSeconds(400)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 5
  if (Select-String -Path $log -Pattern "EndFrameCapture" -Quiet) { break }
  if ($p.HasExited) { break }
}
Select-String -Path $log -Pattern '\[rdc\]' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
