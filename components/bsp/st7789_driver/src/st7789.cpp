/**
 * @file st7789.cpp
 * @brief ST7789V 显示屏驱动 (240x135)
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "st7789.h"
#include "st7789_commands.h"
#include "pwm.h"
#include "Interp.hpp"
#include "backlight_lut.h"
#include <algorithm>

namespace ST7789 {

static const char* TAG = "ST7789";

static constexpr uint32_t SPI_CLOCK_SPEED_HZ = 50 * 1000 * 1000;
static constexpr uint32_t MAX_TRANSFER_SIZE  = WIDTH * HEIGHT * 2 + 8;
// ESP32-C6 的 GPSPI DMA 单次事务长度寄存器为 18 bit，即最多 32768 字节。
// 一帧 240x135 RGB565 数据为 64800 字节，因此必须拆分发送。
static constexpr size_t SPI_DMA_CHUNK_SIZE = 32 * 1024;

static spi_device_handle_t spi            = NULL;
static gpio_num_t          dc_pin         = GPIO_NUM_NC;
static gpio_num_t          rst_pin        = GPIO_NUM_NC;
static uint8_t             colstart       = COLSTART;
static uint8_t             rowstart       = ROWSTART;
static uint16_t            display_width  = WIDTH;
static uint16_t            display_height = HEIGHT;

static pwm_t                                 backlight_pwm;
static EquidistantInterp<uint8_t, uint16_t>* backlight_interp      = nullptr;
static uint8_t                               current_brightness    = 0;
static bool                                  backlight_initialized = false;
static bool                                  bl_active_low         = false;

// 显示契约：单帧全屏 RGB565 缓冲，由同一个 UI 任务串行绘制并同步发送。
// Pro V2 双帧会多占 64,800 B 静态 RAM，联网后曾挤占 Wi-Fi 所需的 DMA 堆。
// 保持 FRAME_BUFFER_COUNT=1；改为双帧或异步 DMA/多任务绘制前必须重新设计缓冲所有权。
static constexpr size_t FRAME_BUFFER_COUNT = 1;
struct double_buffer_t {
    uint16_t data[FRAME_BUFFER_COUNT][WIDTH * HEIGHT];
    uint8_t  current_buffer = 0;
} double_buffer;

static void write_command(uint8_t cmd) {
    spi_transaction_t t = {};
    t.length            = 8;
    t.tx_buffer         = &cmd;
    gpio_set_level(dc_pin, 0);
    spi_device_polling_transmit(spi, &t);
}

static void write_data(const uint8_t* data, size_t len) {
    gpio_set_level(dc_pin, 1);
    while (len > 0) {
        const size_t chunk_len = std::min(len, SPI_DMA_CHUNK_SIZE);
        spi_transaction_t t    = {};
        t.length               = chunk_len * 8;
        t.tx_buffer            = data;
        const esp_err_t ret    = spi_device_polling_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI data transmit failed: %s, chunk=%u", esp_err_to_name(ret),
                     static_cast<unsigned>(chunk_len));
            return;
        }
        data += chunk_len;
        len  -= chunk_len;
    }
}

static inline void write_data_byte(uint8_t byte) {
    write_data(&byte, 1);
}

static void set_address_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t data[4];
    write_command(ST7789_CASET);
    data[0] = (x0 + colstart) >> 8;
    data[1] = (x0 + colstart) & 0xFF;
    data[2] = (x1 + colstart) >> 8;
    data[3] = (x1 + colstart) & 0xFF;
    write_data(data, 4);
    write_command(ST7789_RASET);
    data[0] = (y0 + rowstart) >> 8;
    data[1] = (y0 + rowstart) & 0xFF;
    data[2] = (y1 + rowstart) >> 8;
    data[3] = (y1 + rowstart) & 0xFF;
    write_data(data, 4);
    write_command(ST7789_RAMWR);
}

void switch_buffers() {
    if constexpr (FRAME_BUFFER_COUNT == 2) {
        double_buffer.current_buffer = 1 - double_buffer.current_buffer;
    }
}

void copy_buffers() {
    if constexpr (FRAME_BUFFER_COUNT == 2) {
        memcpy(double_buffer.data[1 - double_buffer.current_buffer], double_buffer.data[double_buffer.current_buffer],
               display_width * display_height * 2);
    }
}

void sync_buffers() {
    set_address_window(0, 0, display_width - 1, display_height - 1);
    write_data((uint8_t*)double_buffer.data[double_buffer.current_buffer], display_width * display_height * 2);
    switch_buffers();
}

esp_err_t init(const Config* cfg, Rotation rotation) {
    esp_err_t ret;
    dc_pin  = static_cast<gpio_num_t>(cfg->dc_io_num);
    rst_pin = static_cast<gpio_num_t>(cfg->rst_io_num);

    ESP_LOGD(TAG, "ST7789V Driver - 1.14 inch TFT 240x135");
    ESP_LOGD(TAG, "PINS: MOSI=%d CLK=%d CS=%d DC=%d RST=%d BL=%d", cfg->mosi_io_num, cfg->sclk_io_num, cfg->cs_io_num,
             cfg->dc_io_num, cfg->rst_io_num, cfg->bl_io_num);

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = (1ULL << dc_pin) | (1ULL << rst_pin);
    io_conf.mode          = GPIO_MODE_OUTPUT;

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (cfg->bl_io_num >= 0) {
        bl_active_low    = !cfg->bl_active_state;
        backlight_interp = new EquidistantInterp<uint8_t, uint16_t>(backlight_lut);
        backlight_pwm.init(static_cast<gpio_num_t>(cfg->bl_io_num));
        backlight_initialized = true;
        set_backlight(0);
        ESP_LOGD(TAG, "Backlight PWM enabled (active %s)", bl_active_low ? "low" : "high");
    }

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num      = cfg->mosi_io_num;
    buscfg.miso_io_num      = -1;
    buscfg.sclk_io_num      = cfg->sclk_io_num;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = MAX_TRANSFER_SIZE;

    spi_device_interface_config_t devcfg = {};
    devcfg.mode                          = 0;
    devcfg.clock_speed_hz                = SPI_CLOCK_SPEED_HZ;
    devcfg.spics_io_num                  = static_cast<gpio_num_t>(cfg->cs_io_num);
    devcfg.flags                         = SPI_DEVICE_NO_DUMMY;
    devcfg.queue_size                    = 7;

    ret = spi_bus_initialize(cfg->host_id, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = spi_bus_add_device(cfg->host_id, &devcfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGD(TAG, "SPI @ %d MHz", SPI_CLOCK_SPEED_HZ / 1000000);

    /* ST7789V 上电和硬复位时序。 */
    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    write_command(ST7789_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    write_command(ST7789_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    write_command(ST7789_COLMOD);
    write_data_byte(0x05);

    // Porch, gate、VCOM 和电源参数采用常见 1.14 英寸 ST7789V 模组推荐值。
    write_command(0xB2);
    { uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; write_data(d, sizeof(d)); }
    write_command(0xB7); write_data_byte(0x35);
    write_command(0xBB); write_data_byte(0x19);
    write_command(0xC0); write_data_byte(0x2C);
    write_command(0xC2); write_data_byte(0x01);
    write_command(0xC3); write_data_byte(0x12);
    write_command(0xC4); write_data_byte(0x20);
    write_command(0xC6); write_data_byte(0x0F);
    write_command(0xD0);
    { uint8_t d[] = {0xA4, 0xA1}; write_data(d, sizeof(d)); }

    set_rotation(rotation);

    write_command(ST7789_GMCTRP1);
    {
        uint8_t d[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
        write_data(d, sizeof(d));
    }
    write_command(ST7789_GMCTRN1);
    {
        uint8_t d[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
        write_data(d, sizeof(d));
    }

    write_command(ST7789_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    write_command(ST7789_INVON);
    write_command(ST7789_DISPON);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGD(TAG, "screen setup success: %dx%d pixels", display_width, display_height);
    return ESP_OK;
}

void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, color_t color) {
    if (x >= display_width || y >= display_height)
        return;
    if (x + w > display_width)
        w = display_width - x;
    if (y + h > display_height)
        h = display_height - y;

    for (uint16_t row = y; row < y + h; row++) {
        for (uint16_t col = x; col < x + w; col++) {
            double_buffer.data[double_buffer.current_buffer][row * display_width + col] =
                color.get_color_raw_big_endian();
        }
    }
}

/**
 * @brief 按透明度混合两个 RGB565 颜色。
 * @param bg 背景色 RGB565 原始值。
 * @param color 前景色 RGB565 原始值。
 * @param alpha 前景色透明度，范围 0-255。
 * @return 混合后的 RGB565 原始值。
 */
static uint16_t blend_aa_rgb565(uint16_t bg, uint16_t color, uint8_t alpha) {
    const uint16_t bg_r    = (bg >> 11) & 0x1F;
    const uint16_t bg_g    = (bg >> 5) & 0x3F;
    const uint16_t bg_b    = bg & 0x1F;
    const uint16_t color_r = (color >> 11) & 0x1F;
    const uint16_t color_g = (color >> 5) & 0x3F;
    const uint16_t color_b = color & 0x1F;

    const uint16_t result_r = (bg_r * (255 - alpha) + color_r * alpha + 127) / 255;
    const uint16_t result_g = (bg_g * (255 - alpha) + color_g * alpha + 127) / 255;
    const uint16_t result_b = (bg_b * (255 - alpha) + color_b * alpha + 127) / 255;
    return uint16_t(result_r << 11) | uint16_t(result_g << 5) | result_b;
}

/**
 * @brief 计算像素被圆角矩形覆盖的比例。
 * @param pixel_x 相对圆角矩形左上角的 X 坐标。
 * @param pixel_y 相对圆角矩形左上角的 Y 坐标。
 * @param w 圆角矩形宽度。
 * @param h 圆角矩形高度。
 * @param radius 圆角半径。
 * @return 覆盖率，范围 0-255。
 */
static uint8_t rounded_rect_coverage(int32_t pixel_x, int32_t pixel_y, uint16_t w, uint16_t h, uint16_t radius) {
    if (w == 0 || h == 0) {
        return 0;
    }

    if (pixel_x < 0 || pixel_x >= w || pixel_y < 0 || pixel_y >= h) {
        return 0;
    }

    radius = std::min<uint16_t>(radius, std::min<uint16_t>(w / 2, h / 2));
    if (radius == 0) {
        return 255;
    }

    if ((pixel_x >= radius && pixel_x < w - radius) || (pixel_y >= radius && pixel_y < h - radius)) {
        return 255;
    }

    // 每轴 4 个采样点，在避免浮点计算的同时提供稳定抗锯齿效果。
    static constexpr int32_t SAMPLE_OFFSETS[] = {1, 3, 5, 7};
    static constexpr int32_t SUBPIXEL_SCALE   = 8;
    const int32_t            width            = w * SUBPIXEL_SCALE;
    const int32_t            height           = h * SUBPIXEL_SCALE;
    const int32_t            corner_radius    = radius * SUBPIXEL_SCALE;
    const int32_t            radius_squared   = corner_radius * corner_radius;
    uint8_t                  inside_count     = 0;

    for (int32_t offset_y : SAMPLE_OFFSETS) {
        const int32_t sample_y = pixel_y * SUBPIXEL_SCALE + offset_y;
        if (sample_y < 0 || sample_y >= height) {
            continue;
        }

        for (int32_t offset_x : SAMPLE_OFFSETS) {
            const int32_t sample_x = pixel_x * SUBPIXEL_SCALE + offset_x;
            if (sample_x < 0 || sample_x >= width) {
                continue;
            }

            const int32_t corner_x = sample_x < corner_radius
                                         ? corner_radius
                                         : (sample_x >= width - corner_radius ? width - corner_radius : sample_x);
            const int32_t corner_y = sample_y < corner_radius
                                         ? corner_radius
                                         : (sample_y >= height - corner_radius ? height - corner_radius : sample_y);
            const int32_t dx       = sample_x - corner_x;
            const int32_t dy       = sample_y - corner_y;
            if (dx * dx + dy * dy <= radius_squared) {
                inside_count++;
            }
        }
    }

    return static_cast<uint8_t>((inside_count * 255 + 8) / 16);
}

/**
 * @brief 将抗锯齿像素写入当前帧缓冲。
 * @param x 屏幕 X 坐标。
 * @param y 屏幕 Y 坐标。
 * @param alpha 前景色透明度，范围 0-255。
 * @param color 前景色。
 * @param bg 背景色。
 */
static void write_aa_pixel(uint16_t x, uint16_t y, uint8_t alpha, color_t color, color_t bg) {
    if (x >= display_width || y >= display_height) {
        return;
    }

    uint16_t px = blend_aa_rgb565(bg.get_color_raw(), color.get_color_raw(), alpha);
    double_buffer.data[double_buffer.current_buffer][y * display_width + x] = (px >> 8) | (px << 8);
}

void fill_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t radius, color_t color, color_t bg) {
    const uint32_t end_x = std::min<uint32_t>(uint32_t(x) + w, display_width);
    const uint32_t end_y = std::min<uint32_t>(uint32_t(y) + h, display_height);
    for (uint32_t screen_y = y; screen_y < end_y; screen_y++) {
        for (uint32_t screen_x = x; screen_x < end_x; screen_x++) {
            const uint8_t alpha = rounded_rect_coverage(screen_x - x, screen_y - y, w, h, radius);
            write_aa_pixel(screen_x, screen_y, alpha, color, bg);
        }
    }
}

void draw_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t radius, uint16_t thickness, color_t color,
                     color_t bg) {
    if (thickness == 0) {
        return;
    }

    const uint32_t end_x          = std::min<uint32_t>(uint32_t(x) + w, display_width);
    const uint32_t end_y          = std::min<uint32_t>(uint32_t(y) + h, display_height);
    const bool     has_inner_rect = uint32_t(thickness) * 2 < w && uint32_t(thickness) * 2 < h;
    const uint16_t inner_w        = has_inner_rect ? w - thickness * 2 : 0;
    const uint16_t inner_h        = has_inner_rect ? h - thickness * 2 : 0;
    const uint16_t inner_radius   = radius > thickness ? radius - thickness : 0;

    for (uint32_t screen_y = y; screen_y < end_y; screen_y++) {
        for (uint32_t screen_x = x; screen_x < end_x; screen_x++) {
            const int32_t local_x     = screen_x - x;
            const int32_t local_y     = screen_y - y;
            const uint8_t outer_alpha = rounded_rect_coverage(local_x, local_y, w, h, radius);
            const uint8_t inner_alpha =
                rounded_rect_coverage(local_x - thickness, local_y - thickness, inner_w, inner_h, inner_radius);
            const uint8_t alpha = outer_alpha > inner_alpha ? outer_alpha - inner_alpha : 0;
            // 只绘制边框像素，透明处保持原像素不变。
            if (alpha) write_aa_pixel(screen_x, screen_y, alpha, color, bg);
        }
    }
}

void draw_pixel(uint16_t x, uint16_t y, color_t color) {
    if (x >= display_width || y >= display_height)
        return;
    double_buffer.data[double_buffer.current_buffer][y * display_width + x] = color.get_color_raw_big_endian();
}

void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, color_t color) {
    // Bresenham 只使用整数运算，适合在曲线页面高频绘制短线段。
    const int16_t dx    = std::abs(x1 - x0);
    const int16_t sx    = x0 < x1 ? 1 : -1;
    const int16_t dy    = -std::abs(y1 - y0);
    const int16_t sy    = y0 < y1 ? 1 : -1;
    int16_t       error = dx + dy;

    while (true) {
        if (x0 >= 0 && y0 >= 0) {
            draw_pixel(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), color);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }

        const int16_t doubled_error = error * 2;
        if (doubled_error >= dy) {
            error += dy;
            x0    += sx;
        }
        if (doubled_error <= dx) {
            error += dx;
            y0    += sy;
        }
    }
}

void fill_screen(color_t color) {
    std::fill(double_buffer.data[double_buffer.current_buffer],
              double_buffer.data[double_buffer.current_buffer] + display_width * display_height,
              color.get_color_raw_big_endian());
}

void set_rotation(Rotation rotation) {
    uint8_t madctl;
    switch (rotation) {
    case Rotation::Vertical:
        madctl         = 0x00;
        colstart       = 52;
        rowstart       = 40;
        display_width  = HEIGHT;
        display_height = WIDTH;
        break;
    case Rotation::Horizontal:
        madctl         = 0x60;
        colstart       = COLSTART;
        rowstart       = ROWSTART;
        display_width  = WIDTH;
        display_height = HEIGHT;
        break;
    case Rotation::VerticalMirror:
        madctl         = 0xC0;
        colstart       = 53;
        rowstart       = 40;
        display_width  = HEIGHT;
        display_height = WIDTH;
        break;
    case Rotation::HorizontalMirror:
        madctl         = 0xA0;
        colstart       = COLSTART;
        rowstart       = ROWSTART;
        display_width  = WIDTH;
        display_height = HEIGHT;
        break;
    default:
        return;
    }
    write_command(ST7789_MADCTL);
    write_data_byte(madctl);
}

void invert_display(bool invert) {
    write_command(invert ? ST7789_INVON : ST7789_INVOFF);
}

static uint16_t map_px_data(uint8_t px_val, uint16_t bg, uint16_t color) {
    // 将RGB565颜色值分解为R、G、B分量
    uint8_t bg_r = (bg >> 11) & 0x1F; // 5位红色
    uint8_t bg_g = (bg >> 5) & 0x3F;  // 6位绿色
    uint8_t bg_b = bg & 0x1F;         // 5位蓝色

    uint8_t color_r = (color >> 11) & 0x1F;
    uint8_t color_g = (color >> 5) & 0x3F;
    uint8_t color_b = color & 0x1F;

    // 分别对R、G、B分量进行插值
    uint8_t result_r = (px_val * (color_r - bg_r) / 255) + bg_r;
    uint8_t result_g = (px_val * (color_g - bg_g) / 255) + bg_g;
    uint8_t result_b = (px_val * (color_b - bg_b) / 255) + bg_b;

    // 重新组合成RGB565格式
    return uint16_t(result_r << 11) | uint16_t(result_g << 5) | uint16_t(result_b);
}

static uint32_t get_char_start_index(char c, const Font_t& font) {
    uint8_t  index       = c - ' ';
    uint32_t start_index = 0;
    for (int i = 0; i < index; i++) {
        start_index += font.font_height * font.width_table[i];
    }
    return start_index;
}

void draw_char(uint16_t x, uint16_t y, char c, color_t color, color_t bg, const Font_t& font) {
    if (x >= display_width || y >= display_height)
        return;
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
        c = '?';
    uint8_t  idx           = c - 32;
    uint32_t start_index   = get_char_start_index(c, font);
    const uint16_t glyph_width = font.width_table[idx];
    const uint16_t visible_width = std::min<uint16_t>(glyph_width, display_width - x);
    const uint16_t visible_height = std::min<uint16_t>(font.font_height, display_height - y);
    for (uint32_t line = 0; line < visible_height; line++) {
        for (uint16_t col = 0; col < visible_width; col++) {
            uint8_t font_val = font.font_data[start_index + line * glyph_width + col];
            uint16_t px       = map_px_data(font_val, bg.get_color_raw(), color.get_color_raw());
            px                = (px >> 8) | (px << 8);
            double_buffer.data[double_buffer.current_buffer][(y + line) * display_width + x + col] = px;
        }
    }
}

void draw_string(uint16_t x, uint16_t y, const char* str, color_t color, color_t bg, const Font_t& font) {
    if (str == nullptr)
        return;
    uint32_t cx = x;
    uint32_t cy = y;
    while (*str) {
        if (*str == '\n') {
            cy += font.font_height;
            cx  = x;
        } else {
            const unsigned char raw = static_cast<unsigned char>(*str);
            const char c = raw >= 32 && raw <= 126 ? static_cast<char>(raw) : '?';
            if (cy >= display_height)
                return;
            if (cx < display_width)
                ST7789::draw_char(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), c, color, bg, font);
            // Saturate long off-screen lines; a following newline can still restart at x.
            cx = std::min<uint32_t>(display_width, cx + font.width_table[c - 32]);
        }
        str++;
    }
}

uint16_t get_width(void) {
    return display_width;
}
uint16_t get_height(void) {
    return display_height;
}

esp_err_t set_backlight(uint8_t brightness) {
    if (!backlight_initialized) {
        ESP_LOGE("ST7789", "backlight not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (brightness == current_brightness) {
        return ESP_OK;
    }

    current_brightness = brightness;
    uint16_t duty      = backlight_interp->interpolate(brightness);
    float    percent   = (float)duty / 65535.0f * 100.0f;
    if (bl_active_low) {
        percent = 100.0f - percent;
    }
    return backlight_pwm.set_duty_percent(percent);
}

uint8_t get_backlight() {
    return current_brightness;
}

void draw_image(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* data) {
    if (data == nullptr || x >= display_width || y >= display_height)
        return;
    const uint16_t source_stride = w;
    if (x + w > display_width)
        w = display_width - x;
    if (y + h > display_height)
        h = display_height - y;
    uint16_t px;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            px = data[row * source_stride + col];
            double_buffer.data[double_buffer.current_buffer][(y + row) * display_width + (x + col)] =
                (px >> 8) | (px << 8);
        }
    }
}

} // namespace ST7789
