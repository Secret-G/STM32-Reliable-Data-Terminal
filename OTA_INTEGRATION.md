# WIFI-1 Wi-Fi OTA 接入说明

## 工程布局

- `Boot_Loader/`：从 `bootloder_02` 复制的完整 Bootloader 工程，主体逻辑未修改。
- `Common/FirmwareUpdate/`：Bootloader 与 APP 共用的协议、CRC、Flash、缓存和双 Flag 实现。
- `Drivers/BSP/app_update/`：复用原 APP1 的候选槽下载状态机。
- `Drivers/BSP/app_trial/`、`Drivers/BSP/boot_confirm/`：复用 Trial 确认逻辑。
- `Drivers/BSP/wifi_ota/`：新增的 DeviceProtocol 到 AppUpdate 的薄适配层。

APP 链接地址为 `0x08020000`，最大镜像长度为 `0x40000`（256 KB），向量表偏移为 `0x20000`。

RTC Backup Register 分配：`BKP0R` 由 Bootloader Trial/Confirm 握手独占，`BKP1R` 保存下一个业务 sequence，`BKP2R` 保存 `DATA_SEQUENCE_MAGIC`。Bootloader 清理握手状态不会再破坏业务序号。

## 新增 DeviceProtocol 消息

所有多字节字段均为大端序。外层 DeviceProtocol 帧结构、CRC-16/CCITT-FALSE 不变。

| 消息 | 类型 | Payload |
| --- | ---: | --- |
| `OTA_START` | `0x20` | `image_size:u32 + image_crc_modbus:u16 + image_version:u32` |
| `OTA_DATA` | `0x21` | 固件原始数据，包序号使用外层 `sequence`，从 0 开始 |
| `OTA_END` | `0x22` | `packet_count:u32` |
| `OTA_RESPONSE` | `0xA0` | `request_type:u8 + result:u16 + value:u32 + max_data_len:u16` |

当前 `WIFI_OTA_DATA_MAX_SIZE` 为 64 字节，等于 DeviceProtocol 最大 Payload。服务器必须停等发送：收到当前包成功响应后才能发送下一包。

Qt 对 START、每个 DATA 和 END 分别启动 3 秒响应定时器；超时后原帧重发，最多重发 3 次。只有请求类型和序号都匹配的 `OTA_RESPONSE` 才能结束当前等待。

整包镜像 CRC 使用现有 Bootloader 的 CRC-16/MODBUS；DeviceProtocol 外层帧仍使用原有 CRC-16/CCITT-FALSE。

## OTA 状态流程

1. 正常状态下实时上传、历史补传保持原逻辑。
2. 收到合法 `OTA_START` 后暂停取新的业务上行帧。
3. 若已有业务帧等待 ACK，先让原重传流程结束。
4. 调用 `AppUpdate_HandleStart()`，设备自动选择非活动候选槽并擦除。
5. START 成功响应返回设备最大包长。
6. 每个 `OTA_DATA` 调用 `AppUpdate_HandleData()`；写入成功后才回复成功响应。
7. 重复的上一包只重新响应，不重复写入；乱序包返回期望序号。
   重复 START 必须与当前会话的设备 ID、序号、镜像大小、CRC 和版本全部一致；等待业务结束时不重复擦除且不刷新进入超时，START 响应丢失时由 AppUpdate 幂等重发响应。
8. `OTA_END` 调用 `AppUpdate_HandleEnd()`，检查包数、字节数和整包 CRC。
9. 校验成功后写入 `PENDING`，先建立 10 秒强制复位期限，再发送最终响应；响应成功则延时 500 ms 后复位，始终发送失败也会在期限到达后复位。重复 END 不延长该期限。
10. Bootloader 按原逻辑安装、试运行、确认或回滚。
11. 接收阶段 30 秒无合法进度则 Abort；TCP 断开且尚未 PENDING 时同样 Abort，并恢复业务。

OTA 期间 DataProducer 和 StorageTask 不停止；实时队列满后继续使用既有的 SD 落盘策略。

## 关键调用关系

```text
ESP32 UART DMA -> +IPD Parser -> ESP32_Network_OnTcpData
  -> DeviceProtocol_ParserFeed
  -> WifiOta_HandleFrame
  -> AppUpdate_HandleStart/Data/End
  -> BootCache / BootFlash / BootFlag / BootCRC
  -> Slot A/B -> PENDING -> NVIC_SystemReset
  -> Bootloader -> Run APP -> App_TrialProcess -> Confirm
```

## 人工验证步骤

1. 使用正式版 Keil/Arm 工具链编译 APP，确认 map 中执行地址为 `0x08020000`，镜像不超过 256 KB。
2. 先烧录 `Boot_Loader`，再将当前 APP 烧录到 Run 区 `0x08020000`。
3. 启动 `wifi-server`，确认原实时上传、ACK、重传和历史补传均正常。
4. 点击“选择固件并 OTA”，选择链接于 `0x08020000` 的 `.bin`，输入高于当前版本的版本号。
5. 观察 START ACK、逐包 DATA ACK、100% 进度、END ACK 和设备复位。
6. 复位后确认 Bootloader 安装新镜像，新 APP 运行约 3 秒后完成 Trial Confirm。
7. OTA 中途断开服务器超过 30 秒，确认设备退出 OTA，业务上传恢复，当前 Run APP 不受影响。
8. 测试重复 DATA、错误序号、错误整包 CRC，确认不会写入 PENDING。
9. 关闭自动业务 ACK 后发起 OTA，确认设备先结束当前业务重试，再进入 OTA。
10. 最后再次验证 Bootloader UART 恢复升级通道未受影响。

## 当前构建说明

设备端全部 C 文件已由 Arm Compiler 6.16 完整编译通过，无编译错误和警告。当前机器的 Keil 链接器许可证限制代码大小，约 57.6 KB 的镜像在链接阶段报 `L6050U`；这不是 256 KB Flash 分区超限。Qt `wifi-server` 已使用 Qt 6.8.3 / MinGW 13.1 成功编译。
