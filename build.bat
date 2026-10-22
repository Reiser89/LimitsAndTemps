@echo off
rem Builds build\LimitsAndTemps.exe: x64 Release, fully static (wxWidgets + /MT CRT), no DLLs needed.
rem The PawnIO installer and modules (third_party\) are embedded via src\app.rc.
setlocal
if "%wxwin%"=="" (
    echo Set the wxwin environment variable to your wxWidgets folder.
    exit /b 1
)
rem Static-runtime wx build (see docs\build.md for how it was produced).
set WXLIB=%wxwin%\build_mt\lib\vc_x64_lib

if "%VCINSTALLDIR%"=="" call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul

cd /d "%~dp0"
if not exist build mkdir build

rem Official signed PawnIO installer and modules (pinned versions + hashes).
if not exist third_party mkdir third_party
if not exist third_party\PawnIO_setup.exe (
    curl -fsSL -o third_party\PawnIO_setup.exe https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe || exit /b 1
)
call :verify third_party\PawnIO_setup.exe 1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032 || exit /b 1

set MODULES=third_party\PawnIO.Modules
if not exist %MODULES%\IntelMSR.bin (
    if not exist %MODULES% mkdir %MODULES%
    curl -fsSL -o build\PawnIO.Modules.zip https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip || exit /b 1
    call :verify build\PawnIO.Modules.zip 43608CB89BC84247FEF1368A139013F7D043E17DB6D6C8DFC9B46BF0905A81F4 || exit /b 1
    tar -xf build\PawnIO.Modules.zip -C %MODULES% IntelMSR.bin AMDFamily10.bin AMDFamily17.bin COPYING || exit /b 1
)
call :verify %MODULES%\IntelMSR.bin D6ED85D65AB17A22F813EF98207D6D537155EE2DED5976A21CB48413C9B92E5F || exit /b 1
call :verify %MODULES%\AMDFamily10.bin 6443080B2968474FFBC38AA4356CC56F9664349FA4B917AFDB33027D6BB50525 || exit /b 1
call :verify %MODULES%\AMDFamily17.bin DAE74615761B78BDF064DFB3E136252DDCC6FC727D88F14738D0E5800D427A91 || exit /b 1

rc /nologo /I"%wxwin%\include" /fobuild\app.res src\app.rc || exit /b 1

rem MSVC's lowest selectable standard is C++14; the code itself is plain C++11.
rem Delay-loaded DLLs are only mapped if actually called (pdh: ACPI fallback; winhttp: agent limits; the rest: wx features this app doesn't use).
cl /nologo /std:c++14 /EHsc /O1 /GL /Gy /MT /W3 /utf-8 ^
   /D_UNICODE /DUNICODE /D__WXMSW__ /DNDEBUG ^
   /I"%WXLIB%\mswu" /I"%wxwin%\include" ^
   /Fobuild\ /Febuild\LimitsAndTemps.exe src\main.cpp src\sensors.cpp src\helper.cpp src\cputemp.cpp src\task.cpp src\usage.cpp build\app.res ^
   /link /MANIFEST:NO /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /LIBPATH:"%WXLIB%" ^
   wxmsw33u_core.lib wxbase33u.lib wxpng.lib wxjpeg.lib wxtiff.lib wxzlib.lib wxregexu.lib ^
   pdh.lib user32.lib gdi32.lib gdiplus.lib msimg32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib ^
   oleaut32.lib uuid.lib advapi32.lib winspool.lib rpcrt4.lib winmm.lib shlwapi.lib version.lib ^
   uxtheme.lib oleacc.lib ws2_32.lib dwmapi.lib imm32.lib taskschd.lib winhttp.lib delayimp.lib ^
   /DELAYLOAD:pdh.dll /DELAYLOAD:winhttp.dll /DELAYLOAD:oleaut32.dll /DELAYLOAD:gdiplus.dll /DELAYLOAD:msimg32.dll /DELAYLOAD:comdlg32.dll /DELAYLOAD:winspool.drv ^
   /DELAYLOAD:oleacc.dll /DELAYLOAD:version.dll /DELAYLOAD:shlwapi.dll /DELAYLOAD:rpcrt4.dll
if errorlevel 1 exit /b 1
echo Built build\LimitsAndTemps.exe
exit /b 0

:verify
powershell -NoProfile -Command "if ((Get-FileHash '%~1').Hash -ne '%~2') { Write-Error '%~1 hash mismatch'; exit 1 }"
exit /b
