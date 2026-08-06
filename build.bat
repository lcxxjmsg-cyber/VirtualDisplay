@echo off
setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build
set CONFIG=Release
set IDDCX_VERSION=1.10
set IDDCX_EXTENSION=IddCx0102
set IDDCX_VERSION_MINOR=0xA
set IDDCX_MINIMUM_VERSION_REQUIRED=4
set WDF_VERSION=2.25
set WDF_EXPORT=WdfFunctions_02025
set UMDF_MINIMUM_VERSION_REQUIRED=
set DRIVER_DLL=VirtualDisplayDriver.dll
set CTRL_EXE=iddctrl.exe

if defined VD_IDDCX_VERSION set IDDCX_VERSION=%VD_IDDCX_VERSION%
if defined VD_IDDCX_EXTENSION set IDDCX_EXTENSION=%VD_IDDCX_EXTENSION%
if defined VD_IDDCX_MINOR set IDDCX_VERSION_MINOR=%VD_IDDCX_MINOR%
if defined VD_IDDCX_MINIMUM set IDDCX_MINIMUM_VERSION_REQUIRED=%VD_IDDCX_MINIMUM%
if defined VD_WDF_VERSION set WDF_VERSION=%VD_WDF_VERSION%
if defined VD_WDF_EXPORT set WDF_EXPORT=%VD_WDF_EXPORT%
if defined VD_UMDF_MINIMUM set UMDF_MINIMUM_VERSION_REQUIRED=%VD_UMDF_MINIMUM%

set WDF_VERSION_MINOR=%WDF_VERSION:2.=%

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%\obj" mkdir "%BUILD_DIR%\obj"
if not exist "%BUILD_DIR%\bin" mkdir "%BUILD_DIR%\bin"

:: Find VS BuildTools
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -products * -latest -property installationPath`) do (
    set VSINSTALL=%%i
)

if "%VSINSTALL%"=="" (
    echo ERROR: Visual Studio not found
    exit /b 1
)

echo Found Visual Studio at: %VSINSTALL%

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
echo AFTER_VSCMD=%ERRORLEVEL%
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to set up VS environment
    exit /b 1
)

:: Auto-detect the newest MSVC toolset instead of hardcoding a version.
:: A temp file is used because for /f parsing of paths containing "(x86)"
:: breaks cmd.exe's parenthesis handling.
set MSVC_TOOLS=
dir /b /ad /o-n "%VSINSTALL%\VC\Tools\MSVC" > "%TEMP%\vd_msvc_list.txt" 2>nul
set /p MSVC_TOOLS=<"%TEMP%\vd_msvc_list.txt"
if defined MSVC_TOOLS set MSVC_TOOLS=%VSINSTALL%\VC\Tools\MSVC\%MSVC_TOOLS%
if "%MSVC_TOOLS%"=="" (
    echo ERROR: No MSVC toolset found under "%VSINSTALL%\VC\Tools\MSVC"
    exit /b 1
)
echo Using MSVC toolset: %MSVC_TOOLS%

:: WDK paths
set WDK_ROOT=%USERPROFILE%\.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.1
set WDK_INC=%WDK_ROOT%\c\Include\10.0.26100.0
set WDK_LIB=%WDK_ROOT%\c\Lib\10.0.26100.0\um\x64
set IDDCX_LIB=%WDK_LIB%\iddcx\%IDDCX_VERSION%
set WDF_LIB=%WDK_ROOT%\c\Lib\wdf\umdf\x64\%WDF_VERSION%
set WDF_INC=%WDK_ROOT%\c\Include\wdf\umdf\%WDF_VERSION%

:: Windows SDK paths
set WIN10_SDK=C:\Program Files (x86)\Windows Kits\10
set WIN10_SDK_VER=10.0.26100.0
set WIN10_INC=%WIN10_SDK%\Include\%WIN10_SDK_VER%
set WIN10_LIB=%WIN10_SDK%\Lib\%WIN10_SDK_VER%\um\x64
set UCRT_LIB=%WIN10_SDK%\Lib\%WIN10_SDK_VER%\ucrt\x64

:: IddCx include path (under WDK)
set IDDCX_INC=%WDK_INC%\um\iddcx\%IDDCX_VERSION%

:: Compile flags
set CFLAGS=/nologo /O2 /W4 /EHsc /std:c++17 /MT /utf-8
set CFLAGS_MT=/nologo /O2 /W4 /EHsc /std:c++17 /MT /utf-8
set CFLAGS=%CFLAGS% /I"%MSVC_TOOLS%\include"
set CFLAGS=%CFLAGS% /I"%WIN10_INC%\um"
set CFLAGS=%CFLAGS% /I"%WIN10_INC%\shared"
set CFLAGS=%CFLAGS% /I"%WIN10_INC%\winrt"
set CFLAGS=%CFLAGS% /I"%WDK_INC%\um"
set CFLAGS=%CFLAGS% /I"%WDK_INC%\shared"
set CFLAGS=%CFLAGS% /I"%IDDCX_INC%"
set CFLAGS=%CFLAGS% /I"%WDK_INC%\kmdf"
set CFLAGS=%CFLAGS% /I"%WDF_INC%"
set CFLAGS=%CFLAGS% /D_WIN32 /D_WIN64 /DWIN32 /DWIN64 /D_UNICODE /DUNICODE /DNDEBUG
set CFLAGS=%CFLAGS% /DUMDF_DRIVER /DUMDF_VERSION_MAJOR=2 /DUMDF_VERSION_MINOR=%WDF_VERSION_MINOR% /DUMDF_USING_NTSTATUS
set CFLAGS=%CFLAGS% /DIDDCX_VERSION_MAJOR=1 /DIDDCX_VERSION_MINOR=%IDDCX_VERSION_MINOR%
if defined IDDCX_MINIMUM_VERSION_REQUIRED set CFLAGS=%CFLAGS% /DIDDCX_MINIMUM_VERSION_REQUIRED=%IDDCX_MINIMUM_VERSION_REQUIRED%
if defined UMDF_MINIMUM_VERSION_REQUIRED set CFLAGS=%CFLAGS% /DUMDF_MINIMUM_VERSION_REQUIRED=%UMDF_MINIMUM_VERSION_REQUIRED%

:: Link flags
set LFLAGS=/nologo /DLL /MACHINE:X64
set LFLAGS=%LFLAGS% /LIBPATH:"%MSVC_TOOLS%\lib\x64"
set LFLAGS=%LFLAGS% /LIBPATH:"%WIN10_LIB%"
set LFLAGS=%LFLAGS% /LIBPATH:"%UCRT_LIB%"
set LFLAGS=%LFLAGS% /LIBPATH:"%WDK_LIB%"
set LFLAGS=%LFLAGS% /LIBPATH:"%IDDCX_LIB%"
set LFLAGS=%LFLAGS% /LIBPATH:"%WDF_LIB%"

echo Compiling dllmain.cpp...
cl.exe %CFLAGS% /c "%SCRIPT_DIR%driver\dllmain.cpp" /Fo"%BUILD_DIR%\obj\dllmain.obj"
if %ERRORLEVEL% neq 0 exit /b 1

echo Compiling Driver.cpp...
cl.exe %CFLAGS% /c "%SCRIPT_DIR%driver\Driver.cpp" /Fo"%BUILD_DIR%\obj\Driver.obj"
if %ERRORLEVEL% neq 0 exit /b 1

echo Compiling Device.cpp...
cl.exe %CFLAGS% /c "%SCRIPT_DIR%driver\Device.cpp" /Fo"%BUILD_DIR%\obj\Device.obj"
if %ERRORLEVEL% neq 0 exit /b 1

echo Compiling resources...
rc.exe /fo"%BUILD_DIR%\obj\VirtualDisplay.res" "%SCRIPT_DIR%driver\VirtualDisplay.rc"
if %ERRORLEVEL% neq 0 exit /b 1

echo Linking %DRIVER_DLL%...
del /f /q "%BUILD_DIR%\bin\%DRIVER_DLL%" >nul 2>&1
link.exe %LFLAGS% /OUT:"%BUILD_DIR%\bin\%DRIVER_DLL%" ^
    /IMPLIB:"%BUILD_DIR%\bin\VirtualDisplay_%WDF_VERSION:.=%.lib" ^
    "%BUILD_DIR%\obj\dllmain.obj" ^
    "%BUILD_DIR%\obj\Driver.obj" ^
    "%BUILD_DIR%\obj\Device.obj" ^
    "%BUILD_DIR%\obj\VirtualDisplay.res" ^
    /EXPORT:%WDF_EXPORT% ^
    iddcxstub.lib WdfDriverStubUm.lib ntdll.lib dxgi.lib d3d11.lib
if %ERRORLEVEL% neq 0 exit /b 1

echo.
echo Driver build complete!
echo DLL: %BUILD_DIR%\bin\%DRIVER_DLL%
echo IddCx extension: %IDDCX_EXTENSION%

:: Build control app
echo.
echo Building control app (%CTRL_EXE%)...

set CTRL_OUT=%BUILD_DIR%\bin\%CTRL_EXE%

cl.exe %CFLAGS_MT% /Fe"%CTRL_OUT%" ^
    "%SCRIPT_DIR%iddctrl\iddctrl.cpp" ^
    /link /SUBSYSTEM:CONSOLE /LIBPATH:"%MSVC_TOOLS%\lib\x64" /LIBPATH:"%WIN10_LIB%" /LIBPATH:"%UCRT_LIB%" setupapi.lib newdev.lib advapi32.lib user32.lib dxgi.lib d3d11.lib shell32.lib wtsapi32.lib userenv.lib
if %ERRORLEVEL% neq 0 exit /b 1

echo.
echo Control app build complete!
echo EXE: %CTRL_OUT%

:: Build GUI app (WebView2 shell + HTML UI)
echo.
echo Building GUI app (VirtualDisplay.exe)...

set GUI_OUT=%BUILD_DIR%\bin\VirtualDisplay.exe
set WV2_INC=%SCRIPT_DIR%webview2sdk\include
set WV2_LIB=%SCRIPT_DIR%webview2sdk\lib

:: Compile GUI resources (app icon + version info)
rc.exe /fo"%BUILD_DIR%\obj\VirtualDisplayGui.res" "%SCRIPT_DIR%gui\VirtualDisplay.rc"
if %ERRORLEVEL% neq 0 exit /b 1

cl.exe %CFLAGS_MT% /I"%MSVC_TOOLS%\include" /I"%WIN10_INC%\um" /I"%WIN10_INC%\shared" /I"%WIN10_INC%\winrt" /I"%WV2_INC%" /Fe"%GUI_OUT%" ^
    "%SCRIPT_DIR%gui\main.cpp" ^
    "%SCRIPT_DIR%gui\shell.cpp" ^
    "%SCRIPT_DIR%gui\vdctl.cpp" ^
    "%SCRIPT_DIR%gui\json.cpp" ^
    "%SCRIPT_DIR%gui\strutil.cpp" ^
    "%BUILD_DIR%\obj\VirtualDisplayGui.res" ^
    /link /SUBSYSTEM:WINDOWS /LIBPATH:"%MSVC_TOOLS%\lib\x64" /LIBPATH:"%WIN10_LIB%" /LIBPATH:"%UCRT_LIB%" /LIBPATH:"%WV2_LIB%" user32.lib gdi32.lib dwmapi.lib shell32.lib ole32.lib comctl32.lib shlwapi.lib advapi32.lib version.lib WebView2LoaderStatic.lib
if %ERRORLEVEL% neq 0 exit /b 1

:: Embed the application manifest (requireAdministrator + PerMonitorV2 DPI)
set MT_EXE="%WIN10_SDK%\bin\%WIN10_SDK_VER%\x64\mt.exe"
if not exist %MT_EXE% set MT_EXE="%WIN10_SDK%\bin\x64\mt.exe"
%MT_EXE% -manifest "%SCRIPT_DIR%gui\VirtualDisplay.exe.manifest" -outputresource:"%GUI_OUT%;1" >nul
if %ERRORLEVEL% neq 0 (
  echo WARNING: mt.exe manifest embedding failed, GUI runs without admin manifest
)

echo.
echo GUI app build complete!
echo EXE: %GUI_OUT%

:: Copy the HTML UI next to the binaries
if exist "%BUILD_DIR%\bin\ui" rmdir /s /q "%BUILD_DIR%\bin\ui"
xcopy /s /e /q /i "%SCRIPT_DIR%gui\ui" "%BUILD_DIR%\bin\ui" >nul

:: Copy the INF next to the binaries for packaging
copy /Y "%SCRIPT_DIR%driver\VirtualDisplay.inf" "%BUILD_DIR%\bin\VirtualDisplay.inf" >nul

echo.
echo All builds complete!
echo Driver: %BUILD_DIR%\bin\%DRIVER_DLL%
echo Control: %BUILD_DIR%\bin\%CTRL_EXE%
echo GUI: %BUILD_DIR%\bin\VirtualDisplay.exe
echo INF: %BUILD_DIR%\bin\VirtualDisplay.inf
