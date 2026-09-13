# Stages the installer payload from a deployed tree (what sync-fx-win.sh
# pushes) into a fresh stage folder, in one of two layouts:
#
#   flat       everything beside Phi.exe — the only layout a COMPONENT
#              framework can have (the exe's static imports must sit in its
#              own directory)
#   framework  the ship layout (docs/static-build-plan.md §6): Phi.exe,
#              WinSparkle.dll and the client's own resources\ at the top,
#              every framework file under framework\ — chrome.dll,
#              chrome_elf.dll + chrome_wer.dll, the paks, locales, ICU, the
#              GL/Vulkan/DXC DLLs, Chromium's resources\, and the assembly
#              manifest (framework\framework.manifest) that lets the exe's
#              one static import, chrome_elf.dll, resolve from inside the
#              folder. Only a non-component framework fits this.
#   auto       framework when the tree looks static (few DLLs), else flat
#
# Usage: stage-payload.ps1 -Source C:\Users\jdxin\phi-fx-ship
#            -Stage C:\Users\jdxin\phi-stage [-Layout auto|flat|framework]
param(
  [Parameter(Mandatory)][string]$Source,
  [Parameter(Mandatory)][string]$Stage,
  [string]$Layout = 'auto'
)
$ErrorActionPreference = 'Stop'
$folder = 'framework'
$globs = @('*.exe', '*.dll', '*.pak', '*.bin', '*.dat', '*.manifest')
$trees = @('Dictionaries', 'locales', 'MEIPreload', 'resources')
$dlls = @(Get-ChildItem $Source -Filter *.dll -File).Count
if ($Layout -eq 'auto') { $Layout = if ($dlls -lt 32) { 'framework' } else { 'flat' } }
if ($Layout -eq 'framework' -and $dlls -ge 32) {
  throw "framework layout asked of a component tree ($dlls DLLs beside the exe)"
}
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Stage | Out-Null

function CopyTop($from, $to, $extraArgs) {
  robocopy $from $to $globs /NJH /NJS /NDL /NFL @extraArgs | Out-Null
}
function CopyTrees($from, $to) {
  foreach ($d in $trees) {
    if (Test-Path "$from\$d") {
      robocopy "$from\$d" "$to\$d" /E /NJH /NJS /NDL /NFL | Out-Null
    }
  }
}

if ($Layout -eq 'flat') {
  CopyTop $Source $Stage @()
  CopyTrees $Source $Stage
} else {
  $fw = Join-Path $Stage $folder
  New-Item -ItemType Directory -Force $fw | Out-Null
  # Top level: the client's own. WinSparkle.dll is loaded by the updater
  # from the exe's directory; the resources list is written by
  # build-client.py beside the files it stages.
  $top = @('Phi.exe', 'WinSparkle.dll')
  foreach ($f in $top) {
    if (Test-Path "$Source\$f") { Copy-Item "$Source\$f" $Stage }
  }
  $list = Join-Path $Source 'resources\phi-client-resources.txt'
  if (-not (Test-Path $list)) { throw "$list missing — rebuild the client (build-client.py writes it)" }
  $client = @(Get-Content $list | Where-Object { $_.Trim() -ne '' })
  New-Item -ItemType Directory -Force "$Stage\resources" | Out-Null
  foreach ($e in $client) {
    if (Test-Path "$Source\resources\$e") {
      Copy-Item "$Source\resources\$e" "$Stage\resources\$e" -Recurse
    }
  }
  # framework\: every other runtime file, then Chromium's share of resources.
  CopyTop $Source $fw (@('/XF') + $top)
  CopyTrees $Source $fw
  foreach ($e in $client + @('phi-client-resources.txt')) {
    Remove-Item "$fw\resources\$e" -Recurse -Force -ErrorAction SilentlyContinue
  }
  # The exe depends on assembly "framework": the loader finds it as
  # framework\framework.manifest. A manifest at the top level would resolve
  # the assembly to the exe's own directory instead, where chrome_elf.dll
  # is not — so none goes there (the top-level copy above took no *.manifest).
  if (-not (Test-Path "$fw\framework.manifest")) {
    throw "$Source has no framework.manifest — a framework built before the assembly rename (chrome/app/version_assembly)"
  }
  Get-ChildItem $fw -Filter *.manifest | Where-Object { $_.Name -ne 'framework.manifest' } | Remove-Item -Force
}
$n = @(Get-ChildItem $Stage -Recurse -File).Count
"staged layout=$Layout dlls=$dlls files=$n"
