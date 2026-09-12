; Copyright 2026 The Fyde Innovations Limited. All rights reserved.
; Use of this source code is governed by a BSD-style license that can be
; found in the LICENSE file.
;
; The Phi Browser per-user installer - WinSparkle's payload and the manual
; first-install, one script. Design (the Mac parity notes in
; windows-framework-status.md):
;   - PER USER: everything under $LOCALAPPDATA\PhiBrowser, no admin, no UAC.
;   - VERSIONED-SWAP UPDATE: chrome.dll is locked while Phi runs, so the
;     installer runs when the app has quit (WinSparkle's flow), snapshots
;     the live install into rollback\ first, then copies the new files into
;     the stable bin\ - the stable path keeps taskbar pins and shortcuts
;     valid, the snapshot is the Mac TimeMachineInstaller's rollback.
;
; Build (makensis 3.x):
;   makensis /DPHI_VERSION=0.1.0 /DPHI_STAGE=payload phi-installer.nsi

!ifndef PHI_VERSION
  !error "pass /DPHI_VERSION=x.y.z"
!endif
!ifndef PHI_STAGE
  !error "pass /DPHI_STAGE=<staged payload dir>"
!endif

Unicode true
Name "Phi Browser"
OutFile "PhiBrowserSetup-${PHI_VERSION}.exe"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\PhiBrowser\bin"
SetCompressor /SOLID lzma

!define UNINST_KEY \
  "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhiBrowser"

Section "Install"
  ; The update path must not race a running Phi: chrome.dll being locked is
  ; the tell - it stays mapped for as long as any Phi process lives. The
  ; probe OPENS it for writing (a rename is no test: Windows lets a mapped
  ; image be renamed, only not overwritten). WinSparkle launches this
  ; installer and THEN asks Phi to quit, so a locked file is the normal
  ; first sight here - wait up to a minute for Chromium to unwind before
  ; asking the user, ask-and-retry rather than failing halfway through a
  ; copy.
  check_running:
    IfFileExists "$INSTDIR\chrome.dll" 0 fresh_install
    StrCpy $1 0
  wait_running:
    ClearErrors
    FileOpen $0 "$INSTDIR\chrome.dll" a
    IfErrors 0 not_running
      IntOp $1 $1 + 1
      IntCmp $1 120 ask_running 0 ask_running
      Sleep 500
      Goto wait_running
  ask_running:
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
      "Phi Browser is running. Close it, then press Retry." \
      IDRETRY check_running
    Abort
  not_running:
    FileClose $0

    ; Rollback snapshot: keep exactly ONE previous version.
    RMDir /r "$LOCALAPPDATA\PhiBrowser\rollback"
    CreateDirectory "$LOCALAPPDATA\PhiBrowser\rollback"
    CopyFiles /SILENT "$INSTDIR\*.*" "$LOCALAPPDATA\PhiBrowser\rollback"

  fresh_install:
  SetOutPath "$INSTDIR"
  File /r "${PHI_STAGE}\*.*"

  ; The sandbox's AppContainer utilities (proxy resolver, media CDM, ...)
  ; run under the chromeInstallFiles / lpacChromeInstallFiles capabilities
  ; and must read and execute the install tree; a user folder grants them
  ; nothing by default and the launch then fails (sandbox_win.cc's
  ; AddAppContainerProfileToConfig access check). Chrome's setup grants the
  ; same two capability SIDs (installer/setup/configure_app_container_
  ; sandbox.cc); the dev sync grants the broader ALL APPLICATION PACKAGES.
  nsExec::ExecToLog 'icacls "$INSTDIR" /grant "*S-1-15-3-1024-3424233489-972189580-2057154623-747635277-1604371224-316187997-3786583170-1043257646:(OI)(CI)(RX)" "*S-1-15-3-1024-2302894289-466761758-1166120688-1039016420-2430351297-4240214049-4028510897-3317428798:(OI)(CI)(RX)"'
  Pop $0

  ; Shortcuts to the STABLE path - pins survive every update.
  CreateShortCut "$SMPROGRAMS\Phi Browser.lnk" "$INSTDIR\Phi.exe"
  CreateShortCut "$DESKTOP\Phi Browser.lnk" "$INSTDIR\Phi.exe"

  WriteUninstaller "$LOCALAPPDATA\PhiBrowser\Uninstall.exe"

  ; Apps & features entry (per-user hive).
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "Phi Browser"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${PHI_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Fyde Innovations"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\Phi.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" \
    "$LOCALAPPDATA\PhiBrowser\Uninstall.exe"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; Binaries, snapshot, shortcuts, registry. USER DATA IS KEPT - deleting
  ; profiles is the user's call, never an uninstaller side effect.
  RMDir /r "$LOCALAPPDATA\PhiBrowser\bin"
  RMDir /r "$LOCALAPPDATA\PhiBrowser\rollback"
  Delete "$LOCALAPPDATA\PhiBrowser\Uninstall.exe"
  RMDir "$LOCALAPPDATA\PhiBrowser"
  Delete "$SMPROGRAMS\Phi Browser.lnk"
  Delete "$DESKTOP\Phi Browser.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
