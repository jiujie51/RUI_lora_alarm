# LoRaWAN FUOTA 固件空中升级方案（Class B 多播路线）

> 版本 V1.0 | 2026-07-17 | 硬件 RAK4630 (nRF52840 1MB Flash / 256KB RAM + SX1262) | NCS 3.3.0 (Zephyr 4.3.99) | LoRaWAN 1.0.3 Class B OTAA US915
>
> 状态：**方案设计，未实施**。本文档包含实施所需的分区表 (pm_static.yml)、Kconfig 清单、代码改动清单与验收用例。
>
> 本文档取代 `firmware_design.md` §2.6"难点六：OTA 固件升级"的旧设计（修订对照见 §3）。

---

## 1. 概述

### 1.1 目标与范围

- 客户硬性需求：*"Need Over-The-Air (OTA) firmware updates without the need to pull the Hub down"*（`alert_design.md:31`）。
- 目标：Badge / Hub 两种固件变体均可经 LoRaWAN 空中升级，升级失败自动回滚，不变砖。
- 范围：设备端 FUOTA 接收栈、Flash 分区规划、MCUboot 策略、服务器侧下发工具、测试验收。**不含**：BLE DFU、RAK 原厂 RUI3/WisToolBox 升级路径（与本方案是两套体系）、网关固件升级。

**成功判据**

| 指标 | 要求 |
|---|---|
| 单轮升级成功率（实验环境） | ≥ 95%，失败设备可重跑 |
| 坏镜像 / 升级中断电 | 100% 自动回滚或保持旧固件，0 变砖 |
| 升级窗口内告警能力 | 传输组之外的告警多播组全程可用 |
| 设备增量耗电 | ≤ 1 mAh / 次（Badge 电池友好） |

### 1.2 术语

| 术语 | 说明 |
|---|---|
| TS004 | LoRa Alliance *Fragmented Data Block Transport* v1.0.0，FPort **201**，本方案的分片传输协议 |
| TS005 | *Remote Multicast Setup* v1.0.0，FPort 200，**本方案不使用**（见 §4） |
| TS003 | *Application Layer Clock Synchronization*，FPort 202，**本方案不使用**（Class B 分片无绝对时间要求） |
| FEC | TS004 前向纠错：数据分片之外附加冗余分片，允许丢片仍完整解码 |
| test-swap / confirm | MCUboot 升级语义：新镜像先以"待确认"状态启动，应用调 `boot_write_img_confirmed()` 后永久生效，否则下次复位自动回滚 |
| swap-using-move | MCUboot 无 scratch 分区的交换算法（本项目实际使用，见 §6.1） |

### 1.3 方案一页总览

```
┌────────────────────┐  HTTP API (单播队列 + 多播队列)  ┌──────────────────┐
│ fuota_class_b_     │────────────────────────────────▶│ ChirpStack V4    │
│ sender.py          │  ① FragSessionSetupReq 单播     │ (SimulAlert LNS) │
│ · imgtool 签名镜像  │  ② DataFragment ×N 入多播队列   └────────┬─────────┘
│ · TS004 FEC 编码   │  ③ FragSessionStatusReq 抽查              │ WSS basicstation
│ · 4s/片 节流       │                                  ┌────────▼─────────┐
└────────────────────┘                                  │ RAK7289 网关      │
                                                        └────────┬─────────┘
                                US915 下行 923.3MHz / DR13 (SF7/BW500)
                                Class B 多播 ping slot（周期 4s，1 片/slot）
                     ┌───────────────────────────────┬───────────┘
                     ▼                               ▼
             ┌───────────────┐               ┌───────────────┐
             │ Badge ×N      │               │ Hub ×N        │
             │ descriptor    │               │ descriptor    │
             │ model=0x01    │               │ model=0x02    │
             └───────┬───────┘               └───────┬───────┘
                     │  分片→FEC 解码→写 slot1→MCUboot test-swap
                     │  →健康检查→confirm（失败自动回滚）
                     ▼
             心跳上报新版本号 → 服务器确认升级完成
```

要点：

1. **复用现有 4 个预烧录 Class B 多播组之一**承载分片下行——不依赖服务器 FUOTA 引擎，不切换设备 Class，不脱离 beacon，ping slot 本就常开所以增量耗电近乎为零。
2. 设备端**全部复用 Zephyr 原生 TS004 子系统**（`subsys/lorawan/services/frag_transport.c` + `frag_flash.c`），不自研传输层；镜像完整性靠 MCUboot ECDSA P-256 验签，不做应用层 CRC。
3. Badge 与 Hub 镜像靠 FragSessionSetupReq 的 4 字节 **Descriptor** 互斥（Zephyr 原生回调，见 §8），分两场次升级。
4. 服务器侧只需**多播下行队列 API**（`integration_guide.md` §已列 `POST /api/multicast-groups/{id}/queue`）+ 一个自研发送脚本。

---

## 2. 现状与约束（已核实事实）

### 2.1 硬件与镜像现状

| 项 | 实测值 | 来源 |
|---|---|---|
| 内部 Flash | 1 MB（无外部 NOR，QSPI 已禁用） | `rak4630_badge.overlay`、合并 DTS |
| RAM | 256 KB，静态占用 60,976 B（23.3%） | `badge_build/firmware/zephyr/zephyr.stat` |
| Badge 签名镜像 | **219,899 B** | `badge_build/dfu_application.zip` |
| Hub 签名镜像 | **216,672 B** | `build/`（Hub 构建目录） |
| MCUboot 镜像 | 33,308 B（48KB 分区占 69%） | `badge_build/mcuboot/zephyr/zephyr.bin` |

注意：OLED 暂时屏蔽、GPS 等功能未完，**镜像仍会增长**——本方案传输参数按 240KB 镜像预算（§7、§10），分区按上限 476KB 规划（§5）。

### 2.2 当前分区与两处冲突

当前无 `pm_static.yml`，分区由 Partition Manager 每次构建自动生成（`badge_build/partitions.yml`）：

```
0x00000 ─ mcuboot (48KB) ─ 0x0C000 ─ mcuboot_primary (484KB) ─ 0x85000
       ─ mcuboot_secondary (484KB) ─ 0xFE000 ─ settings_storage (8KB) ─ 0x100000
```

**冲突 1（会毁配置）**：`src/config/config_store.c` 不走 NVS/settings，而是裸页读写固定地址：

| config_store 槽 | 地址 | 落在哪个分区 | 后果 |
|---|---|---|---|
| primary | 0xFC000 | mcuboot_secondary 内 | FUOTA 首次整区擦除 slot1 时**配置被毁** |
| backup1 | 0xFD000 | mcuboot_secondary 内 | 同上 |
| backup2 | 0xFE000 | settings_storage 内 | 与 LoRaWAN NVM 的 NVS 文件系统**互相踩踏** |
| factory | 0xFF000 | settings_storage 内 | 同上 |

**冲突 2（前置 bug，FUOTA 传输的根基问题）**：`src/hal/lorawan_mc.c:23` `MC_DATARATE=3`。US915 的 RX 合法 DR 范围是 **DR8–DR13**（loramac-node `RegionUS915.h`：`US915_RX_MIN_DATARATE=DR_8`），`LoRaMacMcChannelSetupRxParams()` 内部 `RegionVerify(PHY_RX_DR)` 对 DR3 校验失败 → 走 `lorawan_mc.c:111` "RxParams failed" 分支——**4 个多播组的 ping-slot 接收参数当前应全部未生效**（实施前需用运行日志实证）。`muticast_flow.md` 所写 "DR3 (SF7/125kHz)" 在 US915 下行不存在（US915 下行全部 500kHz 通道），SF7 对应 **DR13 (SF7/BW500)**。修复见 §11。

### 2.3 LoRaWAN 栈现状

- Zephyr `subsys/lorawan` + loramac-node 后端 + `CONFIG_LORA_SX126X` 驱动；join 走 Zephyr API（`hal_sx1262.c` 薄封装），Class B、OTAA、US915 sub-band 2。
- 4 个静态多播组（CODE RED/BLUE/YELLOW/GREEN）由 `lorawan_mc.c` 直调 loramac-node `LoRaMacMcChannelSetup()` 预置密钥建立，**已占满 `LORAMAC_MAX_MC_CTX=4` 全部上下文**。
- FPort 占用：

| FPort | 方向 | 用途 | 管理方 |
|---|---|---|---|
| 0 | 上下行 | MAC 命令 | SDK |
| 10 / 11 | 上行 | Badge / Hub 心跳 | 应用 |
| 20 | 上下行 | 业务数据（单播+多播下行） | 应用 |
| 200 / 202 | 下行 | TS005 / TS003（本方案不用，仍保留给 SDK） | SDK |
| **201** | 上下行 | **TS004 分片传输（本方案使用）** | SDK |

### 2.4 现存缺陷清单（FUOTA 依赖项）

| # | 缺陷 | 位置 | 影响 |
|---|---|---|---|
| D0 | **Zephyr 封装不支持 Class B**（`lorawan_set_class` 恒 -ENOTSUP，MLME indication 全部丢弃，`LORAMAC_CLASSB_ENABLED` 未编译），设备实际以 Class A 运行 | `zephyr/subsys/lorawan/loramac-node/lorawan.c:553/:234` | ping-slot 接收完全不可用——独立方案见 `classb_design.md` |
| D1 | 多播 RX DR=3 非法（§2.2 冲突 2）——已由 2026-07-17 运行日志实证（`mac_status=0x04\|gid` = DR error，频率位通过） | `lorawan_mc.c:23` | 多播收不到 → FUOTA 传输通道不通 |
| D2 | config_store 与 slot1/settings 重叠（§2.2 冲突 1） | `config_store.c` | FUOTA 擦 slot1 毁配置 |
| D3 | 下行回调 `LW_RECV_PORT_ANY` 全端口转发，未过滤 200–202 | `hal_sx1262.c:78` | 业务协议解析器会吞/误解析 TS004 服务报文 |
| D4 | 全工程无 `boot_write_img_confirmed()` 调用 | — | test-swap 后必回滚，升级"成功后失效" |

---

## 3. 对 firmware_design.md §2.6 旧设计的修订

| 项 | 旧设计（§2.6，2026-06） | 本方案 | 修订理由 |
|---|---|---|---|
| Bootloader | 40 KB | **48 KB (0xC000)** | NCS 默认分区大小；实测镜像 33 KB，留 31% 余量 |
| App Slot | 460 KB ×2 | **476 KB ×2** | 1MB 精确划分后的最大等分（§5.1） |
| OTA Scratch | 60 KB | **取消** | 实际构建用 swap-using-move，无需 scratch（§6.1） |
| Config Store | 4 KB @0xF0000 | **16 KB 独立 PM 分区 @0xFA000** | 4 页双备份+factory；解除与 slot1/settings 的重叠 |
| 镜像完整性 | 应用层 CRC32 逐片校验 | **MCUboot ECDSA P-256 验签** | Zephyr frag_flash 无 CRC；签名验证覆盖完整性且防篡改 |
| 断点续传 | fragment bitmask 持久化到 Config Store | **会话重跑 + FEC 冗余**（bitmask 持久化列为未来增强） | Zephyr 解码器状态在 RAM，无持久化钩子；FEC ≤10% 丢片免重传，超限重跑会话代价可接受（~78 min） |
| 下发方 | "服务器按 LoRaWAN FUOTA 规范下发" | **自研 sender 脚本 + 现有 Class B 多播组** | 不依赖 SimulAlert 是否开通 FUOTA 引擎（遗留待确认 #5/#9） |

同时勘误：`muticast_flow.md` 的 "DR3 (SF7/125kHz)" → 应为 **DR13 (SF7/BW500)**（§2.2 冲突 2）；`spec_badge.md` §4.3 / `spec_hub.md` §4.3 的分区一行描述随本方案 §5.1 更新。

---

## 4. 路线选型：为什么用现有 Class B 多播组

两条候选路线（均基于 Zephyr TS004 分片 + MCUboot，仅下发通道不同）：

| 维度 | 路线 A：TS005 临时 Class C 会话 | **路线 B：现有 Class B 多播组（本方案）** |
|---|---|---|
| 传输时长（240KB） | ~20 min（Class C 连续收） | ~78 min（4s/片，§10） |
| 设备增量耗电 | 4–5 mAh（RX 常开 ~30min） | **~0.3 mAh**（ping slot 本就常开） |
| 服务器依赖 | ChirpStack v4.6+ FUOTA 引擎（SimulAlert **未确认开通**） | 仅多播下行队列 API（已有文档接口） |
| 多播上下文 | TS005 `McGroupSetupReq` 会**覆写**已占满的 4 个告警组之一（loramac-node 直接覆写 ctx，无占用检查），需会话后恢复钩子 | **不占用**，告警组原样保留 |
| Class / beacon | 会话期脱离 Class B；且 Zephyr `lorawan_services_class_c_stop()` 硬编码回 **Class A**（非 Class B），异常路径需补救 | 全程 Class B，beacon 不断 |
| 设备端实现量 | + 时钟同步/多播服务 + ctx 恢复看门狗 | 仅 TS004 + 应用胶水 |
| 服务器端实现量 | 零（引擎全自动） | 自研 sender 脚本（含 TS004 FEC 编码器） |

**结论：选路线 B。** 决定性因素：① 电池设备（Badge 数周续航、Hub Li-SOCl₂ 3 年+），Class C 连续收电流不友好；② SimulAlert 侧 FUOTA 引擎能力未确认，路线 B 只依赖已有的多播队列 API；③ 不碰已占满的多播上下文，告警功能零改动。路线 A 保留为未来选项（若服务器开通 FUOTA 引擎且要求缩短维护窗口，设备端只需追加 `CONFIG_LORAWAN_APP_CLOCK_SYNC` + `CONFIG_LORAWAN_REMOTE_MULTICAST` 与 ctx 恢复逻辑）。

---

## 5. Flash 分区规划（核心交付一）

### 5.1 新分区表

按当前镜像 220KB、预留增长到 476KB 上限规划；1MB 精确划分，总和 = 0x100000，全部 4KB 页对齐：

| 分区 | 起始 | 结束 | 大小 | 说明 |
|---|---|---|---|---|
| mcuboot | 0x00000 | 0x0C000 | 0xC000 (48 KB) | 实测 33 KB，余量 31% |
| mcuboot_pad | 0x0C000 | 0x0C200 | 0x200 | MCUboot 镜像头 |
| app | 0x0C200 | 0x83000 | 0x76E00 (486,912 B) | 现 Badge 镜像占 45.2% |
| mcuboot_primary | 0x0C000 | 0x83000 | 0x77000 (476 KB) | = pad + app |
| mcuboot_secondary | 0x83000 | 0xFA000 | 0x77000 (476 KB) | FUOTA 写入区；与 primary 等大（swap-move 要求） |
| config_store | 0xFA000 | 0xFE000 | 0x4000 (16 KB) | 4×4KB 页，独立分区（解除冲突 1） |
| settings_storage | 0xFE000 | 0x100000 | 0x2000 (8 KB) | NVS 独占（LoRaWAN 帧计数/DevNonce） |

```
0x00000 ┌─────────────┐
        │  MCUboot    │ 48KB
0x0C000 ├─────────────┤
        │  Slot-0     │ 476KB  ← 运行镜像 (pad 0x200 + app 0x76E00)
        │  (primary)  │
0x83000 ├─────────────┤
        │  Slot-1     │ 476KB  ← FUOTA 分片写入区 (frag_flash)
        │  (secondary)│
0xFA000 ├─────────────┤
        │ config_store│ 16KB   ← 业务配置 4 页 (primary/backup1/backup2/factory)
0xFE000 ├─────────────┤
        │  settings   │ 8KB    ← NVS: LoRaWAN NVM
0x100000└─────────────┘
```

- swap-using-move 约束下**有效签名镜像上限** ≈ 486,912 − 4,096（移动扇区）− trailer(~1.5KB) ≈ **481 KB**；对应 MCUboot `BOOT_MAX_IMG_SECTORS` 需 ≥ 119（默认 128，无需改）。
- Badge / Hub 两变体**必须共用同一份 pm_static.yml**——布局逐字节一致是两变体互相 FUOTA 的前提。

### 5.2 pm_static.yml 草案（放 `firmware/pm_static.yml`，sysbuild/PM 自动拾取）

```yaml
mcuboot:
  address: 0x0
  size: 0xc000
  region: flash_primary
mcuboot_pad:
  address: 0xc000
  size: 0x200
  region: flash_primary
app:
  address: 0xc200
  size: 0x76e00
  region: flash_primary
mcuboot_primary:
  orig_span: &id001
  - mcuboot_pad
  - app
  span: *id001
  address: 0xc000
  size: 0x77000
  region: flash_primary
mcuboot_primary_app:
  orig_span: &id002
  - app
  span: *id002
  address: 0xc200
  size: 0x76e00
  region: flash_primary
mcuboot_secondary:
  address: 0x83000
  size: 0x77000
  region: flash_primary
config_store:
  address: 0xfa000
  size: 0x4000
  region: flash_primary
settings_storage:
  address: 0xfe000
  size: 0x2000
  region: flash_primary
```

> 实施时以 PM 构建报错为准微调字段（PM 静态文件必填 address/size/region，span 分区需 orig_span/span）；合入后对比 `badge_build/partitions.yml` 生成结果与本表逐行一致后**冻结**——此后任何分区变更都是断代变更（§5.4）。

### 5.3 config_store 迁移设计

新分区内页分配刻意让**旧主/备页恰好落入新分区**，迁移可纯只读完成：

```
0xFA000  primary  (新)                    ← PM_CONFIG_STORE_ADDRESS + 0x0000
0xFB000  backup1  (新)                    ← + 0x1000
0xFC000  backup2  (新) = 旧 primary 页    ← + 0x2000   ┐ 首启迁移只读源
0xFD000  factory  (新) = 旧 backup1 页    ← + 0x3000   ┘
```

- `config_store.c` 的 4 个地址宏改为 `PM_CONFIG_STORE_ADDRESS + n*0x1000`（`#include <pm_config.h>`），删除裸地址。
- `config_store_init()` 增加一次性迁移分支：新 primary 与 backup1 均无效 **且** 0xFC000 或 0xFD000 上存在 CRC 校验通过的旧记录 → 拷入新 primary 并按新布局重写全部页。
- 旧 backup2 (0xFE000) / factory (0xFF000) 落在 settings_storage 内：**不读不写**，交由 NVS 自行回收损坏扇区。

### 5.4 部署断代警告

- **分区变更无法经 FUOTA 自举**（旧设备的 MCUboot/slot 边界已固化），存量设备必须**有线重刷一次** `merged.hex`。
- 重刷会清空 settings_storage → LoRaWAN DevNonce 归零 → 服务器按 LoRaWAN 1.0.3 反重放规则会拒绝 join。重刷排期须与 ChirpStack 侧**清除对应设备 DevNonce/激活记录**同步执行。
- pm_static.yml 合入之日即为"FUOTA 兼容基线"：此后所有版本布局一致，可互相空中升级。

---

## 6. MCUboot 策略

### 6.1 swap 算法钉死

实测当前构建为 `SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE=y`（`build/zephyr/.config`）。这是 FUOTA 的**必要条件**：Zephyr `frag_flash.c` 把镜像从 slot1 偏移 0 写入，只兼容 move/scratch/overwrite 模式；NCS 3.3 的 MCUboot Kconfig 在无 scratch 分区时默认已倾向 **swap-using-offset**（镜像须放在 slot1 偏移 +1 扇区处，FUOTA 写入的镜像将不被识别）。为防未来 NCS 升级后默认值漂移导致 FUOTA 静默失效，在 `firmware/sysbuild.conf` 显式钉死：

```
# frag_flash.c 从 slot1 偏移 0 写镜像, 仅兼容 move/scratch/overwrite;
# 显式钉死, 防止 NCS 默认切到 swap-using-offset 后 FUOTA 静默失效
SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE=y
```

### 6.2 test-swap → 健康检查 → confirm / 回滚

- 分片解码完成时 Zephyr `frag_flash_finish()` 自动调 `boot_request_upgrade(BOOT_UPGRADE_TEST)`（test 模式，非 permanent）。
- 应用重启 → MCUboot 验签通过 → swap（断电安全，上电续 swap）→ 新固件以 **test 态**启动。
- 新固件启动早期 `fuota_boot_check()`：检测到 test 态 → 健康检查 = **join 成功 + config_store 读取 OK + 主循环喂狗正常**，观察 `ALARM_FUOTA_CONFIRM_DELAY_SEC`（默认 60s）→ `boot_write_img_confirmed()`。
- 任何一步失败/崩溃 → 复位 → MCUboot revert 回旧镜像（test 语义保证）。**挂死不复位**的场景依赖看门狗兜底——实施时确认 WDT 覆盖（§14 用例 T3）。

### 6.3 签名密钥

- 现为**开发密钥** `firmware/keys/mcuboot_priv.pem`（ECDSA P-256，路径写死在 sysbuild.conf）。生产密钥的生成、保管、签名流程归属**待确认**（§15 #3）。
- 换钥 = 与存量设备 FUOTA 断代（旧 MCUboot 不认新签名），必须与 §5.4 的有线重刷窗口合并执行。

### 6.4 版本管理

- 新增 Zephyr 标准 `firmware/VERSION` 文件（`VERSION_MAJOR/MINOR/PATCHLEVEL`），sysbuild 签名版本（imgtool `--version`）自动取自该文件。
- FUOTA Descriptor 的版本字节（§8.1）经生成的 `app_version.h` 与镜像版本**同源**，杜绝"descriptor 说 1.2 实际刷进 1.1"。

---

## 7. FUOTA 服务栈配置（Kconfig + RAM 预算）

### 7.1 prj.conf 追加

```
# ── FUOTA: TS004 分片传输 (Class B 多播路线, 不需要 TS005/TS003) ──
CONFIG_LORAWAN_SERVICES=y
CONFIG_LORAWAN_FRAG_TRANSPORT=y                  # TS004, FPort 201
CONFIG_LORAWAN_FRAG_TRANSPORT_DECODER_SEMTECH=y
CONFIG_LORAWAN_FRAG_TRANSPORT_IMAGE_SIZE=262144
CONFIG_LORAWAN_FRAG_TRANSPORT_MIN_FRAG_SIZE=232
CONFIG_LORAWAN_FRAG_TRANSPORT_MAX_FRAG_SIZE=232
CONFIG_LORAWAN_FRAG_TRANSPORT_MAX_REDUNDANCY=10
CONFIG_LORAWAN_SERVICES_THREAD_STACK_SIZE=4096

# ── FUOTA: 镜像写入与 MCUboot 交互 ──
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_STREAM_FLASH=y
CONFIG_IMG_MANAGER=y            # boot_request_upgrade / boot_write_img_confirmed
CONFIG_REBOOT=y
```

**不需要** `CONFIG_LORAWAN_REMOTE_MULTICAST`（TS005，多播组是静态预置的）和 `CONFIG_LORAWAN_APP_CLOCK_SYNC`（TS003，Class B 分片无绝对时间要求）——这是路线 B 比路线 A 省 RAM/省依赖的来源之一。

### 7.2 取值理由

| 配置 | 值 | 理由 |
|---|---|---|
| `IMAGE_SIZE` | 262144 (256KB) | **不能吃默认值**：默认取 DT chosen `zephyr,code-partition`（rak4631 继承的 DTS slot0=483,328B，与 PM 实际布局脱节，且按 483KB 配解码器会多耗 ~30KB RAM）。取 240KB 预算镜像 + 16KB 余量 |
| `MIN/MAX_FRAG_SIZE` | 232 = 232 | Kconfig help 明示"分片尺寸已知时 MIN=MAX 最省 RAM"。232 为 Zephyr 默认上限 |
| `MAX_REDUNDANCY` | 10 (%) | 室内近网关丢包 <5%，10% FEC 已 2 倍覆盖；默认 20% 使 frag_cache 翻倍（+27KB RAM）不值 |
| 解码器 | SEMTECH | loramac-node 后端默认、经 LoRa Alliance 互操作验证；RAM 大头 frag_cache 两种解码器共用，lowmem 仅省 ~4KB 但解码 CPU/flash 回读大增 |
| 服务线程栈 | 4096 | Semtech `FragDecoderProcess` 栈上临时数组 + flash 操作 + 日志，默认 2048 偏紧 |

**派生约束（写入服务器参数要求）**：

- 分片总数上限 `FRAG_MAX_NB = 262144/232 + 1 = 1130` → **可传镜像上限 1130×232 = 262,160 B**。镜像超限时 `IMAGE_SIZE` 提到 327,680 (320KB)，RAM 线性增加约 7KB。
- FragSize 固定 232 → DataFragment FRMPayload = 3 + 232 = 235 B → **会话 DR 必须 ∈ {DR11, DR12, DR13}**（US915 下行最大 FRMPayload：DR8=33 / DR9=109 / DR10=222 / DR11–13=242；DR10 的 222 装不下 235）。目标 **DR13 (SF7/BW500)**。
- FragSize ≠ 232 或 NbFrag > 1130 的会话会被设备端拒绝——这是保护，不是缺陷。

### 7.3 RAM 预算（N=1130, R=113）

| 项 | 计算式 | 字节 |
|---|---|---|
| frag_cache（`frag_flash.c`，冗余恢复期缓存，两种解码器共用） | 113 × (4+232) | 26,668 |
| MatrixM2B（Semtech 解码矩阵） | ((113>>3)+1) × 113 | 1,695 |
| FragNbMissingIndex | 1130 × 2 | 2,260 |
| 解码器杂项（S 向量等） | — | ~65 |
| services 线程栈 | 4096 | 4,096 |
| 服务 ctx/work 结构 | — | ~300 |
| **合计增量** | | **≈ 35 KB** |

当前剩余 RAM ≈ 195KB → FUOTA 占用 18%，升级功能启用后仍余 ~160KB。参考：REDUNDANCY=15% → ≈50KB；20% → ≈62KB（可行但不推荐）。

---

## 8. 镜像型号隔离（防 Hub 收到 Badge 镜像）

Badge/Hub 同硬件同签名密钥——**Hub 若收完 Badge 镜像，MCUboot 验签会通过并成功刷入错误固件**。必须在会话建立阶段拦截。

### 8.1 Descriptor 方案（主）

TS004 `FragSessionSetupReq` 自带 4 字节应用自定义 Descriptor；Zephyr 已原生暴露校验回调（`frag_transport.c:212` 附近，`lorawan_frag_transport_register_descriptor_callback()`，回调返回 <0 → Ans status 置 BIT(3) "Wrong Descriptor"，会话被拒**且不擦 slot1**）——无需 patch SDK。

| 字节 | 含义 | 取值 |
|---|---|---|
| byte0 | model | 0x01=Badge / 0x02=Hub（= `CONFIG_ALARM_DEVICE_TYPE + 1`，避开 0x00） |
| byte1 | hw_rev | 0x01（当前板版本） |
| byte2 | 目标固件 major | 取自 `app_version.h`（§6.4 同源） |
| byte3 | 目标固件 minor | 同上 |

设备校验规则：model 与 hw_rev **不匹配即拒绝**；版本字节仅记录日志（防降级列为未来增强）。sender 脚本由构建产物自动填 Descriptor（§13.1），人工不可绕过。

### 8.2 兜底隔离（SOP）

- Badge 与 Hub **分两场次**升级，同一时刻多播队列里只有一种镜像的分片。
- 传输组选覆盖全设备的组（CODE RED 或 GREEN，见 §13.3）；非目标型号靠 Descriptor 拒绝会话后，后续分片自然被其 frag 服务忽略。
- `ALARM_FUOTA_ALLOW_NULL_DESCRIPTOR`（默认 n）：仅当未来改用第三方下发工具且无法自定义 Descriptor 时的过渡开关；自研脚本路线下**永远保持 n**。

---

## 9. 升级完整时序

```
sender.py            ChirpStack           设备 (Class B 运行中, 心跳周期打开 RX 窗)
   │ ①逐设备单播 FragSessionSetupReq (FPort201:
   │   FragIndex=0 | McGroupBitMask=传输组 | NbFrag | FragSize=232
   │   | Padding | Descriptor[model,hw_rev,fw_maj,fw_min])
   ├──────────────────▶├────────────────────▶│ Descriptor 校验:
   │                   │                     │  ├─ model/hw_rev 不匹配
   │                   │                     │  │   → Ans status|BIT(3) 拒绝 ▣终(不擦盘)
   │                   │                     │  └─ 匹配 → 整区擦除 slot1
   │◀───────────────── │◀── SetupAns ────────┤     (119 页 ≈10s, 见风险 R2)
   │ ②核对全部目标设备 SetupAns 无错误位; 等待 ≥30s
   │ ③1,166 片 DataFragment 依次入多播组下行队列 (节流 4s/片)
   ├──────────────────▶├── ping slot 多播 ──▶│ 逐片写 slot1;
   │                   │   (923.3MHz DR13,   │ 第 >N 片起冗余片入 RAM cache
   │                   │    4s 一片, ~78min) │ FEC 解码完成 → frag_flash_finish():
   │                   │                     │   补写缓存片
   │                   │                     │   + boot_request_upgrade(TEST)
   │                   │                     │ finished_cb → 随机延时 5~60s → reboot
   │                   │                     │   (随机化避免 OTAA join 风暴)
   │                   │                     ├─▶ MCUboot: ECDSA 验签
   │                   │                     │    ├─ 失败 → 引导旧镜像 ▣终
   │                   │                     │    └─ 通过 → swap-using-move
   │                   │                     │        (~20-40s, 断电安全)
   │                   │                     │ 新固件 test 态启动:
   │                   │                     │   健康检查 = join OK + config OK
   │                   │                     │     + 喂狗正常, 观察 60s
   │◀── 心跳(新版本号) ─┤◀────────────────────┤   ├─ 通过 → boot_write_img_confirmed ✔
   │ ⑤按心跳版本号统计升级结果                 │   └─ 崩溃/复位 → MCUboot revert
   │ ④(可选)对未完成设备单播 FragSessionStatusReq │      → 旧固件回归, 心跳报旧版本 ✔安全
   │    → 缺片数 > 0 → 追发冗余片 (上限 R=113)
```

**失败/断电分支**

| 场景 | 行为 | 结果 |
|---|---|---|
| 传输中设备断电/复位 | 解码状态在 RAM，会话作废；重启后回归正常 Class B 运行 | 无损；下一轮会话重跑 |
| 丢片 ≤10% | FEC 冗余片补齐，无需重传 | 正常完成 |
| 丢片 >10% | 解码无法完成；`ALARM_FUOTA_SESSION_TIMEOUT_MIN`（默认 120min）超时清理并上报 | 服务器追发冗余片或重跑 |
| swap 中断电 | swap-using-move 断电安全 | 上电续 swap |
| confirm 前断电/崩溃 | MCUboot revert | 回旧镜像，0 变砖 |
| 验签失败（坏镜像/错签名） | MCUboot 直接引导旧镜像 | 0 变砖 |

---

## 10. 传输时长与耗电估算

按 240KB (245,760 B) 预算镜像、FragSize 232、冗余 10%：

- 数据分片 N = ceil(245,760/232) = **1,060**；冗余 106；**共 1,166 片**
- 单片空口时长（DR13 = SF7/BW500，PHY ~248B）≈ 97 ms

| 项 | 数值 | 说明 |
|---|---|---|
| 纯传输时长 | 1,166 × 4s ≈ **78 min** | 1 片/ping slot（组 ping 周期 4s） |
| 含建立/抽查/追发 | **≈ 1.5 h / 变体** | Badge、Hub 分两场次 |
| 当前 220KB 镜像 | 948+95=1,043 片 ≈ **70 min** | 参考值 |
| 设备增量耗电 | 1,166 × 97ms ≈ 113s RX ≈ **0.3 mAh** | ping slot 本就常开，只多了实收时间；对 Badge（数周续航）与 Hub（19Ah）均可忽略 |
| 反例：DR8 会话 | 不可行 | FragSize≤50 → 4,916 片，超 FRAG_MAX_NB=1130 被设备拒绝；即使能收也需 ~5.5h |

Class B / beacon 全程保持，告警单播与其余多播组不受影响（传输组带宽被占用，见 §13.3 SOP）。

---

## 11. 前置修复项与实施顺序

FUOTA 功能落地前必须先清掉 §2.4 的缺陷，推荐顺序：

| 步骤 | 内容 | 验证方式 |
|---|---|---|
| **P0-0** | **Class B 激活**（缺陷 D0）：应用层直调 loramac-node 实现 bring-up，独立方案见 `classb_design.md`；附带硬依赖：网关 GPS 授时排障 | "Beacon locked" / "Class B active" 日志；服务器侧确认心跳上行 Class B 位 |
| **P0-1** | `lorawan_mc.c` `MC_DATARATE` 3→13（已完成）；ChirpStack 侧 4 组 DR 同步改 DR13；`muticast_flow.md` 勘误 | DR3 报错已由日志实证；修复后日志确认 4 组 "setup OK"；多播下行实收测试（依赖 P0-0） |
| **P0-2** | `pm_static.yml` 合入 + `config_store.c` 迁移改造 + 存量设备有线重刷（含 ChirpStack 清 DevNonce） | 重刷后配置保留（迁移生效）、join 正常、`partitions.yml` 与 §5.1 一致 |
| **P1-3** | 下行回调过滤 FPort 0/200–202（`hal_sx1262.c` 分发处） | 单元测试：201 报文不进 proto_handler |
| **P1-4** | FUOTA 栈：prj.conf/sysbuild.conf（§6.1/§7.1）+ `src/app/fuota.c`（confirm 逻辑 + frag_transport_run + Descriptor 回调 + 会话超时） | §14 台架用例 |
| **P2-5** | `tools/fuota_class_b_sender.py` + 服务器联调 | 单设备端到端升级 |
| **P2-6** | 多设备演练 + 验收 | §14 整机用例 |

P0-1 同时是**当前告警多播功能的 bug 修复**，独立于 FUOTA 也应优先执行。

---

## 12. 代码改动清单（供后续实施，本文档不实施）

**新增**

| 文件 | 内容 |
|---|---|
| `firmware/pm_static.yml` | §5.2 草案（Badge/Hub 共用） |
| `firmware/src/app/fuota.c` / `fuota.h` | ① `fuota_boot_check()`：开机早期判 test 态 → 健康检查 → `boot_write_img_confirmed()`；② `fuota_init()`：join 后 `lorawan_frag_transport_run(finished_cb)` + Descriptor 校验回调；③ `finished_cb`：随机延时 reboot；④ 会话超时清理与上报 |
| `firmware/VERSION` | Zephyr 版本文件（imgtool 签名版本与 Descriptor 同源） |
| `tools/fuota_class_b_sender.py` | TS004 FEC 编码器（**须与 Semtech FragDecoder 位级一致**，参考 chirpstack-fuota-server 的编码实现）+ 单播 Setup + 多播队列节流 + Status 抽查 |

**修改**

| 文件 | 改动 |
|---|---|
| `firmware/src/hal/lorawan_mc.c` | `MC_DATARATE` 3→13（P0-1 前置修复） |
| `firmware/src/config/config_store.c` | 4 个地址宏改 `PM_CONFIG_STORE_ADDRESS + n*0x1000`；`config_store_init()` 一次性迁移分支（§5.3） |
| `firmware/src/hal/hal_sx1262.c`（下行分发处，:78/:118 附近） | 过滤 FPort 0/200–202，不转发给业务解析器 |
| `firmware/src/main.c` | 挂接 `fuota_boot_check()` / `fuota_init()` |
| `firmware/prj.conf` | §7.1 清单 |
| `firmware/sysbuild.conf` | §6.1 钉死 swap-using-move |
| `firmware/Kconfig` | `ALARM_FUOTA`(default y) / `ALARM_FUOTA_CONFIRM_DELAY_SEC`(60) / `ALARM_FUOTA_SESSION_TIMEOUT_MIN`(120) / `ALARM_FUOTA_ALLOW_NULL_DESCRIPTOR`(n) |
| `doc/design/muticast_flow.md` 等 | DR 勘误、分区描述更新（§3） |

---

## 13. 服务器侧操作

### 13.1 sender 脚本流程（`tools/fuota_class_b_sender.py`）

1. 输入：`dfu_application.zip` 内的 `firmware.signed.bin` + 目标型号（badge/hub）+ 目标设备 DevEUI 清单 + 传输组 ID。
2. 镜像按 232B 分片（末片补 padding，padding 数写入 SetupReq）；按 TS004 算法生成 10% 冗余片。
3. Descriptor 自动生成：model/hw_rev + 从镜像 MCUboot header 读出的版本号。
4. 逐设备**单播** `FragSessionSetupReq`（设备心跳周期 5min，全部设备收到 Setup 需 ≥1 心跳周期；也可等 ping slot 下发）；收齐 `FragSessionSetupAns` 并核对无错误位。
5. 等待 ≥30s（避开设备擦 slot1 的 ~10s 阻塞窗口），随后将全部 DataFragment 入多播组下行队列，**节流 4s/片**（与组 ping 周期一致，控制队列深度 ≤8）。
6. 传输尾声：对抽样/未上报设备单播 `FragSessionStatusReq` 查缺片数，追发冗余片（总冗余上限 113 片）。
7. 以心跳版本号为准统计结果；对失败设备重跑会话（可改单播分片，小批量补刷）。

### 13.2 服务器依赖与待办

- 多播下行队列 API：`POST /api/multicast-groups/{id}/queue`（`integration_guide.md` §2 已列）——需 SimulAlert 开放 API 凭证与权限（§15 #1）。
- ChirpStack 侧 4 个多播组 DR 改为 **DR13**（与 P0-1 同步）。
- 单播下行队列 API（Setup/Status 用）。

### 13.3 升级窗口 SOP

- 选**低峰维护窗口**（如夜间），每变体预留 2h。
- 传输组用覆盖全设备的 CODE RED 或 GREEN；窗口内若发生真实紧急告警，**改用另一个全覆盖组下发告警**（两组互为备份），并可清空传输组队列中止升级——中止对设备无损（§9 失败分支）。
- Badge 场次可选约束在"多数设备在充电/在校时段"以提高在网率。

---

## 14. 测试与验收

### 14.1 台架单元用例

| # | 用例 | 预期 |
|---|---|---|
| T1 | 向 Badge 发 model=0x02 (Hub) 的 SetupReq | Ans 置 BIT(3) 拒绝，slot1 未被擦除 |
| T2 | 旧布局设备刷新固件（含旧 config 数据于 0xFC000/0xFD000） | 迁移分支生效，config 完整保留 |
| T3 | 刷入"启动即挂死"的坏镜像走完整 FUOTA | WDT 复位 → revert 回旧镜像；心跳报旧版本 |
| T4 | FragSize=200 或 NbFrag>1130 的会话 | 设备拒绝（参数保护生效） |
| T5 | 正常镜像全流程 | confirm 成功，心跳报新版本 |

### 14.2 断电注入（3 时点）

| 时点 | 注入 | 预期 |
|---|---|---|
| 分片传输中 | 拔电 | 重启回正常运行，会话作废可重跑 |
| MCUboot swap 中 | 拔电 | 上电续 swap，最终正常启动 |
| test 态 confirm 前 | 拔电 | revert 回旧镜像 |

### 14.3 整机验收

- ≥3 台 Badge + ≥1 台 Hub 经真实网关/LNS 全流程升级，测量实际时长与丢片率，对照 §10 估算。
- 升级窗口内触发一次告警走备用全覆盖组，验证告警可达。
- 验收指标见 §1.1。

---

## 15. 风险与待确认事项

**风险**

| # | 风险 | 缓解 |
|---|---|---|
| R1 | DR3 修复后多播链路仍不通（网关/LNS 侧 Class B 排程问题） | P0-1 单独先行验证多播实收，再启动 FUOTA 开发 |
| R2 | 擦 slot1（~10s，每页 ~85ms CPU 停顿）干扰 Class B beacon/ping 时序 | Setup 与首片间隔 ≥30s；实测 beacon 保持；必要时分页擦除+让出 |
| R3 | sender 的 FEC 编码与 Semtech 解码器不位级一致 → 永远解不出 | 直接移植 chirpstack-fuota-server 编码实现 + 台架回环验证 |
| R4 | 镜像增长超 262,160B 上限 | 心跳/CI 监控镜像大小；超限时按 §7.2 上调 IMAGE_SIZE（RAM +7KB/64KB） |
| R5 | 多设备同时 reboot → OTAA join 风暴 | finished_cb 随机延时 5~60s 再重启 |
| R6 | Zephyr frag_transport 对"经多播收到的 Setup"是否应答未验证 | 本方案 Setup 走单播规避；实施时实测组播 Setup 行为再决定是否优化 |

**待确认（外部依赖）**

1. SimulAlert 是否开放多播/单播下行队列 API 凭证与权限。
2. ChirpStack 侧 4 组 DR 改 DR13 的操作窗口（与 P0-1 同步）。
3. 生产签名密钥由谁生成/保管（现开发密钥；换钥须并入有线重刷窗口，§6.3）。
4. 存量设备有线重刷排期 + ChirpStack 清 DevNonce 配合（§5.4）。
5. 维护窗口策略确认（每变体 ~2h；Badge 是否限定充电/在校时段）。
6. 网关 GPS 授时排障（RAK7289 basicstation 发 Class B beacon 的前提，当前日志显示 GPS 无定位——见 `classb_design.md` §6）。

---

## 附录 A：flash_map_pm.h 映射验证记录

结论：**Partition Manager 环境下 Zephyr `frag_flash.c` 无需修改即写入 mcuboot_secondary。** 证据链（NCS 3.3.0 本地树）：

1. `zephyr/include/zephyr/storage/flash_map.h:351`：`#if USE_PARTITION_MANAGER` → `#include <flash_map_pm.h>`（PM 启用时整体接管 flash_map 宏）。
2. `nrf/include/flash_map_pm.h:18`：`#define slot1_partition mcuboot_secondary`；`:52`：`#define FIXED_PARTITION_ID(label) PM_ID(label)`——实参先展开为 `mcuboot_secondary` 再粘接为 `PM_mcuboot_secondary_ID`。
3. 本项目实测 `build/pm.config`：`PM_mcuboot_secondary_ID=5`、地址 0x85000（新布局下将为 0x83000）。
4. 因此 `frag_flash.c:18` `TARGET_IMAGE_AREA = FIXED_PARTITION_ID(slot1_partition)` → flash_area ID 5 → mcuboot_secondary；`frag_flash_finish()` 内 `boot_request_upgrade()` 同样经 PM 定位 secondary 尾部 trailer。

## 附录 B：参数速查卡

```
分区:  mcuboot 0x0+48K | slot0 0xC000+476K | slot1 0x83000+476K
       | config_store 0xFA000+16K | settings 0xFE000+8K
镜像上限: 有线 481KB / FUOTA 262,160B (IMAGE_SIZE=256K, frag=232, N≤1130)
射频:  传输组 923.3MHz, DR13 (SF7/BW500), ping 周期 4s, 1 片/slot
端口:  201=TS004 (Setup 单播 / DataFragment 多播 / Status 单播)
Descriptor: [model 01=Badge 02=Hub | hw_rev 01 | fw_major | fw_minor]
FEC:   冗余 10% (≤113 片), 丢片≤10% 免重传
时长:  240KB ≈ 78min 纯传输, ≈1.5h/变体含建立与抽查
耗电:  ≈0.3mAh/次 (ping slot 本就常开)
RAM:   FUOTA 栈增量 ≈35KB (剩余 195KB 的 18%)
confirm: test-swap → join+config+喂狗 60s → boot_write_img_confirmed()
```
