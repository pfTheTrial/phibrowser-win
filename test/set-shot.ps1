# Settings-window capture (untracked rig). Runs in the INTERACTIVE session via
# the PhiSetShot scheduled task — an ssh session lives on another window
# station and FindWindow there sees no client windows at all.
# Output path comes from PHI_SETSHOT_OUT (User scope).
$ErrorActionPreference = "Stop"
$out = [Environment]::GetEnvironmentVariable("PHI_SETSHOT_OUT", "User")
if (-not $out) { $out = "C:\Users\jdxin\setshot\settings.png" }
$done = "C:\Users\jdxin\setshot\done.txt"
Remove-Item $done -ErrorAction SilentlyContinue
$log = ""
try {
  Add-Type -AssemblyName System.Drawing
  Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class SS {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr u, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }
}
'@
  [SS]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
  $h = [SS]::FindWindowW("PhiSettingsWindow", [NullString]::Value)
  if ($h -eq [IntPtr]::Zero) { throw "no PhiSettingsWindow" }
  [SS]::ShowWindow($h, 5) | Out-Null
  [SS]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 400
  [SS]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero, 0x181) | Out-Null
  Start-Sleep -Milliseconds 300
  $r = New-Object SS+R
  [SS]::GetClientRect($h, [ref]$r) | Out-Null
  $w = $r.Rr - $r.L; $hh = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [SS]::PrintWindow($h, $hdc, 2) | Out-Null
  $g.ReleaseHdc($hdc)
  New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose(); $g.Dispose()
  $log = "OK $out ${w}x${hh}"
} catch {
  $log = "ERR " + $_.Exception.Message
}
New-Item -ItemType Directory -Force -Path (Split-Path $done) | Out-Null
Set-Content -Path $done -Value $log -Encoding ASCII
