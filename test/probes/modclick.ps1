# Modifier-click rig (PhiModClick task): real keyboard modifiers held around a
# real mouse click on the Phi shell, from the interactive session. Steps come
# from C:\Users\jdxin\mod.txt, one per line, CLIENT coordinates of the shell:
#   park              show the shell at 69,40 (1500x950) and raise it
#   keydown,<vk>      keybd_event press (0x10 shift, 0x11 ctrl, 0x12 alt)
#   keyup,<vk>        keybd_event release
#   click,x,y         move the cursor to x,y, press + release the left button
#   rclick,x,y        the same with the right button (a context menu)
#   sclick,x,y        a left click at SCREEN px (a menu item)
#   shot,<name>       full-screen capture to C:\Users\jdxin\mod-<name>.png
#   list              every PhiClientShell/PhiPeekPanel/PhiKioskShell window:
#                     class, visible, rect -> the log
#   tree              the shell's child windows, z-order top-down -> the log
#   sleep,<ms>
# Writes the outcome to modprobe.txt and touches mod.done last.
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class M {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, string cls, string t);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int t, uint f);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, int d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X,Y; }
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }
}
'@
Remove-Item C:\Users\jdxin\mod.done -ErrorAction SilentlyContinue
[M]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$h = [M]::FindWindowW("PhiClientShell", [NullString]::Value)
$out = "hwnd=$h"
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
  $bmp.Save("C:\Users\jdxin\mod-$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
function ToScreen($x, $y) {
  $p = New-Object M+P; $p.X = [int]$x; $p.Y = [int]$y
  [M]::ClientToScreen($h, [ref]$p) | Out-Null
  return $p
}
function ListWindows() {
  $s = ""
  foreach ($cls in @("PhiClientShell", "PhiPeekPanel", "PhiKioskShell")) {
    $w = [IntPtr]::Zero
    while ($true) {
      $w = [M]::FindWindowExW([IntPtr]::Zero, $w, $cls, [NullString]::Value)
      if ($w -eq [IntPtr]::Zero) { break }
      $r = New-Object M+R; [M]::GetWindowRect($w, [ref]$r) | Out-Null
      $sb = New-Object System.Text.StringBuilder 256
      [M]::GetWindowTextW($w, $sb, 256) | Out-Null
      $v = if ([M]::IsWindowVisible($w)) { 1 } else { 0 }
      $s += " [$cls hwnd=$w vis=$v rect=$($r.L),$($r.T),$($r.Rr),$($r.B) title=$($sb.ToString())]"
    }
  }
  return $s
}
# tree: the shell's child windows, z-order top-down, three levels deep.
function Tree($w, $depth) {
  $s = ""
  $c = [M]::GetWindow($w, 5)
  while ($c -ne [IntPtr]::Zero) {
    $sb = New-Object System.Text.StringBuilder 256; [M]::GetClassNameW($c, $sb, 256) | Out-Null
    $r = New-Object M+R; [M]::GetWindowRect($c, [ref]$r) | Out-Null
    $v = if ([M]::IsWindowVisible($c)) { 1 } else { 0 }
    $s += "`n" + ("  " * $depth) + "$($sb.ToString()) hwnd=$c vis=$v rect=$($r.L),$($r.T),$($r.Rr),$($r.B) style=0x$('{0:x}' -f [M]::GetWindowLongW($c, -16)) ex=0x$('{0:x}' -f [M]::GetWindowLongW($c, -20))"
    if ($depth -lt 3) { $s += (Tree $c ($depth + 1)) }
    $c = [M]::GetWindow($c, 2)
  }
  return $s
}
if ($h -ne [IntPtr]::Zero) {
  foreach ($line in (Get-Content C:\Users\jdxin\mod.txt)) {
    $spec = $line.Trim().Split(',')
    if ($spec[0] -eq '') { continue }
    switch ($spec[0]) {
      'park' {
        [M]::ShowWindow($h, 9) | Out-Null
        [M]::SetWindowPos($h, [IntPtr]::Zero, 69, 40, 1500, 950, 0x0004) | Out-Null
        [M]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 900
        $cr = New-Object M+R; [M]::GetClientRect($h, [ref]$cr) | Out-Null
        $o = ToScreen 0 0
        $out += " client=$($cr.Rr)x$($cr.B) origin=$($o.X),$($o.Y)"
      }
      'keydown' {
        [M]::keybd_event([byte][int]$spec[1], 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 120
        $out += " keydown=$($spec[1])"
      }
      'keyup' {
        [M]::keybd_event([byte][int]$spec[1], 0, 2, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 120
        $out += " keyup=$($spec[1])"
      }
      'click' {
        $p = ToScreen $spec[1] $spec[2]
        [M]::SetCursorPos($p.X, $p.Y) | Out-Null
        Start-Sleep -Milliseconds 120
        [M]::mouse_event(0x0001, 1, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 40
        [M]::mouse_event(0x0001, -1, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 120
        $mods = "shift=$([M]::GetAsyncKeyState(0x10) -band 0x8000) ctrl=$([M]::GetAsyncKeyState(0x11) -band 0x8000) alt=$([M]::GetAsyncKeyState(0x12) -band 0x8000)"
        [M]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 90
        [M]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
        $out += " click=$($p.X),$($p.Y) $mods"
      }
      'rclick' {
        # rclick,x,y: a right-button click at CLIENT px (a context menu).
        $p = ToScreen $spec[1] $spec[2]
        [M]::SetCursorPos($p.X, $p.Y) | Out-Null
        Start-Sleep -Milliseconds 120
        [M]::mouse_event(0x0008, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 90
        [M]::mouse_event(0x0010, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 700
        $out += " rclick=$($p.X),$($p.Y)"
      }
      'sclick' {
        # sclick,x,y: a left click at SCREEN px (a menu item).
        [M]::SetCursorPos([int]$spec[1], [int]$spec[2]) | Out-Null
        Start-Sleep -Milliseconds 250
        [M]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 90
        [M]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
        $out += " sclick=$($spec[1]),$($spec[2])"
      }
      'shot' { Shot $spec[1]; $out += " shot=$($spec[1])" }
      'list' { $out += (ListWindows) }
      'tree' { $out += (Tree $h 1) }
      'sleep' { Start-Sleep -Milliseconds ([int]$spec[1]) }
    }
  }
}
# Never leave a modifier stuck down on the box.
foreach ($vk in @(0x10, 0x11, 0x12)) { [M]::keybd_event([byte]$vk, 0, 2, [IntPtr]::Zero) }
$out | Out-File -Encoding utf8 C:\Users\jdxin\modprobe.txt
New-Item -Force -ItemType File "C:\Users\jdxin\mod.done" | Out-Null
