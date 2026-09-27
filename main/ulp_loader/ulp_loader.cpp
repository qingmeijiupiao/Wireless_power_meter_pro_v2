/*
 * @Description: LP 核加载器
 * @Author: qingmeijiupiao
 * @version: 1.0.0
 * @Date: 2026-04-20 00:48:01
 * @LastEditTime: 2026-04-30 18:29:00
 */
#include "ulp_loader.h"
#include "ulp_lp_core.h"
#include "ulp_lp_core_critical_section_shared.h"
#include "ulp_main.h"
#include "esp_log.h"
#include "esp_task.h"
#include "lp_core_i2c.h"
#include "ulp_app/ulp_state.h"
#include "soc/lp_clkrst_reg.h"
#include "soc/lp_clkrst_struct.h"
#include "current_calibration.h"
#include "global_state.h"
#include "diagnostic_log.h"
#include <stddef.h>
const char* LPTAG = "LP_CORE";

ulp_lp_core_cfg_t lp_core_init_cfg = {
    .wakeup_source              = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
    .lp_timer_sleep_duration_us = 0,
};

const lp_core_i2c_cfg_t i2c_cfg = {
    .i2c_pin_cfg =
        {
            .sda_io_num    = gpio_num_t::GPIO_NUM_6,
            .scl_io_num    = gpio_num_t::GPIO_NUM_7,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
        },
    .i2c_timing_cfg =
        {
            .clk_speed_hz = 400000,
        },
    .i2c_src_clk = LP_I2C_SCLK_DEFAULT,
};

HXC::NVS_DATA<CurrentCalib::params_t> CurrentCalib::params_data("CUR_CAL", CurrentCalib::DEFAULT);
HXC::NVS_DATA<uint32_t> VoltageCalib::ina226_k_data("V226_K", VoltageCalib::DEFAULT_INA226);
CurrentCalib::params_t* ulp_calib_params = reinterpret_cast<CurrentCalib::params_t*>(ulp_current_calib_params);
ulp_lp_core_spinlock_t* rtc_shared_lock =
    reinterpret_cast<ulp_lp_core_spinlock_t*>(static_cast<void*>(&ulp_shared_lock));

extern "C" {
extern const uint8_t bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t bin_end[] asm("_binary_ulp_main_bin_end");
}

static bool shared_lock_ready = false;

static int64_t read_shared_int64(volatile uint32_t* symbol) {
    // mapgen 将 LP 的 int64_t 导出为两个 uint32_t。按字节读取可避免 strict-aliasing 问题。
    int64_t           value       = 0;
    volatile uint8_t* source      = reinterpret_cast<volatile uint8_t*>(symbol);
    uint8_t*          destination = reinterpret_cast<uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) {
        destination[i] = source[i];
    }
    return value;
}

bool LP_Core_GetSnapshot(LP_Core_Snapshot* snapshot) {
    if (!shared_lock_ready || snapshot == nullptr) {
        return false;
    }
    // 在同一个跨核临界区复制全部 RTC 字段，避免读到撕裂的 64 位值或混合批次样本。
    ulp_lp_core_enter_critical(rtc_shared_lock);
    snapshot->state.ulp_state_raw    = ulp_ulp_state;
    snapshot->log_data               = ulp_log_data;
    snapshot->voltage_uv             = ulp_voltage_uv;
    snapshot->current_uA             = static_cast<int32_t>(ulp_current_uA);
    snapshot->shunt_register_raw     = static_cast<int16_t>(ulp_shunt_register_raw);
    snapshot->voltage_register_raw   = static_cast<uint16_t>(ulp_voltage_register_raw);
    snapshot->ina228_manufacturer_id = static_cast<uint16_t>(ulp_ina228_manufacturer_id);
    snapshot->frontend              = static_cast<SamplingFrontend>(ulp_sampling_frontend);
    snapshot->voltage_k             = ulp_active_voltage_k;
    snapshot->meter_uah              = read_shared_int64(ulp_meter_uah);
    snapshot->meter_uwh              = read_shared_int64(ulp_meter_uwh);
    ulp_lp_core_exit_critical(rtc_shared_lock);
    return true;
}

VoltageCalib::Runtime VoltageCalib::get_runtime() {
    LP_Core_Snapshot snapshot = {};
    Runtime result;
    if (!LP_Core_GetSnapshot(&snapshot)) return result;
    result.frontend = snapshot.frontend;
    result.active_k = snapshot.voltage_k;
    result.uncalibrated_uv = static_cast<uint32_t>(snapshot.voltage_register_raw) * 1250U;
    result.available = snapshot.state.ulp_state_bits.ulp_ina228_init_ok &&
                       !snapshot.state.ulp_state_bits.ulp_ina228_read_timeout &&
                       !snapshot.state.ulp_state_bits.ulp_i2c_init_err;
    return result;
}

void LP_Core_SetBoardTemperature(int32_t temperature) {
    if (!shared_lock_ready) {
        return;
    }
    // 板温由 HP 核采集，LP 核在下次电流补偿时读取。
    ulp_lp_core_enter_critical(rtc_shared_lock);
    ulp_Board_temperature = static_cast<uint32_t>(temperature);
    ulp_lp_core_exit_critical(rtc_shared_lock);
}

/**
 * @brief : 打印 LP 核日志
 * @return  {*}
 * @param {void*} arg
 */
void print_lp_core_log_task(void* arg) {
    while (1) {
        LP_Core_Snapshot snapshot = {};
        if (LP_Core_GetSnapshot(&snapshot) && snapshot.state.ulp_state_bits.ulp_have_log) {
            ESP_LOGI(LPTAG, "lp core log: %ld", snapshot.log_data);
            ulp_lp_core_enter_critical(rtc_shared_lock);
            reinterpret_cast<ULP_CORE_STATE*>(&ulp_ulp_state)->ulp_state_bits.ulp_have_log = false;
            ulp_lp_core_exit_critical(rtc_shared_lock);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief : 加载校准参数到 LP 核共享变量
 * @return  {*}
 * @param {bool} need_flag 是否需要设置校准参数标志位
 */
void load_current_calib_params(bool need_flag = true) {
    const CurrentCalib::params_t params = CurrentCalib::params_data.read();
    const uint32_t voltage_k = VoltageCalib::ina226_k_data.read();
    ulp_lp_core_enter_critical(rtc_shared_lock);
    ulp_ina226_voltage_k = VoltageCalib::valid(voltage_k) ? voltage_k : VoltageCalib::DEFAULT_INA226;
    *ulp_calib_params = params;
    if (need_flag) {
        reinterpret_cast<ULP_CORE_STATE*>(&ulp_ulp_state)->ulp_state_bits.ulp_reload_calib_params = true;
    }
    ulp_lp_core_exit_critical(rtc_shared_lock);
    DEVICE_EVENT_I(LPTAG, "lp: calib_loaded base_k=%u temperature_k=%d reload=%u",
                   static_cast<uint32_t>(params.current_base_K), params.temperature_K, need_flag ? 1U : 0U);
}

esp_err_t LP_Core_Load(void) {
    DEVICE_EVENT_I(LPTAG, "lp: init_start i2c_hz=%lu", static_cast<uint32_t>(i2c_cfg.i2c_timing_cfg.clk_speed_hz));
    LP_CLKRST.lp_clk_conf.fast_clk_sel =
        1; // IDF 6.0版本默认是内部RC时钟(17.5MHz)，且没有API可以切换到外部时钟源，需要手动操作寄存器切换到外部时钟源(20MHz)

    ESP_LOGI(LPTAG, "main core start init i2c...");
    esp_err_t ret = lp_core_i2c_master_init(LP_I2C_NUM_0, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(LPTAG, "LP I2C controller init failed: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(LPTAG, "main core init i2c success...");

    // 加载 LP 核二进制文件
    ret = ulp_lp_core_load_binary(bin_start, bin_end - bin_start);
    if (ret != ESP_OK) {
        ESP_LOGE(LPTAG, "LP binary load failed: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(LPTAG, "lp core load binary success...");

    ulp_lp_core_spinlock_init(rtc_shared_lock);
    shared_lock_ready = true;
    ulp_ulp_state     = 0;
    load_current_calib_params(false);

    ret = ulp_lp_core_run(&lp_core_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(LPTAG, "LP core start failed: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    int32_t timeout = 600;
    while (timeout -= 10) {
        LP_Core_Snapshot snapshot = {};
        if (LP_Core_GetSnapshot(&snapshot) && snapshot.state.ulp_state_bits.ulp_run &&
            snapshot.state.ulp_state_bits.ulp_ina228_init_ok) {
            break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    LP_Core_Snapshot snapshot = {};
    LP_Core_GetSnapshot(&snapshot);
    if (snapshot.state.ulp_state_bits.ulp_i2c_init_err) {
        ESP_LOGE(LPTAG, "lp: frontend result=unavailable reason=communication_failed manufacturer=0x%04x",
                 static_cast<uint32_t>(snapshot.ina228_manufacturer_id));
    } else {
        ESP_LOGI(LPTAG, "lp core i2c init success...");
    }

    if (timeout <= 0) {
        ESP_LOGE(LPTAG, "lp core run timeout");
        return ESP_ERR_TIMEOUT;
    } else {
        ESP_LOGI(LPTAG, "lp core run success: INA%lu voltage_k_ppm=%lu",
                 static_cast<uint32_t>(snapshot.frontend), snapshot.voltage_k);
        LP_Core_GetSnapshot(&snapshot);
        DEVICE_STATE_I(LPTAG, "lp: lifecycle old=starting new=running voltage_uv=%ld current_ua=%ld",
                       static_cast<int32_t>(snapshot.voltage_uv), static_cast<int32_t>(snapshot.current_uA));
    }

    // LP 日志任务只读取共享快照和输出单条日志，保留约 1.2KB 实测余量。
    xTaskCreate(print_lp_core_log_task, "print_lp_core_log", 1536, NULL, 4, NULL);
    return ESP_OK;
}
