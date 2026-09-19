param([string]$keys = "", [int]$hold = 80)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class KI {
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KEYBDINPUT ki; public long pad; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Key(ushort scan, bool ext, bool up) {
    INPUT[] a = new INPUT[1];
    a[0].type = 1;
    a[0].ki.wScan = scan;
    a[0].ki.dwFlags = (uint)(0x0008 | (ext ? 0x0001 : 0) | (up ? 0x0002 : 0));
    SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@
$map = @{ ESC=@(0x01,$false); ENTER=@(0x1C,$false); SPACE=@(0x39,$false); E=@(0x12,$false); W=@(0x11,$false); A=@(0x1E,$false); S=@(0x1F,$false); D=@(0x20,$false);
          UP=@(0x48,$true); DOWN=@(0x50,$true); LEFT=@(0x4B,$true); RIGHT=@(0x4D,$true); C=@(0x2E,$false); R=@(0x13,$false); TAB=@(0x0F,$false); Q=@(0x10,$false); SHIFT=@(0x2A,$false); V=@(0x2F,$false); Z=@(0x2C,$false); X=@(0x2D,$false); F12=@(0x58,$false) }
$p = Get-Process reeot -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
[KI]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 250
foreach ($k in $keys.Split(',')) {
  $k = $k.Trim().ToUpper(); if ($k -eq '') { continue }
  if ($k -match '^WAIT(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
  $sc = $map[$k]; if (-not $sc) { Write-Output "unknown key $k"; continue }
  [KI]::Key([uint16]$sc[0], [bool]$sc[1], $false); Start-Sleep -Milliseconds $hold
  [KI]::Key([uint16]$sc[0], [bool]$sc[1], $true); Start-Sleep -Milliseconds 400
  Write-Output "sent $k"
}
