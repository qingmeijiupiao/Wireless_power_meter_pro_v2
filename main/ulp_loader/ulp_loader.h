/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description:  LP 核加载模块，负责加载 LP 核二进制文件并启动 LP 核
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-04-05 18:12:38
 */

#ifndef ULP_LOADER_HPP
#define ULP_LOADER_HPP

#include "esp_err.h"
#include "ulp_main.h"
#include "ulp_app/ulp_state.h"
#include <stdint.h>

/**
 * @brief 初始化 LP I2C、加载并启动 LP Core 固件。
 * @return ESP_OK 启动成功；其他值表示初始化、加载或启动失败。
 */
esp_err_t LP_Core_Load(void);

struct LP_Core_Snapshot {
    ULP_CORE_STATE state                  = {};
    uint32_t       log_data               = 0;
    uint32_t       voltage_uv             = 0;
    int32_t        current_uA             = 0;
    int16_t        shunt_register_raw     = 0;
    uint16_t       voltage_register_raw   = 0;
    uint16_t       ina228_manufacturer_id = 0;
    int64_t        meter_uah              = 0;
    int64_t        meter_uwh              = 0;
};

/**
 * @brief 原子读取一份 LP Core 共享状态快照。
 * @param snapshot 接收快照的输出缓冲区。
 * @return true 读取成功；false 参数无效或共享锁获取失败。
 */
bool LP_Core_GetSnapshot(LP_Core_Snapshot* snapshot);

/**
 * @brief 向 LP Core 发布当前板载温度。
 * @param temperature 板载温度，单位 0.01 摄氏度。
 */
void LP_Core_SetBoardTemperature(int32_t temperature);

#endif // ULP_LOADER_HPP
