# PRO V2 240×135 UI

设计参考：[Figma 第一版](https://www.figma.com/design/R9U9UBv5g8ntgI0wLGUphC/PROV2-UI-DESIGN?node-id=2024-6)。
Figma MCP 已达到套餐限额；实现依据本次已读取的原稿数据、新稿创建坐标及截图，未重新导出图片。

## 布局

| 页面 | 主要区域 |
|---|---|
| Main | 左侧V/A读数x=6、y=4/55，单位位于右侧；右侧62px状态区；y=113功率/温度/输出栏 |
| Battery | y=5实时值；y=33/69累计值，单位固定在右侧；y=114双计时 |
| Curve | y=5控制栏；单指标绘图区(50,32)，185×97；ALL绘图区(6,32)，228×97 |
| Wireless | 顶部模式/遥控器电量；228px宽SSID行；左信号柱，右IP与信号信息 |
| Settings | x=60，174×33三行菜单、行距40；弹窗(8,8)，224×119 |
| Boot | 复用现有156×77 Logo，居中于(42,29)，不增加启动图片 |

共用图元在 `widgets/ui_chrome` 中实现。数值小数位、计量单位、时间窗、按键和设置项沿用
原代码。保护标签正常时隐藏；OVP/UVP选择规则不变。设置菜单仍以中间行为编辑选中项，
Figma 里的高亮和告警只是示例，不作为实时状态。

Figma 使用 Inter；固件复用等线 DENGB16/20，并新增 DENGB28_NUM（实际字高27）。
容量页使用28号数字子集；主页V/A使用DENGB44_NUM，功率复用DENGB20。字形和间距因此与
Inter 预览略有不同。累计值单位固定右对齐区域，给极大累计读数留出空间。

长数值优先降至20/16字号，不通过减少小数位挤入。极端长度超出最小字号容纳范围时
显示省略号，避免误显示成另一个数值；SSID同样按像素宽度省略。非ASCII字符仍按原有
ASCII显示能力用问号代替，此版不引入中文字库。

## Flash 与 RAM

- 常规页不再引用RGB565标签、开关、圆点和齿轮图片；旧资源文件保留但不链接入固件。
- 保留的启动Logo数据为24,024字节。
- 新数字字体仅包含 `0123456789.-+`：位图5,400字节，95项宽度表95字节，另加Font_t结构。
- 不增加帧缓冲或曲线历史长度；沿用bringup的两帧RGB565缓冲，共129,600字节RAM。
- 应用分区沿用PRO V2的0x150000，不调整OTA/黑匣子分区。
- `draw_char`裁剪屏幕边缘，127及非ASCII字节不会越界索引字表；裁剪图片保留原始行跨度。

## 生成与检查

在仓库根目录运行（Python需Pillow）：

```powershell
python scripts/generate_font.py components/assets/Fonts/Front_preview/DENGB.TTF 28 DENGB28_NUM --chars '0123456789.-+' --output-dir build_ui_assets
Copy-Item build_ui_assets/DENGB28_NUM.h components/assets/Fonts/Font_include/
Copy-Item build_ui_assets/DENGB28_NUM.cpp components/assets/Fonts/Font_src/
Copy-Item build_ui_assets/DENGB28_NUM_preview.bmp components/assets/Fonts/Front_preview/
python scripts/check_display_ui.py
idf.py build
```

主机检查需要C++17编译器，默认`g++`，可用`--cxx`指定。它抽取并编译实际C++绘图函数和页面
render方法，使用真实字体和曲线历史算法、模拟外设服务；检查裁剪、非法字符、数字子集、
长文本边界与保护标签，并生成11张状态截图和`build_ui_check/ui-preview.png`。
不覆盖真实按键/服务、SPI时序、实际屏幕颜色与位置。业务代码仍通过ESP-IDF全量构建验证。

## 实物确认

按用户要求，横屏`ROWSTART`当前为52（沿用工作区的实物校准值），沿用驱动的正/反横屏映射，保留已校准的Y起始地址。
竖屏模式的已有偏移保持不变。没有烧录设备；应在实物确认正常横屏与180度横屏的上/下边缘、
颜色、30Hz主页刷新及Wi-Fi运行时剩余堆内存。初始化和SPI分段发送沿用bringup实现。

所有页面的最小字号统一为DENGB16（实际字高15），不再回退到DENGB12。
OUTPUT、曲线NOW/MAX和电池百分比区域已加宽；复用现有16号字库，不新增字库数据。

设置菜单保留左侧滑杆图标，名称和值保持单行DENGB16。优先完整名称，
空间不足时使用清楚的短名称（如ESP-NOW info、CAN rate），弹窗标题保持全写。
CAN速率显示Mbps/kbps单位，不增加图片资源。
主机预览额外覆盖全部13个设置项，检查名称与代表性数值的像素宽度。

首页V/A数字使用44号子集，位图14842字节、宽度表95字节。原36号字库不再被页面引用。
去掉V/A/W前置色块，V/A单位与数值合成字符串，以同一字号紧随数值显示，每个字段只调用一次UI::text；功率以16号字体和W后缀显示在底栏。
底栏y=113：功率x=6、温度x=120、ON/OFF色块x=190，不再显示OUTPUT和温度前置T色块。
保持三位小数，超长数值按宽度降字号；不新增页面图片或帧缓冲。
生成命令：`python scripts/generate_font.py components/assets/Fonts/Front_preview/DENGB.TTF 44 DENGB44_NUM --chars '0123456789.-+VA' --output-dir build_ui_assets`。

容量页更新：顶部x=6/55/104显示实时V/A/W，x=154显示容量计时，取消系统时间。
输出圆仍位于(220,8)、直径14。容量两行y=32/82，保留W/A badge；
数字和mWh/mAh单位合并为一次UI::text，统一使用DENGB32_METER（32号，六位数字自动调整小数位）。
字库仅生成0123456789.-+mWAh，位图9425字节，宽度表95字节。

容量页最终字号更新为DENGB44_NUM，与主页共用字库；保留badge和顶部布局。
不足1000时优先显示mWh/mAh，达到换算边界后显示Wh/Ah；按198px宽度逐步减少小数位。
极大累计值使用带明确指数的Wh/Ah科学计数法，保持44号字体。仅改变显示，累计精度不变。
44号字库包含0123456789.-+VAWmhe，位图20213字节；32号容量字库不再被页面引用。

容量页当前布局：W/A badge与44号数值等高，单位独立使用28号字库DENGB28_UNITS，
仅包含mWAh（位图2349字节、宽度表95字节）。按数值44号与单位28号的实际总宽度
减少小数位并换算单位，保证整行在178px内；单位右对齐。
