# ulp_loader

HP侧LP加载与共享数据桥接：初始化LP I2C、加载独立二进制、下发校准参数、启动LP并提供一致快照。调试命令通过GlobalState获取兼容诊断值，不暴露可随意读取的RTC raw指针。

## 启动顺序

1. 将LP fast clock切到20MHz，和LP计时常量匹配。
2. 初始化LP I2C：GPIO6 SDA、GPIO7 SCL、400kHz。
3. 加载 `ulp_embed_binary` 生成的LP二进制。
4. 初始化共享自旋锁，标记快照可用，清零状态位。
5. 从 `CurrentCalib::params_data` 读取NVS校准值，复制到RTC；首次无需设置重载标志。
6. 启动LP，每10ms等待 `ulp_run` 和 `ulp_ina228_init_ok`，总计约600ms。
7. 握手成功后创建LP日志转发任务；超时返回 `ESP_ERR_TIMEOUT`，LP仍可继续重试。

I2C控制器初始化、LP加载和启动错误使用ESP_ERROR_CHECK；不是所有错误都作为可恢复返回值处理。

## API

| 接口 | 作用 |
|---|---|
| LP_Core_Load | 按上述顺序启动LP |
| LP_Core_GetSnapshot | 共享锁内复制状态、采样、兼容raw和64位累计；空参数或锁未初始化返回false |
| LP_Core_SetBoardTemperature | 发布0.01℃板温，锁未初始化时忽略 |
| load_current_calib_params | loader实现内的校准下发函数；运行期调用时设置LP重载标志 |

`LP_Core_Snapshot` 的分流raw单位2.5μV、电压raw单位1.25mV，分别为统一兼容值；INA226电压raw为分压端校准前读数。快照新增frontend和voltage_k，供型号与有效系数诊断。
mapgen导出的64位符号按字节复制，且整个快照在同一临界区读取，避免撕裂和别名问题。

LP日志任务栈1536字节、优先级4、10ms轮询；当前LP主循环预留日志字段但没有周期日志上报。
共享锁内只复制数据，不进行NVS、日志输出、I2C或插值计算。

实际实现见 `ulp_loader.cpp`，公开接口见 `ulp_loader.h`，测量算法见 [ulp_app](../ulp_app/README.md)。
