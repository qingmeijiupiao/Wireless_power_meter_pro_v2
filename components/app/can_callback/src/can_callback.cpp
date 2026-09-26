#include "can_callback.h"
#include "esp_log.h"
#include "hardware.h"
#include <cstdio>
#include "global_state.h"
#include "power_output.h"
#include "diagnostic_log.h"
namespace CanCallback {

static constexpr char     TAG[]                       = "CanCallback";
static constexpr uint32_t DIAGNOSTICS_TASK_STACK_SIZE = 2048;

static HXC_TWAI* can_bus = nullptr;

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
        if (tx_failed != last_tx_failed || bus_off != last_bus_off || bus_error != last_bus_error ||
            rx_overflow != last_rx_overflow) {
            twai_node_status_t status     = {};
            twai_node_record_t statistics = {};
            const esp_err_t    ret        = can_bus->get_info(&status, &statistics);
            DEVICE_STATE_W(TAG,
                           "can: diagnostics info=%s state=%u tx_err=%u rx_err=%u bus_err=%lu bus_off=%lu "
                           "tx_failed=%lu rx_overflow=%lu",
                           esp_err_to_name(ret), static_cast<uint32_t>(status.state),
                           static_cast<uint32_t>(status.tx_error_count), static_cast<uint32_t>(status.rx_error_count),
                           static_cast<uint32_t>(statistics.bus_err_num), static_cast<uint32_t>(bus_off),
                           static_cast<uint32_t>(tx_failed), static_cast<uint32_t>(rx_overflow));
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
    ret     = can_bus->setup();
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
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_PING, [](HXC_CAN_message_t* msg) { can_bus->send(msg); });

    /**
     * @brief  获取状态 回调
     * @action 获取状态 回调，返回状态数据
     */
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_GET_STATE, [](HXC_CAN_message_t* msg) {
        static HXC_CAN_message_t state_msg = {};

        auto                      state      = get_global_state();
        CALLBACK_GET_STATE_DATA_t state_data = {};
        state_data.voltage_mV                = state.voltage_mV;
        state_data.current_mA                = std::abs(state.current_uA / 1000);
        state_data.Board_temperature         = state.board_temperature / 100;
        state_data.Chip_temperature          = state.chip_temperature / 100;
        state_data.output_state              = state.flags.output_enabled;
        state_data.current_direction         = state.current_uA > 0 ? 1 : 0;
        state_data.CAN_resistor              = terminal_resistor_enabled();
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
        can_bus->send(&state_msg);
    });

    /**
     * @brief  设置输出 回调
     * @action 设置输出 回调，根据输出状态设置输出引脚
     */
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
    can_bus->add_can_receive_callback_func(CAN_ID + CALLBACK_SET_RESISTOR, [](HXC_CAN_message_t* msg) {
        const bool      enabled = msg->data[0] == 0x01;
        const esp_err_t ret     = set_terminal_resistor(enabled);
        DEVICE_STATE_I(TAG, "can: resistor source=can target=%u result=%s", enabled ? 1U : 0U, esp_err_to_name(ret));
    });

    /**
     * @brief  -1 - 测试回调
     * @usage  收到任意 CAN 帧时打印内容
     * @note   仅用于调试，生产环境请移除
     */
    can_bus->add_can_receive_callback_func(-1, [](HXC_CAN_message_t* msg) {
        ESP_LOGI(TAG, "ID=%08lX DLC=%d", msg->identifier, msg->data_length_code);
        printf("Data: ");
        for (int i = 0; i < msg->data_length_code; i++) {
            printf("%02X ", msg->data[i]);
        }
        printf("\n");
    });

    // --- 添加新回调模板 ---
    // /**
    //  * @brief  0x<ID> - <简要描述>
    //  * @usage  收到 ID=0x<ID> 的 CAN 帧时执行
    //  * @param  msg - CAN 消息指针
    //  * @note   <注意事项>
    //  */
    // can_bus->add_can_receive_callback_func(0x<ID>,
    //     [](HXC_CAN_message_t* msg) {
    //         // 回调实现
    //     });

    DEVICE_EVENT_I(TAG, "can: init id=0x%lx baud=%lu resistor=%u", static_cast<uint32_t>(CAN_ID.read()),
                   static_cast<uint32_t>(CAN_BAUDRATE.read()), terminal_resistor_enabled() ? 1U : 0U);
    // 诊断任务仅周期读取计数器并在变化时输出日志，2KB 足以覆盖格式化路径。
    if (xTaskCreate(diagnostics_task, "can_diag", DIAGNOSTICS_TASK_STACK_SIZE, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "failed to create diagnostics task");
    }
    return ESP_OK;
}

} // namespace CanCallback
