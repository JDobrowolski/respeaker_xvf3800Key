@echo off
rem Pobiera skladniki syntezy mowy (Piper, licencja MIT) dla hostCPU / hostGPU:
rem   bin\      espeak-ng.dll + espeak-ng-data (fonemy, z wydania Piper)
rem             onnxruntime.dll (Microsoft, MIT) - uruchamia model glosu
rem   include\  naglowki C onnxruntime (potrzebne tylko do kompilacji)
rem   voices\   polskie glosy Piper: gosia (kobieta, domyslny), darkman (mezczyzna), mc_speech
rem   pobierz_tts.bat dml   - dodatkowo bin_dml\ dla vsasr_gpu -ttsgpu (DirectML)
rem Wymaga curl i tar (Windows 10+).
setlocal
cd /d "%~dp0"
set "ORTV=1.20.1"
if not exist dl mkdir dl
if not exist bin mkdir bin
if not exist include mkdir include
if not exist voices mkdir voices

if not exist bin\espeak-ng.dll (
    echo Pobieram Piper ^(espeak-ng^) ...
    curl -sL --fail -o dl\piper.zip https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip || goto fail
    "%SystemRoot%\System32\tar.exe" -xf dl\piper.zip -C dl || goto fail
    copy /y dl\piper\espeak-ng.dll bin\ >NUL
    xcopy /e /i /q /y dl\piper\espeak-ng-data bin\espeak-ng-data >NUL
)
if not exist bin\onnxruntime.dll (
    echo Pobieram onnxruntime %ORTV% ...
    curl -sL --fail -o dl\ort.zip https://github.com/microsoft/onnxruntime/releases/download/v%ORTV%/onnxruntime-win-x64-%ORTV%.zip || goto fail
    "%SystemRoot%\System32\tar.exe" -xf dl\ort.zip -C dl || goto fail
    copy /y dl\onnxruntime-win-x64-%ORTV%\lib\onnxruntime.dll bin\ >NUL
    copy /y dl\onnxruntime-win-x64-%ORTV%\lib\onnxruntime_providers_shared.dll bin\ >NUL
    copy /y dl\onnxruntime-win-x64-%ORTV%\include\*.h include\ >NUL
)
for %%v in (gosia darkman mc_speech) do call :voice %%v
if /i "%~1"=="dml" call :dml
echo Gotowe. Skladniki w %~dp0bin, glosy w %~dp0voices
goto :eof

rem Opcjonalnie: onnxruntime w wersji DirectML (GPU przez DirectX 12, bez CUDA/cuDNN) -> bin_dml
rem dla vsasr_gpu -ttsgpu. Uwaga: dla malego modelu Piper CPU bywa szybsze.
:dml
if exist bin_dml\DirectML.dll goto :eof
echo Pobieram onnxruntime DirectML %ORTV% ...
if not exist bin_dml mkdir bin_dml
if not exist dl\dml mkdir dl\dml
curl -sL --fail -o dl\dml\ort_dml.zip https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/%ORTV% || goto fail
curl -sL --fail -o dl\dml\directml.zip https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4 || goto fail
"%SystemRoot%\System32\tar.exe" -xf dl\dml\ort_dml.zip -C dl\dml runtimes/win-x64/native || goto fail
"%SystemRoot%\System32\tar.exe" -xf dl\dml\directml.zip -C dl\dml bin/x64-win/DirectML.dll || goto fail
copy /y dl\dml\runtimes\win-x64\native\onnxruntime.dll bin_dml\ >NUL
copy /y dl\dml\bin\x64-win\DirectML.dll bin_dml\ >NUL
copy /y bin\espeak-ng.dll bin_dml\ >NUL
xcopy /e /i /q /y bin\espeak-ng-data bin_dml\espeak-ng-data >NUL
goto :eof

:voice
if exist voices\pl_PL-%1-medium.onnx goto :eof
echo Pobieram glos pl_PL-%1-medium ...
curl -sL --fail -o voices\pl_PL-%1-medium.onnx https://huggingface.co/rhasspy/piper-voices/resolve/main/pl/pl_PL/%1/medium/pl_PL-%1-medium.onnx
curl -sL --fail -o voices\pl_PL-%1-medium.onnx.json https://huggingface.co/rhasspy/piper-voices/resolve/main/pl/pl_PL/%1/medium/pl_PL-%1-medium.onnx.json
goto :eof

:fail
echo Blad pobierania.
exit /b 1
