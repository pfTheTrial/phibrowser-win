# The refusal toast, without pixels: ask for Reader View on a page with no
# article and watch for the "PhiToast" window the client raises. Works on a
# locked session, where a screen capture only ever comes back black.
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class T {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static List<string> Find(string cls) {
    var hits = new List<string>();
    EnumWindows((h, l) => {
      var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      if (c.ToString() == cls) {
        var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
        hits.Add(String.Format("hwnd={0} visible={1} text='{2}'", h, IsWindowVisible(h), t));
      }
      return true;
    }, IntPtr.Zero);
    return hits;
  }
}
'@
# MUST run in the INTERACTIVE session (the PhiCap scheduled task, by copying
# this over capture.ps1). A command launched over ssh lands on the service
# window station and enumerates no windows at all, so from there every answer
# is "no toast" whether or not one appeared.
$deadline = (Get-Date).AddSeconds(30)
$seen = @()
$out = @()
while ((Get-Date) -lt $deadline) {
  foreach ($h in [T]::Find("PhiToast")) {
    if ($seen -notcontains $h) { $seen += $h; $out += "TOAST $h" }
  }
  Start-Sleep -Milliseconds 120
}
if ($seen.Count -eq 0) { $out += "NO TOAST" }
$out | Out-File -Encoding utf8 C:\Users\jdxin\hoverprobe.txt
New-Item -Force -ItemType File "C:\Users\jdxin\shot.done" | Out-Null
