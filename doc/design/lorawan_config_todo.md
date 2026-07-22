# LoRaWAN 配置 TODO List

> 基于 2026-06-24 ~ 2026-06-26 讨论整理

---

## 1. FPort 重新规划

当前所有上行统一用 FPort=2，需改为按消息类别分配。

| FPort | 方向 | 内容 |
|-------|------|------|
| 10 | 上行 | Badge 心跳 (CMD 0x00) |
| 11 | 上行 | Hub 心跳 (CMD 0x00) |
| 20 | 上行 | 其他上行 (power 0x01, key_event 0x02) |
| 20 | 下行 | 全部下行（单播+多播，通过 CMDID + group/room 过滤区分） |

**要点**: 多播不单独分 FPort，靠 LoRaWAN MAC 层 DevAddr 区分单播/多播，应用层靠 group_id + room_id 过滤。

### 改动清单

- [ ] [hal_sx1262.c](firmware/src/hal/hal_sx1262.c) — `send_heartbeat` 等调用处传入正确 FPort
- [ ] [main.c](firmware/src/main.c) — heartbeat/power_report 按设备类型选择 FPort
- [ ] [proto_internal.h](firmware/src/proto/proto_internal.h) — 新增 `FPORT_BADGE_UP = 10` / `FPORT_HUB_UP = 11` / `FPORT_COMMON = 20` 宏

---

## 2. 多播组管理

### Phase 1（当前可实现）— 应用层双重过滤

无需新增 CMDID。CMD 0x50 已能设置 group_id，room_id 可复用同一指令扩展。下行 CMD 0x03/0x0A 中增加 room_id 字段即可实现位置过滤。

**下行处理**

- [ ] [hal_sx1262.c](firmware/src/hal/hal_sx1262.c) — 下行回调按 FPort 分流
- [ ] [proto_handler.c](firmware/src/proto/proto_handler.c) — CMD 0x03/0x0A 增加 room_id 过滤
- [ ] 新增 `match_multicast(cmd_group, cmd_room, dev_group, dev_room)` 匹配函数

**文档**

- [ ] [spec_badge.md](doc/spec/spec_badge.md) — 补充 FPort 分配 + 多播过滤逻辑
- [ ] [spec_hub.md](doc/spec/spec_hub.md) — 补充 FPort 分配 + 多播过滤逻辑

### Phase 2（后续）— LoRaWAN 原生多播

- [ ] 探索 loramac-node 内部 API: `LoRaMacMcChannelSetup()` / `LoRaMacMcChannelDelete()`
- [ ] [hal_sx1262.c](firmware/src/hal/hal_sx1262.c) — 新增 `hal_sx1262_multicast_add_group()` / `hal_sx1262_multicast_remove_group()`
- [ ] 新增 CMD 用于远程下发多播组密钥（DevAddr + NwkSKey + AppSKey）

---

## 3. 凭证存储改造

当前 DevEUI/JoinEUI/AppKey 全部从 Kconfig 编译写死，需改为从 Flash ConfigStore 读取。

- [ ] [hal_sx1262.c](firmware/src/hal/hal_sx1262.c) — `hal_sx1262_init()` 中凭证来源从 Kconfig 改为 ConfigStore
- [ ] [config_store.h](firmware/src/config/config_store.h) — 新增字段 `dev_eui[8]` / `join_eui[8]` / `app_key[16]`
- [ ] [config_store.c](firmware/src/config/config_store.c) — 新增 get/set 函数
- [ ] 新增 CMD 或 AT 指令用于生产时写入凭证

**安全目标**: 每台设备独立 AppKey，不共用。

---

## 4. LoRaWAN NVM 持久化（帧计数 + 会话密钥）

**问题**: 当前 `CONFIG_LORAWAN_NVM_NONE`，掉电后帧计数丢失，设备被服务器拒绝。

**方案**: 启用 SDK 内置 NVM

- [ ] [prj.conf](firmware/prj.conf) — 新增:
  ```ini
  CONFIG_SETTINGS=y
  CONFIG_LORAWAN_NVM_SETTINGS=y
  CONFIG_SETTINGS_NVS=y
  ```
- [ ] 验证: 掉电重启后无需重新入网，帧计数连续

**SDK 自动管理**，无需应用层代码改动。

---

## 5. Class B Ping Slot 优化

当前 `hal_sx1262_set_ping_slot_periodicity()` 仅打日志，未真正生效。

- [ ] [hal_sx1262.c](firmware/src/hal/hal_sx1262.c) — 调用 SDK 内部 `LoRaMacClassBSetPingSlotInfo(periodicity)` 真正生效
- [ ] [main.c](firmware/src/main.c) — `update_ping_slot()` 在 NORMAL/ALERTED 间正确切换

---

## 6. AT 调试指令

通过 UART 串口提供 AT 指令，用于研发调试和生产配置。SDK 不提供 AT 框架，需自行实现简单的 UART 指令解析器。

### 指令列表

```
# ── 凭证读写 ──
AT+DEVEUI?                        → 查询当前 DevEUI
AT+DEVEUI=<16-char hex>           → 写入 DevEUI (生产用)
AT+JOINEUI?                       → 查询当前 JoinEUI
AT+JOINEUI=<16-char hex>          → 写入 JoinEUI
AT+APPKEY?                        → 查询当前 AppKey (仅显示前4字节)
AT+APPKEY=<32-char hex>           → 写入 AppKey (生产用)

# ── 身份查询 ──
AT+GROUPID?                       → 查询当前 group_id
AT+ROOMID?                        → 查询当前 room_id
AT+DEVTYPE?                       → 查询设备类型 (Badge/Hub)

# ── LoRaWAN 状态 ──
AT+JOIN?                          → 查询入网状态 (joined/not joined)
AT+JOIN                           → 手动触发重新入网
AT+RSSI?                          → 查询最后一次 RSSI
AT+SNR?                           → 查询最后一次 SNR

# ── Class B ──
AT+PINGSLOT?                      → 查询当前 Ping Slot 周期
AT+PINGSLOT=<0-7>                 → 设置 Ping Slot 周期 (0=1s, 1=2s, 2=4s…)

# ── 告警调试 ──
AT+ALARM?                         → 查询当前告警状态/优先级
AT+ALARM=<0-8>                    → 模拟触发告警 (0=Red, 5=Medical… 8=Normal)

# ── 系统 ──
AT+POWER?                         → 查询电量百分比
AT+RESET                          → 软件复位
AT+FACTORY                        → 恢复出厂设置
```

### 实现要点

- [ ] [serial/at_cmd.c](firmware/src/serial/) — 新文件，UART 中断接收 + 行缓冲 + 指令匹配
- [ ] [serial/at_cmd.h](firmware/src/serial/) — AT 指令表注册宏
- [ ] [main.c](firmware/src/main.c) — 初始化 AT 指令模块
- [ ] UART 参数: 115200 8N1，使用 nRF52840 空闲 UART

---

## 7. 一机一密生产工具

- [ ] 设计生产烧录流程: 串口 AT 指令写入 DevEUI/JoinEUI/AppKey → 校验 → 锁定
- [ ] 或: 通过 LoRaWAN CMD 0x50 实现远程 provisioning

---

## 优先级

| 优先级 | 项目 | 原因 |
|--------|------|------|
| P0 | NVM 持久化 | 掉电失联，影响可用性 |
| P0 | FPort 重新规划 | 通信协议基础，早改代价小 |
| P1 | 凭证存储改造 | 安全基础，量产前必须搞定 |
| P1 | Class B Ping Slot | 下行响应延迟 |
| P2 | AT 调试指令 | 研发调试效率 |
| P2 | 多播 Phase 1 | 空口效率优化 |
| P3 | 多播 Phase 2 | 原生多播，长期架构 |
| P3 | 一机一密工具 | 量产配套 |
