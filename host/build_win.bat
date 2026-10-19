@echo off
rem Budowanie vsmic.exe na Windows. Dziala z TCC (TCC64) albo mingw gcc.
rem   build_win.bat          - wykryje tcc albo gcc
rem   build_win.bat tcc      - wymusza TCC
rem   build_win.bat gcc      - wymusza gcc
setlocal
cd /d "%~dp0"
set "WHICH=%~1"

rem Jesli tcc nie jest w PATH, dociagnij sciezke z AddTccPath.bat (obok tego pliku).
where tcc >NUL 2>NUL || if exist "%~dp0AddTccPath.bat" call "%~dp0AddTccPath.bat"

if /i "%WHICH%"=="tcc" goto tcc
if /i "%WHICH%"=="gcc" goto gcc
where tcc >NUL 2>NUL && goto tcc
where gcc >NUL 2>NUL && goto gcc
echo Nie znalazlem tcc ani gcc w PATH.
exit /b 1

:tcc
echo Buduje TCC: vsmic.exe
tcc vsmic.c -o vsmic.exe -lws2_32
goto done

:gcc
echo Buduje gcc: vsmic.exe
gcc -O2 -Wall -o vsmic.exe vsmic.c -lws2_32
goto done

:done
if errorlevel 1 (echo BLAD budowania & exit /b 1)
echo GOTOWE: vsmic.exe
endlocal
