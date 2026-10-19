@echo off
rem Buduje vsasr_gpu.exe - rozpoznawanie mowy Whisper na GPU NVIDIA (TCC64 + nvcuda.dll).
rem  1) kernels.cu -> kernels.ptx  (tylko gdy jest nvcc; inaczej zostaje dolaczony kernels.ptx)
rem  2) TCC: vsasr_gpu.exe  (bez naglowkow CUDA - Driver API ladowane przez LoadLibrary)
rem Model: ..\models\ggml-small.bin  (pobierz: ..\models\pobierz_model.bat)
setlocal
cd /d "%~dp0"
where tcc >NUL 2>NUL || if exist "%~dp0AddTccPath.bat" call "%~dp0AddTccPath.bat"
where tcc >NUL 2>NUL || (echo Nie znalazlem tcc - popraw AddTccPath.bat & exit /b 1)

where nvcc >NUL 2>NUL || goto noptx
rem nvcc potrzebuje cl.exe (MSVC); gdy nie ma go w PATH - szukamy przez vswhere
set "CCBIN="
where cl >NUL 2>NUL && goto haveccbin
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto noptx
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto noptx
set /p VCVER=<"%VSDIR%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
set "CCBIN=-ccbin "%VSDIR%\VC\Tools\MSVC\%VCVER%\bin\Hostx64\x64""
:haveccbin
echo [ptx] nvcc kernels.cu -^> kernels.ptx
nvcc -ptx -arch=compute_61 %CCBIN% kernels.cu -o kernels.ptx
if errorlevel 1 (echo [ptx] nvcc nie powiodl sie - uzywam istniejacego kernels.ptx)
goto build

:noptx
echo [ptx] brak nvcc/cl - uzywam dolaczonego kernels.ptx

:build
if not exist kernels.ptx (echo Brak kernels.ptx & exit /b 1)
echo Buduje vsasr_gpu.exe
rem synteza mowy (Piper), gdy sa naglowki onnxruntime z ..\tts\pobierz_tts.bat
set "TTS="
if exist ..\tts\include\onnxruntime_c_api.h set "TTS=-DASR_TTS -I..\tts -I..\tts\include ..\asr\tts.c"
if not defined TTS echo [tts] brak ..\tts\include - buduje bez syntezy mowy (uruchom ..\tts\pobierz_tts.bat)
tcc -m64 -Wall -DASR_GPU -o vsasr_gpu.exe ..\asr\asr_main.c ..\asr\whisper.c backend_cuda.c %TTS% -lws2_32
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: vsasr_gpu.exe  (kernels.ptx musi lezec obok exe)
endlocal
