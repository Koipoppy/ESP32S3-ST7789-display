# ESP32-S3 + ST7789 预渲染视频播放器

基于 **ESP-IDF** 的 ST7789 TFT LCD 显示项目，从内部 Flash 读取预渲染 RGB565 视频并循环播放。

## 硬件接线

| 外设 | ESP32-S3 GPIO |
|------|---------------|
| ST7789 MOSI | GPIO 18 |
| ST7789 CLK | GPIO 21 |
| ST7789 CS | GPIO 14 |
| ST7789 DC | GPIO 15 |
| ST7789 RST | GPIO 16 |
| ST7789 BL | GPIO 17 |

> 默认分辨率 172×320，如需修改引脚或参数见 main/main.c 顶部宏定义。

## 快速开始

### 1. 配置环境

安装 ESP-IDF 后，在终端配置环境：

`ash
. /export.sh
`

### 2. 构建并烧录固件

`ash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
`

将 PORT 替换为实际串口（Windows 下为 COMx，Linux 下为 /dev/ttyUSB0）。

### 3. 生成视频

`ash
python tools/generate_video.py --frames 60 --width 172 --height 320
`

### 4. 烧录视频到内部 Flash

`ash
python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x200000 videos/video_with_header.bin
`

### 5. 播放

按 RST 按钮重启，自动播放。

## 视频规格

- 分辨率：172 × 320
- 色彩格式：RGB565（16位）
- 帧数：60 帧（可自定义）
- 存储：ESP32 内部 Flash（video 分区，7MB）

## 分区表

`
0x010000  factory app（固件，1MB）
0x200000  video 分区（视频，7MB）
`

## 项目结构

`
├── main/main.c               # 固件主程序
├── tools/generate_video.py   # 视频生成工具
├── tools/flash_video_esp.bat # esptool 烧录脚本
├── partitions.csv            # 分区表
└── videos/                   # 视频帧数据
`

## License

MIT
