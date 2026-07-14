param(
    [int]    $Seconds  = 60,
    [int]    $Every    = 5,
    [string] $Exe      = "out\build\win-amd64-debug\reeot.exe",
    [string] $GameData = "C:\Users\rieng\Documents\GitHub\reeot\assets",
    [string] $OutDir   = "",
    [switch] $MnkMode,
    [switch] $IssueDraws,
    [switch] $ProtectResolved,
    [switch] $GreenProbe,
    [switch] $PresentBusiest,
    [switch] $FetchBind,
    [switch] $SkipClear
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) {
    $OutDir = Join-Path $repo ("captures\shots_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
}
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$exePath = Join-Path $repo $Exe
if (-not (Test-Path $exePath)) { Write-Error "reeot.exe not found at '$exePath' - build first." }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Shot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    // Without this the capture runs DPI-virtualised: GetClientRect reports
    // logical pixels while the game's swapchain is physical, so a 5120x2880
    // window is captured as its top-left 2560x1440 quadrant and everything
    // looks cropped and twice as large. This cost a false "text is clipped".
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    // CopyFromScreen grabs whatever is on top at those coordinates, so the game
    // window has to be raised first or the shot is of whatever covers it.
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@ -ReferencedAssemblies System.Drawing

try { [Win32Shot]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null } catch {}

$args = @("--game_data_root", $GameData)
if ($MnkMode) { $args += "--mnk_mode=true" }
if ($ProtectResolved) { $args += "--eot_protect_resolved=true" }
if ($GreenProbe) { $args += "--eot_probe_resolve_green=true" }
if ($PresentBusiest) { $args += "--eot_present_busiest=true" }
if ($SkipClear) { $args += "--eot_skip_clear=true" }

Write-Host "Launching $exePath"
Write-Host "  shots -> $OutDir  (every ${Every}s for ${Seconds}s)"
$proc = Start-Process -FilePath $exePath -ArgumentList $args -PassThru

$deadline = (Get-Date).AddSeconds($Seconds)
$index = 0
$captured = 0

try {
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds $Every
        $proc.Refresh()
        if ($proc.HasExited) {
            Write-Host "process exited early (code $($proc.ExitCode))"
            break
        }

        $h = $proc.MainWindowHandle
        if ($h -eq 0 -or -not [Win32Shot]::IsWindowVisible($h)) { continue }

        $rect = New-Object Win32Shot+RECT
        if (-not [Win32Shot]::GetClientRect($h, [ref] $rect)) { continue }
        $w = $rect.R - $rect.L
        $hgt = $rect.B - $rect.T
        if ($w -le 0 -or $hgt -le 0) { continue }

        $origin = New-Object System.Drawing.Point 0, 0
        if (-not [Win32Shot]::ClientToScreen($h, [ref] $origin)) { continue }

        $bmp = New-Object System.Drawing.Bitmap $w, $hgt
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            [Win32Shot]::ShowWindow($h, 5) | Out-Null   # SW_SHOW
            [Win32Shot]::SetForegroundWindow($h) | Out-Null
            Start-Sleep -Milliseconds 400
            $g.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bmp.Size)
            $index++
            $name = Join-Path $OutDir ("shot_{0:D3}_{1}s.png" -f $index, ($index * $Every))
            $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)

            $nonBlack = 0
            $samples = 0
            for ($y = 0; $y -lt $hgt; $y += [Math]::Max(1, [int]($hgt / 24))) {
                for ($x = 0; $x -lt $w; $x += [Math]::Max(1, [int]($w / 24))) {
                    $px = $bmp.GetPixel($x, $y)
                    $samples++
                    if ($px.R -gt 8 -or $px.G -gt 8 -or $px.B -gt 8) { $nonBlack++ }
                }
            }
            $pct = if ($samples -gt 0) { [int](100 * $nonBlack / $samples) } else { 0 }
            Write-Host ("  [{0,3}s] {1}x{2}  {3}% non-black  {4}" -f `
                        ($index * $Every), $w, $hgt, $pct, (Split-Path $name -Leaf))
            $captured++
        }
        finally {
            $g.Dispose()
            $bmp.Dispose()
        }
    }
}
finally {
    $proc.Refresh()
    if (-not $proc.HasExited) {
        Write-Host "stopping reeot"
        $proc.CloseMainWindow() | Out-Null
        Start-Sleep -Milliseconds 800
        $proc.Refresh()
        if (-not $proc.HasExited) { $proc.Kill() }
    }
}

Write-Host ""
Write-Host "$captured shot(s) in $OutDir"
$log = Get-ChildItem (Join-Path $repo "out\build\win-amd64-debug\logs\*.log") |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) { Write-Host "log: $($log.FullName)" }
