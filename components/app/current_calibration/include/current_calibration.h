/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 电流校准管理
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-01 18:38:51
 */
#ifndef CURRENT_CALIBRATION_H
#define CURRENT_CALIBRATION_H
#include "HXC_NVS.h"
#include "CurrentCalib.h"
#include "VoltageCalib.h"

namespace VoltageCalib {
// Separate key preserves the existing CUR_CAL blob layout and calibration.
extern HXC::NVS_DATA<uint32_t> ina226_k_data;
}

namespace CurrentCalib {

constexpr params_t DEFAULT = {
    .current_base_K = 1250, // 默认电流参数K，对应采样电阻值2毫欧姆
    .points         = {},
    .temperature_K  = 0,
};

extern HXC::NVS_DATA<params_t> params_data;

} // namespace CurrentCalib
#endif
