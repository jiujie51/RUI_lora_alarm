# Server（服务器）功能规格与需求

> 版本 V1.0 | LNS: ChirpStack V4 (AWS) | 2026-06-19
>
> **状态：现成产品，仅需部署配置，不需要开发。**
>
> 排序规则：LNS > 协议编解码 > 设备状态管理 > 告警路由 > Dashboard > API > 离线回退

---

## 1. 系统概述

服务器端由两层组成：
- **ChirpStack V4 LNS**：LoRaWAN 网络服务器，管理设备入网、会话、上下行数据
- **应用服务器 + Dashboard**：业务逻辑、告警路由、设备管理、Web 操作面板

---

## 2. ChirpStack V4 LNS ★★★★★

### 2.1 核心功能

| 功能 | 说明 |
|------|------|
| 设备管理 | OTAA 入网、DevEUI/AppEUI/AppKey 管理、设备 Profile |
| 网关管理 | 网关注册、Basic Station 对接、状态监控 |
| 上下行处理 | 接收上行数据 → MQTT/HTTP 推送；接收下行请求 → 排队等待设备唤醒 |
| Class B 支持 | Beacon 管理、Ping Slot 调度、多播组管理 |
| 多播 (Multicast) | 创建 Multicast Group、管理 Session Key、组播下行 |

### 2.2 集成接口

| 接口 | 协议 | 用途 |
|------|------|------|
| 应用服务器 ← ChirpStack | MQTT | 订阅 `application/[app-id]/device/+/event/up` 获取上行数据 |
| 应用服务器 → ChirpStack | HTTP API | `POST /api/devices/{dev_eui}/queue` 下发下行命令 |
| Gateway ← ChirpStack | Basic Station | 网关与 LNS 之间的标准协议 |

### 2.3 部署架构

```
AWS EC2:
┌────────────────────────────────────────────┐
│  Docker Compose                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐ │
│  │ChirpStack│  │PostgreSQL│  │  Redis   │ │
│  │   V4     │  │          │  │          │ │
│  └──────────┘  └──────────┘  └──────────┘ │
│  ┌──────────────────────────┐             │
│  │ chirpstack-gateway-bridge│             │
│  └──────────────────────────┘             │
│  ┌──────────┐  ┌──────────┐              │
│  │App Server│  │Dashboard │              │
│  │(Node/Py) │  │ (Nginx)  │              │
│  └──────────┘  └──────────┘              │
└────────────────────────────────────────────┘
```

---

## 3. 协议编解码 ★★★★★

### 3.1 下行解码（Uplink Decode）

将 Badge/Hub 发送的二进制帧解码为结构化 JSON。

| CMDID | 输入（HEX） | 输出（JSON） |
|-------|------------|-------------|
| 0x00 | `AA55 01 00 00 0A00 [CRC16] 01` | `{cmd:"heartbeat", dev_type:1, group_id:1}` |
| 0x01 | `AA55 01 00 01 0A00 [CRC16] 00 55` | `{cmd:"power", dev_type:0, power_pct:85}` |
| 0x02 | `AA55 01 00 02 [len] [CRC16] 03 01 50 MAC[6] lat[4] lon[4]` | `{cmd:"key_event", button:3, motion:1, rssi:80, hub_mac:"xx:xx:xx:xx:xx:xx", lat:34.123456, lon:-118.123456}` |

**校验要求**：
- CRC16/XMODEM 校验失败 → 丢弃并记录警告
- length 字段必须 ≥9 且 ≤255
- CMDID 白名单过滤

### 3.2 上行编码（Downlink Encode）

将业务命令编码为二进制帧下发。

| 命令 | JSON 输入 | 输出（HEX） |
|------|----------|------------|
| Code Red 广播 | `{cmd:0x03, group_id:0xFF, alarm:3}` | `AA55 01 00 03 000B [CRC16] FF 03` |
| All Clear | `{cmd:0x0A, group_id:0xFF, type:1}` | `AA55 01 00 0A 000A [CRC16] FF 01` |
| Set Group ID | `{cmd:0x50, group_id:0x03}` | `AA55 01 00 50 0009 [CRC16] 03` |

---

## 4. 设备状态管理 ★★★★

### 4.1 设备状态模型

```
DeviceState {
  dev_eui:        string       // 设备唯一标识
  dev_type:       0|1          // 0=Badge, 1=Hub
  hub_type:       0|1|2        // 0=RoomHub, 1=DoorHub, 2=HallwayHub (仅 Hub)
  group_id:       0-255        // 角色 bitmask
  online:         boolean      // 是否在线 (心跳 5min 内)
  last_seen:      timestamp    // 最后心跳时间
  battery_pct:    0-100        // 电量百分比
  battery_alert:  boolean      // <30% 告警
  current_alarms: [Alarm]      // 当前活跃告警列表
  location: {
    hub_mac:      string       // 关联的 Hub MAC (Badge)
    room_id:      string       // 房间号 (服务器解析)
    gps_lat:      float        // GPS 纬度 (Hub)
    gps_lon:      float        // GPS 经度 (Hub)
  }
  alarm_configs:  [AlarmConfig] // 8 种告警的 LED/Buzz/Vib 配置
}
```

### 4.2 状态更新触发

| 事件 | 更新字段 |
|------|---------|
| 收到 CMD 0x00 (心跳) | online, last_seen |
| 收到 CMD 0x01 (电量) | battery_pct, battery_alert |
| 收到 CMD 0x02 (按键) | current_alarms, location |
| 下发告警成功后 | current_alarms 添加告警记录 |
| 超过 10min 无心跳 | online = false |

---

## 5. 告警路由引擎 ★★★★

### 5.1 路由规则

| 触发条件 | 下行目标 | 下行命令 | 方式 |
|---------|---------|---------|------|
| Badge Red 长按 | **所有设备** (GroupID=0xFF) | CMD 0x03 {alarm=3} | Multicast FPort=20 |
| Badge Blue 长按 | **Admin + Nurse** (GroupID bit0\|bit1) | CMD 0x03 {alarm=1} | Unicast 逐设备 |
| Badge Yellow 长按 | **Admin** (GroupID bit0) | CMD 0x03 {alarm=2} | Unicast 逐设备 |
| Badge Green 短按 (Code Red 中) | **所有设备** (GroupID=0xFF) | CMD 0x0A {type=1 AllClear} | Multicast FPort=20 |
| Dashboard Code Red | **所有设备** (GroupID=0xFF) | CMD 0x03 {alarm=3} | Multicast FPort=20 |
| Dashboard SRP (4种) | **所有设备** (GroupID=0xFF) | CMD 0x03 {alarm=4-7} | Multicast FPort=20 |
| Dashboard All Clear | **所有设备** (GroupID=0xFF) | CMD 0x0A {type=1} | Multicast FPort=20 |
| Dashboard Clear All | **所有设备** (GroupID=0xFF) | CMD 0x0A {type=0} | Multicast FPort=20 |

### 5.2 Code Red 处理流程

```
1. 收到 Badge CMD 0x02 {button=Red, motion=long_press, hub_mac, rssi}
2. 查询 hub_mac → room_id (楼层平面映射表)
3. 记录告警源: {room_id, source_badge, timestamp}
4. 构建多播下行: CMD 0x03 {GroupID=0xFF, Alarm=Red}
5. 通过 ChirpStack HTTP API 下发到 Multicast Group
6. 更新 Dashboard: 楼层平面 → 所有房间红色 → 源房间闪烁红
7. 更新所有设备的 current_alarms 状态
```

### 5.3 All Clear 处理流程

```
1. 收到 All Clear 请求（Badge Green 或 Dashboard）
2. 仅清除 Code Red → 其他告警 (Medical/Yellow/SRP) 保留
3. 构建下行: CMD 0x0A {type=1}
4. Dashboard: Code Red 房间变绿
5. 如果仍有其他告警 → 对应颜色保持
```

---

## 6. Dashboard（Web 操作面板）★★★★

### 6.1 功能清单

| 功能 | 优先级 | 说明 |
|------|--------|------|
| **楼层平面图** | ★★★★★ | 上传建筑/楼层平面图，按房间标色显示告警，源房间闪烁 |
| **告警管理** | ★★★★★ | 触发/清除告警按钮 (Code Red/Blue/Yellow/Hold/Secure/Evacuate/Shelter/All Clear/Clear All) |
| **设备管理** | ★★★★ | 设备列表、在线状态、电量、位置、最后上线时间 |
| **设备配对** | ★★★★ | 导入 CSV 或手动输入 DevEUI 注册设备，分配 Group ID |
| **告警历史** | ★★★ | 告警记录表 (时间/类型/来源/位置)，导出 CSV |
| **电池监控** | ★★★ | 低电量设备列表，<30% 短信/邮件通知 |
| **消息推送** | ★★ | 向指定设备或群组发送 LCD 文字消息 (CMD 0x08) |
| **配置管理** | ★★ | 远程修改告警参数 (CMD 0x04 LED/Buzz/Vib 配置) |

### 6.2 平台支持

- 桌面端 (Desktop Browser)
- 手机端 (Mobile Browser)
- 平板端 (Tablet Browser)

### 6.3 告警管理操作

| 操作 | 按钮 | 下发命令 | 效果 |
|------|------|---------|------|
| 触发 Code Red | Red 按钮 | CMD 0x03 {0xFF, 3} | 全校红色 |
| 触发 Medical | Blue 按钮 | CMD 0x03 {Admin\|Nurse, 1} | 定向蓝色 |
| 触发 Admin | Yellow 按钮 | CMD 0x03 {Admin, 2} | 定向黄色 |
| Hold | SRP 按钮 | CMD 0x03 {0xFF, 4} | 全校紫色 |
| Secure | SRP 按钮 | CMD 0x03 {0xFF, 5} | 全校蓝色 |
| Evacuate | SRP 按钮 | CMD 0x03 {0xFF, 6} | 全校绿色 |
| Shelter | SRP 按钮 | CMD 0x03 {0xFF, 7} | 全校橙色 |
| All Clear | 绿色按钮 | CMD 0x0A {0xFF, 1} | 清除 Code Red |
| Clear All | 灰色按钮 | CMD 0x0A {0xFF, 0} | 清除全部 |

---

## 7. REST API ★★★

### 7.1 接口列表

| Method | Path | 说明 |
|--------|------|------|
| GET | `/api/devices` | 设备列表 (支持 ?type=badge\|hub&online=true) |
| GET | `/api/devices/:eui` | 设备详情 |
| POST | `/api/devices/:eui/downlink` | 向指定设备下发命令 |
| POST | `/api/alerts` | 触发告警 (body: {type, group_id}) |
| POST | `/api/alerts/clear` | 清除告警 (body: {type: 0=ClearAll, 1=AllClear}) |
| GET | `/api/floorplan/:building` | 获取建筑楼层平面及房间告警状态 |
| GET | `/api/alerts/history` | 告警历史记录 |
| GET | `/api/devices/low-battery` | 低电量设备列表 |

---

## 8. 通知推送 ★★

| 条件 | 渠道 | 内容 |
|------|------|------|
| 电量 < 30% | 短信/邮件 | "Device [ID] battery low: [X]%" |
| Code Red 触发 | 短信/邮件 (管理员) | "CODE RED Alert - Bldg [X], Floor [Y], Room [Z]" |
| 设备离线 > 30min | Dashboard 标志 | 黄色警告 |
| 网关离线 | 短信/邮件 | "Gateway [ID] offline" |

---

## 9. 离线回退模式 ★★

> v1.0 基础支持，v1.1 完善。

### 9.1 要求

1. 检测 MQTT 断开 + ChirpStack 健康检查失败 → 激活离线模式
2. 切换到本地 LNS 实例（网关内置或 Raspberry Pi）
3. 本地告警路由规则继续工作
4. 事件队列保留原始时间戳
5. 恢复连接后自动同步 → 切换回云端 LNS

### 9.2 事件同步

- 离线期间所有事件用本地时间戳排队
- 恢复连接后按时间顺序推送到云端
- 审计日志保留原始时间戳确保合规

---

## 10. 通信数据流（完整示例）

### 10.1 Code Red 上行 + 下行

```
Badge                Gateway/LNS              App Server            Dashboard
  │                      │                        │                     │
  ├─ Red长按确认          │                        │                     │
  ├─ BLE扫描4s            │                        │                     │
  ├─ CMD 0x02 TX ────────▶│                        │                     │
  │                      ├─ MQTT publish ─────────▶│                     │
  │                      │                        ├─ 解析: Red长按       │
  │                      │                        ├─ 查询 Hub→room_id   │
  │                      │                        ├─ 记录告警源          │
  │                      │                        ├─ 构建多播下行        │
  │                      │                        ├─ HTTP POST downlink─▶│
  │                      │◄── ChirpStack queue ────┤                     │
  │  ◄── CMD 0x03 ──────│ (下一个 ping slot)     │                     │
  │  (Multicast)         │                        │                     │
  ├─ LED红闪+Buzz 60s    │                        │                     │
  │                      │                        │                     ├─ 楼层全红
  │                      │                        │                     ├─ 源房间闪烁
  │                      │                        │                     ├─ 弹窗告警
```

### 10.2 All Clear 流程

```
Dashboard             App Server              Gateway/LNS          All Devices
  │                      │                        │                     │
  ├─ 按 All Clear按钮     │                        │                     │
  │──────────────────────▶│                        │                     │
  │                      ├─ 构建 CMD 0x0A         │                     │
  │                      ├─ HTTP POST ───────────▶│                     │
  │                      │                        ├─ Multicast ────────▶│
  │                      │                        │                     ├─ Red→Green
  │                      │                        │                     ├─ Buzz 停
  │                      │                        │                     ├─ LCD "All Clear"
  │                      │                        │                     │
  │  ◄─ 楼层变绿           │◄── MQTT (CMD 0x00) ──│◄── 设备心跳 ────────│
```
