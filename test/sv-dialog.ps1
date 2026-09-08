# Dismiss any shell dialog (#32770) currently open — the Save As dialog the
# probe raised — and record what was found. Runs INSIDE the interactive
# session (scheduled task PhiSv), where window enumeration works.
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class D {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, int w, int l);
}
'@
$found = New-Object System.Collections.ArrayList
$cb = [D+EnumProc]{ param($h, $l)
  if ([D]::IsWindowVisible($h)) {
    $c = New-Object System.Text.StringBuilder 64; [D]::GetClassNameW($h, $c, 64) | Out-Null
    if ($c.ToString() -eq '#32770') {
      $t = New-Object System.Text.StringBuilder 256; [D]::GetWindowTextW($h, $t, 256) | Out-Null
      [void]$found.Add("dialog hwnd=$h title=$($t.ToString())")
      [D]::PostMessageW($h, 0x0010, 0, 0) | Out-Null
    }
  }
  return $true }
[D]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
$procs = (Get-Process Phi -ErrorAction SilentlyContinue | Measure-Object).Count
("procs=$procs`n" + ($found -join "`n")) | Out-File -Encoding utf8 C:\Users\jdxin\svdialog.txt
