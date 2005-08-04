;tor.nsi - A basic win32 installer for Tor
; Originally written by J Doe.
; See LICENSE for licencing information
;-----------------------------------------
;
; How to make an installer:
;   Step 0. If you are a Tor maintainer, make sure that tor.nsi and
;           src/win32/orconfig.h all have the correct version number.
;   Step 1. Download and install OpenSSL.  Make sure that the OpenSSL
;           version listed below matches the one you downloaded.
;   Step 2. Download and install NSIS (http://nsis.sourceforge.net)
;   Step 3. Make a directory under the main tor directory called "bin".
;   Step 4. Copy ssleay32.dll and libeay32.dll from OpenSSL into "bin".
;   Step 5. Run man2html on tor.1.in; call the result tor-reference.html
;           Run man2html on tor-resolve.1; call the result tor-resolve.html
;   Step 6. Copy torrc.sample.in to torrc.sample.
;   Step 7. Build tor.exe and tor_resolve.exe; save the result into bin.
;   Step 8. cd into contrib and run "makensis tor.nsi".
;
; Problems:
;   - Copying torrc.sample.in to torrc.sample and tor.1.in (implicitly)
;     to tor.1 is a Bad Thing, and leaves us with @autoconf@ vars in the final
;     result.
;   - Building Tor requires too much windows C clue.
;     - We should have actual makefiles for VC that do the right thing.
;   - I need to learn more NSIS juju to solve these:
;     - There should be a batteries-included installer that comes with
;       privoxy too. (Check privoxy license on this; be sure to include
;       all privoxy documents.)
;   - The filename should probably have a revision number.

!include "MUI.nsh"

!define VERSION "0.1.1.3-alpha"
!define INSTALLER "tor-${VERSION}-win32.exe"
!define WEBSITE "http://tor.eff.org/"

!define LICENSE "..\LICENSE"
;BIN is where it expects to find tor.exe, tor_resolve.exe, libeay32.dll and
;  ssleay32.dll
!define BIN "..\bin"

SetCompressor lzma
;SetCompressor zlib
OutFile ${INSTALLER}
InstallDir $PROGRAMFILES\Tor
SetOverWrite ifnewer

Name "Tor"
Caption "Tor ${VERSION} Setup"
BrandingText "The Onion Router"
CRCCheck on

;Use upx on the installer header to shrink the size.
!packhdr header.dat "upx --best header.dat"

!define MUI_WELCOMEPAGE_TITLE "Welcome to the Tor ${VERSION} Setup Wizard"
!define MUI_WELCOMEPAGE_TEXT "This wizard will guide you through the installation of Tor ${VERSION}.\r\n\r\nIf you have previously installed Tor and it is currently running, please exit Tor first before continuing this installation.\r\n\r\n$_CLICK"
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\win-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\win-uninstall.ico"
!define MUI_HEADERIMAGE_BITMAP "${NSISDIR}\Contrib\Graphics\Header\win.bmp"
!define MUI_HEADERIMAGE
!define MUI_FINISHPAGE_RUN "$INSTDIR\tor.exe"
!define MUI_FINISHPAGE_LINK "Visit the Tor website for the latest updates."
!define MUI_FINISHPAGE_LINK_LOCATION ${WEBSITE}

!insertmacro MUI_PAGE_WELCOME
; There's no point in having a clickthrough license: Our license adds
; certain rights, but doesn't remove them.
; !insertmacro MUI_PAGE_LICENSE "${LICENSE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Var configdir
Var configfile

;Sections
;--------

Section "Tor" Tor
;Files that have to be installed for tor to run and that the user
;cannot choose not to install
   SectionIn RO
   SetOutPath $INSTDIR
   File "${BIN}\tor.exe"
   File "${BIN}\tor_resolve.exe"
   WriteIniStr "$INSTDIR\Tor Website.url" "InternetShortcut" "URL" ${WEBSITE}

   StrCpy $configfile "torrc"
   StrCpy $configdir $APPDATA\Tor
;   ;If $APPDATA isn't valid here (Early win95 releases with no updated
;   ; shfolder.dll) then we put it in the program directory instead.
;   StrCmp $APPDATA "" "" +2
;      StrCpy $configdir $INSTDIR
   SetOutPath $configdir
   ;If there's already a torrc config file, ask if they want to
   ;overwrite it with the new one.
   IfFileExists "$configdir\torrc" "" endiftorrc
      MessageBox MB_ICONQUESTION|MB_YESNO "You already have a Tor config file.$\r$\nDo you want to overwrite it with the default sample config file?" IDNO yesreplace
      Delete $configdir\torrc
      Goto endiftorrc
     yesreplace:
      StrCpy $configfile "torrc.sample"
   endiftorrc:
   File /oname=$configfile "..\src\config\torrc.sample"
SectionEnd

Section "OpenSSL 0.9.7e" OpenSSL
   SetOutPath $INSTDIR
   File "${BIN}\libeay32.dll"
   File "${BIN}\ssleay32.dll"
SectionEnd

Section "Documents" Docs
   SetOutPath "$INSTDIR\Documents"
   File "..\doc\tor-spec.txt"
   #File "..\doc\FAQ"
   File "..\doc\HACKING"
   File "..\doc\rend-spec.txt"
   File "..\doc\control-spec.txt"
   File "..\doc\tor-doc.html"
   File "..\doc\tor-doc.css"
   File "..\doc\tor-resolve.html"
   File "..\doc\tor-reference.html"
   File "..\doc\design-paper\tor-design.pdf"
   File "..\README"
   File "..\AUTHORS"
   File "..\ChangeLog"
   File "..\LICENSE"
SectionEnd

SubSection /e "Shortcuts" Shortcuts

Section "Start Menu" StartMenu
   SetOutPath $INSTDIR
   IfFileExists "$SMPROGRAMS\Tor\*.*" "" +2
      RMDir /r "$SMPROGRAMS\Tor"
   CreateDirectory "$SMPROGRAMS\Tor"
   CreateShortCut "$SMPROGRAMS\Tor\Tor.lnk" "$INSTDIR\tor.exe"
   CreateShortCut "$SMPROGRAMS\Tor\Torrc.lnk" "Notepad.exe" "$configdir\torrc"
   CreateShortCut "$SMPROGRAMS\Tor\Tor Website.lnk" "$INSTDIR\Tor Website.url"
   CreateShortCut "$SMPROGRAMS\Tor\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
   IfFileExists "$INSTDIR\Documents\*.*" "" endifdocs
      CreateDirectory "$SMPROGRAMS\Tor\Documents"
      CreateShortCut "$SMPROGRAMS\Tor\Documents\Tor Manual.lnk" "$INSTDIR\Documents\tor-doc.html"
      CreateShortCut "$SMPROGRAMS\Tor\Documents\Tor Documentation.lnk" "$INSTDIR\Documents"
      CreateShortCut "$SMPROGRAMS\Tor\Documents\Tor Specification.lnk" "$INSTDIR\Documents\tor-spec.txt"
   endifdocs:
SectionEnd

Section "Desktop" Desktop
   SetOutPath $INSTDIR
   CreateShortCut "$DESKTOP\Tor.lnk" "$INSTDIR\tor.exe"
SectionEnd

Section /o "Run at startup" Startup
   SetOutPath $INSTDIR
   CreateShortCut "$SMSTARTUP\Tor.lnk" "$INSTDIR\tor.exe" "" "" 0 SW_SHOWMINIMIZED
SectionEnd

SubSectionEnd

Section "Uninstall"
   Delete "$DESKTOP\Tor.lnk"
   Delete "$INSTDIR\libeay32.dll"
   Delete "$INSTDIR\ssleay32.dll"
   Delete "$INSTDIR\tor.exe"
   Delete "$INSTDIR\tor_resolve.exe"
   Delete "$INSTDIR\Tor Website.url"
   Delete "$INSTDIR\torrc"
   Delete "$INSTDIR\torrc.sample"
   StrCmp $configdir $INSTDIR +2 ""
      RMDir /r $configdir
   Delete "$INSTDIR\Uninstall.exe"
   RMDir /r "$INSTDIR\Documents"
   RMDir $INSTDIR
   RMDir /r "$SMPROGRAMS\Tor"
   Delete "$SMSTARTUP\Tor.lnk"
   DeleteRegKey HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Tor"
SectionEnd

Section -End
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    ;The registry entries simply add the Tor uninstaller to the Windows
    ;uninstall list.
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Tor" "DisplayName" "Tor (remove only)"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Tor" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Tor} "The core executable and config files needed for Tor to run."
  !insertmacro MUI_DESCRIPTION_TEXT ${OpenSSL} "OpenSSL libraries required by Tor."
  !insertmacro MUI_DESCRIPTION_TEXT ${Docs} "Documentation about Tor."
  !insertmacro MUI_DESCRIPTION_TEXT ${ShortCuts} "Shortcuts to easily start Tor"
  !insertmacro MUI_DESCRIPTION_TEXT ${StartMenu} "Shortcuts to access Tor and it's documentation from the Start Menu"
  !insertmacro MUI_DESCRIPTION_TEXT ${Desktop} "A shortcut to start Tor from the desktop"
  !insertmacro MUI_DESCRIPTION_TEXT ${Startup} "Launches Tor automatically at startup in a minimized window"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

