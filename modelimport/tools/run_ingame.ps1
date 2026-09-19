param(
  [string]$pak = "build/_DLC002.pak",
  [string]$name = "ReeotS99",
  [int]$shots = 6,                      # screenshots to take once in the level
  [int]$every = 3,                      # seconds between them
  [string]$out = "C:/tmp/ig",           # screenshot prefix -> <out>1.png ...
  [switch]$keep
)
$ErrorActionPreference = 'Continue'
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $tools
$exe = 'C:\Users\rieng\Documents\GitHub\reeot-dni\out\build\win-amd64-relwithdebinfo\reeot.exe'
$logDir = Join-Path (Split-Path -Parent $exe) 'logs'
$shot = 'C:\Users\rieng\Documents\GitHub\reeot-dni\reference\model_import\shot.ps1'
powershell -ExecutionPolicy Bypass -File "$tools\run_test.ps1" -pak (Join-Path $root $pak) -name $name -display "Spider-Man: EoT S99 Suit" -settle 12 -after 3 -keep | Select-String -Pattern 'installed|mount id 300|crash|exited|log:'
Start-Sleep -Seconds 12
foreach ($k in 'ESC', 'SPACE', 'SPACE') {
  powershell -ExecutionPolicy Bypass -File "$tools\key.ps1" -keys $k | Out-Null
  Start-Sleep -Seconds 5
}
Start-Sleep -Seconds 20
for ($i = 1; $i -le $shots; $i++) {
  powershell -ExecutionPolicy Bypass -File $shot -out "$out$i.png" | Out-Null
  Start-Sleep -Seconds $every
}
Write-Output "shots: $out[1..$shots].png"
if (-not $keep) { Get-Process reeot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
