# LoRaWAN 入网与多播完整流程

> 版本 V1.0 | 2026-06-28 | 基于 NCS LoRaWAN 1.0.3 Class B US915

---

## 1. Kconfig 配置

### 1.1 当前配置（[prj.conf](firmware/prj.conf)）

```ini
CONFIG_LORAWAN=y
CONFIG_LORAWAN_REGION_US915=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
```

### 1.2 目标配置（需新增）

```ini
# ── NVM 持久化（帧计数 / DevNonce / 会话密钥）──
CONFIG_SETTINGS=y
CONFIG_LORAWAN_NVM_SETTINGS=y
CONFIG_SETTINGS_NVS=y

# ── LoRaWAN Services（时钟同步 + 远程多播配置）──
CONFIG_LORAWAN_SERVICES=y
CONFIG_LORAWAN_APP_CLOCK_SYNC=y
CONFIG_LORAWAN_REMOTE_MULTICAST=y
```

### 1.3 Kconfig 依赖链

```
CONFIG_LORAWAN_REMOTE_MULTICAST
    ├─ depends on LORAWAN_APP_CLOCK_SYNC      ← 必须先同步时钟
    ├─ depends on !LORAWAN_NVM_NONE            ← 密钥必须持久化
    └─ depends on LORA_MODULE_BACKEND_LORAMAC_NODE

CONFIG_LORAWAN_NVM_SETTINGS
    ├─ depends on SETTINGS
    └─ depends on LORA_MODULE_BACKEND_LORAMAC_NODE

CONFIG_LORAWAN_APP_CLOCK_SYNC
    └─ 无额外依赖，使用 Port 202
```

---

## 2. 密钥体系全景

```
┌─────────────────────────────────────────────────────────┐
│ 单播密钥（NVM 持久化，OTAA 入网自动协商）                  │
│  每台设备独立                                              │
│                                                           │
│  ┌─ DevEUI      │ 设备出厂写入 ConfigStore/NVS              │
│  ├─ JoinEUI     │ 全网统一，ConfigStore                     │
│  ├─ AppKey      │ 每台独立，ConfigStore（一机一密）          │
│  ├─ DevAddr     │ 入网后由 Network Server 分配              │
│  ├─ NwkSKey     │ 入网时派生，掉电后 NVM 恢复               │
│  └─ AppSKey     │ 入网时派生，掉电后 NVM 恢复               │
├─────────────────────────────────────────────────────────┤
│ 多播密钥（NVM 持久化，Port 200 Remote Multicast Setup）     │
│  组内设备共享                                              │
│                                                           │
│  ┌─ McDevAddr_0 │ 全校广播组                               │
│  ├─ McNwkSKey_0 │                                          │
│  ├─ McAppSKey_0 │                                          │
│  ├─ McDevAddr_1 │ Admin 组                                 │
│  ├─ McNwkSKey_1 │                                          │
│  ├─ McAppSKey_1 │                                          │
│  ├─ McDevAddr_2 │ 教室组                                   │
│  ├─ McNwkSKey_2 │                                          │
│  ├─ McAppSKey_2 │                                          │
│  └─ ...         │ 最多 4 组                                │
├─────────────────────────────────────────────────────────┤
│ 应用层身份（ConfigStore 持久化，CMD 0x50 设置）              │
│                                                           │
│  ├─ group_id    │ 角色 bitmask (Bit0=Admin, Bit1=Nurse…)   │
│  └─ room_id     │ 房间编号                                  │
└─────────────────────────────────────────────────────────┘
```

### 密钥归属

| 密钥 | 管理方 | 来源 | 存储位置 |
|------|--------|------|---------|
| DevEUI | 应用层 | 生产烧录 | ConfigStore (NVS Flash) |
| JoinEUI | 应用层 | 生产烧录 | ConfigStore (NVS Flash) |
| AppKey | 应用层 | 生产烧录（一机一密） | ConfigStore (NVS Flash) |
| DevAddr | SDK (NVM) | OTAA 入网 NS 分配 | NVS Flash |
| NwkSKey | SDK (NVM) | OTAA 入网密钥派生 | NVS Flash |
| AppSKey | SDK (NVM) | OTAA 入网密钥派生 | NVS Flash |
| FCnt 帧计数 | SDK (NVM) | LoRaMac 自动维护 | NVS Flash |
| DevNonce | SDK (NVM) | LoRaMac 自动维护 | NVS Flash |
| McDevAddr | SDK (NVM) | Port 200 Remote Multicast Setup | NVS Flash |
| McNwkSKey | SDK (NVM) | Port 200 Remote Multicast Setup | NVS Flash |
| McAppSKey | SDK (NVM) | Port 200 Remote Multicast Setup | NVS Flash |
| group_id | 应用层 | CMD 0x50 远程设置 | ConfigStore (NVS Flash) |
| room_id | 应用层 | CMD 0x50 / 生产烧录 | ConfigStore (NVS Flash) |

---

## 3. 完整流程

### 3.1 阶段 1: 上电启动

```
main()
  ├─ config_store_init()
  │   └─ 从 NVS Flash 恢复:
  │        dev_eui, join_eui, app_key    (凭证，生产时写入)
  │        group_id, room_id             (身份，CMD 0x50 设置)
  │
  ├─ alarm_sm_init()             → 告警状态机复位
  ├─ actuator_mgr_init()         → LED/蜂鸣器/振动
  ├─ power_mgr_init()            → 电池 ADC + 充电检测
  │
  └─ hal_sx1262_init()
        ├─ 从 ConfigStore 加载 DevEUI/JoinEUI/AppKey 到 RAM 缓冲
        └─ lorawan_start()
             │
             │  ┌─ SDK 内部 ────────────────────────────┐
             └─→│ 初始化 LoRaMac 栈                       │
                │ 初始化 SecureElement (soft-se)           │
                │ 检查 NVM: 是否有已保存的会话?             │
                │  ├─ 有 → 恢复 DevAddr + Keys + FCnt     │
                │  └─ 无 → 等待 OTAA 入网                 │
                └─────────────────────────────────────────┘
```

### 3.2 阶段 2: OTAA 入网

```
lora_thread_entry()
  │
  ├─ 1. hal_sx1262_init() — LoRa 硬件初始化
  │
  ├─ 2. BLE 先行启动 (入网前即开始广播/扫描)
  │      Hub: ble_hub_adv_start()
  │      Badge: ble_scan 能力就绪
  │      保证离线状态本地功能仍可用
  │
  ├─ 3. OTAA 入网 — 无限重试 + 指数退避
  │
  │   ┌─ 入网状态机 ─────────────────────────────────────┐
  │   │                                                  │
  │   │  JOINING ──── hal_sx1262_join()                  │
  │   │     │                                            │
  │   │     ├─ 成功 → JOINED (退出循环)                   │
  │   │     │                                            │
  │   │     └─ 失败 → WAIT                               │
  │   │               │                                  │
  │   │               ├─ fail_count < 6 → 退避后 JOINING │
  │   │               └─ fail_count ≥ 6 → FAILED (告警)  │
  │   │                                                  │
  │   └──────────────────────────────────────────────────┘
  │
  │   退避策略 (指数退避):
  │   ┌──────────┬──────────────┐
  │   │ 尝试次数  │ 退避间隔      │
  │   ├──────────┼──────────────┤
  │   │    1     │     10s      │
  │   │    2     │     20s      │
  │   │    3     │     40s      │
  │   │    4     │     80s      │
  │   │    5     │    160s      │
  │   │   6+     │    900s (15min) │  ← LoRaWAN 规范上限
  │   └──────────┴──────────────┘
  │
  │   用户反馈 (入网期间):
  │   ┌──────────┬──────────────────┬──────────────┐
  │   │ 状态      │ Badge OLED        │ Badge LED     │
  │   ├──────────┼──────────────────┼──────────────┤
  │   │ JOINING  │ "LoRa Joining..." │ 蓝慢闪        │
  │   │ WAIT     │ "Retry in XXs"    │ 灭            │
  │   │ JOINED   │ "LoRaWAN OK"      │ 灭 (正常)     │
  │   │ FAILED   │ "Join Failed!"    │ 红快闪        │
  │   └──────────┴──────────────────┴──────────────┘
  │
  └─ hal_sx1262_join()
       │
       ├─ 填充 lorawan_join_config:
       │    dev_eui  ← ConfigStore (生产烧录)
       │    join_eui ← ConfigStore (生产烧录)
       │    app_key  ← ConfigStore (生产烧录，一机一密)
       │    nwk_key  ← 同 AppKey (LoRaWAN 1.0.3 规范)
       │    dev_nonce ← ConfigStore (防重放)
       │
       └─ lorawan_join(&join_cfg)
            │
            │  ┌─ SDK 内部 ────────────────────────────────┐
            ├─→│ MIB_SET DevEUI / JoinEUI / AppKey          │
            │  │ MLME_JOIN (OTAA)                           │
            │  │                                            │
            │  │ 空中:                                      │
            │  │   ──JoinRequest──→                         │
            │  │   {DevEUI, JoinEUI, DevNonce}              │
            │  │                                            │
            │  │   ←──JoinAccept───                         │
            │  │   {DevAddr, NetID, DLSettings, RxDelay…}   │
            │  │                                            │
            │  │ 本地派生:                                   │
            │  │   NwkSKey = aes128_encrypt(AppKey, …)      │
            │  │   AppSKey = aes128_encrypt(AppKey, …)      │
            │  │                                            │
            │  │ NVM 自动持久化:                              │
            │  │   DevAddr, NwkSKey, AppSKey                │
            │  │   FCntUp=0, FCntDown=0                     │
            │  │   DevNonce (下次入网用)                      │
            │  └────────────────────────────────────────────┘
            │
            └─ 返回: joined = true

  ─── 入网完成 ───
             │
             ├─ OLED: 显示 "LoRaWAN OK"
             ├─ LED: 恢复正常状态
             │
             ├─ lorawan_set_class(LORAWAN_CLASS_B)
             │    │
             │    │  ┌─ SDK 内部 ─────────────────────────┐
             │    └─→│ 搜索 Beacon (128s 间隔)              │
             │       │ 锁定 Beacon → 时间同步               │
             │       │ 计算 Ping Slot 偏移                  │
             │       │ Ping Slot 窗口: 30ms                 │
             │       │ Ping 周期: 由服务器配置              │
             │       └─────────────────────────────────────┘
             │
             └─ send_heartbeat()  // 首次上行，告知服务器"我在线"
```

### 3.3 阶段 3: 多播组注册

```
服务器 (ChirpStack / AWS LNS)
  │
  ├─ 管理界面创建多播组:
  │    ├─ 全校广播组: McDevAddr_0, McNwkSKey_0, McAppSKey_0
  │    ├─ Admin 组:   McDevAddr_1, McNwkSKey_1, McAppSKey_1
  │    └─ 教室101组:  McDevAddr_2, McNwkSKey_2, McAppSKey_2
  │
  └─ 将设备关联到对应多播组
       │
       │  ┌─ SDK 自动处理 (Port 200 TS005) ───────────────┐
       └─→│ Remote Multicast Setup 服务在后台自动运行        │
          │ 接收 McGroupSetupReq:                           │
          │   ├─ McGroupID + McAddr + McKeyEncryptionKey   │
          │   ├─ SDK 内部派生 McNwkSKey + McAppSKey         │
          │   ├─ 写入 MIB: MC_KEY_x, MC_APP_S_KEY_x,       │
          │   │            MC_NWK_S_KEY_x                  │
          │   ├─ 调用 LoRaMacMcChannelSetup()               │
          │   └─ NVM 自动持久化多播密钥                      │
          │                                                │
          │ 应用层: 全程无需干预                             │
          └────────────────────────────────────────────────┘
```

### 3.4 阶段 4: 正常运行

```
┌────────────────────────────────────────────────────────────────┐
│ 上行 (应用层主动发送)                                            │
├────────┬──────────────┬──────────┬─────────────────────────────┤
│ FPort  │ CMD          │ 触发     │ 说明                        │
├────────┼──────────────┼──────────┼─────────────────────────────┤
│ 10     │ 0x00 Heartbeat│ 每 5min │ Badge 心跳                  │
│ 11     │ 0x00 Heartbeat│ 每 5min │ Hub 心跳                    │
│ 20     │ 0x01 Power    │ 电量变化 │ Badge/Hub 电量上报           │
│ 20     │ 0x02 KeyEvent │ 按键    │ Badge 按键事件               │
└────────┴──────────────┴──────────┴─────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ 下行单播 (Class B Ping Slot, FPort=20)                         │
│ 服务器 → 单台设备，CMDID 区分指令                                │
├────────┬──────────────────┬────────────────────────────────────┤
│ 0x04   │ Code Setting     │ 配置告警参数 (音量/颜色/时长等)      │
│ 0x05   │ LED Control      │ 手动控灯                            │
│ 0x06   │ Buzzer Control   │ 手动控蜂鸣器                        │
│ 0x07   │ Vibration Ctrl   │ 手动控振动                          │
│ 0x08   │ LCD Content      │ 自定义 LCD 两行文字                 │
│ 0x09   │ LCD Line2 OnOff  │ LCD 第二行显示开关                  │
│ 0x50   │ Set Group ID     │ 修改设备 group_id 身份              │
└────────┴──────────────────┴────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ 下行多播 (Class B Multicast Slot, 任意 FPort)                   │
│ 服务器 → 组内全部设备（一次空口 TX）                              │
│                                                                 │
│ MAC 层自动:                                                      │
│   1. DevAddr 匹配多播 McDevAddr                                  │
│   2. 用对应 McAppSKey / McNwkSKey 解密和 MIC 校验                │
│   3. 上行 → McpsIndication → 应用层回调                          │
│                                                                 │
│ 应用层收到后 (group/room 双重过滤):                               │
├────────┬──────────────────┬────────────────────────────────────┤
│ 0x03   │ Code             │ 告警通知 → alarm_sm                 │
│ 0x0A   │ Clear Packet     │ 清除告警 → alarm_sm                 │
└────────┴──────────────────┴────────────────────────────────────┘
```

### 3.5 阶段 5: 掉电重启

```
┌────────────────────────────────────────────────────────────────┐
│ config_store_init()                                             │
│   → 从 NVS Flash 恢复:                                          │
│       DevEUI, JoinEUI, AppKey  (凭证 → hal_sx1262_init 使用)    │
│       group_id, room_id        (身份 → proto_handler 使用)      │
├────────────────────────────────────────────────────────────────┤
│ lorawan_start()                                                 │
│   → SDK NVM 自动恢复:                                           │
│       DevAddr, NwkSKey, AppSKey                                 │
│       FCntUp, FCntDown   ← 帧计数连续，不掉                     │
│       DevNonce           ← 防重放                               │
│       McDevAddr_x, McNwkSKey_x, McAppSKey_x ← 多播密钥          │
│                                                                 │
│   ✅ 无需重新入网 (如会话有效)                                  │
│   ⚠️ 会话过期 → 触发 OTAA 入网 (指数退避重试)                   │
│   ✅ 帧计数不归零                                                │
│   ✅ 多播组关系不丢                                              │
│   ✅ 直接恢复通信 (如会话有效)                                    │
└────────────────────────────────────────────────────────────────┘
```

---

## 4. 端口分配总览

| Port | 方向 | 用途 | 管理方 |
|------|------|------|--------|
| 0 | 上下行 | MAC 命令 (ACK, LinkCheck…) | SDK 自动 |
| 2 | 上行 | 数据上报 (过渡，待替换为 10/11/20) | 应用层 |
| 10 | 上行 | Badge 心跳 (CMD 0x00) | 应用层 |
| 11 | 上行 | Hub 心跳 (CMD 0x00) | 应用层 |
| 20 | 上下行 | 上行业务数据 + 全部下行 (单播+多播) | 应用层 |
| 200 | 下行 | Remote Multicast Setup (TS005) | SDK 自动 |
| 201 | 下行 | Fragmented Data Transport (TS004) | SDK 自动 |
| 202 | 上下行 | Clock Synchronization (TS003) | SDK 自动 |

> Port 0/200/201/202 应用层不可占用，SDK 内部使用。

---

## 5. 职责划分

| 层 | 职责 | 谁管 |
|---|------|------|
| **DevEUI/JoinEUI/AppKey** | 设备凭证，入网前必备 | 应用层 → ConfigStore |
| **DevAddr/NwkSKey/AppSKey** | 单播会话密钥 | SDK → NVM 自动 |
| **FCnt 帧计数** | 防重放 | SDK → NVM 自动 |
| **DevNonce** | OTAA 防重放 | SDK → NVM 自动 |
| **McDevAddr/McKeys** | 多播会话密钥 | SDK → Port 200 自动 |
| **Beacon 同步** | Class B 时间对齐 | SDK → 自动 |
| **Ping Slot 周期** | 下行响应延迟 | 服务器 (ChirpStack Device Profile 配置) |
| **group_id** | 角色身份 bitmask | 应用层 → CMD 0x50 设置 |
| **room_id** | 物理位置 | 应用层 → CMD 0x50 设置 |
| **告警状态** | Code Red/Medical/… | 应用层 → alarm_sm |

### 核心结论

> 应用层只管：**凭证存储** + **设备身份** + **入网重试策略** + **告警逻辑**。
> SDK 全包：入网、密钥派生、帧计数、Beacon 同步、多播密钥下发、NVM 持久化。
>
> **入网重试策略**：无限重试 + 指数退避（10s → 20s → 40s → 80s → 160s → 900s），BLE 在入网前启动保证离线可用。

---

## 6. FPort 设计原则

- **FPort 区分"协议类别"，CMDID 区分"具体指令"**
- 一个 FPort 对应一套解析规则，同一套协议格式共用一个 FPort
- 单播/多播不靠 FPort 区分，靠 LoRaWAN MAC 层 DevAddr 区分
- 当前项目只有一套应用协议，1~2 个 FPort 足够
- Port 200/201/202 是 SDK 保留端口，不可占用
