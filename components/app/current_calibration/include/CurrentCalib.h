/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 电流校准相关数据结构
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-01 18:39:03
 */
#ifndef CURRENT_CALIBRATION_DATA_H
#define CURRENT_CALIBRATION_DATA_H

#include <stdint.h>

namespace CurrentCalib {

struct point_t {
    int16_t register_value;       // 兼容校准域值，INA228 VSHUNT signed20 / 8，2.5uV/单位
    int16_t offset_current_100uA; // 电流偏移值 单位：100uA
} __attribute__((packed, aligned(4)));

struct params_t {
    uint16_t current_base_K;           // 基准电流K值 单位：每单位寄存器值对应XuA电流
    point_t  points[6];                // 电流校点
    int16_t  temperature_K;            // 温度K值 单位：每单位温度对应1/1000000(ppm)电流漂移
} __attribute__((packed, aligned(4))); // 这个结构体要跨核心共享，必须对齐到4字节
static_assert(sizeof(params_t) % 4 == 0, "CurrentCalib::params_t size must be aligned to 4 bytes");
constexpr int32_t BASE_TEMPERATURE = 3500; // 温度校准系数基准测试温度 单位：0.01℃
} // namespace CurrentCalib

#endif