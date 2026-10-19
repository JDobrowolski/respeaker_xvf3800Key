@echo off
rem Buduje vsasr_cpu.exe - rozpoznawanie mowy Whisper na CPU.
rem   build.bat        MSVC (cl /arch:AVX2) - kernele AVX2+FMA, cel: Intel Core i3-N305
rem   build.bat tcc    TCC64 - czyste C bez SIMD (dziala na kazdym x64, kilka razy wolniej)
rem Model: ..\models\ggml-large-v3-turbo-q8_0.bin  (pobierz: ..\models\pobierz_model.bat)
setlocal
cd /d "%~dp0"

rem synteza mowy (Piper), gdy sa naglowki onnxruntime z ..\tts\pobierz_tts.bat
set "TTS="
if exist ..\tts\include\onnxruntime_c_api.h set "TTS=-DASR_TTS -I..\tts -I..\tts\include ..\asr\tts.c"
if not defined TTS echo [tts] brak ..\tts\include - buduje bez syntezy mowy (uruchom ..\tts\pobierz_tts.bat)

if /i "%~1"=="tcc" goto tcc

rem ---- MSVC: cl w PATH albo vcvars64.bat znaleziony przez vswhere
where cl >NUL 2>NUL && goto msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto nomsvc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto nomsvc
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto nomsvc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >NUL
where cl >NUL 2>NUL || goto nomsvc

:msvc
echo Buduje vsasr_cpu.exe (MSVC, AVX2)
if not exist obj mkdir obj
rem ..\tts\specstrings.h to zaslepka SAL tylko dla TCC - MSVC ma wlasne naglowki
if defined TTS set "TTS=-DASR_TTS -I..\tts\include ..\asr\tts.c"
cl/nologo /O2 /Oi /arch:AVX2 /W3 /D_CRT_SECURE_NO_WARNINGS /Foobj\ /Fe:vsasr_cpu.exe ..\asr\asr_main.c ..\asr\whisper.c backend_cpu.c %TTS% ws2_32.lib
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: vsasr_cpu.exe (AVX2)
goto :eof

:nomsvc
echo [msvc] nie znalazlem Visual Studio (cl.exe) - buduje TCC bez AVX2

:tcc
where tcc >NUL 2>NUL || if exist "%~dp0AddTccPath.bat" call "%~dp0AddTccPath.bat"
where tcc >NUL 2>NUL || (echo Nie znalazlem tcc - popraw AddTccPath.bat & exit /b 1)
echo Buduje vsasr_cpu.exe (TCC, bez SIMD)
tcc -m64 -Wall -o vsasr_cpu.exe ..\asr\asr_main.c ..\asr\whisper.c backend_cpu.c %TTS% -lws2_32
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: vsasr_cpu.exe
endlocal
