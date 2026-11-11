; OpenMM2 Windows installer (NSIS 3, Unicode, Modern UI 2).
;
; Normally built by the `package_installer` CMake target (cmake/Packaging.cmake),
; which stages the install tree and runs makensis with:
;   -DVERSION=<x.y.z>            product version
;   -DSTAGE_DIR=<dir>            staged "app" component (openmm2.exe, DLLs, licences)
;   -DAPP_FILES_NSH=<file>       generated File/Delete/manifest lists for STAGE_DIR
;   -DTOOLS_FILES_NSH=<file>     (optional) same for the "tools" component
;   -DOUTFILE=<file>             installer to write
;   -DSOURCE_DIR=<dir>           repository root (licence, icons)
; packaging/windows/test-installer.sh builds it the same way and tests
; installs, updates and removal under Wine.
;
; Command line options, in addition to NSIS's /S (silent) and /D=<dir>:
;   /SOURCE=<path>     game data for silent installs: a disc image, a drive
;                      root such as D:\ or a Midtown Madness 2 installation folder
;   /COPYDATA          copy the game archives to <install dir>\gamedata
;   /ALLOWDOWNGRADE    replace a newer installed version without asking
; Exit codes:
;   0  success
;   1  cancelled or failed (e.g. a file that another program holds open)
;   2  installed, but the game data given with /SOURCE was rejected (OpenMM2
;      will ask for it on first start)
;   3  a newer version is installed and a silent install was not given
;      /ALLOWDOWNGRADE; nothing was changed
;   4  OpenMM2 is running and a silent install cannot ask to close it;
;      nothing was changed. The uninstaller stops the same way (its exit
;      code is only seen with "uninstall.exe /S _?=<dir>": otherwise it
;      restarts from a copy in %TEMP% and returns at once).
;
; Updates. The registry (HKLM\Software\OpenMM2: InstallDir, Version) names
; the installed version; setup then updates it in its folder: the directory
; page is skipped, the welcome page says what will happen, and the game data
; page and the components are pre-filled from the last installation. The same
; version again repairs; an older one asks first (/ALLOWDOWNGRADE when
; silent). Before copying anything, setup removes the previous version's files
; (RemovePreviousVersion): those in its install-manifest.txt, or for 0.4.1 and
; earlier, which wrote none, the names those versions installed
; (LegacyFiles.nsh). It never removes gamedata\, openmm2-install.ini or the
; per-user settings. /D= naming another folder moves the installation: the old
; folder's program files, uninstaller and firewall rule are removed, its
; gamedata\ and openmm2-install.ini stay, and the folder goes only if nothing
; else is left in it.
;
; $INSTDIR\install-manifest.txt lists every file setup installed, one path
; per line relative to $INSTDIR (ASCII, CRLF, ";" comment lines). The next
; update and the uninstaller remove exactly those files.
;
; Contract with openmm2.exe:
;   openmm2.exe --check-source <path>          exit 0 when usable; prints a
;                                              one-line reason otherwise
;   openmm2.exe --import-source <path> <dir>   copies the archives, printing
;                                              "progress <0-100>" lines; on
;                                              failure it removes its partial
;                                              (.part) files itself
; and the installer writes $INSTDIR\openmm2-install.ini (UTF-8):
;   [GameData]
;   Source=<path>
; which OpenMM2 uses when the per-user configuration names no game data.

Unicode true
ManifestDPIAware true
ManifestSupportedOS all
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!ifndef VERSION
    !error "Pass -DVERSION=<x.y.z>"
!endif
!ifndef STAGE_DIR
    !error "Pass -DSTAGE_DIR=<staged app component>"
!endif
!ifndef APP_FILES_NSH
    !error "Pass -DAPP_FILES_NSH=<generated file list>"
!endif
!ifndef SOURCE_DIR
    !error "Pass -DSOURCE_DIR=<repository root>"
!endif
!ifndef OUTFILE
    !define OUTFILE "OpenMM2-${VERSION}-windows-x64-setup.exe"
!endif

; -DPRODUCT (cmake/Packaging.cmake's OPENMM2_PRODUCT_NAME): a fork's build
; installs beside OpenMM2 under its own name, folder and registry keys.
!ifndef PRODUCT
    !define PRODUCT "OpenMM2"
!endif
!define PUBLISHER "OpenMM2 contributors"
!ifndef HOMEPAGE
    !define HOMEPAGE "https://github.com/lowlevelmetal/OpenMM2"
!endif
!define REG_KEY "Software\${PRODUCT}"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
!define ICONS "${SOURCE_DIR}\packaging\icons"

!define EXIT_FAILED 1
!define EXIT_SOURCE_REJECTED 2
!define EXIT_NEWER_INSTALLED 3
!define EXIT_RUNNING 4

!include MUI2.nsh
!include LogicLib.nsh
!include Sections.nsh
!include FileFunc.nsh
!include TextFunc.nsh
!include WordFunc.nsh
!include x64.nsh
!include WinVer.nsh
!include "${APP_FILES_NSH}"
!ifdef TOOLS_FILES_NSH
    !include "${TOOLS_FILES_NSH}"
!endif
!include "${__FILEDIR__}\Helpers.nsh"
!include "${__FILEDIR__}\LegacyFiles.nsh"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${PRODUCT}"
BrandingText "${PRODUCT} ${VERSION}"
ShowInstDetails show
ShowUninstDetails show
; A file that cannot be written (another program holds it) stops the
; installation (Retry/Cancel; a silent install fails with exit code 1) instead
; of being skipped, which would leave a file of the old version behind.
AllowSkipFiles off

; Version resource: VIProductVersion needs four numeric parts.
!searchparse /noerrors "${VERSION}" "" _V_MAJOR "." _V_MINOR "." _V_PATCH
!ifndef _V_PATCH
    !define _V_PATCH 0
!endif
VIProductVersion "${_V_MAJOR}.${_V_MINOR}.${_V_PATCH}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} Setup"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "GNU General Public License v3 or later"

Var GameSource   ; chosen game data location, "" to configure later
Var CopyData     ; 1 to copy the archives into $INSTDIR\gamedata
Var SourceGiven  ; 1 when /SOURCE= named the game data
Var DefaultInstDir
Var OldInstDir   ; folder of the installed OpenMM2, "" when there is none
Var OldVersion   ; its version, "" if unknown
Var InstallKind  ; new | update | repair | downgrade
Var WelcomeText

; --- Modern UI -------------------------------------------------------------
!define MUI_ICON "${ICONS}\openmm2.ico"
!define MUI_UNICON "${ICONS}\openmm2.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${ICONS}\installer-sidebar.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${ICONS}\installer-sidebar.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "${ICONS}\installer-header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "${ICONS}\installer-header.bmp"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC

; $WelcomeText is set in .onInit: a new installation, an update, a repair or
; a downgrade.
!define MUI_WELCOMEPAGE_TEXT "$WelcomeText$\r$\n$\r$\n$_CLICK"
!insertmacro MUI_PAGE_WELCOME

; An update shows only the pages with choices: welcome, components, game data.
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipPageOnUpdate
!define MUI_LICENSEPAGE_TEXT_TOP "${PRODUCT} is free software, licensed under the GNU General Public License."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "You do not need to accept this licence to use ${PRODUCT}; it describes your rights to copy, modify and share it. Click Next to continue."
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"

!insertmacro MUI_PAGE_COMPONENTS
Page custom GameDataPageCreate GameDataPageLeave
; An update installs into the existing folder (or the one /D= names).
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipPageOnUpdate
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start ${PRODUCT}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchUnelevated
!define MUI_FINISHPAGE_LINK "${PRODUCT} on GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "${HOMEPAGE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; --- Sections --------------------------------------------------------------

Section "${PRODUCT} (required)" SecGame
    SectionIn RO
    Call CheckNotRunning
    Call RemovePreviousVersion
    SetOutPath "$INSTDIR"
    ; The uninstaller and the manifest first, so that an installation that
    ; stops halfway can still be repaired or removed.
    WriteUninstaller "$INSTDIR\uninstall.exe"
    Call WriteManifest
    !insertmacro OPENMM2_APP_INSTALL
    SetOutPath "$INSTDIR"

    ; Let the game accept multiplayer connections and LAN discovery (UDP; see
    ; docs/multiplayer.md). Scoped to the executable, not to fixed ports, so a
    ; changed port in the settings keeps working. Failure (e.g. a third-party
    ; firewall) is not fatal: Windows will prompt on first host instead.
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${PRODUCT}" program="$INSTDIR\openmm2.exe"'
    Pop $0
    nsExec::ExecToLog 'netsh advfirewall firewall add rule name="${PRODUCT}" dir=in action=allow protocol=UDP program="$INSTDIR\openmm2.exe" enable=yes profile=any'
    Pop $0
SectionEnd

Section "Start menu shortcut" SecStartMenu
    CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\openmm2.exe" "" "$INSTDIR\openmm2.exe" 0
SectionEnd

Section "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\openmm2.exe" "" "$INSTDIR\openmm2.exe" 0
SectionEnd

!ifdef TOOLS_FILES_NSH
Section /o "Developer tools (mm2tool)" SecTools
    !insertmacro OPENMM2_TOOLS_INSTALL
SectionEnd
!endif

Section "-Game data" SecGameData
    Call InstallGameData
SectionEnd

Section "-Register"
    SetOutPath "$INSTDIR"
    WriteRegStr HKLM "${REG_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${REG_KEY}" "Version" "${VERSION}"

    ; Remember the optional parts for the next update (see .onInit), and take
    ; away a shortcut that was switched off.
    ${If} ${SectionIsSelected} ${SecStartMenu}
        WriteRegDWORD HKLM "${REG_KEY}" "StartMenuShortcut" 1
    ${Else}
        WriteRegDWORD HKLM "${REG_KEY}" "StartMenuShortcut" 0
        Delete "$SMPROGRAMS\${PRODUCT}.lnk"
    ${EndIf}
    ${If} ${SectionIsSelected} ${SecDesktop}
        WriteRegDWORD HKLM "${REG_KEY}" "DesktopShortcut" 1
    ${Else}
        WriteRegDWORD HKLM "${REG_KEY}" "DesktopShortcut" 0
        Delete "$DESKTOP\${PRODUCT}.lnk"
    ${EndIf}
!ifdef TOOLS_FILES_NSH
    ${If} ${SectionIsSelected} ${SecTools}
        WriteRegDWORD HKLM "${REG_KEY}" "Tools" 1
    ${Else}
        WriteRegDWORD HKLM "${REG_KEY}" "Tools" 0
    ${EndIf}
!endif

    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\openmm2.exe,0"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "${HOMEPAGE}"
    WriteRegStr HKLM "${UNINST_KEY}" "HelpLink" "${HOMEPAGE}/issues"
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecGame} "The ${PRODUCT} program."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Add ${PRODUCT} to the Start menu."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Add a ${PRODUCT} shortcut to the desktop."
!ifdef TOOLS_FILES_NSH
    !insertmacro MUI_DESCRIPTION_TEXT ${SecTools} "mm2tool, a command line tool to inspect and extract the original game files (for modders and developers)."
!endif
!insertmacro MUI_FUNCTION_DESCRIPTION_END

!include "${__FILEDIR__}\GameDataPage.nsh"

; --- Installer functions ---------------------------------------------------

; Selects an optional section as the last installation had it: the choice
; recorded in the registry, or for 0.4.1 and earlier, which recorded none,
; whether its file (a shortcut, mm2tool.exe) is there.
!macro OPENMM2_PRESELECT SECTION VALUE FILE
    ClearErrors
    ReadRegDWORD $R1 HKLM "${REG_KEY}" "${VALUE}"
    ${If} ${Errors}
        StrCpy $R1 0
        ${If} ${FileExists} "${FILE}"
            StrCpy $R1 1
        ${EndIf}
    ${EndIf}
    ${If} $R1 == 1
        !insertmacro SelectSection ${SECTION}
    ${Else}
        !insertmacro UnselectSection ${SECTION}
    ${EndIf}
!macroend

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP|MB_OK "${PRODUCT} requires a 64-bit version of Windows." /SD IDOK
        SetErrorLevel ${EXIT_FAILED}
        Abort
    ${EndIf}
    ${IfNot} ${AtLeastWin7}
        MessageBox MB_ICONSTOP|MB_OK "${PRODUCT} requires Windows 7 or later." /SD IDOK
        SetErrorLevel ${EXIT_FAILED}
        Abort
    ${EndIf}
    ; Never show "insert a disk" error boxes while probing optical drives.
    System::Call 'kernel32::SetErrorMode(i 1)'
    SetRegView 64
    SetShellVarContext all
    ${GetParameters} $R0

    ; The installed version. Install into its folder unless /D= chose another
    ; directory (NSIS has set $INSTDIR from /D= already).
    ReadRegStr $OldInstDir HKLM "${REG_KEY}" "InstallDir"
    ReadRegStr $OldVersion HKLM "${REG_KEY}" "Version"
    StrCpy $DefaultInstDir "$PROGRAMFILES64\${PRODUCT}"
    ${If} $OldInstDir != ""
    ${AndIf} $INSTDIR == $DefaultInstDir
        StrCpy $INSTDIR $OldInstDir
    ${EndIf}
    ; A registration whose folder is gone is a new installation.
    ${If} $OldInstDir == ""
    ${OrIfNot} ${FileExists} "$OldInstDir\*.*"
        StrCpy $OldInstDir ""
        StrCpy $OldVersion ""
    ${EndIf}

    StrCpy $InstallKind "new"
    ${If} $OldInstDir != ""
        StrCpy $InstallKind "update"
        ${If} $OldVersion != ""
            ${VersionCompare} "${VERSION}" "$OldVersion" $R1
            ${If} $R1 == 0
                StrCpy $InstallKind "repair"
            ${ElseIf} $R1 == 2
                StrCpy $InstallKind "downgrade"
            ${EndIf}
        ${EndIf}
    ${EndIf}

    ${If} $InstallKind == "downgrade"
        ClearErrors
        ${GetOptions} $R0 "/ALLOWDOWNGRADE" $R1
        ${If} ${Errors}
            MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "${PRODUCT} $OldVersion is installed, which is newer than this setup (${VERSION}).$\r$\n$\r$\nReplace it with the older version?" /SD IDNO IDYES downgrade_confirmed
            ${If} ${Silent}
                SetErrorLevel ${EXIT_NEWER_INSTALLED}
            ${Else}
                SetErrorLevel ${EXIT_FAILED}
            ${EndIf}
            Abort
        downgrade_confirmed:
        ${EndIf}
    ${EndIf}

    ${Select} $InstallKind
    ${Case} "update"
        ${If} $OldVersion == ""
            StrCpy $WelcomeText "This will update ${PRODUCT} to ${VERSION}."
        ${Else}
            StrCpy $WelcomeText "This will update ${PRODUCT} from $OldVersion to ${VERSION}."
        ${EndIf}
    ${Case} "repair"
        StrCpy $WelcomeText "${PRODUCT} ${VERSION} is already installed. Setup will install it again, which repairs missing or damaged files."
    ${Case} "downgrade"
        StrCpy $WelcomeText "This will replace ${PRODUCT} $OldVersion with the older version ${VERSION}."
    ${CaseElse}
        StrCpy $WelcomeText "This will install ${PRODUCT} ${VERSION}, an open source reimplementation of Midtown Madness 2.$\r$\n$\r$\n${PRODUCT} does not include any of the original game's content. You need your own copy of Midtown Madness 2 (the CD, a disc image, or an existing installation); setup asks where it is."
    ${EndSelect}
    ${If} $OldInstDir != ""
        Push $OldInstDir
        Push $INSTDIR
        Call SameDir
        Pop $R1
        ${If} $R1 == 1
            StrCpy $WelcomeText "$WelcomeText$\r$\n$\r$\nIt stays in $INSTDIR. Your settings, driver profiles, replays and game data are kept."
        ${Else}
            StrCpy $WelcomeText "$WelcomeText$\r$\n$\r$\nIt moves from $OldInstDir to $INSTDIR. Your settings, driver profiles and replays are kept, and so is game data copied to the old folder."
        ${EndIf}
        StrCpy $WelcomeText "$WelcomeText Please close ${PRODUCT} before you continue."
        StrCpy $GD_NextIsInstall 1

        ; Shortcuts and tools as the last installation had them.
        !insertmacro OPENMM2_PRESELECT ${SecStartMenu} "StartMenuShortcut" "$SMPROGRAMS\${PRODUCT}.lnk"
        !insertmacro OPENMM2_PRESELECT ${SecDesktop} "DesktopShortcut" "$DESKTOP\${PRODUCT}.lnk"
!ifdef TOOLS_FILES_NSH
        !insertmacro OPENMM2_PRESELECT ${SecTools} "Tools" "$OldInstDir\mm2tool.exe"
!endif
    ${EndIf}

    ; Previous choice of game data (kept in the old folder when /D= moves
    ; the installation).
    StrCpy $CopyData 0
    StrCpy $SourceGiven 0
    Push "$INSTDIR\openmm2-install.ini"
    Push "Source"
    Call ReadUtf8IniValue
    Pop $GameSource
    ${If} $GameSource == ""
    ${AndIf} $OldInstDir != ""
        Push "$OldInstDir\openmm2-install.ini"
        Push "Source"
        Call ReadUtf8IniValue
        Pop $GameSource
    ${EndIf}

    ClearErrors
    ${GetOptions} $R0 "/SOURCE=" $R1
    ${IfNot} ${Errors}
        StrCpy $GameSource $R1
        StrCpy $SourceGiven 1
    ${EndIf}
    ClearErrors
    ${GetOptions} $R0 "/COPYDATA" $R1
    ${IfNot} ${Errors}
        StrCpy $CopyData 1
        SectionSetSize ${SecGameData} ${GAMEDATA_SIZE_KB}
    ${EndIf}
FunctionEnd

; A stopped installation that did not set its own exit code (a file that
; could not be written) ends with 1, not NSIS's 2, which means something else
; here.
Function .onInstFailed
    GetErrorLevel $0
    ${If} $0 = -1
        SetErrorLevel ${EXIT_FAILED}
    ${EndIf}
FunctionEnd

Function SkipPageOnUpdate
    ${If} $OldInstDir != ""
        Abort
    ${EndIf}
FunctionEnd

; Sets $R0 to the first program in <dir> that is running (or otherwise held
; open), "" if none.
;   Push <dir>
;   Call FindProgramInUse
!macro OPENMM2_FINDPROGRAMINUSE un
Function ${un}FindProgramInUse
    Exch $0
    Push $1
    StrCpy $R0 ""
    ${If} $0 != ""
        Push "$0\openmm2.exe"
        Call ${un}IsFileInUse
        Pop $1
        ${If} $1 == 1
            StrCpy $R0 "$0\openmm2.exe"
        ${Else}
            Push "$0\mm2tool.exe"
            Call ${un}IsFileInUse
            Pop $1
            ${If} $1 == 1
                StrCpy $R0 "$0\mm2tool.exe"
            ${EndIf}
        ${EndIf}
    ${EndIf}
    Pop $1
    Pop $0
FunctionEnd

; Waits until OpenMM2 is closed before anything is changed: its files cannot
; be replaced while it runs. Retry checks again; Cancel (and a silent run,
; which cannot ask) stops setup with nothing changed.
Function ${un}CheckNotRunning
    Push $R0
retry:
    Push $INSTDIR
    Call ${un}FindProgramInUse
    ${If} $R0 == ""
    ${AndIf} $OldInstDir != "" ; empty in the uninstaller
        Push $OldInstDir
        Call ${un}FindProgramInUse
    ${EndIf}
    ${If} $R0 != ""
        DetailPrint "In use: $R0"
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${PRODUCT} is running. Close it, then click Retry.$\r$\n$\r$\n($R0 is in use.)" /SD IDCANCEL IDRETRY retry
        ${If} ${Silent}
            SetErrorLevel ${EXIT_RUNNING}
        ${Else}
            SetErrorLevel ${EXIT_FAILED}
        ${EndIf}
        Pop $R0
        Abort "${PRODUCT} is running; nothing was changed."
    ${EndIf}
    Pop $R0
FunctionEnd
!macroend
!insertmacro OPENMM2_FINDPROGRAMINUSE ""
!insertmacro OPENMM2_FINDPROGRAMINUSE "un."

; Removes the installed version's files before the new ones are copied, so
; nothing that this version no longer ships stays behind (and the uninstaller,
; which knows only this version's manifest, leaves nothing either):
;   - files listed in $INSTDIR\install-manifest.txt;
;   - for an installation of 0.4.1 or earlier (no manifest; the registry names
;     this folder), every name those versions and this one install;
;   - when /D= moved the installation, the same in the old folder, plus its
;     firewall rule and the folder itself if nothing else is left.
; gamedata\ and openmm2-install.ini are never removed, and nothing outside
; these folders is touched.
Function RemovePreviousVersion
    Push $0
    Push $R1
    StrCpy $R1 0 ; 1 when the installed version is in $INSTDIR
    ${If} $OldInstDir != ""
        Push $OldInstDir
        Push $INSTDIR
        Call SameDir
        Pop $R1
    ${EndIf}

    ${If} ${FileExists} "$INSTDIR\install-manifest.txt"
        DetailPrint "Removing the files of the installed version"
        Push $INSTDIR
        Call RemoveListedFiles
    ${ElseIf} $R1 == 1
        DetailPrint "Removing the files of ${PRODUCT} $OldVersion"
        !insertmacro OPENMM2_LEGACY_UNINSTALL "$INSTDIR"
        !insertmacro OPENMM2_APP_UNINSTALL "$INSTDIR"
!ifdef TOOLS_FILES_NSH
        !insertmacro OPENMM2_TOOLS_UNINSTALL "$INSTDIR"
!endif
    ${EndIf}

    ${If} $OldInstDir != ""
    ${AndIf} $R1 != 1
        DetailPrint "Removing ${PRODUCT} $OldVersion from $OldInstDir"
        nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${PRODUCT}" program="$OldInstDir\openmm2.exe"'
        Pop $0
        ${If} ${FileExists} "$OldInstDir\install-manifest.txt"
            Push $OldInstDir
            Call RemoveListedFiles
        ${Else}
            !insertmacro OPENMM2_LEGACY_UNINSTALL "$OldInstDir"
            !insertmacro OPENMM2_APP_UNINSTALL "$OldInstDir"
!ifdef TOOLS_FILES_NSH
            !insertmacro OPENMM2_TOOLS_UNINSTALL "$OldInstDir"
!endif
        ${EndIf}
        RMDir "$OldInstDir"
    ${EndIf}
    Pop $R1
    Pop $0
FunctionEnd

; Writes $INSTDIR\install-manifest.txt for the files this installation copies.
Function WriteManifest
    Push $0
    ClearErrors
    FileOpen $0 "$INSTDIR\install-manifest.txt" w
    ${If} ${Errors}
        DetailPrint "Could not write $INSTDIR\install-manifest.txt"
    ${Else}
        FileWrite $0 "; Files installed by ${PRODUCT} ${VERSION} setup, relative to this folder.$\r$\n"
        FileWrite $0 "; The next update and the uninstaller remove them. Do not edit.$\r$\n"
        !insertmacro OPENMM2_APP_MANIFEST $0
!ifdef TOOLS_FILES_NSH
        ${If} ${SectionIsSelected} ${SecTools}
            !insertmacro OPENMM2_TOOLS_MANIFEST $0
        ${EndIf}
!endif
        FileWrite $0 "uninstall.exe$\r$\n"
        FileClose $0
    ${EndIf}
    Pop $0
FunctionEnd

; Validates $GameSource (again: silent installs never saw the page), copies
; the archives when requested and writes openmm2-install.ini.
Function InstallGameData
    ${If} $GameSource == ""
        DetailPrint "No game data location chosen; ${PRODUCT} will ask for it when it starts."
        Return
    ${EndIf}

    Push $GameSource
    Call QuoteArg
    Pop $R1
    DetailPrint "Checking game data at $GameSource"
    nsExec::ExecToStack '"$INSTDIR\openmm2.exe" --check-source $R1'
    Pop $0
    Pop $1
    ${TrimNewLines} "$1" $1
    ${If} $0 != 0
        DetailPrint "Game data not usable: $1"
        ${If} ${Silent}
        ${AndIf} $SourceGiven != 1
            ; A silent update re-checks the last installation's choice, e.g.
            ; a disc that is not in the drive now. That is no reason to fail:
            ; openmm2-install.ini keeps the choice.
            DetailPrint "Keeping the game data setting of the last installation."
            Return
        ${EndIf}
        MessageBox MB_ICONEXCLAMATION|MB_OK "The game data at $GameSource cannot be used:$\n$1$\n$\n${PRODUCT} will ask for the game files when it starts." /SD IDOK
        SetErrorLevel ${EXIT_SOURCE_REJECTED}
        Return
    ${EndIf}

    StrCpy $R2 $GameSource
    ${If} $CopyData == 1
        StrCpy $R3 "$INSTDIR\gamedata"
        ; A folder that is already there holds an earlier copy (an update or
        ; repair): a failed copy must never remove it.
        StrCpy $R5 0
        ${If} ${FileExists} "$R3\*.*"
            StrCpy $R5 1
        ${EndIf}
        DetailPrint "Copying game data to $R3"
        Push $R3
        Call QuoteArg
        Pop $R4
        Push "Copying game data"
        Push '"$INSTDIR\openmm2.exe" --import-source $R1 $R4'
        Call RunWithProgress
        Pop $0
        ${If} $0 == 0
            StrCpy $R2 $R3
        ${Else}
            DetailPrint "Copying failed (exit code $0)"
            ${If} $R5 == 0
                ; Everything in the folder came from this copy.
                RMDir /r "$R3"
                MessageBox MB_ICONEXCLAMATION|MB_OK "Copying the game data failed. ${PRODUCT} will read it from $GameSource instead." /SD IDOK
            ${Else}
                ; The import removed its partial files; the earlier copy is
                ; untouched. Keep using it if it is complete.
                Push $R3
                Call QuoteArg
                Pop $R4
                nsExec::ExecToStack '"$INSTDIR\openmm2.exe" --check-source $R4'
                Pop $0
                Pop $1
                ${If} $0 == 0
                    StrCpy $R2 $R3
                    MessageBox MB_ICONEXCLAMATION|MB_OK "Copying the game data failed. ${PRODUCT} keeps using the copy that is already in $R3." /SD IDOK
                ${Else}
                    MessageBox MB_ICONEXCLAMATION|MB_OK "Copying the game data failed. ${PRODUCT} will read it from $GameSource instead." /SD IDOK
                ${EndIf}
            ${EndIf}
        ${EndIf}
    ${EndIf}

    ClearErrors
    Push "$INSTDIR\openmm2-install.ini"
    Push "; Written by the ${PRODUCT} installer. Settings in %APPDATA%\OpenMM2\openmm2.ini take precedence.$\r$\n[GameData]$\r$\nSource=$R2$\r$\n"
    Call WriteUtf8File
    ${If} ${Errors}
        DetailPrint "Could not write $INSTDIR\openmm2-install.ini"
    ${Else}
        DetailPrint "Game data: $R2"
    ${EndIf}
FunctionEnd

; The installer runs elevated; start the game as the logged-in user instead.
Function LaunchUnelevated
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\openmm2.exe"'
FunctionEnd

; --- Uninstaller -----------------------------------------------------------

Function un.onInit
    SetRegView 64
    SetShellVarContext all
FunctionEnd

Section "Uninstall"
    Call un.CheckNotRunning
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${PRODUCT}" program="$INSTDIR\openmm2.exe"'
    Pop $0
    ${If} ${FileExists} "$INSTDIR\install-manifest.txt"
        Push $INSTDIR
        Call un.RemoveListedFiles
    ${Else}
        ; Installed by 0.4.1 or earlier, or the manifest is gone.
        !insertmacro OPENMM2_APP_UNINSTALL "$INSTDIR"
!ifdef TOOLS_FILES_NSH
        !insertmacro OPENMM2_TOOLS_UNINSTALL "$INSTDIR"
!endif
        !insertmacro OPENMM2_LEGACY_UNINSTALL "$INSTDIR"
    ${EndIf}
    Delete "$INSTDIR\openmm2-install.ini"
    Delete "$INSTDIR\uninstall.exe"

    ; The shortcuts and the registration belong to the registered
    ; installation; leave them alone if that is in another folder.
    ReadRegStr $1 HKLM "${REG_KEY}" "InstallDir"
    StrCpy $2 1
    ${If} $1 != ""
        Push $1
        Push $INSTDIR
        Call un.SameDir
        Pop $2
    ${EndIf}
    ${If} $2 == 1
        Delete "$SMPROGRAMS\${PRODUCT}.lnk"
        Delete "$DESKTOP\${PRODUCT}.lnk"
    ${EndIf}

    ${If} ${FileExists} "$INSTDIR\gamedata\*.*"
        MessageBox MB_YESNO|MB_ICONQUESTION "Also delete the copy of the Midtown Madness 2 game data in $INSTDIR\gamedata?$\n$\nKeep it if you plan to reinstall ${PRODUCT}." /SD IDNO IDNO keep_gamedata
        RMDir /r "$INSTDIR\gamedata"
    keep_gamedata:
    ${EndIf}

    ; Per-user settings and saves of the user running the uninstaller.
    SetShellVarContext current
    ${If} ${FileExists} "$APPDATA\OpenMM2\*.*"
    ${OrIf} ${FileExists} "$LOCALAPPDATA\OpenMM2\*.*"
        MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Delete your ${PRODUCT} settings, driver profiles and replays too?$\n$\n$APPDATA\OpenMM2$\n$LOCALAPPDATA\OpenMM2" /SD IDNO IDNO keep_userdata
        RMDir /r "$APPDATA\OpenMM2"
        RMDir /r "$LOCALAPPDATA\OpenMM2"
    keep_userdata:
    ${EndIf}
    SetShellVarContext all

    RMDir "$INSTDIR"
    ${If} $2 == 1
        DeleteRegKey HKLM "${UNINST_KEY}"
        DeleteRegKey HKLM "${REG_KEY}"
    ${EndIf}
SectionEnd
