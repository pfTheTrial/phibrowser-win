# Captures client-drawn windows to PNGs, for the rasteriser comparison that
# docs/architecture-review.md §3.6 (phase 5) needs and no host can run: the
# shape primitives in client/phi_dwrite.cc moved from a software distance
# field to Direct2D, and the pixels that changed are the antialiased edge of
# every curve in the shell.
#
# Runs ON the box, from C:\Users\jdxin\phitest. Shoot a set BEFORE the new
# build and a set AFTER it, then diff them with rast-compare.mjs:
#
#   powershell -File rast-shot.ps1 -Tag before
#   ... install the new Phi.exe, restore the same window state ...
#   powershell -File rast-shot.ps1 -Tag after
#   node rast-compare.mjs C:\Users\jdxin\rast\before C:\Users\jdxin\rast\after
#
# The comparison is only as good as the state: put the shell in the SAME
# place, at the same size and DPI, with the same Space, the same hovered row
# and the same theme both times. The surfaces worth shooting are listed in
# -Windows below; every one of them is drawn by a primitive that changed.
param(
  [string]$Tag = "shot",
  [string]$Dir = "C:\Users\jdxin\rast",
  # Window classes to capture, in order. The shell carries the sidebar rows,
  # the Spaces strip pips and the theme slider; the layered children carry
  # the corner caps, the status bubble and the Space hover card, each of
  # which is committed as per-pixel alpha rather than painted through
  # WM_PAINT (so PrintWindow on the shell alone does not show them).
  [string]$Windows = "PhiClientShell;PhiCornerCap;PhiStatusBubble;PhiSpaceHoverCard;PhiFloatingSidebar"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$sig = @"
using System;using System.Runtime.InteropServices;
public class S {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr a, IntPtr b, string c, string d);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr u, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public struct R { public int L,T,Rr,B; }
}
"@
Add-Type -TypeDefinition $sig
# Physical pixels, the space the client lays out in: a DPI-virtualised
# capture is resampled, and resampling is exactly what this test measures.
[S]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$out = Join-Path $Dir $Tag
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = New-Object System.Collections.Generic.List[string]

$shell = [S]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, "PhiClientShell", $null)
if ($shell -eq [IntPtr]::Zero) { throw "no PhiClientShell window" }

foreach ($class in $Windows.Split(";")) {
  if ($class -eq "") { continue }
  $index = 0
  $h = [IntPtr]::Zero
  # The caps and the bubble are CHILDREN of the shell, so an enumeration
  # rooted at the desktop never sees them; the hover card and the floating
  # sidebar are owned popups and do live at the top level. Try both.
  $parent = [IntPtr]::Zero
  if ([S]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, $class, $null) -eq [IntPtr]::Zero) {
    $parent = $shell
  }
  # Several windows can share a class (four corner caps); shoot them all.
  while ($true) {
    $h = [S]::FindWindowEx($parent, $h, $class, $null)
    if ($h -eq [IntPtr]::Zero) { break }
    if (-not [S]::IsWindowVisible($h)) { continue }
    $r = New-Object S+R
    [S]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.Rr - $r.L
    $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { continue }
    # RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN: PrintWindow can
    # otherwise hand back the last composited frame.
    [S]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero, 0x181) | Out-Null
    Start-Sleep -Milliseconds 250
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    # PW_RENDERFULLCONTENT (2): a layered child renders nothing without it.
    [S]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $name = "$class-$index.png"
    $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $g.Dispose()
    $log.Add("$name ${w}x${hh}")
    $index++
  }
  if ($index -eq 0) { $log.Add("$class MISSING") }
}
$log.Add("DONE")
Set-Content -Path (Join-Path $out "index.txt") -Value $log -Encoding ASCII
$log | ForEach-Object { Write-Output $_ }
