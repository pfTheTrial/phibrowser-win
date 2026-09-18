# Drives the windows OUTSIDE the Phi shell (WinSparkle's dialogs, the NSIS
# installer) from the interactive session. Steps come from
# C:\Users\jdxin\setup\steps.txt, one per line, SCREEN coordinates (physical px):
#   list                       append every visible titled top-level window
#   wait,<title-substr>,<ms>   poll until such a window is visible
#   focus,<title-substr>       raise it (and record its rect)
#   click,x,y                  left click at screen x,y
#   wclick,<title-substr>,dx,dy  left click at dx,dy from that window's top-left
#   keys,<SendKeys spec>       into the focused window
#   shot,<name>                full-screen capture to C:\Users\jdxin\upd-<name>.png
#   sleep,<ms>
# Writes upd-out.txt and touches upd.done last.
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class U {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, int d, IntPtr e);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(P p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  public static List<IntPtr> Edits(IntPtr top) {
    var o = new List<IntPtr>();
    EnumChildWindows(top, (h, l) => { var c = new StringBuilder(64); GetClassNameW(h, c, 64); if (c.ToString() == "Edit" && IsWindowVisible(h)) o.Add(h); return true; }, IntPtr.Zero);
    return o;
  }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  public static void SetText(IntPtr h, string s) { SendMessageW(h, 0x000C, IntPtr.Zero, s); }
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X,Y; }
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }
  public static List<string> List() {
    var o = new List<string>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
      if (t.Length == 0) return true;
      var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      R r; GetWindowRect(h, out r); uint pid; GetWindowThreadProcessId(h, out pid);
      o.Add(h.ToInt64() + "|" + pid + "|" + c + "|" + t + "|" + r.L + "," + r.T + "," + r.Rr + "," + r.B);
      return true; }, IntPtr.Zero);
    return o;
  }
  public static IntPtr Find(string sub) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
      if (t.ToString().IndexOf(sub, StringComparison.OrdinalIgnoreCase) < 0) return true;
      found = h; return false; }, IntPtr.Zero);
    return found;
  }
}
'@
Remove-Item C:\Users\jdxin\setup\done -ErrorAction SilentlyContinue
[U]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$out = @()
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
  $bmp.Save("C:\Users\jdxin\setup\shot-$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
foreach ($line in (Get-Content C:\Users\jdxin\setup\steps.txt)) {
  $spec = $line.Trim().Split(',')
  if ($spec[0] -eq '') { continue }
  switch ($spec[0]) {
    'list' { $out += "-- windows"; $out += [U]::List() }
    'wait' {
      $deadline = (Get-Date).AddMilliseconds([int]$spec[2]); $h = [IntPtr]::Zero
      while ($h -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300; $h = [U]::Find($spec[1]) }
      $out += "wait '$($spec[1])' -> $h"
    }
    'focus' {
      $h = [U]::Find($spec[1])
      if ($h -ne [IntPtr]::Zero) {
        [U]::ShowWindow($h, 9) | Out-Null; [U]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 600
        $r = New-Object U+R; [U]::GetWindowRect($h, [ref]$r) | Out-Null
        $out += "focus '$($spec[1])' -> $h rect=$($r.L),$($r.T),$($r.Rr),$($r.B)"
      } else { $out += "focus '$($spec[1])' -> none" }
    }
    'click' {
      [U]::SetCursorPos([int]$spec[1], [int]$spec[2]) | Out-Null
      Start-Sleep -Milliseconds 150
      [U]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
      [U]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 500
      $out += "click=$($spec[1]),$($spec[2])"
    }
    'wclick' {
      $h = [U]::Find($spec[1])
      if ($h -ne [IntPtr]::Zero) {
        $r = New-Object U+R; [U]::GetWindowRect($h, [ref]$r) | Out-Null
        [U]::SetCursorPos($r.L + [int]$spec[2], $r.T + [int]$spec[3]) | Out-Null
        Start-Sleep -Milliseconds 150
        [U]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
        [U]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 500
        $out += "wclick '$($spec[1])' at $($r.L + [int]$spec[2]),$($r.T + [int]$spec[3])"
      } else { $out += "wclick '$($spec[1])' -> none" }
    }
    'keys' {
      $spec2 = ($line.Trim() -split ',', 2)[1]
      [System.Windows.Forms.SendKeys]::SendWait($spec2); Start-Sleep -Milliseconds 400
      $out += "keys"
    }
    'wtext' {
      $parts = ($line.Trim() -split ',', 5)
      $h = [U]::Find($parts[1])
      if ($h -ne [IntPtr]::Zero) {
        $r = New-Object U+R; [U]::GetWindowRect($h, [ref]$r) | Out-Null
        $pt = New-Object U+P; $pt.X = $r.L + [int]$parts[2]; $pt.Y = $r.T + [int]$parts[3]
        $c = [U]::WindowFromPoint($pt)
        [U]::SetText($c, $parts[4])
        $out += "wtext -> $c '$($parts[4])'"
      } else { $out += "wtext '$($parts[1])' -> none" }
    }
    'wedit' {
      $parts = ($line.Trim() -split ',', 4)
      $h = [U]::Find($parts[1])
      if ($h -ne [IntPtr]::Zero) {
        $eds = [U]::Edits($h)
        $e = $eds[[int]$parts[2]]
        [U]::SetText($e, $parts[3])
        $out += "wedit -> $e of $($eds.Count) '$($parts[3])'"
      } else { $out += "wedit '$($parts[1])' -> none" }
    }
    'shot' { Shot $spec[1]; $out += "shot=$($spec[1])" }
    'sleep' { Start-Sleep -Milliseconds ([int]$spec[1]) }
  }
}
$out | Out-File -Encoding utf8 C:\Users\jdxin\setup\out.txt
New-Item -Force -ItemType File "C:\Users\jdxin\setup\done" | Out-Null
