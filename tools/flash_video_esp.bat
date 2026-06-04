@echo off
REM 将视频数据烧录到 ESP32-S3 内部 Flash 的 0x200000 地址
REM 用法: flash_video_esp.bat COM8
if \"%1\"==\"\" (
    echo 请指定串口: flash_video_esp.bat COM8
    exit /b 1
)

python -m esptool --chip esp32s3 -p %1 -b 460800 --before default-reset --after hard-reset write-flash 0x200000 videos\video_all.bin
if %errorlevel% equ 0 (
    echo.
    echo 烧录完成！请按 RST 重启 ESP32
)
