# LoRaWAN 多播流程与生产设备凭证管理

> 适用: RAK4630 + RUI3 V4.2.4 + ChirpStack V4 + LoRaWAN Class B OTAA US915

---

## 一、多播完整流程

### 1.1 架构概览

```
┌─────────────────────────────────────────────────────────┐
│  ChirpStack V4                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐│
│  │ Group 0  │  │ Group 1  │  │ Group 2  │  │ Group 3  ││
│  │ Code Red │  │Code Blue │  │Code Yel  │  │Code Green││
│  │ 0x83C2A6A8│ │0x1CF26AA9│ │0xF59367B5│ │0xF7C48FB6││
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘│
│       └──────────────┴────────────┴──────────────┘      │
│                         │                               │
│              Class B ping-slot 广播                     │
│              923.3MHz DR13 SF7/500kHz                   │
└─────────────────────────┬───────────────────────────────┘
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
   ┌─────────┐      ┌─────────┐       ┌─────────┐
   │  Hub-1  │      │  Hub-2  │       │ Badge-1 │  ...
   │ grp=0x01│      │ grp=0x04│       │ grp=0x02│
   └─────────┘      └─────────┘       └─────────┘
```

### 1.2 设备端注册时序

```
上电启动
  │
  ├─ 1. OTAA Join (Class A)
  │     Join Request → Gateway → ChirpStack
  │     Join Accept  ← ChirpStack (含 DevAddr/NwkSKey/AppSKey)
  │
  ├─ 2. BLE 广播重启 (SoftDevice 可能在上一步挂起 BLE)
  │
  ├─ 3. Class B 初始化
  │     ├─ DeviceTimeReq (时间同步)
  │     ├─ Beacon 搜索 (最慢 128s, US915 信标周期)
  │     ├─ Beacon Lock ✓
  │     └─ PingSlotInfoReq (申请 Class B 下行时隙)
  │
  ├─ 4. 多播组注册 ← app_hal_setup_multicast()
  │     ├─ api.lorawan.addmulc(session_red)    → Group 0
  │     ├─ api.lorawan.addmulc(session_blue)   → Group 1
  │     ├─ api.lorawan.addmulc(session_yellow) → Group 2
  │     └─ api.lorawan.addmulc(session_green)  → Group 3
  │
  ├─ 5. 发送首条心跳
  │
  └─ 6. 正常运行
        ├─ Class A uplink: 心跳 (5min), 电量上报 (5min)
        ├─ Class B ping-slot: 每 4s 打开一次 RX 窗
        └─ 多播下行接收 (匹配 group_id/room_id 后处理)
```

**关键约束**: 多播组注册必须在 beacon lock 之后。Class B ping slot 依赖信标同步，lock 前无法正确打开 RX 窗口。

### 1.3 多播下行接收与过滤

```
ChirpStack 向 Group 0 发送 Code Red 告警
  │
  ├─ LoRaWAN MAC 层
  │     SX1262 在下一个 ping-slot 打开 RX
  │     收到数据 → McAppSKey/McNwkSKey 解密
  │
  ├─ RUI3 回调链
  │     service_lora → ruiv3_recv_cb → on_lora_downlink()
  │
  ├─ 协议解析
  │     proto_parser_feed() → 逐字节喂入状态机
  │     proto_parser_get_frame() → 取出完整帧
  │
  ├─ 路由到处理函数
  │     proto_handle_frame() → cmd_handlers[CMDID]
  │
  └─ 多播过滤 (proto_handler.cpp:44 match_multicast)
        ├─ 检查 cmd_group (下行帧中的 group_id 位掩码)
        │     & device_group_id → 任意 bit 重叠即匹配
        ├─ 检查 cmd_room (下行帧中的 room_id)
        │     与 device_room_id 相同 或 0xFF 全局广播
        ├─ 通过 → alarm_sm_set(alarm_type, ALARM_SRC_LORAWAN)
        └─ 不匹配 → 静默丢弃
```

### 1.4 多播 vs 单播

| | 单播 | 多播 |
|---|---|---|
| 100 设备同时告警 | 100 条下行, 串行发送 | **1 条**下行, Class B 广播 |
| Code Red 延迟 | 数十秒 (逐台轮询) | **亚秒级** (下个 ping-slot 即收到) |
| 下行信道占用 | 每设备占用一个下行窗口 | 4 组共享固定频率/DR |
| 角色过滤 | 不适用 (单播有地址) | 应用层 `match_multicast()` 过滤 |

### 1.5 多播组参数

| 参数 | 值 | 说明 |
|---|---|---|
| Frequency | 923.3 MHz | US915 下行信道 0 |
| Data Rate | DR13 | SF7 / BW500kHz |
| Periodicity | 2 | ping slot 周期 = 2^2 = 4s |
| McDevclass | 2 | Class B |

---

## 二、ChirpStack 服务器配置

### 2.1 Device Profile

```
Name:          RAK4630-LoRa-Alarm
Region:        US915 (US902-928)
MAC version:   LoRaWAN 1.0.4
Regional parameters: RP002-1.0.3

Class B:       Enabled
  Ping slot periodicity: 2 (every 4s)

ADR:           Enabled (Hub) / Disabled (Badge, 移动设备)
Max EIRP:      20
```

### 2.2 多播组创建

ChirpStack V4: `Tenant` → `Multicast Groups` → `Create`

**4 组参数表**:

| 字段 | Group 0 (Red) | Group 1 (Blue) | Group 2 (Yellow) | Group 3 (Green) |
|---|---|---|---|---|
| Name | Code Red | Code Blue | Code Yellow | Code Green |
| Region | US915 | US915 | US915 | US915 |
| Class | B | B | B | B |
| Frequency | 923300000 | 923300000 | 923300000 | 923300000 |
| DR | 13 | 13 | 13 | 13 |
| Periodicity | 2 | 2 | 2 | 2 |
| McAddress | 83C2A6A8 | 1CF26AA9 | F59367B5 | F7C48FB6 |
| McNwkSKey | 生产密钥 | 生产密钥 | 生产密钥 | 生产密钥 |
| McAppSKey | 生产密钥 | 生产密钥 | 生产密钥 | 生产密钥 |

每组创建后在 `Devices` 标签添加**所有** Hub 和 Badge 设备。

### 2.3 密钥一致性

**当前 `board.h` 中密钥为占位值**。生产部署前:

1. ChirpStack 端生成 4 组正式密钥 (McNwkSKey + McAppSKey, 各 16 字节随机数)
2. 将同一组密钥写入 `board.h` 重新编译固件 (静态方案)
3. 或通过协议下行命令远程设置 (动态方案, 需额外开发)

---

## 三、设备身份分配 (DevEUI / AppEUI / AppKey / Group / Room)

### 3.1 当前问题

`board.h` 硬编码了三元组:

```c
#define OTAA_DEVEUI  {0x20, 0x26, 0x06, 0x18, 0x01, 0x00, 0x00, 0x02}
#define OTAA_APPEUI  {0x8A, 0x61, 0x2A, 0x8B, 0x62, 0x0D, 0x6E, 0xBC}
#define OTAA_APPKEY  {0xB7, 0xD1, 0x5D, 0x51, 0xA8, 0x0A, 0xFF, 0x45, ...}
```

烧两台设备就 DevEUI 冲突, **仅可用于开发调试**。

### 3.2 方案 A: 模块自带 DevEUI (推荐: <100 台)

> RAK4630 每块模块出厂带唯一 64-bit IEEE DevEUI, 贴纸在屏蔽罩上, 格式 `6081F9xxxxxxxx`

**AppEUI**: 所有设备同用一个 (应用级标识, ChirpStack V4 叫 JoinEUI)

**AppKey**: 由 DevEUI + 主密钥 HMAC-SHA256 派生, 每台唯一:

```
AppKey = HMAC-SHA256(master_key, DevEUI)[0:16]
```

**生产流程**:

```
1. 烧录同一个固件 (硬件从模块内部读取 DevEUI; AppKey 运行时派生)
2. 扫描模块贴纸, 登记 DevEUI
3. PC 工具: 输入 DevEUI + master_key → 计算 AppKey → 通过 API 写入 ChirpStack
4. 设备上电自动入网
```

**优点**: 一版固件通烧, 不修改代码
**前提**: 模块有 DevEUI 贴纸

### 3.3 方案 B: 芯片 ID 做 DevEUI (推荐: 100-1000 台)

> nRF52840 内建 64-bit 唯一 ID (`NRF_FICR->DEVICEID[0]`, `DEVICEID[1]`), 读出来当 DevEUI

```cpp
uint8_t dev_eui[8];
uint32_t id0 = NRF_FICR->DEVICEID[0];
uint32_t id1 = NRF_FICR->DEVICEID[1];
// 大端转小端以满足 LoRaWAN LSB 序
dev_eui[0] = (id0 >> 0)  & 0xFF;
dev_eui[1] = (id0 >> 8)  & 0xFF;
dev_eui[2] = (id0 >> 16) & 0xFF;
dev_eui[3] = (id0 >> 24) & 0xFF;
dev_eui[4] = (id1 >> 0)  & 0xFF;
dev_eui[5] = (id1 >> 8)  & 0xFF;
dev_eui[6] = (id1 >> 16) & 0xFF;
dev_eui[7] = (id1 >> 24) & 0xFF;
```

AppEUI 固件写死, AppKey 同方案 A 用派生算法。

**优点**: 无需扫描贴纸, 上电自动获取唯一 ID; 零接触部署
**缺点**: DevEUI 不符合 IEEE OUI 格式 (ChirpStack 接受, 但不规范)

**登记流程**:

```
设备首次上电 → 串口/RTT 打印 DevEUI → 操作员记录 → 手动录入 ChirpStack
  或
设备首次上电 → 发一条 Join Request (会被拒绝) → ChirpStack 日志中提取 DevEUI
```

### 3.4 方案 C: 生产烧录工具 (推荐: >1000 台)

> PC 端 (Python) 通过 USB AT 命令批量烧录

**数据库管理**:

```
┌─────────────────────────────────────┐
│  provisioning.db                   │
│  ┌──────────┬──────────┬──────────┐│
│  │ DevEUI   │ AppKey   │ Status   ││
│  ├──────────┼──────────┼──────────┤│
│  │ 6081...01│ A1B2...01│ assigned ││
│  │ 6081...02│ A1B2...02│ pending  ││
│  │ ...      │ ...      │ ...      ││
│  └──────────┴──────────┴──────────┘│
└─────────────────────────────────────┘
```

**生产流水线**:

```
1. PC 工具从数据库取下一个未分配的 DevEUI/AppKey
2. 通过 AT 命令写入设备 NVS Flash:
     AT+DEVEUI=6081F9xxxxxxxx
     AT+APPKEY=A1B2C3D4...
3. 设备入网 → 自动在 ChirpStack 注册 (或通过 API 批量导入)
4. 标记数据库记录的 Status = assigned
```

### 3.5 方案 D: ChirpStack API 自动注册 (补充方案)

配合方案 A/B, 设备首次入网时 ChirpStack 收到 Join Request, 如果 OTAA 模式且该 DevEUI 未注册, 可以拒绝但记录到待审核列表。管理员批量审批后设备自动入网。

也可以预先把所有 DevEUI 通过 ChirpStack REST API 批量导入:

```bash
# 批量创建设备
curl -X POST https://chirpstack.local/api/devices \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"device": {
    "devEui": "6081F90000000001",
    "applicationId": "...",
    "deviceProfileId": "...",
    "skipFcntCheck": false,
    "isDisabled": false
  }}'
```

### 3.6 Group ID 和 Room ID 分配

这两个字段**不烧写死**, 通过协议 CMD 0x50 远程设置:

```
设备入网后:
  ChirpStack Web → Device → Queue Downlink:
    CMD 0x50: Set Group ID (Port 20)
    CMD 0x50: Set Room ID (Port 20)
  → 设备写入 NVS Flash → 断电不丢失
```

| 角色 | group_id (位掩码) | 典型 room_id |
|---|---|---|
| Admin | 0x01 | 0 (全校) |
| Nurse | 0x02 | 12 (医务室) |
| Security | 0x04 | 0 |
| Principal | 0x08 | 0 |
| 混合角色 | 0x03 (Admin+Nurse) | 12 |

---

## 四、方案选择

| 规模 | DevEUI | AppKey | 工具需求 |
|---|---|---|---|
| <100 台 | 模块贴纸 | 派生 | 贴纸扫描 + ChirpStack Web 手动录 |
| 100-1000 | 芯片 ID | 派生 | 串口日志提取 + API 批量导入 |
| >1000 | 数据库分配 | 随机生成 | PC 烧录工具 + 数据库 + API |

**对于校园部署 (数十台)**: 推荐方案 A (模块贴纸), 操作简单, 无需额外工具。
