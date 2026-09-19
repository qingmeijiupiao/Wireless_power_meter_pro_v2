# ota_manager

APP 固件 OTA 写入与启动分区切换中间件。仅封装 ESP-IDF `app_update`
组件，不负责固件下载、网络连接、进度展示或重启时机。

## 行为

- `begin()` 通过 `esp_ota_get_next_update_partition()` 自动选择非当前运行分区。
- `write()` 将应用层下载到的数据按顺序写入 OTA 目标分区。
- `finish()` 调用 `esp_ota_end()` 完成 ESP-IDF 原生镜像校验，但不切换启动分区。
- `activate()` 在用户确认后调用 `esp_ota_set_boot_partition()` 设置下次启动分区。
- `activate()` 成功后不会主动重启，由应用层决定何时调用 `esp_restart()`。
- 同时只允许一个写入会话；完成分区切换后，重启前不允许开始新会话。
- 只接受顺序写入，不负责 HTTP 断点续传或乱序写入。上层可在同一 `WRITING` 会话中
  重新建立网络连接，从当前累计偏移继续按顺序调用 `write()`。

ESP-IDF APP 镜像格式内置校验信息。`esp_ota_end()` 会校验新写入镜像；
启用 Secure Boot 时也会进行签名校验。

## 基本流程

```mermaid
flowchart LR
    App["APP 下载模块"] --> Begin["OtaManager::begin(size)"]
    Begin --> Select["选择非运行 OTA 分区"]
    Select --> Write["OtaManager::write(data, size)"]
    Write --> Finish["OtaManager::finish()"]
    Finish --> Verify["esp_ota_end()<br/>校验固件"]
    Verify --> Confirm["APP 请求用户二次确认"]
    Confirm --> Activate["OtaManager::activate()"]
    Activate --> Boot["esp_ota_set_boot_partition()"]
    Boot --> Restart["APP 决定何时重启"]
```

## 集成示例

```cpp
#include "ota_manager.h"

ESP_ERROR_CHECK(OtaManager::begin(content_length));

while (download_has_data()) {
    ESP_ERROR_CHECK(OtaManager::write(buffer, received_size));
}

ESP_ERROR_CHECK(OtaManager::finish());
// APP 展示新固件版本并等待用户确认。
ESP_ERROR_CHECK(OtaManager::activate());
// APP 完成响应、保存状态或提示用户后，再自行调用 esp_restart()。
```

下载失败时中止会话：

```cpp
OtaManager::abort();
```

下载前无法获得固件长度时：

```cpp
OtaManager::begin(OtaManager::IMAGE_SIZE_UNKNOWN);
```

## API

| API | 说明 |
|-----|------|
| `begin(image_size)` | 选择目标分区并开始 OTA；固件长度未知时传 `IMAGE_SIZE_UNKNOWN` |
| `write(data, size)` | 顺序写入固件数据 |
| `finish()` | 校验固件，不切换启动分区 |
| `activate()` | 设置下次启动分区，不主动重启 |
| `abort()` | 中止当前写入，或放弃尚未激活的已校验固件 |
| `get_status()` | 获取状态、目标分区和累计写入长度 |
| `get_running_partition()` | 获取当前运行分区 |
| `get_boot_partition()` | 获取已配置为下次启动的分区 |
| `get_target_partition()` | 获取当前会话目标分区 |
| `get_next_update_partition()` | 获取下一次 OTA 写入应使用的分区 |
| `get_running_app_description()` | 读取当前运行固件描述和版本号 |
| `get_boot_app_description()` | 读取下次启动固件描述和版本号 |
| `get_target_app_description()` | 读取 OTA 目标固件描述和版本号 |
| `confirm_running_firmware()` | 标记当前固件有效，取消自动回滚 |
| `rollback_is_possible()` | 查询是否存在可回滚固件 |
| `rollback_and_reboot()` | 标记当前固件无效，回滚并立即重启 |

`get_target_app_description()` 只有在目标分区已写入有效 APP 镜像后才能成功读取。
下载过程中如需提前展示新版本，建议由 APP 下载模块从服务端元数据获取。

## 状态

| 状态 | 说明 |
|------|------|
| `IDLE` | 没有 OTA 写入会话 |
| `WRITING` | 正在接收并写入固件 |
| `VERIFIED` | 固件校验通过，等待应用层确认激活 |
| `READY_TO_REBOOT` | 固件校验通过，已切换下次启动分区 |

## 回滚

回滚接口直接封装 ESP-IDF API。当前工程未开启
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。启用自动回滚时，应用层需要：

1. 在新固件启动后执行必要自检。
2. 自检通过后调用 `confirm_running_firmware()`。
3. 自检失败时调用 `rollback_and_reboot()`。

## 分区要求

工程分区表已包含 OTA 所需分区：

| 分区 | 类型 | 用途 |
|------|------|------|
| `otadata` | `data, ota` | 保存下次启动 OTA 分区选择 |
| `app0` | `app, ota_0` | APP 固件槽位 A |
| `app1` | `app, ota_1` | APP 固件槽位 B |

<!-- dependency-links:start -->
## 依赖导航

无工程内组件依赖；仅依赖 ESP-IDF 组件或 C/C++ 标准库。

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->
