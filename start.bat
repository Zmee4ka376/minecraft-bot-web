@echo off
echo === BotCraft Web Control ===
echo.
echo Укажите параметры:
echo   Адрес сервера и имя бота будут запрошены ниже,
echo   или используйте аргументы командной строки:
echo.
echo   WebBot.exe --address 127.0.0.1:25565 --login WebBot --web-port 8080
echo.

REM Копируем статические файлы рядом с exe
if not exist "bin\static" mkdir "bin\static"
copy /Y "static\index.html" "bin\static\index.html" >nul
copy /Y "static\style.css" "bin\static\style.css" >nul
copy /Y "static\app.js" "bin\static\app.js" >nul

echo Файлы веб-интерфейса скопированы в bin\static
echo.
echo Запуск бота...
echo.

cd bin
WebBot.exe --address 127.0.0.1:25565 --login WebBot --web-port 8080
