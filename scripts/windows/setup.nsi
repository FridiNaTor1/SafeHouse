Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "Sections.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

; SafeHouse was called ProjectCane until 2026-11-03. Setup uses new registry keys and a new default folder, so an old
; ProjectCane installation stays as it is (uninstall it from Windows Settings; its uninstaller keeps AppData). The
; games' data folder is still %LOCALAPPDATA%\ProjectCane, shared by both, so installed games and saves carry over.
!define APP_NAME "SafeHouse"
!define APP_KEY "Software\SafeHouse\Launcher"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SafeHouse"

Name "${APP_NAME}"
OutFile "${OUTPUT_EXE}"
InstallDir "$LOCALAPPDATA\Programs\SafeHouse"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
SetOverwrite on
CRCCheck force
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "SafeHouse"
VIAddVersionKey /LANG=1033 "FileDescription" "SafeHouse Offline Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "SafeHouse contributors"

!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install SafeHouse"
!define MUI_WELCOMEPAGE_TEXT "Install or update the SafeHouse launcher for your Windows account.$\r$\n$\r$\nSaves, settings and extracted game files are kept in AppData. Updates and uninstalling the launcher preserve them.$\r$\n$\r$\nYou will need your own Sly Cooper disc image (ISO) to install each game."
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\safehouse.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var SetupMutex
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
  WriteRegStr HKCU "${APP_KEY}" "InstallLocation" "$INSTDIR"
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

Section -RememberOptions
  ${If} ${SectionIsSelected} ${DesktopShortcut}
    WriteRegDWORD HKCU "${APP_KEY}" "DesktopShortcut" 1
  ${Else}
    Delete "$DESKTOP\SafeHouse.lnk"
    WriteRegDWORD HKCU "${APP_KEY}" "DesktopShortcut" 0
  ${EndIf}
SectionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "SafeHouse requires 64-bit Windows 10 or later." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "SafeHouse requires Windows 10 or later." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\SafeHouseSetup") p .r0 ?e'
  Pop $1
  StrCpy $SetupMutex $0
  ${If} $1 = 183
    MessageBox MB_OK|MB_ICONSTOP "Another SafeHouse setup is already running." /SD IDOK
    SetErrorLevel 2
    Quit
  ${EndIf}
  ${If} $INSTDIR == "$LOCALAPPDATA\Programs\SafeHouse"
    ReadRegStr $0 HKCU "${APP_KEY}" "InstallLocation"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
  ReadRegDWORD $HadDesktopShortcut HKCU "${APP_KEY}" "DesktopShortcut"
  ${If} $HadDesktopShortcut = 1
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
  DeleteRegKey HKCU "${APP_KEY}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
