# ESP32-S3 + ST7789 Video Player

Pre-rendered RGB565 video player for ST7789 TFT LCD display, powered by ESP32-S3. Reads video data from the internal flash partition and plays it in a loop.

## Hardware Wiring

| Peripheral | ESP32-S3 GPIO |
|------------|---------------|
| ST7789 MOSI | GPIO 18 |
| ST7789 CLK | GPIO 21 |
| ST7789 CS | GPIO 14 |
| ST7789 DC | GPIO 15 |
| ST7789 RST | GPIO 16 |
| ST7789 BL | GPIO 17 |

> Default resolution: 172×320. Edit macros at the top of main/main.c to change pins or parameters.

## Quick Start

### 1. Setup ESP-IDF

Install ESP-IDF, then set up the environment:

`ash
. /export.sh
`

### 2. Build & Flash Firmware

`ash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
`

Replace PORT with your serial port (e.g. COM3 on Windows, /dev/ttyUSB0 on Linux).

### 3. Generate Video

`ash
python tools/generate_video.py --frames 60 --width 172 --height 320
`

This generates 60 frames of a bouncing ball animation (white background + black circle).

### 4. Flash Video to Internal Flash

`ash
python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x200000 videos/video_with_header.bin
`

### 5. Play

Press the **RST button** to restart. The firmware detects the video marker and starts playback automatically.

## Video Specs

| Parameter | Value |
|-----------|-------|
| Resolution | 172 × 320 (portrait) |
| Color format | RGB565 (16-bit, 2 bytes/pixel) |
| Frame size | 110,080 bytes |
| Frame count | 60 (configurable) |
| Total size | ~6.3 MB |
| Storage | ESP32 internal flash (video partition, 7MB) |

## Partition Layout

`
0x010000  factory app (firmware, 1MB)
0x200000  video partition (7MB)
`

## Project Structure

`
├── main/main.c               # Firmware
├── tools/generate_video.py   # Video frame generator
├── tools/flash_video_esp.bat # esptool flash script
├── partitions.csv            # Partition table
└── videos/                   # Video frame data
`

## License

MIT
