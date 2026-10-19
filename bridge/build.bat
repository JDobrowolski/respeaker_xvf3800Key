@echo off
rem Buduje vsbridge.exe - mostek ReSpeaker <-> urzadzenia audio Windows (RtAudio, WASAPI).
rem   build.bat         MSVC (cl; vcvars64 szukany przez vswhere)
rem   build.bat gcc     MinGW-w64 g++ (np. z MSYS2: pacman -S mingw-w64-x86_64-gcc)
rem RtAudio 6.0.1 (2 pliki) pobierany raz do rtaudio\ (curl jest w Windows 10/11).
setlocal
cd /d "%~dp0"

set "RTV=6.0.1"
if not exist rtaudio mkdir rtaudio
for %%f in (RtAudio.h RtAudio.cpp) do (
  if not exist rtaudio\%%f (
    echo [rtaudio] pobieram %%f %RTV%
    curl -fsSL -o rtaudio\%%f https://raw.githubusercontent.com/thestk/rtaudio/%RTV%/%%f
    if errorlevel 1 (echo BLAD pobierania RtAudio & exit /b 1)
  )
)
set "LIBS=ole32 ksuser mfplat mfuuid wmcodecdspuuid winmm ws2_32"

if /i "%~1"=="gcc" goto gcc

where cl >NUL 2>NUL && goto msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto nomsvc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto nomsvc
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto nomsvc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >NUL
where cl >NUL 2>NUL || goto nomsvc

:msvc
echo Buduje vsbridge.exe (MSVC)
if not exist obj mkdir obj
set "MLIBS="
for %%l in (%LIBS%) do call set "MLIBS=%%MLIBS%% %%l.lib"
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /D__WINDOWS_WASAPI__ /D_CRT_SECURE_NO_WARNINGS /I..\main /Irtaudio /Foobj\ /Fe:vsbridge.exe vsbridge.cpp rtaudio\RtAudio.cpp %MLIBS%
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: vsbridge.exe
goto :eof

:nomsvc
echo [msvc] nie znalazlem Visual Studio (cl.exe) - probuje g++ (MinGW-w64)

:gcc
where g++ >NUL 2>NUL || (echo Nie znalazlem g++ w PATH. & exit /b 1)
echo Buduje vsbridge.exe (g++)
set "GLIBS="
for %%l in (%LIBS%) do call set "GLIBS=%%GLIBS%% -l%%l"
g++ -O2 -std=c++17 -Wall -D__WINDOWS_WASAPI__ -I..\main -Irtaudio -o vsbridge.exe vsbridge.cpp rtaudio\RtAudio.cpp -static %GLIBS%
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: vsbridge.exe
endlocal
