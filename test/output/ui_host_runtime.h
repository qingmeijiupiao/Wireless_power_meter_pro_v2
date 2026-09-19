#pragma once
#include "output_host_runtime.h"
#include <cstring>
#include "widgets/output_view.h"
enum class ButtonEvent { SHORT_PRESS, DOUBLE_CLICK, LONG_PRESS, SUPER_LONG_PRESS, PRESS, RELEASE };
namespace SCREEN { enum class ButtonId { Main, Side, Previous }; }
constexpr uint32_t portTICK_PERIOD_MS=1;
inline uint32_t xTaskGetTickCount(){return Host::ui_ticks;}
struct HostQueue { size_t capacity, size; std::deque<std::vector<char>> items; };
using QueueHandle_t=HostQueue*;
inline QueueHandle_t xQueueCreate(size_t count,size_t size){return new HostQueue{count,size,{}};}
inline int xQueueSend(QueueHandle_t queue,const void* data,int) {
    if(queue->items.size()==queue->capacity)return pdFALSE;
    queue->items.emplace_back(static_cast<const char*>(data),static_cast<const char*>(data)+queue->size);
    return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue,void* data,int) {
    if(queue->items.empty())return pdFALSE;
    std::memcpy(data,queue->items.front().data(),queue->size);queue->items.pop_front();return pdTRUE;
}
inline void xQueueReset(QueueHandle_t queue){queue->items.clear();}
namespace UiHost {
inline bool dialog=false;
inline int requests=0, page_events=0;
inline uint32_t render_cost_ms=2;
inline int page_renders=0, transfers=0;
inline uint32_t periods[]={67,250,200,500,200};
inline std::vector<uint32_t> render_times, history_times;
inline int current_page=0;
}
namespace ST7789 {
enum class Rotation { Horizontal, HorizontalMirror };
inline void set_rotation(Rotation){}
inline void set_backlight(int){}
inline void copy_buffers(){}
inline void sync_buffers(){++UiHost::transfers;}
}
namespace SCREEN {
inline bool ui_config_get_rotation_180(){return false;}
inline uint8_t ui_config_get_backlight_level(){return 1;}
inline int backlight_value_from_level(uint8_t){return 1;}
inline void draw_edit_indicator(){}
class CurveHistory {
  public:
    static constexpr uint32_t SAMPLE_INTERVAL_MS=500;
    static CurveHistory& instance(){static CurveHistory history;return history;}
    void poll(uint32_t now){UiHost::history_times.push_back(now);}
};
namespace UI {
inline OutputView current_output_view;
inline const OutputView& output_view(){return current_output_view;}
inline void set_output_view(const OutputView& view){current_output_view=view;}
inline void output_feedback_overlay(bool){}
inline void short_circuit_dialog(bool,uint16_t,uint16_t){UiHost::dialog=true;} }
}
