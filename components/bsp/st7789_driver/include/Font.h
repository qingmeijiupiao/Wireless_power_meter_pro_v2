/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 字体结构体，用于表示字体数据
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-01 18:34:46
 */
#ifndef FONT_H
#define FONT_H
#include <stdint.h>

#define FONT_CHAR_NUM 95

struct Font_t {
    const uint8_t  font_height; // 只支持等高字体，高度为font_height
    const uint8_t* width_table;
    const uint8_t* font_data;
};

#endif