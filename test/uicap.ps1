# Captures the client shell window (PrintWindow) and reports pixel samples.
# Points come from $env:PHI_UICAP_POINTS (User scope), "x,y;x,y;...".
# Writes the whole result ONCE (readers poll for the trailing DONE marker).
param([string]$Points = "")
$transcript = "C:\Users\jdxin\uicap.txt"
Remove-Item $transcript -ErrorAction SilentlyContinue
if ($Points -eq "") {
  $Points = [Environment]::GetEnvironmentVariable("PHI_UICAP_POINTS", "User")
}
$lines = New-Object System.Collections.Generic.List[string]
try {
  Add-Type -AssemblyName System.Drawing
  $sig = @"
using System;using System.Runtime.InteropServices;
public class C {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr a, IntPtr b, string c, string d);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr u, uint flags);
  public struct R { public int L,T,Rr,B; }
}
"@
  Add-Type -TypeDefinition $sig
  # Capture in PHYSICAL pixels, the same space the client lays out in. Without
  # this PowerShell is DPI-unaware, GetClientRect reports virtualised size and
  # PrintWindow renders a RESAMPLED image — blurred edges that break the
  # light/dark classification the assertions rely on.
  # -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  [C]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
  # Prefer an explicitly requested HWND (the slot under test): several shell
  # windows can exist, and "the process's main window" may not be the right one.
  $h = [IntPtr]::Zero
  $want = [Environment]::GetEnvironmentVariable("PHI_UICAP_HWND", "User")
  if ($want -and $want -ne "0") { $h = [IntPtr][int64]$want }
  if ($h -eq [IntPtr]::Zero) {
    $p = Get-Process phi_client -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
    if ($p) { $h = $p.MainWindowHandle }
  }
  if ($h -eq [IntPtr]::Zero) {
    $h = [C]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, "PhiClientShell", $null)
  }
  if ($h -eq [IntPtr]::Zero) {
    $lines.Add("ERR no shell window")
  } else {
    # Force a real repaint before capturing: PrintWindow can otherwise return
    # the last composited frame rather than current state.
    # RDW_INVALIDATE(0x1) | RDW_UPDATENOW(0x100) | RDW_ALLCHILDREN(0x80)
    [C]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero, 0x181) | Out-Null
    Start-Sleep -Milliseconds 250
    $r = New-Object C+R
    [C]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Rr - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [C]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $lines.Add("SIZE " + $bmp.Width + "x" + $bmp.Height)
    foreach ($pt in $Points.Split(";")) {
      if ($pt -eq "") { continue }
      $xy = $pt.Split(",")
      $x = [int]$xy[0]; $y = [int]$xy[1]
      if ($x -lt $bmp.Width -and $y -lt $bmp.Height -and $x -ge 0 -and $y -ge 0) {
        $c = $bmp.GetPixel($x, $y)
        $lines.Add("PX " + $x + "," + $y + " " + $c.R + "," + $c.G + "," + $c.B)
      } else {
        $lines.Add("PX " + $x + "," + $y + " OOB")
      }
    }
    $bmp.Dispose()
  }
} catch {
  $lines.Add("ERR " + $_.Exception.Message)
}
$lines.Add("DONE")
Set-Content -Path $transcript -Value $lines -Encoding ASCII
