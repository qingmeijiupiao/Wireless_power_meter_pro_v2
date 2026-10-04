# ota_service

远端 APP 固件检查与在线升级应用服务。组件通过 HTTPS 获取 OTA Manifest，复用
`ota_manager` 完成备用分区写入、镜像校验和启动分区切换。

## 版本检查

- Manifest 路径：
  `/gh/qingmeijiupiao/Wireless_power_meter_pro_v2@firmware-dist/ota/latest.json`
- 依次尝试 jsDelivr 镜像源 `fastly.jsdelivr.net`、`gcore.jsdelivr.net`、`cdn.jsdelivr.net`
  （顺序经实测调整，`cdn.jsdelivr.net` 在部分网络会被重置，放最后），任一源成功即停止；
  单次请求超时 5 秒。不同主机名会触发独立 DNS 解析，用于绕开 ESP-IDF 只使用首条 A
  记录、可能连接到不可达边缘 IP 的问题。
- Manifest 由 Release 工作流生成，包含版本、固件大小和不可变下载 URL。
- 发布内容保存在独立的 `firmware-dist` 分支，不依赖开发者本地构建或手工同步。
- 仅当远端语义版本严格高于当前运行版本时允许在线升级。
- 相同版本和较低版本都返回 `up_to_date`；降级只能由用户手动上传 APP 固件。
- HTTPS 请求前要求系统时间有效，避免 TLS 证书有效期校验误判。

## 固件下载

固件使用 Manifest 下发的 jsDelivr commit SHA 固定地址下载。该地址指向
`firmware-dist` 中不可变的发布提交，不依赖 GitHub Release 代理。

下载使用 ESP-IDF HTTP Client 自动处理重定向，通过证书 Bundle 校验 TLS。
响应数据按块写入备用 OTA 分区，不在 RAM 中缓存完整固件。下载完成后校验
Manifest 声明的固件大小，并由 ESP-IDF OTA API 执行固件镜像校验。校验和激活
成功后等待 2 秒并自动重启。

固件下载先在完成 Manifest 的镜像源上做 HTTP `Range` 断点续传（同一源内容一致），
该源整体失败后才换源并从头重下，避免不同镜像返回的固件内容不一致。连接中断后保留
当前 OTA 写入会话，并从已写入偏移继续请求；续传响应必须返回 `206 Partial Content`，
且 `Content-Range` 的起始偏移和固件总长度必须与本地状态一致。协议校验、固件长度或
Flash 写入发生错误时立即停止，避免错误内容写入目标分区。

针对 ESP32-C6 堆内存偏小、TLS 接收 16KB 记录易失败的场景，启用 mbedTLS 动态缓冲
（`CONFIG_MBEDTLS_DYNAMIC_BUFFER`），并在握手后把 RX 缓冲切换为静态复用
（`HTTP_TLS_DYN_BUF_RX_STATIC`），避免逐条记录反复申请/释放造成堆碎片。

每次尝试及其结果都通过 `diagnostic_log` 写入 ESP 日志，并由全局 Hook 自动持久化。
状态迁移附带快照，失败使用 `WARN` / `ERROR`。全部自动重试失败后，用户可从 Web
或屏幕重新发起，也可从固件页面手动下载 APP 固件后上传。

## 状态

`idle`、`checking`、`update_available`、`up_to_date`、`downloading`、
`verifying`、`restarting`、`failed`。

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`ota_manager`](https://github.com/qingmeijiupiao/wireless-power-components/blob/0e4e8d5a31da8bbdd6d8149ecfc5ae73416b2a3d/components/middleware/ota_manager/README.md)（`middleware`）
- [`diagnostic_log`](https://github.com/qingmeijiupiao/wireless-power-components/blob/0e4e8d5a31da8bbdd6d8149ecfc5ae73416b2a3d/components/common/diagnostic_log/README.md)（`common`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->
