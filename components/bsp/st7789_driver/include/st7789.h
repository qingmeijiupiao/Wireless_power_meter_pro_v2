/**
 * @file st7789.h
 * @brief ST7789V 1.14 英寸显示屏驱动
 *
 * 通过 SPI 控制 ST7789V TFT 显示屏的公共 API。
 * 包含像素、矩形、抗锯齿圆角矩形、文本、图像和双缓冲同步接口。
 *
 * @example
 * ```cpp
 * ST7789::Config cfg = {
 *     .mosi_io_num = 2, .sclk_io_num = 23, .cs_io_num = 8,
 *     .dc_io_num = 22, .rst_io_num = 21, .bl_io_num = 1,
 *     .bl_active_state = true,
 *     .host_id = SPI2_HOST
 * };
 * ST7789::init(&cfg);
 * ST7789::fill_screen(ST7789::BLACK);
 * ```
 */

#ifndef __ST7789_H__
#define __ST7789_H__

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "color.h"
#include "Font.h"
namespace ST7789 {

/* ==================== 显示屏配置 ==================== */

/** 显示屏在横向模式下的宽度（像素） */
static constexpr uint16_t WIDTH = 240;

/** 显示屏在横向模式下的高度（像素） */
static constexpr uint16_t HEIGHT = 135;

enum class Rotation { Vertical = 0, Horizontal = 1, VerticalMirror = 2, HorizontalMirror = 3 };

/* ==================== 像素偏移 ==================== */
static constexpr uint8_t COLSTART = 40;
static constexpr uint8_t ROWSTART = 52;

/**
 * @brief 显示屏硬件配置
 */
struct Config {
    int               mosi_io_num;     /**< MOSI的GPIO引脚（显示屏上的SI） */
    int               sclk_io_num;     /**< 时钟的GPIO引脚（SCK） */
    int               cs_io_num;       /**< 片选的GPIO引脚（TCS） */
    int               dc_io_num;       /**< 数据/命令的GPIO引脚（DC） */
    int               rst_io_num;      /**< 复位的GPIO引脚（RST） */
    int               bl_io_num;       /**< 背光的 GPIO 引脚，-1 表示未使用 */
    bool              bl_active_state; /**< 背光开启时引脚的电平状态 */
    spi_host_device_t host_id;         /**< SPI主机（SPI2_HOST或SPI3_HOST） */
};

/* ==================== 公共函数 ==================== */

/**
 * @brief 初始化ST7789显示屏
 * @param cfg 指向配置结构的指针
 * @param rotation 旋转方向
 * @return 成功返回ESP_OK，否则返回错误代码
 */
esp_err_t init(const Config* cfg, Rotation rotation = Rotation::Horizontal);

/**
 * @brief 绘制一个像素
 * @param x X坐标（0到width-1）
 * @param y Y坐标（0到height-1）
 * @param color RGB565格式的颜色
 */
void draw_pixel(uint16_t x, uint16_t y, color_t color);

/**
 * @brief 使用 Bresenham 算法绘制一条直线
 * @param x0 起点 X 坐标
 * @param y0 起点 Y 坐标
 * @param x1 终点
 * X 坐标
 * @param y1 终点 Y 坐标
 * @param color 线条颜色
 */
void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, color_t color);

/**
 * @brief 用颜色填充矩形
 * @param x 左上角的X坐标
 * @param y 左上角的Y坐标
 * @param w 矩形的宽度
 * @param h 矩形的高度
 * @param color RGB565格式的颜色
 */
void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, color_t color);

/**
 * @brief 绘制带抗锯齿的填充圆角矩形
 * @param x 左上角的X坐标
 * @param y 左上角的Y坐标
 * @param w 矩形的宽度
 * @param h 矩形的高度
 * @param radius 圆角半径
 * @param color 填充颜色
 * @param bg 抗锯齿边缘使用的背景颜色
 */
void fill_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t radius, color_t color, color_t bg);

/**
 * @brief 绘制带抗锯齿的圆角矩形边框
 * @param x 左上角的X坐标
 * @param y 左上角的Y坐标
 * @param w 矩形的宽度
 * @param h 矩形的高度
 * @param radius 圆角半径
 * @param thickness 边框宽度，单位为像素
 * @param color 边框颜色
 * @param bg 抗锯齿边缘和边框内部使用的背景颜色
 */
void draw_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t radius, uint16_t thickness, color_t color,
                     color_t bg);

/**
 * @brief 用颜色填充整个屏幕
 * @param color RGB565格式的颜色
 */
void fill_screen(color_t color);

/**
 * @brief 设置显示屏的旋转方向
 * @param rotation 旋转方向
 */
void set_rotation(Rotation rotation);

/**
 * @brief 反转显示屏的颜色
 * @param invert true表示反转，false表示正常
 */
void invert_display(bool invert);

/**
 * @brief 绘制一个字符
 * @param x X坐标
 * @param y Y坐标
 * @param c ASCII字符（32-126），其余字节用问号替代；右/下边缘自动裁剪
 * @param color 文本颜色
 * @param bg 背景颜色
 * @param font 字体资源
 */
void draw_char(uint16_t x, uint16_t y, char c, color_t color, color_t bg, const Font_t& font);

/**
 * @brief 绘制文本字符串
 * @param x 起始X坐标
 * @param y 起始Y坐标
 * @param str 以NULL结尾的字符串
 * @param color 文本颜色
 * @param bg 背景颜色
 * @param font 字体资源
 */
void draw_string(uint16_t x, uint16_t y, const char* str, color_t color, color_t bg, const Font_t& font);

/**
 * @brief 获取当前显示屏的宽度
 * @return 宽度（像素）（取决于旋转方向）
 */
uint16_t get_width(void);

/**
 * @brief 获取当前显示屏的高度
 * @return 高度（像素）（取决于旋转方向）
 */
uint16_t get_height(void);

/**
 * @brief 在显示屏上绘制RGB565图像
 * @param x 左上角的X坐标
 * @param y 左上角的Y坐标
 * @param w 图像的宽度
 * @param h 图像的高度
 * @param data 指向RGB565原始值数组的指针；驱动写入帧缓冲时转换为发送字节序
 */
void draw_image(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* data);

/**
 * @brief 同步双缓冲区，将当前缓冲区的内容发送到显示屏
 */
void sync_buffers();

/**
 * @brief 切换当前显示缓冲区
 */
void switch_buffers();

/**
 * @brief 复制当前缓冲区内容到另一个缓冲区
 */
void copy_buffers();

/**
 * @brief 设置背光亮度
 * @param brightness 亮度值 0-255，0为关闭，255为最亮
 * @return 成功返回ESP_OK，未配置背光引脚返回ESP_ERR_NOT_SUPPORTED
 */
esp_err_t set_backlight(uint8_t brightness);

/**
 * @brief 获取当前背光亮度
 * @return 亮度值 0-255
 */
uint8_t get_backlight();

} // namespace ST7789

#endif
