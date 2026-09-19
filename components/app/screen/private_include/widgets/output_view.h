#pragma once
#include <cstdint>

namespace SCREEN::UI {
/** 纯显示数据，不依赖输出服务或绘图库；只由屏幕任务修改。 */
enum class OutputVisual : uint8_t { Off, On, Checking, Wait, Locked, Error };
struct OutputView {
    OutputVisual state = OutputVisual::Off;
    bool pressed = false;
    bool bypassed = false;
    uint8_t animation = 0;
    char label[12] = "OFF";
    char detail[48] = {};
};
void set_output_view(const OutputView& view);
const OutputView& output_view();
} // namespace SCREEN::UI
