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
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr a, IntPtr b, string c, string d);
  public struct R { public int L,T,Rr,B; }
}
"@
  Add-Type -TypeDefinition $sig
  $h = [IntPtr]::Zero
  $p = Get-Process phi_client -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
  if ($p) { $h = $p.MainWindowHandle }
  if ($h -eq [IntPtr]::Zero) {
    $h = [C]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, "PhiClientShell", $null)
  }
  if ($h -eq [IntPtr]::Zero) {
    $lines.Add("ERR no shell window")
  } else {
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
