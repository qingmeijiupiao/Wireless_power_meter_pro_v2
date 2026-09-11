/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 历史曲线页面声明
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#ifndef SCREEN_CURVE_PAGE_H
#define SCREEN_CURVE_PAGE_H

#include <cstddef>
#include "core/page.h"
#include "pages/curve/curve_history.h"
#include "st7789.h"

namespace SCREEN {

class CurvePage final : public Page {
  public:
    /**
     * @brief 获取曲线页 ID
     * @return 曲线页 ID
     */
    PageId id() const override;

    /**
     * @brief 获取曲线页标题
     * @return 静态标题字符串
     */
    const char* title() const override;

    /**
     * @brief 获取曲线页刷新周期
     * @return 刷新周期，单位 ms
     */
    uint32_t refresh_interval_ms() const override;

    /** @brief 首次进入页面时从 NVS 恢复曲线配置。 */
    void on_enter() override;

    /**
     * @brief 声明曲线页支持参数编辑模式
     * @return 始终返回 true
     */
    bool supports_edit_mode() const override;

    /**
     * @brief 判断曲线页是否处于参数编辑状态
     * @return true 表示需要显示全局编辑提示
     */
    bool is_overlay_active() const override;

    /**
     * @brief 进入曲线参数编辑状态
     */
    void on_edit_enter() override;

    /**
     * @brief 退出曲线参数编辑状态
     */
    void on_edit_exit() override;

    /**
     * @brief 处理曲线页按键事件
     * @param button 按键 ID
     * @param event 按键事件
     * @return true 表示事件已由曲线页消费
     */
    bool handle_button(ButtonId button, ButtonEvent event) override;

    /**
     * @brief 绘制曲线页面
     * @param mode 页面渲染模式
     */
    void render(RenderMode mode) override;

  private:
    enum class DisplayMode : uint8_t {
        Voltage,
        Current,
        Power,
        All,
        Count,
    };

    enum class EditItem : uint8_t {
        Display,
        TimeWindow,
        Count,
    };

    struct AutoRange {
        float    minimum             = 0.0f;  // 当前量程最小值
        float    maximum             = 1.0f;  // 当前量程最大值
        uint32_t shrink_candidate_ms = 0;     // 满足缩小量程条件的起始时间，单位 ms
        bool     initialized         = false; // 自动量程是否已经初始化
    };

    /**
     * @brief 获取当前时间跨度，单位 ms
     * @return 当前时间跨度
     */
    uint32_t window_ms() const;

    /**
     * @brief 获取显示模式短文本
     * @return 显示模式短文本
     */
    const char* display_mode_text() const;

    /**
     * @brief 获取时间跨度短文本
     * @return 时间跨度短文本
     */
    const char* window_text() const;

    /** @brief 从 NVS 加载并校验显示模式和时间窗口。 */
    void load_config();

    /** @brief 将当前显示模式和时间窗口立即保存到 NVS。 */
    void save_config() const;

    /**
     * @brief 更新指定指标的自动量程
     * @param metric 指标类型
     * @param buckets 当前指标的像素桶
     * @param bucket_count 像素桶数量
     * @param now_ms 当前系统时间，单位 ms
     */
    void update_auto_range(CurveMetric metric, const CurveBucket* buckets, size_t bucket_count, uint32_t now_ms);

    /**
     * @brief 绘制单指标曲线模式
     * @param metric 指标类型
     * @param color 曲线颜色
     */
    void draw_single_metric(CurveMetric metric, ST7789::color_t color);

    /**
     * @brief 绘制三指标叠加模式
     */
    void draw_all_metrics();

    /**
     * @brief 绘制曲线区域网格
     * @param x 左上角 X 坐标
     * @param y 左上角 Y 坐标
     * @param width 网格宽度
     * @param height 网格高度
     */
    void draw_grid(uint16_t x, uint16_t y, uint16_t width, uint16_t height) const;

    /**
     * @brief 绘制单组像素桶曲线
     * @param buckets 像素桶数组
     * @param bucket_count 像素桶数量
     * @param range 使用的自动量程
     * @param x 曲线区域左上角 X 坐标
     * @param y 曲线区域左上角 Y 坐标
     * @param height 曲线区域高度
     * @param color 曲线颜色
     */
    void draw_bucket_curve(const CurveBucket* buckets, size_t bucket_count, const AutoRange& range, uint16_t x,
                           uint16_t y, uint16_t height, ST7789::color_t color) const;

    DisplayMode display_mode_                                     = DisplayMode::Voltage; // 当前曲线显示模式
    EditItem    edit_item_                                        = EditItem::Display;    // 当前编辑项
    uint8_t     window_index_                                     = 1;     // 当前时间窗口索引，默认 30s
    bool        editing_                                          = false; // 是否处于曲线参数编辑状态
    bool        config_loaded_                                    = false; // 是否已经从 NVS 加载配置
    AutoRange   ranges_[static_cast<uint8_t>(CurveMetric::Count)] = {};    // 各指标自动量程
    CurveBucket buckets_[ST7789::WIDTH]                           = {};    // 单帧像素桶工作缓冲区
};

} // namespace SCREEN

#endif // SCREEN_CURVE_PAGE_H
