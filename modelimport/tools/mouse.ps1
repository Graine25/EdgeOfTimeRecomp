param([int]$dx = 0, [int]$dy = 0, [int]$steps = 20)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MI {
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Move(int dx, int dy) {
    INPUT[] a = new INPUT[1];
    a[0].type = 0;
    a[0].mi.dx = dx; a[0].mi.dy = dy; a[0].mi.dwFlags = 0x0001;
    SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@
$p = Get-Process reeot -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
[MI]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 200
for ($i = 0; $i -lt $steps; $i++) {
  [MI]::Move([int]($dx / $steps), [int]($dy / $steps))
  Start-Sleep -Milliseconds 16
}
Write-Output "moved $dx,$dy"
