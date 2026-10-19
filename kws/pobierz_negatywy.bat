@echo off
rem Pobiera polska mowe z audiobookow LibriVox (domena publiczna) jako negatywy do treningu:
rem po 10 minut z kilku rozdzialow, 16 kHz mono -> dataset\negative\librivox\*.wav
rem Wymaga curl (Windows 10+) i ffmpeg w PATH.
setlocal
cd /d "%~dp0"
set "D=dataset\negative\librivox"
if not exist "%D%" mkdir "%D%"
where ffmpeg >NUL 2>NUL || (echo Brak ffmpeg w PATH - zainstaluj: winget install ffmpeg & exit /b 1)

call :get potop1_2209_librivox potop1_01_sienkiewicz_64kb.mp3 potop1
call :get na_srebrnym_globie_1705_librivox nasrebrnymglobie_01_Zulawski_64kb.mp3 globie1
call :get siedem_wybranych_1202_librivox siedemwybranychopowiadan_1a_orkan_64kb.mp3 siedem1
call :get mojstosunekdokosciola_2511_librivox mojstosunekdokosciola_1_baudouindecourtenay_64kb.mp3 boy1
echo Gotowe: %D%
goto :eof

:get
if exist "%D%\%3.wav" (echo %3.wav juz jest & goto :eof)
echo Pobieram %2 ...
curl -sL --fail -o "%D%\%3.mp3" "https://archive.org/download/%1/%2" || (echo  nie udalo sie: %1/%2 & del "%D%\%3.mp3" 2>NUL & goto :eof)
ffmpeg -loglevel error -y -i "%D%\%3.mp3" -t 600 -ac 1 -ar 16000 -sample_fmt s16 "%D%\%3.wav"
del "%D%\%3.mp3"
goto :eof
