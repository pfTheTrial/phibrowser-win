; Copyright 2026 The Fyde Innovations Limited. All rights reserved.
; Use of this source code is governed by a BSD-style license that can be
; found in the LICENSE file.
;
; The Phi Browser installer - WinSparkle's payload and the manual
; first-install, one script. Design (the Mac parity notes in
; windows-framework-status.md; the flow itself in docs/static-build-plan.md):
;   - PER MACHINE by default: C:\Program Files\Phi Browser, elevated (UAC),
;     Apps & Features entry and shortcuts for all users. The user may point
;     it anywhere else on the Program folder page; a folder outside Program
;     Files makes install_static treat the install as per-user, so the
;     consent and data-dir values are written to BOTH HKLM and HKCU.
;   - THE DATA FOLDER IS THE USER'S CHOICE: profiles, history, settings and
;     the telemetry config live in a folder separate from the program,
;     default %LOCALAPPDATA%\Phi\User Data. Another folder is recorded as
;     Software\Phi\UserDataDir, which install_static consults after
;     --user-data-dir and the policy (chrome/install_static/user_data_dir.cc)
;     - every Phi process, the crash handler included, follows it.
;   - LAYOUT: Phi.exe + framework\ (the non-component framework;
;     make-installer.sh stages it so, /DPHI_LAYOUT_FRAMEWORK tells this
;     script). A component framework can only be flat: the exe's static
;     imports must sit in its own directory.
;   - VERSIONED-SWAP UPDATE: chrome.dll is locked while Phi runs, so the
;     installer runs when the app has quit (WinSparkle's flow), snapshots
;     the live program files into rollback\ first, then copies the new ones
;     over - the stable path keeps taskbar pins and shortcuts valid, the
;     snapshot is the Mac TimeMachineInstaller's rollback. Silent (/S)
;     installs keep the recorded program and data folders.
;
;   - ONE INSTALLER PER ARCHITECTURE: PhiBrowserSetup-<ver>.exe carries the
;     x64 build, PhiBrowserSetup-<ver>-arm64.exe the Windows on ARM build
;     (/DPHI_ARCH). The stub itself is NSIS's 32-bit code, which every
;     Windows runs (x64 through WOW64, ARM64 through its x86 emulation), so
;     the payload's architecture is checked against the MACHINE in .onInit:
;     the arm64 installer refuses anything but an ARM64 PC, the x64 one
;     warns on an ARM64 PC (its build runs there emulated) and continues
;     silently - WinSparkle picks the enclosure by architecture, so a silent
;     update on an ARM PC that has the x64 build is the x64 build again.
;
; Build (makensis 3.x, on Linux via make-installer.sh):
;   makensis -DPHI_VERSION=0.1.0 -DPHI_STAGE=<staged tree> -DPHI_ART=art [-DPHI_LAYOUT_FRAMEWORK] \
;            [-DPHI_ARCH=x64|arm64] [-DPHI_OUTDIR=dist] [-DPHI_TELEMETRY_JSON=phi_telemetry.json] \
;            [-DPHI_SIGN_CMD=<sign-pe.sh>] phi-installer.nsi

!ifndef PHI_VERSION
  !error "pass /DPHI_VERSION=x.y.z"
!endif
!ifndef PHI_STAGE
  !error "pass /DPHI_STAGE=<staged payload dir>"
!endif
!ifndef PHI_ART
  !error "pass /DPHI_ART=<dir with welcome.bmp and header.bmp>"
!endif

!ifndef PHI_ARCH
  !define PHI_ARCH "x64"
!endif
!if "${PHI_ARCH}" == "arm64"
  !define PHI_ARCH_SUFFIX "-arm64"
!else
  !if "${PHI_ARCH}" != "x64"
    !error "PHI_ARCH must be x64 or arm64"
  !endif
  !define PHI_ARCH_SUFFIX ""
!endif

Unicode true
!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Name "Phi Browser"
; Where the setup exe lands (makensis resolves a bare OutFile against this
; script's directory, not the working directory).
!ifndef PHI_OUTDIR
  !define PHI_OUTDIR "."
!endif
OutFile "${PHI_OUTDIR}/PhiBrowserSetup-${PHI_VERSION}${PHI_ARCH_SUFFIX}.exe"
; Authenticode (make-installer.sh passes its sign-pe.sh): the setup exe once
; it is written, and the uninstaller stub before it is packed into the
; installer - WriteUninstaller only drops that stub, so this is the one
; place Uninstall.exe can pick up a signature (NSIS 3.08+). The payload was
; signed in the stage. A failed sign fails the build (= 0).
!ifdef PHI_SIGN_CMD
  !finalize '"${PHI_SIGN_CMD}" "%1"' = 0
  !uninstfinalize '"${PHI_SIGN_CMD}" "%1"' = 0
!endif
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\Phi Browser"
InstallDirRegKey HKLM "Software\Phi" "InstallDir"
SetCompressor /SOLID lzma
BrandingText "Phi Browser ${PHI_VERSION}"
ShowInstDetails nevershow
ShowUninstDetails nevershow

VIProductVersion "${PHI_VERSION}.0"
VIAddVersionKey "ProductName" "Phi Browser"
VIAddVersionKey "FileDescription" "Phi Browser Setup (${PHI_ARCH})"
VIAddVersionKey "CompanyName" "Fyde Innovations"
VIAddVersionKey "LegalCopyright" "Copyright 2026 The Fyde Innovations Limited"
VIAddVersionKey "FileVersion" "${PHI_VERSION}"
VIAddVersionKey "ProductVersion" "${PHI_VERSION}"

!define REG_PRODUCT "Software\Phi"
!define UNINST_KEY \
  "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhiBrowser"

Var DataDir          ; the data folder in effect
Var DataDirDefault   ; %LOCALAPPDATA%\Phi\User Data
Var DataDirText      ; the page's text field
Var DataDirChosen    ; 1 once the page ran (silent installs leave the record alone)

; ---------------------------------------------------------------- look
!define MUI_ICON "${PHI_STAGE}\resources\phi.ico"
!define MUI_UNICON "${PHI_STAGE}\resources\phi.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${PHI_ART}\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${PHI_ART}\welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${PHI_ART}\header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_ABORTWARNING
!define MUI_ABORTWARNING_TEXT "Stop installing Phi Browser?"

!define MUI_WELCOMEPAGE_TITLE "Welcome to Phi Browser"
!define MUI_WELCOMEPAGE_TEXT "Phi is a browser built around Spaces and agents.$\r$\n$\r$\nThis wizard installs Phi Browser ${PHI_VERSION}. You choose where the program goes and where your browsing data is kept.$\r$\n$\r$\nClick Next to continue."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Phi Browser will be installed in the folder below. To put it somewhere else, click Browse and choose another folder."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Program folder"
!define MUI_FINISHPAGE_TITLE "Phi Browser is installed"
!define MUI_FINISHPAGE_TEXT "Phi Browser ${PHI_VERSION} is ready.$\r$\n$\r$\nProgram: $INSTDIR$\r$\nBrowsing data: $DataDir"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchPhi
!define MUI_FINISHPAGE_RUN_TEXT "Launch Phi Browser"
!define MUI_FINISHPAGE_NOREBOOTSUPPORT

!define MUI_UNCONFIRMPAGE_TEXT_TOP "Phi Browser will be removed from $INSTDIR. You will be asked whether to keep your browsing data."

!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirLeave
!insertmacro MUI_PAGE_DIRECTORY
Page custom DataDirCreate DataDirLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- init
Function .onInit
  ; The payload's architecture against this PC's (the header's note on
  ; PHI_ARCH). ${IsNativeARM64} is the machine, whatever this 32-bit stub
  ; runs under.
!if "${PHI_ARCH}" == "arm64"
  ${IfNot} ${IsNativeARM64}
    MessageBox MB_OK|MB_ICONSTOP "This is Phi Browser for Windows on ARM, and this PC is not ARM-based.$\r$\n$\r$\nPlease use the standard installer, PhiBrowserSetup-${PHI_VERSION}.exe." /SD IDOK
    Abort
  ${EndIf}
!else
  ${If} ${IsNativeARM64}
    MessageBox MB_YESNO|MB_ICONEXCLAMATION "This PC is Windows on ARM. This installer carries the x64 build of Phi Browser, which runs here through emulation; the ARM64 build, PhiBrowserSetup-${PHI_VERSION}-arm64.exe, is the better fit.$\r$\n$\r$\nInstall the x64 build anyway?" /SD IDYES IDYES x64_on_arm_ok
    Abort
  x64_on_arm_ok:
  ${EndIf}
!endif
  StrCpy $DataDirDefault "$LOCALAPPDATA\Phi\User Data"
  ; A recorded choice from a previous install is the starting point.
  ReadRegStr $DataDir HKCU "${REG_PRODUCT}" "UserDataDir"
  ${If} $DataDir == ""
    ReadRegStr $DataDir HKLM "${REG_PRODUCT}" "UserDataDir"
  ${EndIf}
  ${If} $DataDir == ""
    StrCpy $DataDir $DataDirDefault
  ${EndIf}
  StrCpy $DataDirChosen 0
FunctionEnd

; The finish page's launch: through the shell, so Phi runs as the user, not
; with this installer's elevated token (an elevated browser would own the
; data folder's files).
Function LaunchPhi
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\Phi.exe"'
FunctionEnd

; A folder we cannot write is no place to install: probe before leaving the
; Program folder page.
Function DirLeave
  ClearErrors
  CreateDirectory "$INSTDIR"
  FileOpen $0 "$INSTDIR\.phi-write-probe" w
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONEXCLAMATION \
      "Phi Browser can't be installed in$\r$\n$INSTDIR$\r$\n$\r$\nChoose a folder you can write to."
    Abort
  ${EndIf}
  FileClose $0
  Delete "$INSTDIR\.phi-write-probe"
FunctionEnd

; ---------------------------------------------------------------- data page
Function DataDirCreate
  ${If} ${Silent}
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Browsing data" "Choose where Phi keeps what you do in it"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 40u "Your Spaces, history, bookmarks, settings and passwords live in a data folder separate from the program, so an update never touches them. The default is inside your local app data; pick another folder to keep them on a different drive."
  Pop $0
  ${NSD_CreateGroupBox} 0 50u 100% 36u "Data folder"
  Pop $0
  ${NSD_CreateText} 8u 65u 216u 13u "$DataDir"
  Pop $DataDirText
  ${NSD_CreateBrowseButton} 230u 64u 56u 15u "Browse..."
  Pop $0
  ${NSD_OnClick} $0 DataDirBrowse
  ${NSD_CreateLabel} 0 96u 100% 24u "Space is needed for the browser cache as well as your data. If you have used Phi on this computer before, keep the folder you used then and everything will still be there."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function DataDirBrowse
  ${NSD_GetText} $DataDirText $1
  nsDialogs::SelectFolderDialog "Choose the folder for Phi's browsing data" "$1"
  Pop $0
  ${If} $0 != error
    StrCpy $DataDir $0
    ${NSD_SetText} $DataDirText $DataDir
  ${EndIf}
FunctionEnd

Function DataDirLeave
  ${NSD_GetText} $DataDirText $DataDir
  ; Trim trailing separators - Chromium is unhappy with them.
  loop:
    StrCpy $0 $DataDir 1 -1
    ${If} $0 == "\"
    ${OrIf} $0 == "/"
      StrCpy $DataDir $DataDir -1
      Goto loop
    ${EndIf}
  StrCpy $0 $DataDir 2 1
  ${If} $DataDir == ""
  ${OrIf} $0 != ":\"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Enter a full folder path for the browsing data, like D:\Phi Data."
    Abort
  ${EndIf}
  ClearErrors
  CreateDirectory "$DataDir"
  FileOpen $0 "$DataDir\.phi-write-probe" w
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONEXCLAMATION \
      "Phi Browser can't write to$\r$\n$DataDir$\r$\n$\r$\nChoose a folder you can write to."
    Abort
  ${EndIf}
  FileClose $0
  Delete "$DataDir\.phi-write-probe"
  StrCpy $DataDirChosen 1
FunctionEnd

; ---------------------------------------------------------------- install
Section "Install"
  SetShellVarContext all

  ; The update path must not race a running Phi: chrome.dll being locked is
  ; the tell - it stays mapped for as long as any Phi process lives. The
  ; probe OPENS it for writing (a rename is no test: Windows lets a mapped
  ; image be renamed, only not overwritten). WinSparkle launches this
  ; installer and THEN asks Phi to quit, so a locked file is the normal
  ; first sight here - wait up to a minute for Chromium to unwind before
  ; asking the user, ask-and-retry rather than failing halfway through a
  ; copy. The chrome.dll a previous install keeps mapped is
  ; framework\chrome.dll (the ship layout) or chrome.dll beside the exe
  ; (the flat, component-era layout) - whichever is there.
  check_running:
    StrCpy $2 "$INSTDIR\framework\chrome.dll"
    IfFileExists $2 have_dll
    StrCpy $2 "$INSTDIR\chrome.dll"
    IfFileExists $2 have_dll fresh_install
  have_dll:
    StrCpy $1 0
  wait_running:
    ClearErrors
    FileOpen $0 $2 a
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

    ; Rollback snapshot: exactly ONE previous version's program files.
    RMDir /r "$INSTDIR\rollback"
    CreateDirectory "$INSTDIR\rollback"
    CopyFiles /SILENT "$INSTDIR\Phi.exe" "$INSTDIR\rollback"
    CopyFiles /SILENT "$INSTDIR\WinSparkle.dll" "$INSTDIR\rollback"
    CopyFiles /SILENT "$INSTDIR\framework" "$INSTDIR\rollback\framework"
    CopyFiles /SILENT "$INSTDIR\resources" "$INSTDIR\rollback\resources"

!ifdef PHI_LAYOUT_FRAMEWORK
    ; Upgrading a FLAT install: its 500-odd DLLs, paks and manifests sit
    ; beside the exe and nothing in the framework\ layout would ever
    ; remove them. Everything at the top level but the client's own
    ; resources goes; the stage puts back what belongs there.
    Delete "$INSTDIR\*.dll"
    Delete "$INSTDIR\*.pak"
    Delete "$INSTDIR\*.bin"
    Delete "$INSTDIR\*.dat"
    Delete "$INSTDIR\*.manifest"
    Delete "$INSTDIR\Phi.exe"
    Delete "$INSTDIR\chrome_proxy.exe"
    RMDir /r "$INSTDIR\locales"
    RMDir /r "$INSTDIR\MEIPreload"
    RMDir /r "$INSTDIR\Dictionaries"
    RMDir /r "$INSTDIR\resources\accessibility"
    RMDir /r "$INSTDIR\resources\inspector_overlay"
!endif

  fresh_install:
  SetOutPath "$INSTDIR"
  File /r "${PHI_STAGE}\*.*"

  ; The sandbox's AppContainer utilities (proxy resolver, media CDM, ...)
  ; run under the chromeInstallFiles / lpacChromeInstallFiles capabilities
  ; and must read and execute the install tree; a user folder grants them
  ; nothing by default and the launch then fails (sandbox_win.cc's
  ; AddAppContainerProfileToConfig access check). Chrome's setup grants the
  ; same two capability SIDs (installer/setup/configure_app_container_
  ; sandbox.cc).
  nsExec::ExecToLog 'icacls "$INSTDIR" /grant "*S-1-15-3-1024-3424233489-972189580-2057154623-747635277-1604371224-316187997-3786583170-1043257646:(OI)(CI)(RX)" "*S-1-15-3-1024-2302894289-466761758-1166120688-1039016420-2430351297-4240214049-4028510897-3317428798:(OI)(CI)(RX)"'

  CreateShortCut "$SMPROGRAMS\Phi Browser.lnk" "$INSTDIR\Phi.exe"
  CreateShortCut "$DESKTOP\Phi Browser.lnk" "$INSTDIR\Phi.exe"

  ; The program folder, for the next (silent) update and the uninstaller;
  ; the data folder only when the user chose one on the page - a silent
  ; update keeps whatever is recorded. The default is recorded as "no
  ; value" so a later move back to the default is a plain delete.
  WriteRegStr HKLM "${REG_PRODUCT}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REG_PRODUCT}" "InstallDir" "$INSTDIR"
  ${If} $DataDirChosen == 1
    ${If} $DataDir == $DataDirDefault
      DeleteRegValue HKLM "${REG_PRODUCT}" "UserDataDir"
      DeleteRegValue HKCU "${REG_PRODUCT}" "UserDataDir"
    ${Else}
      WriteRegStr HKLM "${REG_PRODUCT}" "UserDataDir" "$DataDir"
      WriteRegStr HKCU "${REG_PRODUCT}" "UserDataDir" "$DataDir"
    ${EndIf}
  ${EndIf}

!ifdef PHI_TELEMETRY_JSON
  ; Deployment configuration (docs/sentry-crash-reporting.md): the crash
  ; endpoint and the analytics key, read once at startup from the data
  ; folder. The installer OWNS this file - rewritten on every install so a
  ; rotated key or endpoint reaches the machine with the next update.
  ; Crash uploads also need the consent install_static reads
  ; (Software\Phi\usagestats = 1 - HKLM for a Program Files install, HKCU
  ; otherwise; both are written). Analytics stay behind the in-app switch.
  CreateDirectory "$DataDir"
  SetOutPath "$DataDir"
  File "/oname=phi_telemetry.json" "${PHI_TELEMETRY_JSON}"
  SetOutPath "$INSTDIR"
  WriteRegDWORD HKLM "${REG_PRODUCT}" "usagestats" 1
  WriteRegDWORD HKCU "${REG_PRODUCT}" "usagestats" 1
!endif

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ; Apps & Features: the 64-bit view, where a 64-bit program's entry
  ; belongs (this stub is 32-bit, so the default view would be WOW6432Node).
  ; Software\Phi above stays in the 32-bit view - that is where
  ; install_static reads it (nt::WOW6432).
  SetRegView 64
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Phi Browser"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${PHI_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Fyde Innovations"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\Phi.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  SetRegView 32
SectionEnd

; ---------------------------------------------------------------- uninstall
Function un.onInit
  StrCpy $DataDirDefault "$LOCALAPPDATA\Phi\User Data"
  ReadRegStr $DataDir HKCU "${REG_PRODUCT}" "UserDataDir"
  ${If} $DataDir == ""
    ReadRegStr $DataDir HKLM "${REG_PRODUCT}" "UserDataDir"
  ${EndIf}
  ${If} $DataDir == ""
    StrCpy $DataDir $DataDirDefault
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetShellVarContext all

  ; Only what this installer put there - never a blind RMDir /r of a folder
  ; the user chose.
  Delete "$INSTDIR\Phi.exe"
  Delete "$INSTDIR\WinSparkle.dll"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR\framework"
  RMDir /r "$INSTDIR\resources"
  RMDir /r "$INSTDIR\rollback"
  ; A flat (component-era) install's files, should one be uninstalled by
  ; this uninstaller.
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\*.pak"
  Delete "$INSTDIR\*.bin"
  Delete "$INSTDIR\*.dat"
  Delete "$INSTDIR\*.manifest"
  Delete "$INSTDIR\*.exe"
  RMDir /r "$INSTDIR\locales"
  RMDir /r "$INSTDIR\MEIPreload"
  RMDir /r "$INSTDIR\Dictionaries"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\Phi Browser.lnk"
  Delete "$DESKTOP\Phi Browser.lnk"

  ; The browsing data is the user's: ask, and only when it looks like a
  ; Phi data folder. A silent uninstall keeps it.
  ${IfNot} ${Silent}
    ${If} ${FileExists} "$DataDir\Local State"
    ${OrIf} ${FileExists} "$DataDir\PhiSettings.json"
      MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 \
        "Also delete your browsing data?$\r$\n$\r$\n$DataDir$\r$\n$\r$\nThis removes your Spaces, history, bookmarks, settings and saved passwords. Choose No to keep them for a later install." \
        IDNO keep_data
      RMDir /r "$DataDir"
      keep_data:
    ${EndIf}
  ${EndIf}

  SetRegView 64
  DeleteRegKey HKLM "${UNINST_KEY}"
  SetRegView 32
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "${REG_PRODUCT}"
  DeleteRegKey HKCU "${REG_PRODUCT}"
  ; The updater's own state (last check time, skipped version).
  DeleteRegKey HKCU "Software\Fyde Innovations\Phi Browser\WinSparkle"
SectionEnd
