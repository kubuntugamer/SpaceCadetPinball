; Space Cadet Pinball - Windows installer (NSIS 3.x).
;
; Packages what the Windows build produced in bin\ into a single setup
; executable: the game, its runtime DLLs, the original game data and, when
; present, a General MIDI SoundFont.
;
;   cd Platform\Windows
;   makensis /DAPPVERSION="$(cat ../../VERSION)" installer.nsi
;
; build-installer.sh does that for you; without /DAPPVERSION the version
; falls back to 0.0.0 below. The release number lives in the VERSION file at
; the repository root (single source of truth).
;
; On Linux the same script compiles through Wine - see build-installer.sh and
; BUILD.md in this folder.
;
; Everything the installer writes is per user (no administrator rights, no UAC
; prompt) and is listed in the Uninstall section below.  /S /D=<dir> installs
; silently into <dir>, which also works as "installer for a portable copy".

Unicode true

!include "MUI2.nsh"

!define APPNAME "Space Cadet Pinball"
; Version comes from the VERSION file at the repo root (single source of
; truth); build-installer.sh passes it with /DAPPVERSION=.
!ifndef APPVERSION
  !define APPVERSION "0.0.0"
!endif
!define EXENAME "SpaceCadetPinball.exe"
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SpaceCadetPinball"

; Source root, relative to this script's folder (the repository root).
!ifndef SRCDIR
  !define SRCDIR "..\.."
!endif
; Size in KB for the Add/Remove Programs entry; build-installer.sh fills this in.
!ifndef DATASIZE
  !define DATASIZE 0
!endif

Name "${APPNAME} ${APPVERSION}"
OutFile "${SRCDIR}\bin\SpaceCadetPinball-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\SpaceCadetPinball"
InstallDirRegKey HKCU "${REGKEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
BrandingText "${APPNAME} ${APPVERSION}"

; Windows requires a four-component numeric file version.
VIProductVersion "${APPVERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${APPNAME}"
VIAddVersionKey /LANG=1033 "FileDescription" "${APPNAME} setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${APPVERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APPVERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Code: MIT. Game data: Microsoft. SoundFont: see its upstream project."

!define MUI_ICON "${SRCDIR}\SpaceCadetPinball\Icon_1.ico"
!define MUI_UNICON "${SRCDIR}\SpaceCadetPinball\Icon_1.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Play ${APPNAME}"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the readme (keys, requirements, troubleshooting)"
!define MUI_FINISHPAGE_LINK "A GPU driver with Vulkan is required; get the Vulkan Runtime if yours has none"
!define MUI_FINISHPAGE_LINK_LOCATION "https://vulkan.lunarg.com/sdk/home"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

LangString DESC_Game ${LANG_ENGLISH} "The game, its runtime DLLs and the game data (about 2 MB)."
LangString DESC_Music ${LANG_ENGLISH} "A General MIDI SoundFont for background music, if one was placed in bin\soundfonts (about 140 MB)."
LangString DESC_Desktop ${LANG_ENGLISH} "Add a shortcut on the desktop."

Section "!${APPNAME}" SEC_GAME
  SectionIn RO
  SetOutPath "$INSTDIR"

  File "${SRCDIR}\bin\${EXENAME}"
  ; Runtime dependencies of the build (FluidSynth and its codec library, the
  ; Vulkan loader, ...).  Whatever the build copied next to the executable.
  File /nonfatal "${SRCDIR}\bin\*.dll"
  ; Original game data, provided by the user - not part of this repository.
  File "${SRCDIR}\bin\*.DAT"
  File /nonfatal "${SRCDIR}\bin\SOUND*.WAV"
  File /nonfatal "${SRCDIR}\bin\*.MID"
  File "/oname=README.txt" "${SRCDIR}\Platform\Windows\PLAYER-README.txt"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\${EXENAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\README.lnk" "$INSTDIR\README.txt"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "${REGKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${REGKEY}" "DisplayVersion" "${APPVERSION}"
  WriteRegStr HKCU "${REGKEY}" "Publisher" "SpaceCadetPinball contributors"
  WriteRegStr HKCU "${REGKEY}" "DisplayIcon" "$INSTDIR\${EXENAME}"
  WriteRegStr HKCU "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${REGKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${REGKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${REGKEY}" "EstimatedSize" ${DATASIZE}
SectionEnd

; Optional share of the download: without a SoundFont the game plays normally,
; just without music.
Section "Background music (SoundFont)" SEC_MUSIC
  SetOutPath "$INSTDIR\soundfonts"
  File /nonfatal "${SRCDIR}\bin\soundfonts\*.sf2"
  File /nonfatal "${SRCDIR}\bin\soundfonts\*.sf3"
  SetOutPath "$INSTDIR"
SectionEnd

Section "Desktop shortcut" SEC_DESKTOP
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\${EXENAME}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_GAME} $(DESC_Game)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_MUSIC} $(DESC_Music)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} $(DESC_Desktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\README.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"

  ; Only the files this installer created, so anything the user placed in the
  ; install folder survives an uninstall.
  Delete "$INSTDIR\${EXENAME}"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\*.DAT"
  Delete "$INSTDIR\SOUND*.WAV"
  Delete "$INSTDIR\*.MID"
  Delete "$INSTDIR\README.txt"
  RMDir /r "$INSTDIR\soundfonts"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"   ; leaves a folder that still holds other files in place

  DeleteRegKey HKCU "${REGKEY}"

  ; Options and high scores in %APPDATA%\SpaceCadetPinball are kept on purpose.
SectionEnd
