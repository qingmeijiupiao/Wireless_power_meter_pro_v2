# PRO V2 240×135 UI

本文描述当前已提交实现，不保留适配过程中的中间字号、坐标或工具会话记录。页面源码是布局的最终依据。

## 当前布局与交互

| 页面 | 当前布局 | 刷新周期 |
|---|---|---|
| Main | 左侧 V/A 两行大读数，右侧运行时间与 OTP/OVP或UVP/OCP 标签；底部 W/T/ON-OFF 栏 | 67ms，约15 FPS |
| Battery | 顶部实时 V/A/W、会话时间和输出圆；y=32/82 两行累计值，W/A badge、44号数字与右对齐28号单位 | 250ms |
| Curve | 顶部指标/窗口栏；单指标区(50,32)、185×97；ALL区(6,32)、228×97 | 200ms |
| Wireless | 网络模式、遥控器电量、SSID、信号柱、IP和信道信息 | 500ms |
| Settings | x=60 的三行174×33菜单，行距40；左侧程序绘制滑杆；详情弹窗 | 200ms |
| Boot | 156×77 Logo 居中于(42,29) | 按保存的Logo时长 |

普通状态主键切换输出，侧键向后翻页，BOOT复用键向前翻页。页面优先消费事件，编辑或弹窗中的按键行为以各页面为准。
Battery 侧键长按重置共享会话；Curve 侧键双击切换 V/A/W/ALL，长按进入编辑；Wireless 侧键长按进入 AP；Settings 长按进入菜单。

## 字体与数值

- 主页面 V/A 和容量大数字使用 `DENGB44_NUM`，子集为 `0123456789.-+VAWmhe`。
- 容量单位使用 `DENGB28_UNITS`，子集为 `mWAh`；普通文本使用 DENGB16/20，最低回退字号为 DENGB16。
- Main 电压固定三位小数，电流和功率使用 `UI::format_fixed_digits`，最多五位数字、三位小数。
- Battery 顶部实时值最多三位数字、两位小数；实时值和累计显示均取绝对值，底层积分仍保留符号。
- 容量数值与单位总宽度限制178px，按实际字体宽度逐步减少小数位；约999.5m单位起优先换算 Wh/Ah，极大值使用明确指数。
- 公共 `UI::text` 按字符支持与区域宽度选择字号，仍过长则加省略号；非ASCII字节显示问号，不提供中文字库。
- 正常保护标签隐藏；仅显示实际告警/保护。OVP正常时显示UVP，不能同时占用该标签区域。
- 设置名称和值保持单行，空间不足时使用可辨识短名称；弹窗保留完整标题。共13个设置项。

## 资源与驱动

常规页面图标由 `widgets/ui_chrome` 和页面基础图元绘制，仅启动画面引用位图。
旧标签图片与中间字号文件仍保留在资源目录，但未引用资源不代表实际链接占用。

两帧 RGB565 像素数据共129600字节；曲线缓存保持1200点、每点4字节。
当前每帧整屏重绘，SPI polling 按最多32768字节拆分发送64800字节像素数据，再切换缓冲，不是异步局部刷新。

横屏 `COLSTART=40`、`ROWSTART=52`，见 `st7789.h`；正/反横屏使用现有旋转映射。
字符和图片在屏幕边缘裁剪，图片裁剪保留原始行跨度。
APP分区每槽0x150000，UI修改后仍需检查固件体积与联网时堆余量。

## 字体生成与验证

从仓库根目录运行，Python需Pillow：

```powershell
python scripts/generate_font.py components/assets/Fonts/Front_preview/DENGB.TTF 44 DENGB44_NUM --chars '0123456789.-+VAWmhe' --output-dir build_ui_assets
python scripts/generate_font.py components/assets/Fonts/Front_preview/DENGB.TTF 28 DENGB28_UNITS --chars 'mWAh' --output-dir build_ui_assets
```

将生成的对应 `.h`、`.cpp`、`_preview.bmp` 分别同步至 Fonts 的 `Font_include`、`Font_src`、`Front_preview`，然后检查：

```powershell
python scripts/check_display_ui.py
idf.py reconfigure
idf.py build
```

主机检查需要C++17编译器，默认g++，可用 `--cxx` 指定。脚本编译实际绘图函数、页面render和字体，模拟业务服务，检查裁剪、非法字符、格式化边界、保护标签与全部设置项宽度，并生成 `build_ui_check/ui-preview.png` 及各状态图。
它不覆盖真实按键调度、SPI时序、实物颜色、偏移或射频并发。提交 `e4b555d` 记录了当时编译、主机检查与手板测试通过；后续修改仍应重新执行相关验证。
