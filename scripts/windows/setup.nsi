Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "Sections.nsh"
!include "WinVer.nsh"

; SafeHouse was called ProjectCane until 2026-11-03. Setup uses new registry keys and a new default folder, so an old
; ProjectCane installation stays as it is (uninstall it from Windows Settings; its uninstaller keeps AppData). The
; games' data folder is %LOCALAPPDATA%\SafeHouse since 2026-11-10: the launcher moves an old %LOCALAPPDATA%\ProjectCane
; there at its first start (platform/user_folders.h), so installed games and saves carry over. Settings > Games
; "Install games to" puts new installations on another drive.
;
; Kept deliberately small, so that the setup does nothing a malware scanner's behaviour report would list beyond
; installing files:
;   - per user, never elevated (RequestExecutionLevel user; the manifest NSIS writes says asInvoker), into
;     %LOCALAPPDATA%\Programs\SafeHouse
;   - no plugin DLLs at all (no System / nsDialogs / nsExec: nothing is unpacked into %TEMP% and loaded), so no
;     Welcome / Finish pages (nsDialogs) and no x64.nsh (System); the OS checks are WinVer.nsh (built in) and the
;     environment
;   - one registry key: the per-user Apps & features entry (HKCU ...\Uninstall\SafeHouse); no file associations, shell
;     extensions, startup entries, services or scheduled tasks
;   - standard LZMA compression of NSIS (no executable packer such as UPX)
;   - the uninstaller removes exactly the files setup installed, its shortcuts and that key (and, from installations
;     before 2026-11-17, HKCU Software\SafeHouse\Launcher); AppData (games, saves, settings) is never touched
;   - Authenticode: scripts/windows/sign.py signs the uninstaller and the setup when a certificate is configured
!define APP_NAME "SafeHouse"
!ifndef ARCH
  !define ARCH "x86_64"
!endif
; (written by setups before 2026-11-17; only removed by the uninstaller now)
!define LEGACY_APP_KEY "Software\SafeHouse\Launcher"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SafeHouse"

Name "${APP_NAME}"
OutFile "${OUTPUT_EXE}"
InstallDir "$LOCALAPPDATA\Programs\SafeHouse"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
SetOverwrite on
CRCCheck force
ManifestDPIAware true
ManifestSupportedOS Win10
VIProductVersion "${VERSION}.0"
VIFileVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "CompanyName" "SafeHouse contributors"
VIAddVersionKey /LANG=1033 "ProductName" "SafeHouse"
VIAddVersionKey /LANG=1033 "FileDescription" "SafeHouse Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "InternalName" "SafeHouse-Setup"
VIAddVersionKey /LANG=1033 "OriginalFilename" "SafeHouse-Launcher-Windows-${ARCH}-Setup.exe"
VIAddVersionKey /LANG=1033 "LegalCopyright" "SafeHouse contributors"
; Authenticode (scripts/windows/sign.py; a no-op without a certificate): the uninstaller, then the setup
!ifdef SIGN_COMMAND
  !uninstfinalize '${SIGN_COMMAND} "%1"' = 0
  !finalize '${SIGN_COMMAND} "%1"' = 0
!endif

!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_TEXT_TOP "Install or update the SafeHouse launcher for your Windows account (no administrator rights needed). Saves, settings and installed games are kept in AppData; updating or uninstalling the launcher keeps them. You need your own Sly Cooper disc image (ISO) to install each game."
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Parameters
Var OptionValue
Var HadDesktopShortcut


Function .onVerifyInstDir
  IfFileExists "$INSTDIR\portable.txt" 0 done
  MessageBox MB_OK|MB_ICONSTOP "Choose a separate folder for the installed launcher. This folder contains a portable copy." /SD IDOK
  Abort
  done:
FunctionEnd

Section "Launcher and Start Menu shortcut" LauncherFiles
  SectionIn RO
  ; Also guard silent installs, where the directory page is skipped.
  IfFileExists "$INSTDIR\portable.txt" 0 +4
    MessageBox MB_OK|MB_ICONSTOP "Setup cannot install into a portable copy." /SD IDOK
    SetErrorLevel 2
    Quit
  ; Check all executable files before changing the installed application.
  !include "${CHECK_LOCKS}"
  SetOutPath "$INSTDIR"
  !include "${PAYLOAD_FILES}"
  IfErrors install_failed
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\SafeHouse"
  SetOutPath "$INSTDIR\bin"
  CreateShortcut "$SMPROGRAMS\SafeHouse\SafeHouse.lnk" "$INSTDIR\bin\safehouse.exe" "" "$INSTDIR\bin\safehouse.exe" 0
  CreateShortcut "$SMPROGRAMS\SafeHouse\Uninstall SafeHouse.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "SafeHouse"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "SafeHouse contributors"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\bin\safehouse.exe,0"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  Goto installed
  install_failed:
    MessageBox MB_OK|MB_ICONSTOP "Installation failed. Close SafeHouse and check that the installation folder is writable, then run setup again." /SD IDOK
    SetErrorLevel 2
    Quit
  installed:
SectionEnd

Section /o "Desktop shortcut" DesktopShortcut
  CreateShortcut "$DESKTOP\SafeHouse.lnk" "$INSTDIR\bin\safehouse.exe" "" "$INSTDIR\bin\safehouse.exe" 0
SectionEnd

Section -DesktopShortcutRemoved
  ; (only our own shortcut, and only when it was there and the player unticked it)
  ${IfNot} ${SectionIsSelected} ${DesktopShortcut}
  ${AndIf} $HadDesktopShortcut = 1
    Delete "$DESKTOP\SafeHouse.lnk"
  ${EndIf}
SectionEnd

; Instead of a Finish page (it needs the nsDialogs plugin): offer to start SafeHouse. Silent installs (/S) do not.
Function .onInstSuccess
  MessageBox MB_YESNO|MB_ICONQUESTION "SafeHouse is installed. Start it now?" /SD IDNO IDNO done
    Exec '"$INSTDIR\bin\safehouse.exe"'
  done:
FunctionEnd

Function .onInit
  ; The PC's processor without the System plugin: setup is a 32-bit program, so Windows tells a 64-bit PC's
  ; architecture in PROCESSOR_ARCHITEW6432 (AMD64 or ARM64); it is empty on 32-bit Windows.
  ReadEnvStr $0 PROCESSOR_ARCHITEW6432
  ${If} $0 == ""
    ReadEnvStr $0 PROCESSOR_ARCHITECTURE
  ${EndIf}
!if "${ARCH}" == "arm64"
  ${If} $0 != "ARM64"
    MessageBox MB_OK|MB_ICONSTOP "This SafeHouse setup is for Windows on ARM PCs. Download SafeHouse-Launcher-Windows-x86_64-Setup.exe for this PC." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
!else
  ; (Windows 11 on ARM runs x64 programs too)
  ${If} $0 != "AMD64"
  ${AndIf} $0 != "ARM64"
    MessageBox MB_OK|MB_ICONSTOP "SafeHouse requires 64-bit Windows 10 or later." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
!endif
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "SafeHouse requires Windows 10 or later." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
  ; An update goes into the folder of the existing installation (its Apps & features entry; older setups also kept it
  ; in the legacy key).
  ${If} $INSTDIR == "$LOCALAPPDATA\Programs\SafeHouse"
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $0 == ""
      ReadRegStr $0 HKCU "${LEGACY_APP_KEY}" "InstallLocation"
    ${EndIf}
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
  ; The desktop shortcut stays ticked when the installation has one.
  StrCpy $HadDesktopShortcut 0
  ${If} ${FileExists} "$DESKTOP\SafeHouse.lnk"
    StrCpy $HadDesktopShortcut 1
    !insertmacro SelectSection ${DesktopShortcut}
  ${EndIf}
  ${GetParameters} $Parameters
  ClearErrors
  ${GetOptions} $Parameters "/DESKTOP" $OptionValue
  ${IfNot} ${Errors}
    !insertmacro SelectSection ${DesktopShortcut}
  ${EndIf}
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${LauncherFiles} "Install the launcher, game runtime and extractor, and add SafeHouse to the Start Menu."
  !insertmacro MUI_DESCRIPTION_TEXT ${DesktopShortcut} "Also add a SafeHouse shortcut to your desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Uninstall"
  ; Delete only files shipped by setup. Preserve all AppData game/user files.
  !include "${CHECK_LOCKS}"
  !include "${UNINSTALL_FILES}"
  ; What the launcher's updater leaves between an update and the next start (launcher/update.h).
  RMDir /r "$INSTDIR\.update"
  Delete "$DESKTOP\SafeHouse.lnk"
  Delete "$SMPROGRAMS\SafeHouse\SafeHouse.lnk"
  Delete "$SMPROGRAMS\SafeHouse\Uninstall SafeHouse.lnk"
  RMDir "$SMPROGRAMS\SafeHouse"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${LEGACY_APP_KEY}"
  DeleteRegKey /ifempty HKCU "Software\SafeHouse"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
