# Microsoft Developer Studio Project File - Name="or" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

CFG=or - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "or.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "or.mak" CFG="or - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "or - Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "or - Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""
CPP=cl.exe
RSC=rc.exe

!IF  "$(CFG)" == "or - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /c
# ADD CPP /nologo /MT /W3 /GX /O2 /I "d:\openssl\include  ..\win32" /I "..\win32" /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /c
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib libeay32.lib ssleay32.lib /nologo /subsystem:console /machine:I386 /libpath:"d:\openssl\lib\vc"

!ELSEIF  "$(CFG)" == "or - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /GZ /c
# ADD CPP /nologo /MTd /W3 /Gm /GX /ZI /Od /I "..\..\src\win32" /I "D:\openssl\include" /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /GZ /c
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386 /pdbtype:sept
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib libeay32.lib ssleay32.lib /nologo /subsystem:console /debug /machine:I386 /out:"Debug/tor.exe" /pdbtype:sept /libpath:"d:\openssl\lib\vc"

!ENDIF 

# Begin Target

# Name "or - Win32 Release"
# Name "or - Win32 Debug"
# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Source File

SOURCE=..\..\src\common\aes.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\buffers.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\circuit.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\command.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\config.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\connection.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\connection_edge.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\connection_or.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\cpuworker.c
# End Source File
# Begin Source File

SOURCE=..\..\src\common\crypto.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\directory.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\dirserv.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\dns.c
# End Source File
# Begin Source File

SOURCE=..\..\src\common\fakepoll.c
# End Source File
# Begin Source File

SOURCE=..\..\src\common\log.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\main.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\onion.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\router.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\routerlist.c
# End Source File
# Begin Source File

SOURCE=..\..\src\or\tor_main.c
# End Source File
# Begin Source File

SOURCE=..\..\src\common\tortls.c
# End Source File
# Begin Source File

SOURCE=..\..\src\common\util.c
# End Source File
# End Group
# Begin Group "Header Files"

# PROP Default_Filter "h;hpp;hxx;hm;inl"
# Begin Source File

SOURCE=..\..\src\common\aes.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\crypto.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\fakepoll.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\log.h
# End Source File
# Begin Source File

SOURCE=..\..\src\or\or.h
# End Source File
# Begin Source File

SOURCE=..\..\src\win32\orconfig.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\test.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\torint.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\tortls.h
# End Source File
# Begin Source File

SOURCE=..\..\src\or\tree.h
# End Source File
# Begin Source File

SOURCE=..\..\src\common\util.h
# End Source File
# End Group
# Begin Group "Resource Files"

# PROP Default_Filter "ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe"
# End Group
# End Target
# End Project
