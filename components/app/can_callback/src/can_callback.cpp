#include "can_callback.h"
#include "esp_log.h"
#include "hardware.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "global_state.h"
#include "power_output.h"
#include "short_circuit_detect.h"
#include "diagnostic_log.h"
namespace CanCallback {

static constexpr char     TAG[]                       = "CanCallback";
// 诊断日志会经由 USB-Serial-JTAG / stdio 写路径（递归锁 + vfprintf），峰值栈占用超过 2KB。
// 原 2048 字节在总线错误计数变化触发日志时导致 can_diag 栈溢出（Stack protection fault）。
static constexpr uint32_t DIAGNOSTICS_TASK_STACK_SIZE = 4096;

static HXC_TWAI* can_bus = nullptr;

// 已分发到应用层的 CAN 帧总数（含数据帧与控制帧）。用于满载压测时核对是否丢帧。
static uint32_t rx_frame_count = 0;

// 控制帧回复发送失败计数（包括发送队列超时），用于满载压测时定位丢包。
static uint32_t control_tx_fail = 0;

// CAN 终端电阻控制器：整机唯一实例，通用持久化能力由 nvs_gpio_output 提供。
static NvsGpioOutput terminal_resistor_controller("can_term", false, true);

NvsGpioOutput& terminal_resistor() {
    return terminal_resistor_controller;
}

bool terminal_resistor_enabled() {
    return terminal_resistor_controller.get();
}

esp_err_t set_terminal_resistor(bool enabled) {
    return terminal_resistor_controller.set(enabled);
}

esp_err_t toggle_terminal_resistor() {
    return terminal_resistor_controller.toggle();
}

HXC::NVS_DATA<uint32_t> CAN_BAUDRATE("CAN_BAUDRATE", DEFAULT_CAN_BAUDRATE);
HXC::NVS_DATA<uint32_t> CAN_ID("CAN_ID", DEFAULT_DEVICE_CAN_ID);

// 按 CAN_CALLBACK_FILTER_SPAN 配置硬件验收过滤器，只接收本机控制帧。
[[maybe_unused]] static esp_err_t apply_hardware_filter() {
    if (can_bus == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    twai_mask_filter_config_t filter = {};
    filter.dual_filter               = false;
    // 接受 CAN_ID .. CAN_ID + CAN_CALLBACK_FILTER_SPAN - 1；SPAN 为 2 的幂，SPAN-1 即忽略位掩码。
    const uint32_t device_can_id = CAN_ID.read();
    const bool     is_ext        = device_can_id > 0x7FF;
    const uint32_t id_limit      = is_ext ? 0x1FFFFFFFu : 0x7FFu;
    const uint32_t ignore_mask   = CAN_CALLBACK_FILTER_SPAN - 1u;
    filter.id                    = device_can_id;
    filter.mask                  = ~ignore_mask & id_limit;
    filter.is_ext                = is_ext;
    return can_bus->set_filter(filter);
}

static void diagnostics_task(void*) {
    uint32_t last_tx_failed   = 0;
    uint32_t last_bus_off     = 0;
    uint32_t last_bus_error   = 0;
    uint32_t last_rx_overflow = 0;
    while (true) {
        const uint32_t tx_failed   = can_bus->get_tx_failed_count();
        const uint32_t bus_off     = can_bus->get_bus_off_count();
        const uint32_t bus_error   = can_bus->get_bus_error_count();
        const uint32_t rx_overflow = can_bus->get_rx_overflow_count();
        const uint32_t rx_frames   = __atomic_load_n(&rx_frame_count, __ATOMIC_RELAXED);
        if (tx_failed != last_tx_failed || bus_off != last_bus_off || bus_error != last_bus_error ||
            rx_overflow != last_rx_overflow) {
            twai_node_status_t status     = {};
            twai_node_record_t statistics = {};
            const esp_err_t    ret        = can_bus->get_info(&status, &statistics);
            DEVICE_STATE_W(TAG,
                           "can: diagnostics info=%s state=%u tx_err=%u rx_err=%u bus_err=%lu bus_off=%lu "
                           "tx_failed=%lu rx_overflow=%lu rx_frames=%lu ctrl_tx_fail=%lu",
                           esp_err_to_name(ret), static_cast<uint32_t>(status.state),
                           static_cast<uint32_t>(status.tx_error_count), static_cast<uint32_t>(status.rx_error_count),
                           static_cast<uint32_t>(statistics.bus_err_num), static_cast<uint32_t>(bus_off),
                           static_cast<uint32_t>(tx_failed), static_cast<uint32_t>(rx_overflow),
                           static_cast<uint32_t>(rx_frames),
                           static_cast<uint32_t>(__atomic_load_n(&control_tx_fail, __ATOMIC_RELAXED)));
            last_tx_failed   = tx_failed;
            last_bus_off     = bus_off;
            last_bus_error   = bus_error;
            last_rx_overflow = rx_overflow;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

HXC_TWAI& get_can_bus() {
    return *can_bus;
}

bool is_available() {
    return can_bus != nullptr;
}

esp_err_t init() {
    auto& hw       = get_hardware_config();
    auto& resistor = terminal_resistor();
    resistor.set_on_change_callback([](bool enabled) {
        update_global_state([enabled](GlobalState& state) { state.flags.can_resistor_enabled = enabled; });
        ESP_LOGW(TAG, "CAN resistor changed to %s", enabled ? "ON" : "OFF");
    });

    esp_err_t ret = resistor.init(hw.CAN_RESISTOR_ENABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN resistor init failed on GPIO %d: %s", hw.CAN_RESISTOR_ENABLE, esp_err_to_name(ret));
        return ret;
    }

    can_bus = new HXC_TWAI(hw.CAN_TX, hw.CAN_RX, CAN_BAUDRATE.read());

    // 硬件过滤器由编译期常量 CAN_USE_HARDWARE_FILTER 控制（调试时才需要接收非本机帧）。
    if constexpr (CAN_USE_HARDWARE_FILTER) {
        const esp_err_t filter_ret = apply_hardware_filter();
        if (filter_ret != ESP_OK) {
            ESP_LOGW(TAG, "can: hardware filter config failed: %s", esp_err_to_name(filter_ret));
        } else {
            const uint32_t device_can_id = CAN_ID.read();
            ESP_LOGI(TAG, "can: hardware filter enabled 0x%lx..0x%lx", static_cast<unsigned long>(device_can_id),
                     static_cast<unsigned long>(device_can_id + CAN_CALLBACK_FILTER_SPAN - 1));
        }
    } else {
        ESP_LOGW(TAG, "can: hardware filter disabled, receiving all bus frames");
    }

    ret = can_bus->setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN controller setup failed: %s", esp_err_to_name(ret));
        delete can_bus;
        can_bus = nullptr;
        return ret;
    }

    // ====== 回调列表 ======

    /**
     * @brief  PING 回调
     * @action PING 回调，返回 PING 帧
     */
    assert_callback_filtered<CALLBACK_PING>();
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_PING, [](HXC_CAN_message_t* msg) {
        if (can_bus->send(msg) != ESP_OK) {
            __atomic_fetch_add(&control_tx_fail, 1U, __ATOMIC_RELAXED);
        }
        ESP_LOGI(TAG, "can: rx ping id=0x%lx", static_cast<unsigned long>(msg->identifier));
    });

    /**
     * @brief  获取状态 回调
     * @action 获取状态 回调，返回状态数据
     */
    assert_callback_filtered<CALLBACK_GET_STATE>();
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_GET_STATE, [](HXC_CAN_message_t* msg) {
        static HXC_CAN_message_t state_msg = {};

        auto                      state      = get_global_state();
        CALLBACK_GET_STATE_DATA_t state_data = {};
        state_data.voltage_mV                = state.voltage_mV;
        // 电流绝对值按 10mA 单位上报，饱和到 uint16 量程（0..655.35A）。
        const int32_t current_10mA           = std::abs(state.current_uA) / 10000;
        state_data.current_10mA              = static_cast<uint16_t>(std::min<int32_t>(current_10mA, UINT16_MAX));
        state_data.Board_temperature         = state.board_temperature / 100;
        state_data.Chip_temperature          = state.chip_temperature / 100;
        state_data.output_state              = state.flags.output_enabled;
        state_data.current_direction         = state.current_uA > 0 ? 1 : 0;
        state_data.CAN_resistor              = terminal_resistor_enabled();
        state_data.short_detect_running      = ShortCircuitDetect::is_testing() ? 1 : 0;
        state_data.short_detect_passed =
            ShortCircuitDetect::last_result().state == ShortCircuitDetect::ResultState::PASSED ? 1 : 0;
        state_data.UVP_flag                  = state.protect_states.states_bit.low_voltage_protect_state;
        state_data.OVP_flag                  = state.protect_states.states_bit.high_voltage_protect_state;
        state_data.OTP_flag                  = state.protect_states.states_bit.temperature_protect_state;
        state_data.OCP_flag                  = state.protect_states.states_bit.current_protect_state;

        if (CAN_ID + CALLBACK_GET_STATE > 0x7ff) {
            state_msg.extd = true;
        } else {
            state_msg.extd = false;
        }

        state_msg.identifier       = CAN_ID + CALLBACK_GET_STATE;
        state_msg.data_length_code = sizeof(CALLBACK_GET_STATE_DATA_t);
        memcpy(state_msg.data, &state_data, sizeof(CALLBACK_GET_STATE_DATA_t));
        if (can_bus->send(&state_msg) != ESP_OK) {
            __atomic_fetch_add(&control_tx_fail, 1U, __ATOMIC_RELAXED);
        }
        ESP_LOGI(TAG, "can: rx get_state id=0x%lx", static_cast<unsigned long>(msg->identifier));
    });

    /**
     * @brief  设置输出 回调
     * @action 设置输出 回调，根据输出状态设置输出引脚
     */
    assert_callback_filtered<CALLBACK_SET_OUTPUT>();
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_SET_OUTPUT, [](HXC_CAN_message_t* msg) {
        if (msg->data_length_code < 1) return;
        const bool target = msg->data[0] == 0x01;
        PowerOutput::request(target ? PowerOutput::OutputOperation::ON : PowerOutput::OutputOperation::OFF, TAG,
            [target](PowerOutput::OutputResult result, bool state) {
                DEVICE_EVENT_I(TAG, "can: set_output target=%u result=%s state=%u", target ? 1U : 0U,
                               PowerOutput::result_to_string(result), state ? 1U : 0U);
            });
    });

    /**
     * @brief  设置终端电阻 回调
     * @action 设置终端电阻 回调，根据终端电阻状态设置终端电阻引脚
     */
    assert_callback_filtered<CALLBACK_SET_RESISTOR>();
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_SET_RESISTOR, [](HXC_CAN_message_t* msg) {
        const bool      enabled = msg->data[0] == 0x01;
        const esp_err_t ret     = set_terminal_resistor(enabled);
        DEVICE_STATE_I(TAG, "can: resistor source=can target=%u result=%s", enabled ? 1U : 0U, esp_err_to_name(ret));
    });

    /**
     * @brief  -1 - 全量帧计数回调
     * @usage  收到任意 CAN 帧时计数，用于满载压测核对丢帧与总线负载
     * @note   逐帧打印会成为 8000fps 满载下的瓶颈，因此这里只做原子计数，
     *         累计值由诊断任务每秒输出一次。
     */
    can_bus->add_can_receive_callback_func(-1, [](HXC_CAN_message_t*) {
        __atomic_fetch_add(&rx_frame_count, 1U, __ATOMIC_RELAXED);
    });

    // --- 添加新回调模板 ---
    // 1. 在 can_callback.h 的 CALLBACK_ID 中新增偏移（放在 CALLBACK_ID_COUNT 之前并保持连续）。
    // 2. 若偏移超出 CAN_CALLBACK_FILTER_SPAN，编译会失败——把 SPAN 提升到 2 的幂即可。
    // 3. 注册前加 assert_callback_filtered<CALLBACK_XXX>(); 复用同一检查。
    // /**
    //  * @brief  0x<ID> - <简要描述>
    //  * @usage  收到 ID=0x<ID> 的 CAN 帧时执行
    //  * @param  msg - CAN 消息指针
    //  * @note   <注意事项>
    //  */
    // assert_callback_filtered<CALLBACK_XXX>();
    // can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_XXX,
    //     [](HXC_CAN_message_t* msg) {
    //         // 回调实现
    //     });

    DEVICE_EVENT_I(TAG, "can: init id=0x%lx baud=%lu resistor=%u", static_cast<uint32_t>(CAN_ID.read()),
                   static_cast<uint32_t>(CAN_BAUDRATE.read()), terminal_resistor_enabled() ? 1U : 0U);
    // 诊断任务仅周期读取计数器并在变化时输出日志；日志格式化路径需要一个完整的 4KB 栈。
    if (xTaskCreate(diagnostics_task, "can_diag", DIAGNOSTICS_TASK_STACK_SIZE, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "failed to create diagnostics task");
    }
    return ESP_OK;
}

} // namespace CanCallback
