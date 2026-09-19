param([int]$settle = 30)
$ErrorActionPreference = 'Continue'
$exe = 'C:\Users\rieng\Documents\GitHub\reeot-dni\out\build\win-amd64-relwithdebinfo\reeot.exe'
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$logDir = Join-Path (Split-Path -Parent $exe) 'logs'
Get-Process reeot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$before = (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
$p = Start-Process -FilePath $exe -ArgumentList "--eot_update_apply=false" -WorkingDirectory (Split-Path -Parent $exe) -PassThru
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
while ($tries -lt 6 -and -not (Select-String -Path $log -Pattern "mount id 200 " -Quiet)) {
  foreach ($k in 'ESC', 'SPACE', 'SPACE') {
    powershell -ExecutionPolicy Bypass -File "$tools\key.ps1" -keys $k | Out-Null
    Start-Sleep -Seconds 5
  }
  Start-Sleep -Seconds 10
  $tries++
}
Write-Output "in the level after $tries tries; settling $settle s"
Start-Sleep -Seconds $settle
Write-Output "log: $log"
