# Runs a title's RPX in the private Cemu copy in build_win\cemu, for check_ports.py, and asks Cemu itself to save its own game output with its screenshot key,
# sent only to Cemu's own windows. Nothing else on the screen can end up in the result. Waits for another run to finish instead of closing it.
# Cemu runs on a Windows desktop of its own, never shown, so it can't cover the user's screen or take their keyboard (the user's rule, 2026-11-07).
param([string]$Rpx, [string]$Low, [int]$Seconds = 120, [int]$Shots = 6, [string]$Out, [switch]$Interpreter)
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class W3 {
  public delegate bool Cb(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumDesktopWindows(IntPtr d, Cb c, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, Cb c, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr mode, uint flags, uint access, IntPtr sa);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct SI {
    public int cb; public string reserved, desktop, title; public int x, y, w, h, cx, cy, fill, flags; public short show, cbReserved2;
    public IntPtr reserved2, stdIn, stdOut, stdErr; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr process, thread; public int pid, tid; }
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref SI si, out PI pi);
  public static IntPtr Desk;
  public static int Start(string exe, string cmd, string dir) {
    Desk = CreateDesktopW("cemu_check", IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
    if (Desk == IntPtr.Zero) throw new Exception("CreateDesktop failed " + Marshal.GetLastWin32Error());
    var si = new SI(); si.cb = Marshal.SizeOf(typeof(SI)); si.desktop = "cemu_check";
    PI pi;
    if (!CreateProcessW(exe, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi)) throw new Exception("CreateProcess failed " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
  public static List<IntPtr> Windows(uint want, bool children) {
    var o = new List<IntPtr>();
    EnumDesktopWindows(Desk, (h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p != want || !IsWindowVisible(h)) return true;
      o.Add(h);
      if (children) EnumChildWindows(h, (c, m) => { o.Add(c); return true; }, IntPtr.Zero);
      return true; }, IntPtr.Zero);
    return o;
  }
}
"@
$cemuDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build_win\cemu')).Path
$data = "$cemuDir\portable"
$sd = "$data\mlc01\usr\save\00050000\$Low\user\common"
$waited = 0
while (Get-Process CemuMario -ErrorAction SilentlyContinue) {
  if ($waited -ge 900) { "another Cemu has been running for 15 minutes: giving up"; exit 1 }
  Start-Sleep -Seconds 10; $waited += 10
}
New-Item -ItemType Directory -Force -Path $sd, $Out | Out-Null
if (Test-Path -LiteralPath "$sd\log.txt") { Remove-Item -LiteralPath "$sd\log.txt" -Force -Confirm:$false }
# Cemu killed while it writes this title's shader or pipeline cache leaves a half-written file, and the next start crashes in ntdll (heap corruption: Kirby,
# NSMB2 and both Laytons on 2026-11-08/09; deleting the pipeline cache alone didn't stop it). So a run ends by asking Cemu to close, which lets it finish
# writing, and any run that ends otherwise (a crash, or no answer) deletes both files, which Cemu then rebuilds.
$caches = "${Low}_shaders.bin", "${Low}_vkpipeline.bin" | ForEach-Object { "$data\shaderCache\transferable\00050000$_" }
function Drop-Caches { foreach ($f in $caches) { Remove-Item -LiteralPath $f -Force -Confirm:$false -ErrorAction SilentlyContinue } }
$shotDir = "$data\screenshots"
if (Test-Path -LiteralPath $shotDir) { Get-ChildItem -LiteralPath $shotDir -File -Recurse | Remove-Item -Force -Confirm:$false }
Get-ChildItem -LiteralPath $Out -File -ErrorAction SilentlyContinue | Remove-Item -Force -Confirm:$false
$exe = "$cemuDir\CemuMario.exe"
$cmd = "`"$exe`" -g `"$Rpx`"" + $(if ($Interpreter) { ' --force-interpreter' } else { '' })
$p = Get-Process -Id ([W3]::Start($exe, $cmd, $cemuDir))
$per = [int]($Seconds / [Math]::Max($Shots, 1))
for ($n = 0; $n -lt $Shots -and -not $p.HasExited; $n++) {
  Start-Sleep -Seconds $per
  foreach ($h in [W3]::Windows([uint32]$p.Id, $true)) {
    [W3]::PostMessage($h, 0x100, [IntPtr]0x7B, [IntPtr]0x00580001) | Out-Null
    [W3]::PostMessage($h, 0x101, [IntPtr]0x7B, [IntPtr]([int64]0xC0580001)) | Out-Null
  }
}
Start-Sleep -Seconds 3
if ($p.HasExited) {
  "Cemu exited by itself, code $($p.ExitCode)"
  Drop-Caches
} else {
  $wins = [W3]::Windows([uint32]$p.Id, $false)
  "windows: $($wins.Count)"
  foreach ($h in $wins) { [W3]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }  # WM_CLOSE
  if (-not $p.WaitForExit(15000)) {
    Stop-Process -Id $p.Id -Force -Confirm:$false -ErrorAction SilentlyContinue
    $p.WaitForExit(5000) | Out-Null
    Drop-Caches
    "Cemu didn't close when asked within 15 s: stopped, and this title's caches deleted"
  }
}
Start-Sleep -Seconds 1
if (Test-Path -LiteralPath $shotDir) {
  $k = 0
  Get-ChildItem -LiteralPath $shotDir -File -Recurse | Sort-Object LastWriteTime | ForEach-Object { Move-Item -LiteralPath $_.FullName -Destination ("$Out\{0:d2}_{1}" -f $k, $_.Name) -Force; $k++ }
}
