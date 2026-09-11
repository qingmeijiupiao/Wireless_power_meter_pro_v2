// Auto-generated backlight LUT
// Gamma = 2.2, min_duty = 200
// Brightness: uint8_t (0-255), Duty: uint16_t (0-65535)
// Table size: 32

#ifndef BACKLIGHT_LUT_H
#define BACKLIGHT_LUT_H

#include <stdint.h>
#include <vector>
/*
 * @Description: 背光LUT表
 * @param {uint8_t} brightness 亮度,0-255 ～0-100%
 * @param {uint16_t} duty 占空比,0-65535  ～0-100%
 * @return {std::vector<std::pair<uint8_t, uint16_t>>} 背光LUT表
 * @note 亮度和占空比成线性关系,亮度增加,占空比增加
*/
const std::vector<std::pair<uint8_t, uint16_t>> backlight_lut = {
    {  0,     0},
    {  8, 13586},
    { 16, 18617},
    { 24, 22385},
    { 32, 25512},
    { 41, 28555},
    { 49, 30964},
    { 57, 33168},
    { 65, 35208},
    { 74, 37346},
    { 82, 39130},
    { 90, 40821},
    { 98, 42432},
    {106, 43973},
    {115, 45632},
    {123, 47049},
    {131, 48416},
    {139, 49738},
    {148, 51177},
    {156, 52416},
    {164, 53621},
    {172, 54795},
    {180, 55939},
    {189, 57194},
    {197, 58281},
    {205, 59346},
    {213, 60387},
    {222, 61534},
    {230, 62532},
    {238, 63512},
    {246, 64473},
    {255, 65535},
};

#endif // BACKLIGHT_LUT_H
