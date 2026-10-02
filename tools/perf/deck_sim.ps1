param(
  [string]$Build = "$PSScriptRoot\..\..\out\build\win-amd64-relwithdebinfo",
  [int]$Seconds = 0,
  [long]$MemGB = 8,
  [UInt64]$Affinity = 0xFF,
  [string]$Profile = 'deck',
  [string]$Resolution = '720p',
  [string]$Preset = 'low',
  [int]$FpsLimit = 60
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class DeckSim {
  [StructLayout(LayoutKind.Sequential)] public struct BASIC {
    public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags;
    public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit;
    public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
  [StructLayout(LayoutKind.Sequential)] public struct IO { public ulong a, b, c, d, e, f; }
  [StructLayout(LayoutKind.Sequential)] public struct EXT {
    public BASIC Basic; public IO Io; public UIntPtr ProcessMemoryLimit, JobMemoryLimit,
    PeakProcessMemoryUsed, PeakJobMemoryUsed; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI {
    public int cb; public string r, desk, title; public int x, y, xs, ys, xc, yc, fill, flags;
    public short show, r2; public IntPtr r3, hin, hout, herr; }
  [StructLayout(LayoutKind.Sequential)] public struct PI {
    public IntPtr hProcess, hThread; public int pid, tid; }
  [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateJobObject(IntPtr a, string n);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool SetInformationJobObject(IntPtr j, int c, ref EXT i, int n);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool QueryInformationJobObject(IntPtr j, int c, ref EXT i, int n, IntPtr r);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool AssignProcessToJobObject(IntPtr j, IntPtr p);
  [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh,
    uint flags, IntPtr env, string dir, ref SI si, out PI pi);
  [DllImport("kernel32", SetLastError=true)] public static extern uint ResumeThread(IntPtr t);
  [DllImport("kernel32", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool SetProcessAffinityMask(IntPtr p, UIntPtr m);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool GetProcessAffinityMask(IntPtr p, out UIntPtr pm, out UIntPtr sm);
  public static int Size() { return Marshal.SizeOf(typeof(EXT)); }
  // Puts the mask back each time something widens it, for ms milliseconds.
  public static string HoldAffinity(IntPtr h, UIntPtr mask, int ms) {
    var sw = System.Diagnostics.Stopwatch.StartNew(); int fixes = 0; long firstAt = -1;
    while (sw.ElapsedMilliseconds < ms) {
      UIntPtr pm, sm;
      if (!GetProcessAffinityMask(h, out pm, out sm)) break;
      if (pm.ToUInt64() != mask.ToUInt64()) {
        SetProcessAffinityMask(h, mask); fixes++; if (firstAt < 0) firstAt = sw.ElapsedMilliseconds; }
      System.Threading.Thread.Sleep(0);
    }
    return fixes + " time(s), first at " + firstAt + " ms";
  }
}
"@

function Fail($what) { throw "$what failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

if (Get-Process reeot -ErrorAction SilentlyContinue) { throw 'reeot is already running' }
$exe = Join-Path $Build 'reeot.exe'
$cmd = "`"$exe`" --profile=$Profile --eot_resolution=$Resolution --eot_quality_preset=$Preset " +
       "--eot_fps_limit=$FpsLimit --eot_vsync=false --eot_update_apply=false"

$job = [DeckSim]::CreateJobObject([IntPtr]::Zero, $null)
$info = New-Object DeckSim+EXT
$bytes = [UInt64]($MemGB * 1GB)
$info.Basic.LimitFlags = 0x1 -bor 0x100
$info.Basic.MinimumWorkingSetSize = [UIntPtr]::new([UInt64]64MB)
$info.Basic.MaximumWorkingSetSize = [UIntPtr]::new($bytes)
$info.ProcessMemoryLimit = [UIntPtr]::new($bytes + [UInt64]2GB)
if (-not [DeckSim]::SetInformationJobObject($job, 9, [ref]$info, [DeckSim]::Size())) { Fail 'SetInformationJobObject' }

$si = New-Object DeckSim+SI
$si.cb = [Runtime.InteropServices.Marshal]::SizeOf($si)
$pi = New-Object DeckSim+PI
if (-not [DeckSim]::CreateProcess($exe, $cmd, [IntPtr]::Zero, [IntPtr]::Zero, $false, 0x4,
                                  [IntPtr]::Zero, $Build, [ref]$si, [ref]$pi)) { Fail 'CreateProcess' }
if (-not [DeckSim]::AssignProcessToJobObject($job, $pi.hProcess)) { Fail 'AssignProcessToJobObject' }
if (-not [DeckSim]::SetProcessAffinityMask($pi.hProcess, [UIntPtr]::new($Affinity))) { Fail 'SetProcessAffinityMask' }
[void][DeckSim]::ResumeThread($pi.hThread)
$start = Get-Date
$held = [DeckSim]::HoldAffinity($pi.hProcess, [UIntPtr]::new($Affinity), 4000)
Write-Host ("reeot {0} started: working set <= {1} GB, CPUs {2:X} (mask restored {3}), {4} {5} {6} fps" -f
            $pi.pid, $MemGB, $Affinity, $held, $Resolution, $Preset, $FpsLimit)

$p = Get-Process -Id $pi.pid
$peakWs = 0L
while (-not $p.HasExited -and ($Seconds -le 0 -or ((Get-Date) - $start).TotalSeconds -lt $Seconds)) {
  Start-Sleep -Seconds 1
  $p.Refresh()
  if (-not $p.HasExited) { $peakWs = [Math]::Max($peakWs, $p.WorkingSet64) }
}
$q = New-Object DeckSim+EXT
[void][DeckSim]::QueryInformationJobObject($job, 9, [ref]$q, [DeckSim]::Size(), [IntPtr]::Zero)
if (-not $p.HasExited) { Stop-Process -Id $pi.pid -Force; Write-Host "killed after $Seconds s" }
Write-Host ("peak working set {0:N0} MB, peak commit {1:N0} MB (caps {2:N0} / {3:N0} MB)" -f
            ($peakWs / 1MB), ($q.PeakProcessMemoryUsed.ToUInt64() / 1MB), ($bytes / 1MB), (($bytes + 2GB) / 1MB))
[void][DeckSim]::CloseHandle($pi.hThread); [void][DeckSim]::CloseHandle($pi.hProcess); [void][DeckSim]::CloseHandle($job)
$log = Get-ChildItem (Join-Path $Build 'logs') -Filter 'reeot_*.log' -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) { Write-Host "log: $($log.FullName)" }
