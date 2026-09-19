#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <deque>
using namespace std::chrono_literals;
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=1, ESP_ERR_INVALID_STATE=2,
              ESP_ERR_INVALID_ARG=3, ESP_ERR_INVALID_RESPONSE=4, ESP_ERR_TIMEOUT=5;
inline const char* esp_err_to_name(int) { return "fake_error"; }
#define ESP_LOGD(...) ((void)0)
#define DEVICE_EVENT_I(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define DEVICE_STATE_I(...) ((void)0)
using gpio_num_t = int;
constexpr int GPIO_NUM_NC=-1, GPIO_MODE_OUTPUT=1;
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin)>=0 && (pin)<31)
struct gpio_config_t { uint64_t pin_bit_mask=0; int mode=0; };
inline esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }

namespace Host {
inline std::atomic<bool> output{false}, pulse{false}, fault{false}, bypass{false};
inline std::atomic<bool> adc_error{false}, disable_error{false}, output_error{false};
inline std::atomic<bool> disable_bypass_on_idle{false};
inline std::atomic<int> voltage{2100}, samples{0}, pulses{0}, enables{0};
inline std::atomic<int64_t> clock_offset{0};
inline const auto boot = std::chrono::steady_clock::now();
inline std::mutex gate_mutex;
inline std::condition_variable gate_cv;
inline bool gate=false, entered=false;
inline std::deque<int> voltage_sequence;
inline std::deque<esp_err_t> adc_error_sequence;
inline std::function<void(int,int)> protect_callback;
inline void gate_on() { std::lock_guard lock(gate_mutex); gate=true; entered=false; }
inline void gate_wait() {
    std::unique_lock lock(gate_mutex);
    assert(gate_cv.wait_for(lock, 2s, []{return entered;}));
}
inline void gate_release() { std::lock_guard lock(gate_mutex); gate=false; gate_cv.notify_all(); }
inline void cool() { clock_offset += 600000; }
}
inline int64_t esp_timer_get_time() {
    return 1 + Host::clock_offset + std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-Host::boot).count();
}
inline esp_err_t gpio_set_level(gpio_num_t, int value) {
    if (Host::output_error) return ESP_FAIL;
    if (value) { assert(!Host::pulse); ++Host::enables; }
    Host::output = value;
    return ESP_OK;
}
inline esp_err_t set_short_test_enabled(bool value) {
    if (value) { assert(!Host::output); ++Host::pulses; }
    if (!value && Host::disable_error) return ESP_FAIL;
    Host::pulse = value;
    if (!value && Host::disable_bypass_on_idle.exchange(false)) Host::bypass=false;
    return ESP_OK;
}
inline esp_err_t read_short_detect_voltage_mV(int& voltage) {
    esp_err_t sample_error=ESP_OK;
    {
        std::unique_lock lock(Host::gate_mutex);
        Host::entered=true;
        Host::gate_cv.notify_all();
        Host::gate_cv.wait(lock, []{return !Host::gate;});
        voltage=Host::voltage;
        if (!Host::voltage_sequence.empty()) {
            voltage=Host::voltage_sequence.front();
            Host::voltage_sequence.pop_front();
        }
        if (!Host::adc_error_sequence.empty()) {
            sample_error=Host::adc_error_sequence.front();
            Host::adc_error_sequence.pop_front();
        }
    }
    ++Host::samples;
    return Host::adc_error ? ESP_FAIL : sample_error;
}
struct GlobalState {
    struct { bool output_enabled=false; } flags;
    struct { struct { int temperature_protect_state=0, high_voltage_protect_state=0,
                         low_voltage_protect_state=0, current_protect_state=0; } states_bit; } protect_states;
};
inline std::mutex global_mutex;
inline GlobalState global_state;
inline GlobalState get_global_state() { std::lock_guard lock(global_mutex); return global_state; }
template<class F> void update_global_state(F function) { std::lock_guard lock(global_mutex); function(global_state); }
using ProtectState_t=int;
constexpr int PROTECT_STATE_PROTECT=2;
inline bool protect_should_block_output() { return Host::fault && !Host::bypass; }
inline bool protect_is_bypassed() { return Host::bypass; }
inline std::mutex protect_gate;
class ProtectOutputGuard {
    std::unique_lock<std::mutex> lock{protect_gate};
};
inline void add_on_protect_change_callback(std::function<void(int,int)> callback) {
    Host::protect_callback=std::move(callback);
}
namespace HXC {
template<class T> class NVS_DATA {
    T value;
  public:
    NVS_DATA(const char*, T initial) : value(initial) {}
    T read() { return value; }
    esp_err_t set(T next) { value=next; return ESP_OK; }
};
}
using TickType_t=uint32_t;
constexpr int pdTRUE=1, pdFALSE=0, pdPASS=1;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
struct HostSemaphore {
    std::mutex mutex;
    std::condition_variable cv;
    int count;
    explicit HostSemaphore(int value):count(value){}
};
using SemaphoreHandle_t=HostSemaphore*;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new HostSemaphore(1);}
inline SemaphoreHandle_t xSemaphoreCreateBinary(){return new HostSemaphore(0);}
inline int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) {
    std::unique_lock lock(semaphore->mutex);
    if (timeout==portMAX_DELAY) semaphore->cv.wait(lock,[&]{return semaphore->count>0;});
    else if (!semaphore->cv.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return semaphore->count>0;})) return pdFALSE;
    --semaphore->count;
    return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t semaphore) {
    std::lock_guard lock(semaphore->mutex);
    semaphore->count=1;
    semaphore->cv.notify_one();
    return pdTRUE;
}
struct HostTask { HostSemaphore notify{0}; };
using TaskHandle_t=HostTask*;
inline thread_local TaskHandle_t current_task=nullptr;
inline TaskHandle_t xTaskGetCurrentTaskHandle(){return current_task;}
inline int xTaskCreate(void (*function)(void*),const char*,int,void* context,int,TaskHandle_t* task) {
    *task=new HostTask;
    std::thread([=, handle=*task]{current_task=handle;function(context);}).detach();
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task){xSemaphoreGive(&task->notify);}
#ifdef UI_SCHEDULER_HOST_TEST
namespace Host {
inline TickType_t ui_ticks=0, last_wait=0;
inline std::function<void(TickType_t)> notification_wait_hook;
}
inline uint32_t ulTaskNotifyTake(int,TickType_t timeout){
    Host::last_wait=timeout;
    if(xSemaphoreTake(&current_task->notify,0))return 1;
    if(timeout){
        const auto hook=Host::notification_wait_hook;
        if(hook)hook(timeout);
        if(xSemaphoreTake(&current_task->notify,0))return 1;
        Host::ui_ticks+=timeout;
    }
    return 0;
}
#else
inline uint32_t ulTaskNotifyTake(int,TickType_t timeout){return xSemaphoreTake(&current_task->notify,timeout);}
#endif
namespace Host { inline std::function<void(TickType_t)> delay_hook; }
inline void vTaskDelay(TickType_t ticks){
#ifdef UI_SCHEDULER_HOST_TEST
    Host::ui_ticks+=ticks;
#endif
    if (ticks && Host::delay_hook) Host::delay_hook(ticks);
#ifndef UI_SCHEDULER_HOST_TEST
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
#endif
}
