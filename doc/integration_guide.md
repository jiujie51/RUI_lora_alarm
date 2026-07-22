# 网关 & 服务器联调指南 — 固件侧需关心的全部事项

> 本文列出固件开发完成后与网关、ChirpStack LNS、应用服务器联调时，你需要关心的接口协议、性能参数、时序约束和常见问题。

---

## 1. 联调架构总览

```
Hub/Badge                    Gateway                   LNS                         App Server
┌──────────┐    LoRa RF     ┌──────────────┐  Basic   ┌──────────────┐  MQTT/HTTP  ┌──────────────┐
│ FPort=10 │ ←──────────→  │ RAK7289CV2   │ Station  │ ChirpStack   │ ←─────────→ │ App Server   │
│ 上行数据  │   RSSI/SNR    │ 透传,不解析   │ ←──────→ │ V4  (AWS)    │  JSON       │ + Dashboard  │
│          │               │              │  WSS/IP  │              │             │              │
│ FPort=10 │ ←────单播──── │              │          │              │             │              │
│ FPort=20 │ ←────组播──── │              │          │              │             │              │
└──────────┘               └──────────────┘          └──────────────┘             └──────────────┘
```

**关键认知**：Gateway 是**纯透传**设备。它不解析你的 0xAA55 协议帧，只负责 LoRa RF ↔ IP 数据包中继。协议帧的编解码在**固件**和**应用服务器**各自完成。

---

## 2. LoRaWAN 空中接口参数（你必须遵守的限制）

### 2.1 FPort 分配

| FPort | 方向 | 用途 |
|-------|------|------|
| **10** | 上行 | 所有上行数据（心跳/电量/按键事件） |
| **10** | 下行 (单播) | 服务器→单个设备（Medical/Admin 定向告警、配置命令） |
| **20** | 下行 (多播) | 服务器→所有设备（Code Red、SRP、All Clear/Clear All） |

> ⚠️ 固件必须在上行时使用 FPort=10。ChirpStack 根据 FPort 路由到不同的 Application。如果 FPort 不匹配，服务器收不到数据。

### 2.2 数据速率与有效载荷

| 频段 | 默认 DR | 最大 Payload | 实际可用 |
|------|--------|-------------|---------|
| US915 | DR0 (SF10/125kHz) | 11 字节 | 11 |
| US915 | DR3 (SF7/125kHz) | 222 字节 | 222 |
| EU868 | DR0 (SF12/125kHz) | 51 字节 | 51 |
| EU868 | DR5 (SF7/125kHz) | 222 字节 | 222 |
| CN470 | DR0 (SF12/125kHz) | 51 字节 | 51 |
| CN470 | DR5 (SF7/125kHz) | 222 字节 | 222 |

> ⚠️ ADR 会自动调节速率。低速率（SF12/DR0）时 payload 仅 **51 字节**！加上协议帧头 9 字节（head+ver+control+cmdid+length+crc），实际 data 字段最多 42 字节。CMD 0x02 包含 MAC 6B + lat 4B + lon 4B = 14B，还有余量。

### 2.3 占空比限制（Duty Cycle）

| 频段 | 占空比 | 实际限制 |
|------|--------|---------|
| EU868 | **1%** (ETSI) | 每 100s 只能发 1s 数据 ≈ 每天最多 864 帧上行 |
| US915 | 无限制 (FCC) | 跳频免占空比限制 |
| CN470 | 无限制 | 跳频免占空比限制 |

> ⚠️ EU868 频段占空比最严。以 DR0 (ToA≈1.5s) 计算：每天最多 ~576 帧。心跳 5min=288 帧/天，剩余充足。但如果用 Class B ping slot 2s 周期且大量下行，可能触发限制。

### 2.4 空中时间（Time-on-Air）

| DR | SF | BW | ToA (11B payload) | ToA (51B payload) |
|----|----|----|--------------------|---------------------|
| DR0 | 12 | 125kHz | ~1.5s | ~2.8s |
| DR3 | 7 | 125kHz | ~0.06s | ~0.1s |
| DR5 | 7 | 125kHz | ~0.06s | ~0.1s |

> 固件心跳只有 1B data（device_type）+ 1B group_id = 11B 总 payload，ToA 很短。

### 2.5 RSSI / SNR 参考值

| 信号等级 | RSSI | SNR | 丢包率 |
|---------|------|-----|--------|
| 优秀 | > -60 dBm | > 10 | <1% |
| 良好 | -60 ~ -90 dBm | 0 ~ 10 | 1~5% |
| 边缘 | -90 ~ -110 dBm | -10 ~ 0 | 10~30% |
| 断线 | < -110 dBm | < -10 | >50% |

> ⚠️ 固件上行设置 **Confirmed Uplink** 时，网关需回复 ACK。边缘信号下 ACK 丢失会触发重传（最多 8 次），导致功耗和延迟大增。报警类上行建议用 **Unconfirmed**（应用层重传），心跳用 Confirmed。

---

## 3. Class B 联调参数（核心难关）

### 3.1 Beacon 时序

```
GPS 时间 ──→ Gateway GPS Lock ──→ 每 128s 发送 Beacon
                                          ↓
                              设备听到 Beacon → 计算 Ping Slot 偏移
```

| 参数 | 值 | 说明 |
|------|-----|------|
| Beacon 周期 | **128s** | GPS 同步，全网统一 |
| Beacon 保护窗口 | 3s | Beacon 前后保留，不排 ping slot |
| Ping Slot 周期 | 可配 (2s/4s/8s/16s/...) | 由设备侧 `LoRaMacSetClassBParam()` 设置 |
| Beacon 丢失容限 | 最多丢失 **3 个** Beacon (384s) | 丢失第 4 个后视为 Class B 失步，退化为 Class A |

### 3.2 联调验证步骤（必须按顺序）

```
1. 网关 GPS Lock（必须！）
   验证: ChirpStack → Gateway → 显示 GPS 坐标 + "Time OK"

2. Beacon 发送
   验证: ChirpStack → Gateway → "Class B Beacon: enabled"

3. 设备听到 Beacon
   验证: 设备日志 "Beacon acquired, ping slot offset=XXXms"

4. 服务器下行
   验证: ChirpStack → Device → Queue Downlink → 设备在 ping slot 内收到
```

> ⚠️ **最常见的联调失败原因**：网关 GPS 没有 Lock → Beacon 不发送 → 设备收不到下行 → Class B 退化为 Class A！

### 3.3 Ping Slot 与功耗/延迟关系

| Ping Slot 周期 | 设备唤醒频率 | 下行延迟 | 功耗 (1000mAh) |
|---------------|------------|---------|---------------|
| **2s** (ALERTED) | 最高 | < 2s | 6.70 mAh/天 |
| **4s** (过渡) | 中 | < 4s | 4.15 mAh/天 |
| **8s** (NORMAL) | 低 | < 8s | 2.87 mAh/天 |

> 固件实现的自适应策略：收到 Code Red → 切 2s 持续 5min → 4s 持续 10min → 回 8s。

---

## 4. ChirpStack 接口协议

### 4.1 上行数据流（设备 → LNS → App Server）

```
设备 CMD 0x02 TX (FPort=10)
  → Gateway 收到，附加 metadata {rssi, snr, gw_time, gw_gps}
  → ChirpStack 解码为 JSON → MQTT Publish
```

**ChirpStack MQTT 上行消息格式** (topic: `application/{app-id}/device/{dev_eui}/event/up`)：

```json
{
  "deduplicationId": "abc123",
  "time": "2026-06-19T10:30:00Z",
  "deviceInfo": {
    "devEui": "2026061801000001",
    "deviceName": "Hub-001",
    "devAddr": "01AB02CD",
    "deviceProfileName": "LoRa_Alarm_ClassB"
  },
  "devAddr": "01AB02CD",
  "fPort": 10,
  "data": "AAUUAQACAABbAwoCVQAAAA...",    // ★ 这是你的 0xAA55 帧的 Base64 编码
  "object": null,
  "rxInfo": [{
    "gatewayId": "abcdef...",
    "rssi": -55,
    "snr": 9.5,
    "context": "...",
    "metadata": {
      "gateway_lat": 34.0522,
      "gateway_lon": -118.2437
    }
  }],
  "txInfo": {
    "frequency": 903900000,
    "modulation": "LORA",
    "loRaModulationInfo": {
      "spreadingFactor": 7,
      "bandwidth": 125,
      "codeRate": "4/5"
    }
  }
}
```

> ⚠️ `data` 字段是 Base64 编码的原始 payload。**App Server 必须 Base64 解码后再按 0xAA55 协议解析**。你的 CRC16 校验覆盖整个 payload 减去 2B CRC 字段。

### 4.2 下行数据流（App Server → LNS → 设备）

**ChirpStack HTTP API**：`POST https://{chirpstack}/api/devices/{dev_eui}/queue`

```json
{
  "queueItem": {
    "confirmed": false,
    "fPort": 10,
    "data": "AAUVAQMAAAALAP8D..."   // ★ 你的 0xAA55 帧的 Base64 编码
  }
}
```

**多播下行**：`POST https://{chirpstack}/api/multicast-groups/{mc_id}/queue`

```json
{
  "queueItem": {
    "fPort": 20,
    "data": "AAUVAQMAAAALAP8D...",
    "multicastGroupId": "..."
  }
}
```

> ⚠️ 下行 data 也是 Base64 编码。**App Server 必须先把 0xAA55 帧 Base64 编码后再放入 HTTP body**。

### 4.3 ChirpStack 下行排队机制

```
App Server → POST /devices/{eui}/queue → ChirpStack 存入下行队列
                                            ↓
设备发送上行 (触发 RX1/RX2)             设备 ping slot 到达
  → ChirpStack 在 ACK 中捎带下行           → ChirpStack 主动发送下行
     (Class A)                              (Class B)
```

> ⚠️ **Class B 下行不需要设备先上行**。但 ChirpStack 的下行队列有 **max 10 pending** 限制。如果服务器在同一 ping slot 前连续发了 11 个下行命令，第 11 个会返回 429 错误。

---

## 5. 应用层协议校验（服务器侧）

### 5.1 服务器需要对你的帧做哪些校验

| 校验项 | 动作 | 原因 |
|--------|------|------|
| `data.length >= 9` | 丢弃，记录 WARN | 帧头最小 9B（head+ver+ctrl+cmdid+len+crc）|
| `data[0:2] == 0xAA55` | 丢弃，记录 WARN | 帧同步头 |
| CRC16/XMODEM | **必须** 丢弃，记录 ERROR | LoRa MIC 不覆盖应用层比特错误 |
| `length` 字段 vs 实际 data 长度 | 丢弃，记录 WARN | 防协议栈出错 |
| CMDID 白名单 (0x00/01/02) | 丢弃，记录 WARN | 防未知命令 |
| `ver != 1` | 丢弃，记录 WARN | 版本不匹配 |

### 5.2 下行编码时服务器需要确保

| 要求 | 原因 |
|------|------|
| CRC16 必须重新计算 | 每帧独立 |
| `length` 字段 = 帧头 9B + data 长度 | 长度包含自身和 CRC |
| 多播 GroupID 必须 `\| 0x80` (Bit7=1) 表示全部 | 或按 bitmask 精确匹配 |
| 下行帧 ver=1, control=0x00 | 与上行一致 |

---

## 6. 时序与延迟约束

### 6.1 Code Red 端到端延迟（关键性能指标）

```
按键触发 ─┬─→ BLE 扫描 4s ─→ LoRa TX ToA ~0.1s ─→ ChirpStack处理 ~0.1s
          │
          ├─→ App Server 路由 ~0.1s
          │
          └─→ HTTP POST 下行队列 ~0.1s ─→ 等待设备 ping slot ≤ 2s(ALERTED)
              ─→ LoRa 下行 ToA ~0.1s ─→ 设备执行器响应
──────────────────────────────────────────────────────────────────────
总延迟: 4s(扫描) + ≤2s(ping slot) + 0.5s(处理) ≈ 最坏 6.5s, 平均 5.5s
```

> ⚠️ BLE 扫描的 4s 是延迟最大的环节。如果要加快 Code Red 广播，可考虑先发 LoRa 上行（不等 BLE 扫描完成），后续补充位置信息。但这违反当前协议（CMD 0x02 包含 BLE 结果）。

### 6.2 时序约束清单

| 约束 | 值 | 后果 |
|------|-----|------|
| Beacon 连续丢失 | >3 个 (384s) | Class B 失步，下行收不到 |
| Ping Slot 错过 | — | 下行推迟到下一个 ping slot (≤8s) |
| 上行队列满 | >10 帧 | 丢帧 |
| 下行队列满 | >10 帧 | HTTP 429 |
| BLE 扫描窗口 | 4s | — |
| 心跳间隔 | 5min | 超时 >10min 服务器标记 offline |
| ChirpStack MQTT 推送延迟 | <1s | — |
| HTTP API 延迟 | <500ms | — |

---

## 7. 设备注册与入网

### 7.1 设备需要在 ChirpStack 预先注册

| 信息 | 来源 | 格式 |
|------|------|------|
| DevEUI | `devices_20260618_hub.csv` / `devices_20260618_badge.csv` | 16 字符 HEX |
| AppEUI (JoinEUI) | 同上 CSV | 16 字符 HEX |
| AppKey | 同上 CSV | 32 字符 HEX |
| Device Profile | 在 ChirpStack 创建 | 指定 Class B, 频段, MAC 版本 1.0.3 |

### 7.2 设备 Profile 关键配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| LoRaWAN MAC version | **1.0.3** | 必须与固件一致 |
| Regional Parameters | 按频段选择 | US915 / EU868 / CN470 等 |
| Class-B enabled | **true** | 必须勾选 |
| Class-B ping slot period | **8s** (默认) | 会被设备端动态覆盖 |
| Supports OTAA | **true** | 必须勾选 |
| Multicast Group | 需要预先创建 | 设备入网后下发 Multicast Session |

### 7.3 OTAA 入网过程

```
设备                                 ChirpStack
  │                                      │
  ├─ Join Request ──────────────────────▶│
  │   (DevEUI + AppEUI + DevNonce)       │
  │                                      ├─ 验证 DevEUI/AppEUI
  │                                      ├─ 检查 Device Profile
  │   ◀──── Join Accept ────────────────│
  │   (DevAddr + NwkSKey + AppSKey)      │
  │                                      │
  ├─ 保存 Session Context                │
  ├─ 启动 Class B: Beacon 搜索             │
  ├─ 首次上行 (必须以 Confirmed 确认入网)   │
  │                                      │
  ├─ CMD 0x00 Heartbeat ────────────────▶│ ← 此时服务器可见设备在线
```

> ⚠️ OTAA 入网失败排查：① DevEUI/AppKey 是否已在 ChirpStack 注册 ② 频段是否匹配 ③ Device Profile 是否允许 OTAA ④ 信号是否足够（检查网关 RSSI）。

---

## 8. 性能与压力测试参数

### 8.1 单网关容量

| 参数 | 值 | 说明 |
|------|-----|------|
| 最大并发设备 | **~1000** | 取决于占空比和数据速率 |
| 8 通道网关 | 8 路并行解调 | US915/CN470 跳频 |
| 推荐网络规模 | **≤ 200 设备/网关** | 留有足够的 ping slot 窗口 |

### 8.2 压力测试场景

| 场景 | 触发条件 | 期望 |
|------|---------|------|
| 全校 Code Red | 100 个 Badge 同时按 Red | 所有 Hub 在 10s 内变红 |
| 密集心跳 | 200 设备同时上报心跳 | 无丢包，ChirpStack 无背压 |
| 快速切换 | 连续 10 次 Red ↔ All Clear | 状态机不卡死 |
| 信号边缘 | 某设备 RSSI≈-110dBm | 心跳延迟但最终送达（Confirmed retry） |
| GPS 信号丢失 | 室内 Hub GPS 无信号 | lat/lon=无效值，不影响 LoRa 通信 |

---

## 9. 安全相关

### 9.1 密钥管理

| 密钥 | 长度 | 用途 | 存储位置 |
|------|------|------|---------|
| AppKey | 128bit | OTAA 入网根密钥 | 固件 Flash Config Store |
| NwkSKey | 128bit | 网络层加密+MIC | OTAA 派生，存 RAM |
| AppSKey | 128bit | 应用层加密 | OTAA 派生，存 RAM |
| McAppSKey | 128bit | 多播应用层加密 | 入网后服务器下发 |

> ⚠️ AppKey 出厂烧录后不可明文传输。当前 CSV 中的 AppKey 用于在 ChirpStack 注册和设备端写入。

### 9.2 ChirpStack API 认证

- ChirpStack HTTP API 使用 **Bearer Token (JWT)**
- 在 ChirpStack Web UI → API Keys 中创建
- App Server 持有此 token 才能下发下行命令

---

## 10. 联调检查清单

### 10.1 网关侧

- [ ] 网关 GPS Lock → ChirpStack 显示 GPS 坐标
- [ ] ChirpStack 显示网关 "Online" + "Last seen" 刷新正常
- [ ] ChirpStack → Gateway → Class B Beacon → "enabled" + "Time OK"
- [ ] 网关能收到设备上行 → Device Frames 有记录
- [ ] 上行的 RSSI/SNR 在预期范围内
- [ ] 多播组已创建且关联了 Class B 设备

### 10.2 服务器侧

- [ ] App Server 订阅 MQTT 正确收到上行 JSON
- [ ] Base64 解码成功
- [ ] CRC16/XMODEM 校验通过
- [ ] CMDID 解析正确（0x00=心跳, 0x01=电量, 0x02=按键）
- [ ] 下行编码→Base64→HTTP POST→ChirpStack 返回 200
- [ ] 下行在设备 ping slot 内被收到（设备日志确认）

### 10.3 固件侧

- [ ] OTAA 入网成功 → `DevAddr` 非零
- [ ] Class B Beacon acquired → 日志 "Beacon sync OK"
- [ ] 心跳 CMD 0x00 每 5min 上发 → ChirpStack Device Frames 可见
- [ ] 下行 CMD 0x03 收到 → LED+蜂鸣器正确响应
- [ ] 下行 CMD 0x0A 收到 → 状态正确清除
- [ ] Ping Slot 周期动态切换：8s → 2s → 4s → 8s
- [ ] GPS 有效时 lat/lon 正确，无效时上报无效标记

### 10.4 协议校验

- [ ] 上行帧 CRC16 正确 → 服务器解码不报错
- [ ] 下行帧 CRC16 正确 → 设备解析不报错
- [ ] `length` 字段与实际数据一致
- [ ] 多播 FPort=20 与单播 FPort=10 路由正确
- [ ] GroupID bitmask 匹配逻辑正确（全播 0xFF、定向 bitmask）

---

## 11. 常见联调问题

| 现象 | 可能原因 | 排查方法 |
|------|---------|---------|
| 设备不入网 | ①AppKey 不匹配 ②频段不对 ③信号不到网关 | ChirpStack→Device→查看 Join Request 日志 |
| 设备入网但无上行 | ①FPort 不是 10 ②ADR 配置错误 ③占空比限制 | ChirpStack→Gateway→Live Frames |
| 上行有但服务器收不到 | ①MQTT 未订阅正确 topic ②Base64 解码失败 | MQTT Explorer 查看 topic |
| 下行发不出去 | ①设备 Class B 未同步 ②队列已满 ③Token 无权限 | ChirpStack API 返回码 |
| 下行发出但设备收不到 | ①Beacon 失步 ②ping slot 错过 ③信号太弱 | 设备日志 "Beacon lost" |
| CRC 校验失败 | ①服务器解码顺序错误 ②CRC 计算未 init | 用已知向量 0x58C7 验证 CRC 实现 |
| 多播收不到 | ①Multicast Session 未下发 ②设备不在多播组 ③FPort 不是 20 | ChirpStack Multicast Group→Devices |
| GPS 数据无效 | ①室内无信号 ②UART 波特率不匹配 ③NMEA 校验失败 | 直连 RAK12501 UART 检查原始输出 |
