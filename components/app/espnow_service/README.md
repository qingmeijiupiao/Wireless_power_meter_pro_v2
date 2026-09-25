# espnow_service

`espnow_service` 只实现 Wireless Power Meter 的产品业务定义和处理逻辑：

- 开关控制请求与响应
- 遥控开关电量上报
- 实时数据读取请求与响应
- 周期数据上报
- 业务 payload 编解码
- 功率输出、测量状态和能量计业务回调

可靠收发、ACK、重传、去重、ESP-NOW 驱动回调、配对、peer/LMK 持久化、信道恢复和
消息 ID 分发全部由 `espnow_link` 负责。

## 结构

```text
espnow_service/
├── include/espnow_service.h
├── private_include/espnow_service_internal.h
└── src/
    ├── espnow_service_business.cpp
    └── espnow_service_business_protocol.cpp
```

`init()` 为每个业务消息 ID 直接向 `espnow_link` 注册独立回调，不创建额外业务队列或
二次分发任务。每个接收回调内部直接完成校验、解码和业务处理，不再注册第二层产品
处理回调。回调由 `espnow_link` 消息分发任务调用，因此业务处理必须快速返回。

开关请求复制 peer、request_id、action 后提交到 `PowerOutput`。开启前短路检测由输出工作任务执行，完成后才发送原格式的 7 字节响应；不保存接收 payload 指针。短路、忙和取消映射为 `REJECTED`，检测/硬件错误或超时映射为 `INTERNAL_ERROR`。链路 ACK 仍不代表输出成功。

## 消息

| ID | 语义 |
|---|---|
| `0x0200` | 可靠开关控制请求 |
| `0x0201` | 可靠开关控制响应 |
| `0x0202` | 尽力远程开关电量上报 |
| `0x0210` | 可靠实时数据请求 |
| `0x0211` | 可靠实时数据响应 |
| `0x0212` | 尽力周期数据上报 |

请求和响应使用非零 `request_id` 关联。协议字段通过 `espnow_codec.h` 按明确的小端格式
读写，不使用可能产生未对齐访问、严格别名违规和本机端序依赖的 `reinterpret_cast`。

远程开关在控制事务结束后以加密单播发送 1 字节电量百分比，范围为 `0..100`，
不等待 ACK。功率计在本次运行首次收到合法控制包后将对应来源视为已连接，并只接受
该来源后续发送的电量包；连接和电量状态不写入 NVS。

## 初始化

```cpp
ESP_ERROR_CHECK(EspNowLink::init());
ESP_ERROR_CHECK(EspNowService::init());
```

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`global_state`](../global_state/README.md)（`app`）
- [`power_output`](../power_output/README.md)（`app`）
- [`energy_meter`](https://github.com/qingmeijiupiao/wireless-power-components/blob/326101ce6642ac052fccea73469651e540f6e48e/components/middleware/energy_meter/README.md)（`middleware`）
- [`espnow_link`](../../middleware/espnow_link/README.md)（`middleware`）
- [`diagnostic_log`](https://github.com/qingmeijiupiao/wireless-power-components/blob/326101ce6642ac052fccea73469651e540f6e48e/components/common/diagnostic_log/README.md)（`common`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->
