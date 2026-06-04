/**
 * @file main.c
 * @brief ESP32-S3 + W25Q128 (Flash) + ST7789 (LCD) 总成演示
 *
 * 架构：
 *   SPI2_HOST — W25Q128 NOR Flash（模式 0，500kHz 低速求稳）
 *   SPI3_HOST — ST7789 TFT LCD （模式 3，20MHz 高速刷新）
 *
 * 功能：
 *   1. 检测 W25Q128 是否存在（读取 JEDEC ID）
 *   2. 初始化 ST7789（240×240 RGB565）
 *   3. 从 Flash 读取图像数据 → 显示到 LCD
 *   4. 如果 Flash 中无有效图像 → 生成内置测试图案并写入 Flash
 *   5. 写入后回读验证
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"

/* ======================= 日志标签 ======================= */
static const char *TAG = "lolovivi";

/* ======================= 引脚定义（按需修改） ======================= */

/* --- W25Q128 (SPI2_HOST, 模式 0) --- */
#define PIN_FLASH_MOSI  11
#define PIN_FLASH_MISO  13
#define PIN_FLASH_CLK   12
#define PIN_FLASH_CS    10

/* --- ST7789 (SPI3_HOST, 模式 3) --- */
#define PIN_LCD_MOSI    18
#define PIN_LCD_CLK     21
#define PIN_LCD_CS      14
#define PIN_LCD_DC      15   /* 1=数据, 0=命令 */
#define PIN_LCD_RST     16   /* 低电平复位 */
#define PIN_LCD_BL      17   /* PWM 背光（高电平点亮） */

/* ======================= SPI & Flash 参数 ======================= */

#define FLASH_SPI_HOST  SPI2_HOST
#define FLASH_SPI_FREQ  (500 * 1000)     /* 500 kHz */

#define LCD_SPI_HOST    SPI3_HOST
#define LCD_SPI_FREQ    (80 * 1000 * 1000)  /* 拉满到 80MHz！ */

/* Flash 存储参数 */
#define FLASH_BASE_ADDR 0x00000000
#define SECTOR_SIZE     4096
#define PAGE_SIZE       256

/* 显示参数 */
#define LCD_WIDTH       172
#define LCD_HEIGHT      320
#define LCD_OFFSET_X   34   /* (240-172)/2，居中偏移 */
#define LCD_OFFSET_Y   0
#define LCD_BPP         16

/* 图像头 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t data_size;  /* 像素数据字节数 */
    uint32_t bpp;        /* 每像素位数 */
    uint32_t magic;      /* 魔数 0x4C4F5649 ("LOVI") */
} __attribute__((packed)) image_header_t;

#define IMAGE_MAGIC     0x4C4F5649

/* 视频标记 — Flash 0x10000 处存 8 字节：[magic][帧数] */
#define RENDER_MAGIC    0x44495256  /* "VRDI" = 新版本标记，区别于旧版 "IVER" */

/* ======================= 句柄 ======================= */

static spi_device_handle_t g_flash = NULL;   /* W25Q128 设备句柄 */
static spi_device_handle_t g_lcd   = NULL;   /* ST7789   设备句柄 */

/* ================================================================ */
/*                    W25Q128 Flash 驱动                              */
/* ================================================================ */

/** SPI 初始化 — W25Q128 */
static esp_err_t flash_spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_FLASH_MOSI,
        .miso_io_num     = PIN_FLASH_MISO,
        .sclk_io_num     = PIN_FLASH_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = FLASH_SPI_FREQ,
        .mode           = 0,              /* SPI 模式 0 */
        .spics_io_num   = PIN_FLASH_CS,
        .queue_size     = 1,
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .flags          = 0,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(FLASH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(FLASH_SPI_HOST, &devcfg, &g_flash));

    ESP_LOGI(TAG, "Flash SPI initialized (SPI2, mode=0, %d Hz)", FLASH_SPI_FREQ);
    return ESP_OK;
}

/* -------- Flash 基础 SPI 收发 -------- */

static esp_err_t flash_tx(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.length    = len * 8;
    t.tx_buffer = data;
    return spi_device_transmit(g_flash, &t);
}

static esp_err_t flash_txrx(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {0};
    t.length    = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(g_flash, &t);
}

/* -------- W25Q128 指令封装 -------- */

static __attribute__((unused)) esp_err_t w25q_read_jedec_id(uint8_t *mid, uint8_t *did1, uint8_t *did2)
{
    uint8_t tx[4] = {0x9F, 0x00, 0x00, 0x00};
    uint8_t rx[4] = {0};
    ESP_ERROR_CHECK(flash_txrx(tx, rx, 4));
    *mid  = rx[1];
    *did1 = rx[2];
    *did2 = rx[3];
    return ESP_OK;
}

static esp_err_t w25q_write_enable(void)
{
    uint8_t cmd = 0x06;
    return flash_tx(&cmd, 1);
}

static esp_err_t w25q_read_status(uint8_t *sr)
{
    uint8_t tx[2] = {0x05, 0x00};
    uint8_t rx[2] = {0};
    ESP_ERROR_CHECK(flash_txrx(tx, rx, 2));
    *sr = rx[1];
    return ESP_OK;
}

static esp_err_t w25q_wait_busy(uint32_t timeout_ms)
{
    uint8_t sr = 0;
    uint32_t elapsed = 0;
    while (1) {
        ESP_ERROR_CHECK(w25q_read_status(&sr));
        if (!(sr & 0x01)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
        if (elapsed >= timeout_ms) {
            ESP_LOGE(TAG, "Flash busy timeout, SR=0x%02X", sr);
            return ESP_ERR_TIMEOUT;
        }
    }
}

static esp_err_t w25q_sector_erase(uint32_t addr)
{
    uint8_t cmd[4];
    ESP_ERROR_CHECK(w25q_write_enable());
    cmd[0] = 0x20;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >>  8) & 0xFF;
    cmd[3] = (addr)       & 0xFF;
    ESP_ERROR_CHECK(flash_tx(cmd, 4));
    return w25q_wait_busy(5000);
}

static esp_err_t w25q_page_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if (len == 0 || len > PAGE_SIZE) return ESP_ERR_INVALID_ARG;

    uint8_t *buf = (uint8_t *)malloc(4 + len);
    if (!buf) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(w25q_write_enable());
    buf[0] = 0x02;
    buf[1] = (addr >> 16) & 0xFF;
    buf[2] = (addr >>  8) & 0xFF;
    buf[3] = (addr)       & 0xFF;
    memcpy(buf + 4, data, len);

    esp_err_t ret = flash_tx(buf, 4 + len);
    free(buf);
    if (ret != ESP_OK) return ret;
    return w25q_wait_busy(1000);
}

static const esp_partition_t *g_video_part = NULL;

static esp_err_t w25q_read_data(uint32_t addr, uint8_t *out, size_t len)
{
    if (!g_video_part) {
        g_video_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "video");
        if (!g_video_part) {
            ESP_LOGE(TAG, "Video partition not found!");
            return ESP_ERR_NOT_FOUND;
        }
    }
    return esp_partition_read(g_video_part, addr, out, len);
}

/* -------- 高级 Flash 操作 -------- */

static esp_err_t flash_write_buffer(uint32_t addr, const uint8_t *data, size_t len)
{
    if (!g_video_part) {
        g_video_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "video");
        if (!g_video_part) return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_write(g_video_part, addr, data, len);
}

static esp_err_t flash_erase_range(uint32_t addr, size_t len)
{
    if (!g_video_part) {
        g_video_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "video");
        if (!g_video_part) return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_erase_range(g_video_part, addr, len);
}

/* ================================================================ */
/*                    ST7789 LCD 驱动                                */
/* ================================================================ */

/** SPI 初始化 — ST7789 */
static esp_err_t lcd_spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,              /* LCD 不需要 MISO */
        .sclk_io_num     = PIN_LCD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2 + 8,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_FREQ,
        .mode           = 3,               /* SPI 模式 3 (CPOL=1, CPHA=1) */
        .spics_io_num   = PIN_LCD_CS,
        .queue_size     = 2,
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .flags          = SPI_DEVICE_HALFDUPLEX,  /* LCD 是单向写入 */
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI_HOST, &devcfg, &g_lcd));

    ESP_LOGI(TAG, "LCD SPI initialized (SPI3, mode=3, %d Hz)", LCD_SPI_FREQ);
    return ESP_OK;
}

/* -------- GPIO 初始化 -------- */

static void lcd_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC) |
                        (1ULL << PIN_LCD_RST) |
                        (1ULL << PIN_LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(PIN_LCD_RST, 1);
    gpio_set_level(PIN_LCD_DC,  0);
    gpio_set_level(PIN_LCD_BL,  1);  /* 背光默认点亮 */
}

/* -------- LCD SPI 发送 -------- */

static esp_err_t lcd_tx(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.length    = len * 8;
    t.tx_buffer = data;
    return spi_device_transmit(g_lcd, &t);
}

static void lcd_write_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);  /* 命令模式 */
    lcd_tx(&cmd, 1);
}

static void lcd_write_data(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_LCD_DC, 1);  /* 数据模式 */
    lcd_tx(data, len);
}

static void lcd_write_data_byte(uint8_t data)
{
    lcd_write_data(&data, 1);
}

/* -------- ST7789 初始化序列 -------- */

static void lcd_hardware_reset(void)
{
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void lcd_init(void)
{
    lcd_hardware_reset();

    /* ST7789V 标准初始化序列 */
    lcd_write_cmd(0x01);          /* SWRESET 软件复位 */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_write_cmd(0x11);          /* SLPOUT 休眠退出 */
    vTaskDelay(pdMS_TO_TICKS(200));

    lcd_write_cmd(0x36);          /* MADCTL 内存数据访问控制 */
    lcd_write_data_byte(0x00);    /* RGB 顺序, 正常方向 */

    lcd_write_cmd(0x3A);          /* COLMOD 像素格式 */
    lcd_write_data_byte(0x05);    /* 16-bit RGB565 */

    lcd_write_cmd(0xB2);          /* PORCH  porch 设置 */
    {
        uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
        lcd_write_data(d, 5);
    }

    lcd_write_cmd(0xB7);          /* GCTRL 栅极控制 */
    lcd_write_data_byte(0x35);

    lcd_write_cmd(0xBB);          /* VCOMS VCOM 设置 */
    lcd_write_data_byte(0x35);    /* 0x28~0x35, 视模块而定 */

    lcd_write_cmd(0xC0);          /* LCMCTRL LCM 控制 */
    lcd_write_data_byte(0x2C);

    lcd_write_cmd(0xC2);          /* VDVVRHEN */
    lcd_write_data_byte(0x01);

    lcd_write_cmd(0xC3);          /* VRHS */
    lcd_write_data_byte(0x13);

    lcd_write_cmd(0xC4);          /* VDVS */
    lcd_write_data_byte(0x20);

    lcd_write_cmd(0xC6);          /* FRCTRL2 帧率控制 */
    lcd_write_data_byte(0x0F);

    lcd_write_cmd(0xD0);          /* PWCTRL1 电源控制 */
    {
        uint8_t d[] = {0xA4, 0xA1};
        lcd_write_data(d, 2);
    }

    lcd_write_cmd(0x21);          /* INVON 显示反转（IPS 屏需要） */
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_write_cmd(0x13);          /* NORON 正常显示模式 */
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_write_cmd(0x29);          /* DISPON 显示开启 */
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ST7789 initialized");
}

/* -------- 设置绘图窗口 -------- */

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* 列地址 */
    lcd_write_cmd(0x2A);
    {
        uint8_t d[4] = {
            (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
        };
        lcd_write_data(d, 4);
    }

    /* 行地址 */
    lcd_write_cmd(0x2B);
    {
        uint8_t d[4] = {
            (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
            (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
        };
        lcd_write_data(d, 4);
    }

    /* 开始写内存 */
    lcd_write_cmd(0x2C);
}

/* -------- 填充全屏颜色 -------- */

static __attribute__((unused)) void lcd_fill_color(uint16_t color)
{
    uint32_t pixels = (uint32_t)LCD_WIDTH * LCD_HEIGHT;
    uint32_t chunk  = 4096;     /* 分块发送避免缓冲区过载 */
    uint8_t *buf    = (uint8_t *)malloc(chunk * 2);
    if (!buf) {
        ESP_LOGE(TAG, "fill_color: OOM");
        return;
    }

    for (uint32_t i = 0; i < chunk; i++) {
        buf[i * 2]     = (uint8_t)(color >> 8);
        buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    uint32_t remaining = pixels;
    while (remaining > 0) {
        uint32_t send = (remaining > chunk) ? chunk : remaining;
        lcd_write_data(buf, send * 2);
        remaining -= send;
    }

    free(buf);
}

/* -------- 显示 RGB565 图像数据（已弃用，改用逐行读取版） -------- */

static __attribute__((unused)) void lcd_draw_image_from_flash(image_header_t *hdr)
{
    uint32_t w = hdr->width;
    uint32_t h = hdr->height;
    size_t row_bytes = (size_t)w * 2;     /* 一行 RGB565 的字节数 */
    uint8_t *row_buf = (uint8_t *)malloc(row_bytes);

    if (!row_buf) {
        ESP_LOGE(TAG, "Cannot allocate row buffer (%zu bytes)", row_bytes);
        return;
    }

    ESP_LOGI(TAG, "Reading image from flash: %" PRIu32 "x%" PRIu32, w, h);
    lcd_set_window(0, 0, (uint16_t)(w - 1), (uint16_t)(h - 1));

    uint32_t flash_addr = FLASH_BASE_ADDR + sizeof(image_header_t);
    for (uint32_t y = 0; y < h; y++) {
        esp_err_t ret = w25q_read_data(flash_addr + y * row_bytes, row_buf, row_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Flash read failed at row %" PRIu32 ": %s", y, esp_err_to_name(ret));
            free(row_buf);
            return;
        }
        lcd_write_data(row_buf, row_bytes);
    }

    free(row_buf);
    ESP_LOGI(TAG, "Image displayed on LCD");
}

/* ================================================================ */
/*                    内置工具函数                                   */
/* ================================================================ */

/** 将 RGB (8:8:8) 转换为 RGB565 (16-bit) */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ================================================================ */
/*                    交互式边界校准                                 */
/* ================================================================ */

#include "driver/uart.h"

/**
 * 交互式边界校准——按键调白线，即时响应
 * 使用 uart_get_buffered_data_len 非阻塞查键盘
 * 使用预分配缓冲区 + 单次 SPI 传输，毫秒级重绘
 */
static __attribute__((unused)) void lcd_interactive_calibrate(void)
{
    int16_t t=0, b=319, l=0, r=319;
    int16_t pt=0, pb=319, pl=0, pr=319;

    /* 预分配线缓冲区 */
    uint8_t *white = (uint8_t *)malloc(640);  /* 320 * 2 */
    uint8_t *black = (uint8_t *)malloc(640);
    if (!white || !black) { ESP_LOGE(TAG,"OOM"); while(1); }
    memset(white, 0xFF, 640);
    memset(black, 0, 640);

    /* 快速 SPI 写线 */
    #define SPI_LINE(x0,x1,y, buf) do { \
        lcd_set_window(x0,y,x1,y); \
        gpio_set_level(PIN_LCD_DC,1); \
        spi_transaction_t _t={0}; \
        _t.length=(x1-x0+1)*2*8; _t.tx_buffer=buf; \
        spi_device_transmit(g_lcd,&_t); \
    } while(0)

    #define SPI_COL(x,y0,y1, buf) do { \
        lcd_set_window(x,y0,x,y1); \
        gpio_set_level(PIN_LCD_DC,1); \
        spi_transaction_t _t={0}; \
        _t.length=(y1-y0+1)*2*8; _t.tx_buffer=buf; \
        spi_device_transmit(g_lcd,&_t); \
    } while(0)

    /* 刷黑板：一次画好 */
    pt=0; pb=319; pl=0; pr=319; /* 假装这些是旧值，实际画满 */
    ESP_LOGW(TAG, "正在初始化屏幕...约3秒");
    {
        uint8_t *bg = (uint8_t *)malloc(8192);
        if (bg) {
            memset(bg, 0, 8192);
            lcd_set_window(0, 0, 319, 319);
            for (int off = 0; off < 320*320; off += 4096) {
                int cnt = (off + 4096 <= 320*320) ? 4096 : (320*320 - off);
                gpio_set_level(PIN_LCD_DC, 1);
                spi_transaction_t tx = {0};
                tx.length = cnt * 2 * 8;
                tx.tx_buffer = bg;
                spi_device_transmit(g_lcd, &tx);
            }
            free(bg);
        }
    }

    /* 画初始四条白线 */
    SPI_LINE(0,319, t, white);
    SPI_LINE(0,319, b, white);
    SPI_COL(l, 0,319, white);
    SPI_COL(r, 0,319, white);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  交互校准：直接按键调白线         ║");
    ESP_LOGI(TAG, "╠════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ W/S上  I/K下  A/D左  J/L右       ║");
    ESP_LOGI(TAG, "║ P=打印  R=重置                   ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════╝");

    while (1) {
        /* 非阻塞查键盘：有数据才读，没有就等20ms */
        size_t avail;
        esp_err_t ue = uart_get_buffered_data_len(UART_NUM_0, &avail);
        if (ue != ESP_OK || avail == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int ch = getchar();  /* 不会阻塞，数据已经在缓冲区 */

        pt=t; pb=b; pl=l; pr=r;

        if (ch=='w'||ch=='W') t -= (ch=='W'?10:1);
        else if (ch=='s'||ch=='S') t += (ch=='S'?10:1);
        else if (ch=='i'||ch=='I') b -= (ch=='I'?10:1);
        else if (ch=='k'||ch=='K') b += (ch=='K'?10:1);
        else if (ch=='a'||ch=='A') l -= (ch=='A'?10:1);
        else if (ch=='d'||ch=='D') l += (ch=='D'?10:1);
        else if (ch=='j'||ch=='J') r -= (ch=='J'?10:1);
        else if (ch=='l'||ch=='L') r += (ch=='L'?10:1);
        else if (ch=='p') { ESP_LOGI(TAG,">>> %dx%d (T%d B%d L%d R%d)", r-l+1,b-t+1,t,b,l,r); continue; }
        else if (ch=='r') { t=0;b=319;l=0;r=319;
            SPI_LINE(0,319, pt, black); SPI_LINE(0,319, pb, black);
            SPI_COL(pl,0,319, black); SPI_COL(pr,0,319, black);
            SPI_LINE(0,319, t, white); SPI_LINE(0,319, b, white);
            SPI_COL(l,0,319, white); SPI_COL(r,0,319, white);
            ESP_LOGI(TAG,"已重置"); continue;
        }
        else continue;

        if (t < 0) t = 0;
        if (b > 319) b = 319;
        if (t >= b - 5) t = b - 5;
        if (l < 0) l = 0;
        if (r > 319) r = 319;
        if (l >= r - 5) l = r - 5;

        if(pt!=t){ SPI_LINE(0,319, pt, black); SPI_LINE(0,319, t, white); }
        if(pb!=b){ SPI_LINE(0,319, pb, black); SPI_LINE(0,319, b, white); }
        if(pl!=l){ SPI_COL(pl,0,319, black); SPI_COL(l,0,319, white); }
        if(pr!=r){ SPI_COL(pr,0,319, black); SPI_COL(r,0,319, white); }

        ESP_LOGI(TAG,"T%3d B%3d L%3d R%3d  =>  %dx%d",t,b,l,r,r-l+1,b-t+1);
    }
    free(white); free(black);
}

/**
 * 生成 LoloVivi 品牌测试图案（色彩渐变 + 品牌文字区域）
 * 将数据直接写入 Flash
 */
static __attribute__((unused)) esp_err_t generate_and_store_test_pattern(image_header_t *hdr)
{
    uint32_t w = hdr->width;
    uint32_t h = hdr->height;
    size_t   total_bytes = (size_t)w * h * 2;

    ESP_LOGI(TAG, "Generating test pattern %" PRIu32 "x%" PRIu32 " (%zu bytes)...",
             w, h, total_bytes);

    /* 擦除目标区域 */
    ESP_ERROR_CHECK(flash_erase_range(FLASH_BASE_ADDR,
                     sizeof(image_header_t) + total_bytes));

    /* 逐行生成并写入 Flash（避免一次性 malloc 115KB） */
    uint8_t *row_buf = (uint8_t *)malloc((size_t)w * 2);
    if (!row_buf) return ESP_ERR_NO_MEM;

    /* 写图像头 */
    ESP_LOGI(TAG, "Writing image header...");
    ESP_ERROR_CHECK(flash_write_buffer(FLASH_BASE_ADDR,
                     (const uint8_t *)hdr, sizeof(image_header_t)));

    uint32_t addr = FLASH_BASE_ADDR + sizeof(image_header_t);

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint16_t color;

            /* --- 渐变背景 (HSL-inspired) --- */
            float hue = (float)x / (float)w;
            uint8_t r = (uint8_t)(127.5f * (1 + cosf(2 * M_PI * (hue - 0.0f / 3.0f))));
            uint8_t g = (uint8_t)(127.5f * (1 + cosf(2 * M_PI * (hue - 1.0f / 3.0f))));
            uint8_t b = (uint8_t)(127.5f * (1 + cosf(2 * M_PI * (hue - 2.0f / 3.0f))));

            /* --- 亮度从上到下渐变（底部暗） --- */
            float brightness = 1.0f - 0.5f * (float)y / (float)h;
            r = (uint8_t)(r * brightness);
            g = (uint8_t)(g * brightness);
            b = (uint8_t)(b * brightness);

            /* --- 棋盘格网格线（每 16 像素） --- */
            if ((x % 32 < 2) || (y % 32 < 2)) {
                r = r > 30 ? r - 30 : 0;
                g = g > 30 ? g - 30 : 0;
                b = b > 30 ? b - 30 : 0;
            }

            /* --- 边框 4px --- */
            if (x < 4 || x >= w - 4 || y < 4 || y >= h - 4) {
                color = rgb565(255, 255, 255);
            } else {
                color = rgb565(r, g, b);
            }

            /* 行缓冲写入 */
            row_buf[x * 2]     = (uint8_t)(color >> 8);
            row_buf[x * 2 + 1] = (uint8_t)(color & 0xFF);
        }

        /* 写一行到 Flash */
        ESP_ERROR_CHECK(flash_write_buffer(addr, row_buf, (size_t)w * 2));
        addr += (size_t)w * 2;

        if (y % 20 == 0) {
            ESP_LOGI(TAG, "Writing row %" PRIu32 "/%" PRIu32, y + 1, h);
        }
    }

    free(row_buf);
    ESP_LOGI(TAG, "Test pattern stored to flash");
    return ESP_OK;
}

/* ================================================================ */
/*                       主程序                                     */
/* ================================================================ */

/** 20×20 色块 + 1px 白色网格线，方便数分辨率 */
static __attribute__((unused)) void lcd_draw_hsl_pattern(uint16_t w, uint16_t h)
{
    ESP_LOGI(TAG, "Drawing grid pattern %dx%d...", w, h);

    uint8_t *row = (uint8_t *)malloc((size_t)w * 2);
    if (!row) { ESP_LOGE(TAG, "OOM"); return; }

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            /* 网格线：每 20 像素一条 1px 白线 */
            if (x % 21 == 0 || y % 21 == 0) {
                row[x * 2] = 0xFF; row[x * 2 + 1] = 0xFF;
                continue;
            }

            /* 20x20 色块 */
            float hue = (float)(x / 21 * 20) / (float)w;
            uint8_t r = (uint8_t)(127.5f * (1 + cosf(2 * M_PI * hue)));
            uint8_t g = (uint8_t)(127.5f * (1 + cosf(2 * M_PI * (hue - 1.0f/3.0f))));
            uint8_t b = (uint8_t)(127.5f * (1 + cosf(2 * M_PI * (hue - 2.0f/3.0f))));

            float bright = 1.0f - 0.5f * (float)(y / 21 * 20) / (float)h;
            r = (uint8_t)(r * bright);
            g = (uint8_t)(g * bright);
            b = (uint8_t)(b * bright);

            uint16_t color = rgb565(r, g, b);
            row[x * 2]     = (uint8_t)(color >> 8);
            row[x * 2 + 1] = (uint8_t)(color & 0xFF);
        }
        lcd_write_data(row, (size_t)w * 2);
    }
    free(row);
    ESP_LOGI(TAG, "Grid pattern displayed — 数一下横竖各有多少个色块？");
}

void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  LoloVivi — 预渲染视频播放器");
    ESP_LOGI(TAG, "============================================");

    /* === 1. 硬件初始化 === */
    lcd_gpio_init();
    ESP_ERROR_CHECK(flash_spi_init());
    ESP_ERROR_CHECK(lcd_spi_init());
    lcd_init();

    int w = LCD_WIDTH, h = LCD_HEIGHT;
    size_t fb_size = (size_t)w * h * 2;
    esp_task_wdt_deinit();

    uint8_t *fb = (uint8_t *)malloc(fb_size);
    if (!fb) { ESP_LOGE(TAG, "OOM"); while(1); }

    /* === 2. ?? Flash ?????? === */
    uint32_t marker = 0, num_frames = 0;
    w25q_read_data(FLASH_BASE_ADDR, (uint8_t *)&marker, 4);
    if (marker == RENDER_MAGIC) {
        /* Flash ????????????? */
        w25q_read_data(FLASH_BASE_ADDR + 4, (uint8_t *)&num_frames, 4);
        ESP_LOGI(TAG, "Video found, start playback");
        goto playback;
    }

    /* === 3. ????????????? === */
    ESP_LOGI(TAG, "No video found, entering recording mode");
    vTaskDelay(pdMS_TO_TICKS(5000));
    printf("RECORD_READY\n");

    /* 读 8 字节视频头 */
    uint8_t hdr[8];
    for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)getchar();
    printf("GOT_HEADER\n");  /* 确认收到头 */
    fflush(stdout);
    num_frames   = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
    uint32_t frame_size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);

    if (frame_size != fb_size) {
        fb_size = frame_size;
        free(fb);
        fb = (uint8_t *)malloc(fb_size);
        if (!fb) { while(1); }
    }

        /* 先写标记（只擦标记所在的一个扇区） */
    /* 直接擦除标记所在扇区（内部 Flash） */
    if (g_video_part) esp_partition_erase_range(g_video_part, FLASH_BASE_ADDR, SECTOR_SIZE);
    marker = RENDER_MAGIC;
    flash_write_buffer(FLASH_BASE_ADDR, (uint8_t *)&marker, 4);
    flash_write_buffer(FLASH_BASE_ADDR + 4, (uint8_t *)&num_frames, 4);

    /* 立即通知 PC 可以发数据 */
    printf("RDY\n");
    fflush(stdout);

    /* 逐帧接收：边擦边写，不需要提前擦完全部 */
    uint32_t addr = FLASH_BASE_ADDR + 8;
    for (uint32_t i = 0; i < num_frames; i++) {
        /* 接收一帧 */
        for (size_t j = 0; j < frame_size; j++) fb[j] = (uint8_t)getchar();

        /* 列擦除这帧要用的扇区 */
        uint32_t frame_start = addr;
        uint32_t frame_end = addr + frame_size;
        for (uint32_t sa = frame_start; sa < frame_end; sa += SECTOR_SIZE) {
            if (g_video_part) esp_partition_erase_range(g_video_part, sa, SECTOR_SIZE);
        }

        /* 写入这帧 */
        flash_write_buffer(addr, fb, frame_size);
        addr += frame_size;
        printf("OK\n");
    }

playback:
    /* === 4. 播放 === */
    lcd_set_window(LCD_OFFSET_X, LCD_OFFSET_Y, (uint16_t)(LCD_OFFSET_X + w - 1), (uint16_t)(LCD_OFFSET_Y + h - 1));

    uint32_t fi = 0;
    uint32_t data_ofs = FLASH_BASE_ADDR + 8;
    int64_t t_last = esp_timer_get_time();
    int total_frames = 0;

    while (1) {
        w25q_read_data(data_ofs + fi * fb_size, fb, fb_size);
        for (size_t off = 0; off < fb_size; off += 4096) {
            size_t sz = (off + 4096 <= fb_size) ? 4096 : (fb_size - off);
            lcd_write_data(fb + off, sz);
        }
        fi = (fi + 1) % num_frames;
        total_frames++;

        int64_t now = esp_timer_get_time();
        if (now - t_last >= 1000000) {
            printf("FPS: %d\n", total_frames);
            total_frames = 0;
            t_last = now;
        }
    }
    free(fb);
}
