# Gateway（网关）功能规格与需求

> 版本 V1.0 | 硬件 RAK7289CV2（现成产品） | 2026-06-19
>
> **状态：现成产品，仅需配置，不需要固件开发。**
>
> 排序规则：LoRa 射频 > 网络接入 > LNS 对接 > Class B Beacon > 多播 > 离线回退 > 供电

---

## 1. 产品概述

RAK7289CV2 是 RAKwireless 的现成 LoRaWAN 网关产品，负责在射频覆盖范围内与 Hub/Badge 进行双向 LoRaWAN 通信，将数据中继到云端 LNS 服务器。支持多网络接入和备份链路。

### 1.1 在系统中的角色

```
Badge/Hub ←→ [LoRaWAN] ←→ Gateway ←→ [IP 网络] ←→ ChirpStack V4 LNS ←→ 应用服务器
```

Gateway 是**透传设备**，不解析应用层协议，只做 LoRaWAN ↔ IP 数据包中继。

---

## 2. LoRa 射频 ★★★★★

### 2.1 射频参数

| 参数 | 规格 |
|------|------|
| LoRaWAN 版本 | 1.0.3 |
| 支持频段 | IN865 / EU868 / AU915 / US915 / KR920 / RU864 / AS923 |
| Class 支持 | A / B / C |
| 下行方式 | 单播 (Unicast) + 多播 (Multicast) |
| 天线 | 外置 LoRa 天线 |

### 2.2 Class B Beacon

| 项目 | 说明 |
|------|------|
| Beacon 周期 | 每 128 秒一次 |
| GPS 同步 | **必须**：网关需要 GPS 信号以产生精确 Beacon 时序 |
| 配置要点 | 开启 GPS 模块 → 启用 Class B Beacon → 等待 GPS Lock |

> Class B Beacon 是系统下行通信的关键。如果 Beacon 不工作，所有 Class B 设备将退化为 Class A，服务器无法随时下发命令。

---

## 3. 网络接入 ★★★★★

### 3.1 主链路

| 接入方式 | 用途 |
|---------|------|
| **Ethernet (LAN)** | 主连接，推荐 |
| **Wi-Fi** | 备选主连接 |

### 3.2 备份链路

| 接入方式 | 用途 |
|---------|------|
| **4G/LTE/5G** | 备份连接，主链路断开时自动切换 |

### 3.3 连接要求

- 与 ChirpStack V4 服务器之间的 IP 可达（公网或 VPN）
- 稳定低延迟连接
- 支持 DHCP 或静态 IP

---

## 4. LNS 对接 ★★★★★

### 4.1 对接协议

| 项目 | 规格 |
|------|------|
| LNS 服务器 | ChirpStack V4 |
| 对接协议 | **Basic Station** (WSS / IP:port) |
| 组件 | chirpstack-gateway-bridge |

### 4.2 配置要点

1. 网关刷入支持 Basic Station 的固件
2. 配置 ChirpStack V4 服务器地址和端口
3. 配置 TLS/认证证书
4. 验证网关在 ChirpStack 控制台显示为 "Online"

### 4.3 参考文档

```
https://docs.rakwireless.com/product-categories/wisgate/rak7289v2/lorawan-configuration/
https://learn.rakwireless.com/hc/en-us/articles/35978760611607-how-to-connect-rak-gateway-to-chirpstack-v4-using-basics-station-lns
```

---

## 5. 多播 (Multicast) ★★★★

### 5.1 用途

Code Red 和 SRP 告警通过 Class B Multicast 一次性下发到所有设备，避免逐设备单播的延迟。

### 5.2 配置

- 在 ChirpStack V4 中创建 **Multicast Group**
- 设备入网后，服务器下发 Multicast Session（McAddr + McNwkSKey + McAppSKey）
- 多播下行使用 **FPort=20**

### 5.3 优势

| 方式 | 延迟 | 适用 |
|------|------|------|
| Multicast | 一次覆盖全部 | Code Red / SRP |
| Unicast | 逐设备发送 | Medical / Admin（定向组播） |

---

## 6. 离线回退模式 ★★★

### 6.1 场景

当所有互联网连接 (Ethernet/Wi-Fi/LTE) 同时断开时，网关需要本地处理告警。

### 6.2 要求

1. **检测**：自动检测连接断开，无需人工干预
2. **本地 LNS**：切换到嵌入式本地 LNS（网关内置或外接 Raspberry Pi）
3. **本地路由**：在本地应用告警路由规则，下发正确的下行命令
4. **本地 Dashboard**：通过本地网络提供最小告警面板
5. **事件队列**：所有事件本地排队，保留原始时间戳
6. **同步恢复**：连接恢复后自动同步到云端，切换回主 LNS

### 6.3 状态

> v1.0 可选功能，v1.1 完善。

---

## 7. 供电 ★★

| 项目 | 规格 |
|------|------|
| 主供电 | 市电 (AC Power) 或 USB-C |
| 备用电池 | 内置，断电时自动切换 |
| 工作环境 | 室内安装 |

---

## 8. 配置清单（部署时需完成）

| # | 配置项 | 说明 |
|---|--------|------|
| 1 | 固件升级 | 确保网关固件支持 Basic Station |
| 2 | 网络设置 | 配置 Ethernet/Wi-Fi/LTE 参数 |
| 3 | LNS 地址 | ChirpStack V4 服务器 URL 和端口 |
| 4 | TLS 证书 | 配置 Gateway 与 LNS 之间的安全连接 |
| 5 | GPS 天线 | 连接 GPS 天线，确保有信号 |
| 6 | Class B Beacon | 在 ChirpStack 中开启 Class B，等待 GPS Lock |
| 7 | 频段选择 | 选择对应地区的 LoRaWAN 频段 |
| 8 | Multicast Group | 在 ChirpStack 创建多播组 |
| 9 | 备份链路 | 配置 4G/LTE 备份 |
| 10 | 验证 | 网关在 ChirpStack 显示 Online，Beacon 正常 |

---

## 9. 网络架构图

```
┌──────────┐   LoRaWAN    ┌──────────────┐    Basic Station    ┌─────────────────┐
│  Badge   │ ←→ RF ←→     │              │ ←→ WSS/IP:port ←→  │  ChirpStack V4  │
│  Hub     │              │ RAK7289CV2   │                     │  (AWS)          │
└──────────┘              │              │                     └────────┬────────┘
                          │ Ethernet/WiFi│                              │
                          │ 4G/LTE 备份  │                     ┌────────┴────────┐
                          └──────────────┘                     │  App Server     │
                                                               │  Dashboard      │
                                                               └─────────────────┘
```
