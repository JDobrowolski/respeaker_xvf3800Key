@echo off
rem Buduje kwstool.exe (TCC64): nagrywanie probek, trening sieci, test, wgrywanie modelu.
rem Wspolny kod z firmware: ..\components\kws (cechy, siec, detektor).
setlocal
cd /d "%~dp0"
where tcc >NUL 2>NUL || if exist "%~dp0..\hostCPU\AddTccPath.bat" call "%~dp0..\hostCPU\AddTccPath.bat"
where tcc >NUL 2>NUL || (echo Nie znalazlem tcc - popraw ..\hostCPU\AddTccPath.bat & exit /b 1)
echo Buduje kwstool.exe
tcc -m64 -Wall -o kwstool.exe kwstool.c nn_train.c ..\components\kws\kws_feat.c ..\components\kws\nn.c ..\components\kws\kws_detect.c -lws2_32
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: kwstool.exe
endlocal
