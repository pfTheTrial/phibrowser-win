# Installer look-and-feel rig (Windows test box)

Drives the NSIS installer's window on the box (jdxin@192.168.100.170) from
this machine and pulls cropped screenshots. Everything runs through two
scheduled tasks, both `RunLevel Highest` (the installer is elevated, and
Windows drops injected input from a lower-integrity process):

- **PhiSetupRun** runs `C:\Users\jdxin\setup\run-setup.ps1`: starts the
  exe named in `setup\cmd.txt` (line 1 = exe, line 2 = arguments) in the
  interactive session, writes `pid.txt`. Register once:
  `schtasks /create /tn PhiSetupRun /tr "powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\Users\jdxin\setup\run-setup.ps1" /sc once /st 00:00 /rl highest /it /f`
- **PhiSetupCap** runs `setup\setupcap.ps1` (the updater rig's updcap.ps1
  plus `wtext` / `wedit`): steps from `setup\steps.txt`, log to
  `setup\out.txt`, shots to `setup\shot-<name>.png`. Steps: `wait,<title
  substr>,<ms>` · `focus,<title>` · `wclick,<title>,dx,dy` (window-relative
  physical px) · `wedit,<title>,<n>,<text>` (the n-th visible EDIT, via
  WM_SETTEXT - SetWindowText cannot cross processes, and typing goes
  through the box's Chinese IME) · `keys,<SendKeys>` · `shot,<name>` ·
  `sleep,<ms>` · `list`.

From here: `relaunch.sh [exe-on-box] [args] [local exe name]` kills any
running setup, copies `installer/dist/<exe>` to `setup\` and starts it;
`setup-rig.sh <steps-file> [shot names...]` runs the steps and crops each
named shot to the last focused window (`$PHI_SETUP_SHOTS`, default
/tmp/phi-setup-shots). `lock.ps1 <path> <seconds>` holds a file open with
no sharing (the "Phi is still running" state) - run it in a BACKGROUND ssh
session that stays open, a process spawned from a finished ssh command is
killed with it.

Build a throwaway test installer (no real payload, installs under
C:\phi-setup-test, records itself under Software\PhiSetupTest):

    makensis -V2 -INPUTCHARSET UTF8 -DPHI_VERSION=0.1.0 -DPHI_STAGE=<dir with
      resources/phi.ico, Phi.exe, framework/chrome.dll, framework/payload.bin>
      -DPHI_UI_TEST -DPHI_LAYOUT_FRAMEWORK -DPHI_OUTDIR=<dist> phi-installer.nsi

Window-relative click points at 125 % (client origin is 3,33 px into the
window): Install/Update/Reinstall 523,510 · Cancel/Close 663,510 · the
collapsed expander's chevron 673,250 (673,276 under an InfoBar) · the
expanded one 673,175 · the desktop toggle 623,384 · a lone primary 653,510.
The box's Phi data dir is the real one: keep the uninstaller's "Also delete
my browsing data" box off.
