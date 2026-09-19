# Battery 页面

页面显示实时 V/A/W、输出状态、共享会话累计能量/电量和会话时间，不显示系统运行时间。

## 数据流

`render()` 分别读取 `EnergyMeter::snapshot()` 和 `get_global_state()`，每250ms整屏重绘。
侧键长按调用 `EnergyMeter::reset()` 并记录事件，只更新共享基线，不清零LP累计；Web和Shell同步使用新会话。
其他按键交给 UIManager 的默认翻页或输出操作。

## 布局和数值

- 顶部实时值位于x=6/55/104，会话时间位于x=154，输出状态圆位于(220,8)，直径14。
- 累计两行位于y=32/82，使用44号数字和右对齐28号单位，左侧为W/A标记。
- 实时值经过 `format_fixed_digits` 取绝对值，最多三位数字、两位小数。
- 累计显示也取绝对值；LP和会话缓存保持有符号整数，显示规则不改变积分方向。
- 默认优先mWh/mAh，约999.5m单位起换Wh/Ah；按数字和单位总宽度178px减少小数位，极大值使用科学计数法。
- 累计值使用 `DENGB44_NUM`，单位使用 `DENGB28_UNITS`，不是固定六位数字或32号字体。

实现见 `src/pages/battery_page.cpp`，整体说明见 [PRO V2 UI](pro_v2_ui.md)。
