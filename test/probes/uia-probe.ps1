# Dumps the client's UI Automation tree, and watches its change events, so
# the accessibility provider added in docs/architecture-review.md §3.6 / §4
# item 31 (client/phi_uia.cc) can be verified ON THE BOX WITHOUT A GUI TOOL.
# Inspect.exe and Narrator are the human checks; this is the one that fits
# in a terminal and can be pasted into a report.
#
# Before phi_uia.cc every dump below was EMPTY — lane3-ui-substrate.md §2
# "Cross-cutting absences": no WM_GETOBJECT, no IRawElementProvider*, no
# NotifyWinEvent anywhere in the tree, so a screen reader saw a bare window
# with nothing in it. The provider rides `hits_`, the rect list every
# PhiWindow already keeps for hover and clicks, so the dump is exactly the
# set of things the mouse can reach.
#
# Runs ON the box, from C:\Users\jdxin\phitest, against a running Phi.exe:
#
#   # every Phi window, one line per element
#   powershell -File uia-probe.ps1
#
#   # one surface (open the downloads popover first, then run this)
#   powershell -File uia-probe.ps1 -Class PhiDownloadsPopover
#
#   # watch structure/name events for 20 s while you click around
#   powershell -File uia-probe.ps1 -Class PhiClientShell -Watch 20
#
#   # press an element by name, through the provider's Invoke pattern
#   powershell -File uia-probe.ps1 -Class PhiDownloadsPopover -Invoke "All Downloads"
#
# A CORRECT DUMP for the downloads popover looks like this — a Pane for the
# window (the HWND host provider's answer, not ours), then one child per
# hit, each with a name, a control type from PhiHit::role, a screen
# rectangle matching the pixels, and Invoke on the actionable ones:
#
#   PhiDownloadsPopover  hwnd=0x004213A6
#     Pane  ""  (1180,318)-(1480,548)
#       ListItem  "report.pdf"        (1188,358)-(1468,394)  id=phi-hit-0     [Invoke SelectionItem]
#       Button    "Open"              (1388,364)-(1432,388)  id=phi-hit-1     [Invoke]
#       Button    "Show in folder"    (1432,364)-(1476,388)  id=phi-hit-2     [Invoke]
#       Button    "All Downloads"     (1188,402)-(1468,430)  id=phi-hit-1000  [Invoke]
#     4 element(s)
#
# WHAT IS WRONG IF: "0 element(s)" under a Pane means WM_GETOBJECT answered
# but hits_ was empty when the client asked — the surface had not laid out
# yet, or it is one of the surfaces not yet migrated onto PhiWindow (see the
# report). "no automation element" means the window is not a PhiWindow at
# all. Names in the dump come straight from PhiHit::name, so a blank name
# is a surface that built a clickable rect without labelling it — that hit
# has no tooltip either, and the fix belongs in the surface, not here.
param(
  # Window class to dump. Empty means every top-level window of this
  # process whose class starts with "Phi".
  [string]$Class = "",
  # Seconds to watch for StructureChanged / Name-property events instead of
  # exiting after the dump. 0 dumps and exits.
  [int]$Watch = 0,
  # Name of an element to Invoke after the dump (exact, case-sensitive).
  [string]$Invoke = "",
  [int]$Depth = 6
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$sig = @"
using System;using System.Text;using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
Add-Type -TypeDefinition $sig
# Physical pixels: the provider reports screen rectangles in the same space
# the client lays out in, and a DPI-virtualised client would report them
# scaled and look wrong for no reason.
[W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$phi = Get-Process -Name Phi -ErrorAction SilentlyContinue
if (-not $phi) { throw "Phi.exe is not running" }
$phiPids = @($phi | ForEach-Object { $_.Id })

# --- collect the windows to dump -------------------------------------------
$targets = New-Object System.Collections.Generic.List[object]
$cb = [W+EnumProc]{
  param($h, $l)
  $pidOut = 0
  [W]::GetWindowThreadProcessId($h, [ref]$pidOut) | Out-Null
  if ($phiPids -notcontains $pidOut) { return $true }
  if (-not [W]::IsWindowVisible($h)) { return $true }
  $sb = New-Object System.Text.StringBuilder 256
  [W]::GetClassName($h, $sb, 256) | Out-Null
  $name = $sb.ToString()
  if ($Class -ne "") {
    if ($name -ne $Class) { return $true }
  } elseif (-not $name.StartsWith("Phi")) { return $true }
  $targets.Add([pscustomobject]@{ Hwnd = $h; Class = $name })
  return $true
}
[W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($targets.Count -eq 0) { throw "no matching Phi window is visible" }

# --- dump -------------------------------------------------------------------
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$found = @{}

function Patterns($element) {
  $names = New-Object System.Collections.Generic.List[string]
  foreach ($p in $element.GetSupportedPatterns()) {
    # "InvokePatternIdentifiers.Pattern" -> "Invoke"
    $names.Add(($p.ProgrammaticName -replace 'PatternIdentifiers\.Pattern$', ''))
  }
  return ($names -join " ")
}

function Describe($element, $indent) {
  $type = $element.Current.ControlType.ProgrammaticName -replace '^ControlType\.', ''
  $r = $element.Current.BoundingRectangle
  $box = "({0},{1})-({2},{3})" -f [int]$r.Left, [int]$r.Top, [int]$r.Right, [int]$r.Bottom
  $line = "{0}{1,-10} `"{2}`"  {3}" -f $indent, $type, $element.Current.Name, $box
  if ($element.Current.AutomationId -ne "") {
    $line += "  id=" + $element.Current.AutomationId
  }
  $pats = Patterns $element
  if ($pats -ne "") { $line += "  [$pats]" }
  if (-not $element.Current.IsEnabled) { $line += "  DISABLED" }
  if ($element.Current.IsOffscreen) { $line += "  OFFSCREEN" }
  # Selection and toggle state, where the role has them: this is
  # PhiHit::selected coming back out through the two patterns.
  foreach ($p in $element.GetSupportedPatterns()) {
    if ($p.ProgrammaticName -like "SelectionItem*") {
      $sel = $element.GetCurrentPattern($p).Current.IsSelected
      $line += "  selected=$sel"
    } elseif ($p.ProgrammaticName -like "Toggle*") {
      $state = $element.GetCurrentPattern($p).Current.ToggleState
      $line += "  toggle=$state"
    }
  }
  Write-Output $line
}

function Walk($element, $depth, $indent) {
  $count = 0
  $child = $walker.GetFirstChild($element)
  while ($child -ne $null) {
    Describe $child $indent
    if ($Invoke -ne "" -and $child.Current.Name -eq $Invoke) { $found[$Invoke] = $child }
    $count++
    if ($depth -gt 1) { $count += Walk $child ($depth - 1) ($indent + "  ") }
    $child = $walker.GetNextSibling($child)
  }
  return $count
}

foreach ($t in $targets) {
  Write-Output ("{0}  hwnd=0x{1:X8}" -f $t.Class, [int64]$t.Hwnd)
  $root = $null
  try { $root = [System.Windows.Automation.AutomationElement]::FromHandle($t.Hwnd) } catch { }
  if ($root -eq $null) { Write-Output "  no automation element"; continue }
  Describe $root "  "
  $n = Walk $root $Depth "    "
  Write-Output "  $n element(s)"
}

# --- invoke -----------------------------------------------------------------
if ($Invoke -ne "") {
  if (-not $found.ContainsKey($Invoke)) {
    Write-Output "INVOKE: no element named `"$Invoke`""
  } else {
    $element = $found[$Invoke]
    $pattern = $null
    try {
      $pattern = $element.GetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern)
    } catch { }
    if ($pattern -eq $null) {
      Write-Output "INVOKE: `"$Invoke`" has no Invoke pattern"
    } else {
      # This runs PhiWindow::InvokeHit on the client's UI thread, which
      # sends the SAME WM_LBUTTONDOWN/WM_LBUTTONUP pair a mouse click sends
      # at a point inside the hit's own rect. If the surface reacts exactly
      # as a click on it does, the provider is wired to the real path and
      # not to a copy of it.
      $pattern.Invoke()
      Write-Output "INVOKE: pressed `"$Invoke`""
    }
  }
}

# --- watch ------------------------------------------------------------------
if ($Watch -gt 0) {
  Write-Output "watching $Watch s — click around in the client"
  $handler = [System.Windows.Automation.StructureChangedEventHandler]{
    param($sender, $e)
    Write-Output ("  structure {0}" -f $e.StructureChangeType)
  }
  $property = [System.Windows.Automation.AutomationPropertyChangedEventHandler]{
    param($sender, $e)
    $short = $e.Property.ProgrammaticName -replace '^AutomationElementIdentifiers\.', ''
    Write-Output ("  property  {0} = {1}" -f $short, $e.NewValue)
  }
  foreach ($t in $targets) {
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($t.Hwnd)
    [System.Windows.Automation.Automation]::AddStructureChangedEventHandler(
      $root, [System.Windows.Automation.TreeScope]::Subtree, $handler)
    [System.Windows.Automation.Automation]::AddAutomationPropertyChangedEventHandler(
      $root, [System.Windows.Automation.TreeScope]::Subtree, $property,
      @([System.Windows.Automation.AutomationElement]::NamePropertyId,
        [System.Windows.Automation.AutomationElement]::IsEnabledPropertyId,
        [System.Windows.Automation.SelectionItemPattern]::IsSelectedProperty,
        [System.Windows.Automation.TogglePattern]::ToggleStateProperty))
  }
  Start-Sleep -Seconds $Watch
  [System.Windows.Automation.Automation]::RemoveAllEventHandlers()
  # A rebuild that changed nothing must print NOTHING: the provider diffs
  # the hit list and raises the narrowest event that describes the
  # difference (phi_uia.h, "the event decision table"). Hovering alone
  # should be silent; switching Space or opening a popover should print one
  # structure line; pinning an extension should print one property line.
  Write-Output "watch done"
}
