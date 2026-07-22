# RUI3 并发模型分析：事件竞争与一致性保证

> 分析 RUI3 单线程协作式模型下，LoRaWAN 下行事件与按键事件的安全性和延迟。

---

## 1. 执行模型

```
while(1) {
    rui_running();    // ① 系统事件：Radio IRQ、LoRaMacProcess、定时器回调
    rui_loop();       // ② 用户代码：RT_SCHEDULE(thread1) → RT_SCHEDULE(thread2) → ...
}
```

- `rui_running()` **先于** `rui_loop()` 执行
- `rui_loop()` 内 Protothreads 按 `RT_SCHEDULE` 顺序依次执行，`RT_YIELD`/`RT_SLEEP` 让出 CPU
- Protothreads **没有优先级**，是纯粹的协作式协程
- DIO1 ISR **不处理下行数据**，只投递 `UDRV_SYS_EVT_OP_LORAWAN` 事件到队列（<5µs）

---

## 2. 下行事件响应链路

```
Radio DIO1 引脚中断
  │
  ▼
GPIOTE ISR → 投递 UDRV_SYS_EVT_OP_LORAWAN
  │
  ▼
rui_running() 消费事件（每次 loop() 最先执行）
  │
  ├── Radio.IrqProcess()          // 读 SX1262 FIFO
  ├── LoRaMacProcess()            // MAC 解密、MIC 校验
  │     └── recvCallback(data)    // 同步调用（非 ISR，非 Protothread）
  │           ├── proto_parser_feed()
  │           ├── proto_handle_frame()
  │           └── alarm_sm_set()  // 直接修改 active_alarms[]
  │
  ▼
rui_loop() → RT_SCHEDULE(actuatorThread) → actuator_mgr_sync()
  └── LED / 蜂鸣器 / 振动 立即响应
```

**关键点**：`recvCallback` 在 `rui_running()` 中同步执行，在所有 Protothreads 之前运行，不需要等协程调度。

### 延迟估算

| 阶段 | 耗时 |
|------|------|
| Radio 帧接收 | ~100µs（硬件） |
| DIO1 ISR → 事件投递 | ~5µs |
| `rui_running()` 处理 + MAC 解密 | ~200µs |
| `recvCallback`（proto 解析 + alarm_sm） | ~50µs（纯 C 逻辑） |
| `rui_loop()` actuatorThread 消费 flag | ≤10ms（最坏：前一个协程 RT_SLEEP 中） |
| GPIO/analogWrite 翻转 | ~5µs |
| **总计** | **<11ms** |

---

## 3. 按键 + 下行同时到达

### 3.1 时序分析（最坏情况）

```
┌─ rui_running() ─┐     ┌─ rui_loop() ────────────────────┐
│ 无待处理事件      │     │ buttonThread:                   │
│                   │     │   alarm_sm_set(BLUE)            │
└───────────────────┘     │   sort_by_priority()            │
                           │   [0] = BLUE (P5)              │
                           │                                │
        ⚡ DIO1 ISR ──────→│   只投递事件 ← 不处理！         │
                           │                                │
                           │   继续执行 actuatorThread...    │
                           └────────────────────────────────┘
┌─ 下一轮 rui_running() ─┐
│ 消费 Radio 事件:        │
│   recvCallback →        │
│   alarm_sm_set(YELLOW)  │
│   sort → [BLUE, YELLOW] │
│   → 显示 BLUE ✅         │
└─────────────────────────┘
```

### 3.2 为什么安全

1. **DIO1 ISR 只投递事件，不碰 `active_alarms[]`**
2. **实际处理在下一轮 `rui_running()`** — `rui_loop()` 当前迭代安全完成，BLUE 已写入
3. **单线程顺序执行** — 不存在两个执行上下文同时写 `active_alarms[]`
4. **Yellow 延迟仅一帧** — 最多等 `rui_loop()` 跑完当前迭代（≤10ms）

### 3.3 同类型合并

```c
// alarm_sm.c: 同类型不重复，合并来源
int existing = find_alarm(alarm_type);
if (existing >= 0) {
    active_alarms[existing].source |= (1 << source);
    return 0;
}
```

| 场景 | 结果 | 原因 |
|------|------|------|
| 按键 Red + 收到自己的 Red 回传 | 1 条告警，source=`BADGE_BTN \| LORAWAN` | `find_alarm` 已存在 → 合并 |
| 按键 Blue + 下行 Yellow | **显示 Blue** (P5 < P6) | `sort_by_priority` 保证 `[0]` 最高优先级 |
| 按键 Red + All Clear | Red 被清除 | All Clear 只清 P0 |

---

## 4. 不同类型告警并发

### 4.1 优先级抢占表

| 代号 | 优先级 |
|------|--------|
| Code Red | P0（最高） |
| Shelter | P1 |
| Evacuate | P2 |
| Secure | P3 |
| Hold | P4 |
| Code Blue | P5 |
| Code Yellow | P6 |
| All Clear | P7 |
| Normal | P8（最低） |

### 4.2 无论到达顺序如何，`sort_by_priority()` 保证最高优先级显示

```c
static void sort_by_priority(void) {
    for (int i = 0; i < active_count - 1; i++) {
        for (int j = i + 1; j < active_count; j++) {
            if (active_alarms[i].priority > active_alarms[j].priority) {
                struct active_alarm tmp = active_alarms[i];
                active_alarms[i] = active_alarms[j];
                active_alarms[j] = tmp;
            }
        }
    }
}
```

每次 `alarm_sm_set()` 后立即排序，`active_alarms[0]` 始终是最高优先级。

---

## 5. NCS vs RUI3 对比

| 维度 | NCS（抢占式线程） | RUI3（协作式） |
|------|-----------------|-------------|
| 下行→回调延迟 | DIO1 ISR → 信号量唤醒 lora_thread → 调度 | `rui_running()` 同步调用，**零调度延迟** |
| 回调→执行器 | lora_thread → actuator_thread 消费 | `recvCallback` 设 flag → 同 loop 中消费 |
| 状态竞争 | lora_thread + ui_thread 可能同时写（代码未加锁） | **不可能竞争**，单线程顺序执行 |
| 最坏延迟 | 线程调度抢占时机 | 当前 RT_SLEEP 剩余（≤10ms） |

---

## 6. 结论

- **亚秒级 Code Red 响应**：<11ms 端到端延迟，完全满足要求
- **并发安全**：RUI3 单线程模型比 NCS 多线程更安全 — `alarm_sm_set()` 天然原子，不需要锁
- **数据一致性**：每次 `sort_by_priority()` 后状态确定，中断仅投递事件不修改状态
