param(
  [int]$index = 0,        # card position in Alternate Suits, 0 = SPIDER-MAN 2099; the S99 card is the last (use -last)
  [switch]$last,
  [switch]$launch,        # also launch the dev exe and get to the main menu first (Escape at the front screen, Space on the save)
  [switch]$continue_game  # afterwards back out to the main menu and press Continue
)
$ErrorActionPreference = 'Continue'
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
function Key($k, $ms = 1200) { powershell -ExecutionPolicy Bypass -File "$tools\key.ps1" -keys $k | Out-Null; Start-Sleep -Milliseconds $ms }
if ($launch) {
  $exe = 'C:\Users\rieng\Documents\GitHub\reeot-dni\out\build\win-amd64-relwithdebinfo\reeot.exe'
  $logDir = Join-Path (Split-Path -Parent $exe) 'logs'
  Get-Process reeot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  $before = (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
  Start-Process -FilePath $exe -ArgumentList "--eot_update_apply=false" -WorkingDirectory (Split-Path -Parent $exe) | Out-Null
  $deadline = (Get-Date).AddSeconds(150)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $log = (Get-ChildItem "$logDir\reeot_*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    if ($log -ne $before -and (Select-String -Path $log -Pattern "mount id 3 'L:/FrontScreen.pak'" -Quiet)) { break }
  }
  Start-Sleep -Seconds 20
  Key 'ESC' 5000
  Key 'SPACE' 5000
}
Key 'D' 2000            # BONUS GALLERY
Key 'SPACE' 4000        # gallery bar (CONCEPT ART)
Key 'A' 1500; Key 'A' 1500   # ALTERNATE SUITS
Key 'SPACE' 6000        # the card row
for ($i = 0; $i -lt 40; $i++) { Key 'A' 350 }   # to the first card
if ($last) { for ($i = 0; $i -lt 40; $i++) { Key 'D' 350 } }
else { for ($i = 0; $i -lt $index; $i++) { Key 'D' 700 } }
Start-Sleep -Seconds 1
Key 'SPACE' 3000        # select -> CHANGE DEFAULT SUIT prompt (DON'T EQUIP is the default)
Key 'W' 1000; Key 'SPACE' 3000   # EQUIP
Write-Output "equipped card $(if ($last) { 'last' } else { $index })"
if ($continue_game) {
  Key 'E' 3000; Key 'E' 3000    # back to the gallery bar, then the main menu (CONTINUE)
  Key 'SPACE' 2000              # CONTINUE
  Write-Output "continuing into the level"
}
