; FilePathX — NSIS installer script
;
; Build with make-installer.bat (it reads the version out of src/version.h and
; passes it in):
;     makensis /DAPP_VERSION=0.7.10 /DAPP_VERSION4=0.7.10.0 installer.nsi
;
; Per-user install (no UAC): %LOCALAPPDATA%\Programs\FilePathX. The in-app
; auto-updater replaces FilePathX.exe in place, so it keeps working from here.
; User settings live in %APPDATA%\filepathx and are left alone on uninstall.

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef APP_VERSION
  !define APP_VERSION "0.0.0"
!endif
!ifndef APP_VERSION4
  !define APP_VERSION4 "0.0.0.0"
!endif

!define APP_NAME      "FilePathX"
!define APP_EXE       "FilePathX.exe"
!define APP_PUBLISHER "goldalworming"
!define APP_URL       "https://github.com/goldalworming/filepathx"
!define APP_KEY       "Software\FilePathX"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\FilePathX"
!define STAGE         "build\stage"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "build\${APP_NAME}-${APP_VERSION}-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma

VIProductVersion "${APP_VERSION4}"
VIAddVersionKey "ProductName"     "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} setup"
VIAddVersionKey "FileVersion"     "${APP_VERSION}"
VIAddVersionKey "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "MIT License"

!define MUI_ICON   "icon.ico"
!define MUI_UNICON "icon.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN      "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- sections

Section "FilePathX (required)" SecMain
  SectionIn RO
  SetShellVarContext current

  ; An auto-update can only replace FilePathX.exe once the old copy exits, so
  ; close it first — but only when the copy we are about to overwrite is the
  ; one running. FileOpen fails while the image is mapped.
  ClearErrors
  FileOpen $9 "$INSTDIR\${APP_EXE}" a
  IfErrors 0 close_done
    DetailPrint "Closing running ${APP_NAME}…"
    nsExec::ExecToLog 'taskkill /IM "${APP_EXE}" /F'
    Sleep 1000
  close_done:
  ClearErrors
  FileClose $9

  SetOutPath "$INSTDIR"
  File "${STAGE}\${APP_EXE}"
  File "${STAGE}\README.md"
  File "${STAGE}\LICENSE"
  SetOutPath "$INSTDIR\themes"
  File "${STAGE}\themes\*.ini"
  File "${STAGE}\themes\README.md"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_KEY}" "Version"    "${APP_VERSION}"

  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr   HKCU "${UNINST_KEY}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr   HKCU "${UNINST_KEY}" "URLInfoAbout"    "${APP_URL}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
  WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Start Menu shortcut" SecStartMenu
  SetShellVarContext current
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  SetShellVarContext current
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  ; Settings in %APPDATA%\filepathx deliberately stay behind.
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\themes"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
SectionEnd

; ------------------------------------------------------------ descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}      "The application itself and its bundled themes."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Shortcut in the Start Menu."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop}   "Shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END
