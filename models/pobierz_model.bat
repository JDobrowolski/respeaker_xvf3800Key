@echo off
rem Pobiera modele Whisper (OpenAI, licencja MIT) w formacie ggml z repozytorium whisper.cpp.
rem   pobierz_model.bat            - large-v3-turbo-q8_0 (CPU, 834 MB) i small (GPU)
rem   pobierz_model.bat tiny       - wybrany: tiny 75 MB | base 142 MB | small 466 MB | medium 1.5 GB
rem                                  | large-v3-turbo-q8_0 834 MB
rem   pobierz_model.bat test       - probka jfk.wav do sprawdzenia (angielski)
setlocal
cd /d "%~dp0"
if "%~1"=="" (
    call :get large-v3-turbo-q8_0
    call :get small
    goto :eof
)
if /i "%~1"=="test" (
    curl -L -o jfk.wav https://raw.githubusercontent.com/ggml-org/whisper.cpp/master/samples/jfk.wav
    goto :eof
)
call :get %1
goto :eof

:get
if exist ggml-%1.bin (echo ggml-%1.bin juz jest & goto :eof)
echo Pobieram ggml-%1.bin ...
curl -L --fail -o ggml-%1.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%1.bin
if errorlevel 1 (echo Blad pobierania ggml-%1.bin & del ggml-%1.bin 2>NUL)
goto :eof
