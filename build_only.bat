@echo off
setlocal
cd /d D:\.espressif\v6.0\esp-idf
call export.bat > nul
cd /d D:\Projects\lolovivi-display\ESP32S3-W25Q128-ST7789
echo BUILDING...
idf.py build
echo BUILD EXIT CODE: %ERRORLEVEL%
