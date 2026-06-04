@echo off
cd /d D:\.espressif\v6.0\esp-idf
call export.bat > nul 2>&1
cd /d D:\Projects\lolovivi-display\ESP32S3-W25Q128-ST7789
idf.py -p COM8 flash
exit /b %ERRORLEVEL%
