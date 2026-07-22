# LoRa 报警系统方案设计
## 系统框图

```
@startuml LoRaWAN架构流程

title LORA WAN 通信架构

' 定义模块
rectangle "Badge\nCLASS B" as Badge
rectangle "Hub\nCLASS B" as Hub
rectangle "Gateway" as Gateway

' 数据流
Badge --> Gateway : 上行数据
Hub --> Gateway : 上行数据
Gateway --> Badge : 下行数据（单播/多播）
Gateway --> Hub : 下行数据（单播/多播）

' 顶部说明注释
note top of Gateway
Gateway support Unicast or Multicast during downlink.
Badge and hub should be in the same group in one project.
end note

@enduml
```
```
@startuml LoRa报警系统蓝牙定位流程

' 标题说明
title BLE Location & Alert Workflow\n(LoRa 报警系统蓝牙定位流程)

' 定义参与者
rectangle "RAK4630 模块\nTx: +4dBm\nRx: -95dBm\n有效距离: 20~40米" as Rak
rectangle "Hub\n(集线器)" as Hub
rectangle "Badge\n(胸牌终端)" as Badge
rectangle "BLE Adv [MAC]\n(蓝牙广播)" as BleAdv
rectangle "报警处理逻辑" as AlertLogic
rectangle "Gateway\n(网关)" as Gateway

' 流程连接
Rak <--> Hub : 基于蓝牙通信
Rak <--> Badge : 基于蓝牙通信

Hub --> BleAdv : 周期广播(每2秒一次)

AlertLogic --> Badge : 触发报警时
Badge --> AlertLogic : 扫描4秒蓝牙信号\n获取RSSI最高的Hub MAC地址
AlertLogic --> Gateway : 携带报警信息 + Hub MAC地址发送

' 顶部注释说明
note top of Hub
户外场景说明：
集线器需安装在室外
end note

@enduml
```
```
@startuml RAK7289CV2 网络连接架构

title About network

' 定义模块
rectangle "RAK7289CV2\n(网关设备)" as Gateway
rectangle "网络接入方式\nWi-Fi, Ethernet, LTE, LoRaWAN" as NetworkIf
rectangle "网络服务器配置说明" as DocNote

' 连接关系
NetworkIf --> Gateway : 数据接入

Gateway --> DocNote : 配置参考文档

' 文档链接说明
note bottom of DocNote
https://docs.rakwireless.com/
product-categories/wisgate/
rak7289v2/lorawan-configuration/
end note

@enduml
```
```
@startuml LNS架构 (ChirpStack V4)

title About LNS

' 定义模块
rectangle "Gateway\nRAK7289CV2" as Gateway
rectangle "网络接入层\nWi-Fi / Ethernet / LTE" as Network
box "AWS Server" #lightblue
    rectangle "ChirpStack V4" as ChirpStack {
        rectangle "chirpstack-gateway-bridge" as Bridge
    }
    rectangle "User Server" as UserServer
end box

' 连接关系
Gateway --> Network : 上行数据
Network <--> Bridge : Basic Station\nWSS/IP:port
ChirpStack <--> UserServer : 业务数据交互

@enduml
```

## LoRaWAN 说明
### 胸牌与集线器（RAK4630）
|参数名称|参数值|
|----|----|
|适用频段|IN865、EU868、AU915、US915、KR920、RU864、AS923|
|LoRaWAN 设备等级|CLASS B（CLASS A 不支持服务器随机下行）|
|扩频因子|ADR（自动调节）|
|LoRaWAN 激活方式|OTAA|
|LoRaWAN 版本|LoRaWAN 1.0.3|
|占空比|按对应地区规范执行|

### 网关（RAK7289CV2）
|参数名称|参数值|
|----|----|
|LoRaWAN 版本|LoRaWAN 1.0.3|
|支持的 LNS|Basic Station 对接 ChirpStack V4|

网关配置详情请参考以下网址：
https://learn.rakwireless.com/hc/en-us/articles/35978760611607-how-to-connect-rak-gateway-to-chirpstack-v4-using-basics-station-lns?utm_source=docs_center&utm_medium=content&utm_campaign=crossdomain&utm_content=rak7289v2_rak7289cv2_lorawan_configuration&utm_term=how_to_connect_rak_gateways_to_chirpstack_v4_using_basics_station_lns&_gl=1*198l0bk*_gcl_au*OTI2NDUyNTcwLjE3NzczODI1MDA.

## 硬件框图
badge RAK4630

![alt text](image.png)

hub RAK4630

![alt text](image-1.png)
## 通信流程图

@startuml 跨角色通信时序流程

title 跨角色通信时序流程

' 定义泳道角色
|House|
|User App|
|Hub|
|Gateway|
|SDK|
|APP Server|

' 时序步骤
|House| : Event Trigger (Alarm)
|House| -> |User App| : Forward alarm message
|User App| : Verify the alarm
|User App| : Trigger alarm confirmation
|User App| : Broadcast downlink command (LoRaWAN Class B)
|User App| : Convert downlink command to LoRaWAN format
|User App| -> |Gateway| : Send command to Gateway
|Gateway| -> |SDK| : Forward command to SDK
|SDK| : Downlink message parsing
|SDK| : LoRaWAN message processing
|SDK| -> |APP Server| : Send to APP Server

' 第二阶段流程
|Hub| : Periodic BLE broadcast (with MAC)
|Hub| -> |Gateway| : Send BLE data (Hub MAC + RSSI)
|Gateway| -> |SDK| : Forward BLE data
|SDK| : Location parsing (RSSI filtering)
|SDK| -> |APP Server| : Send location info to APP Server

' 第三阶段流程
|User App| : Periodic LoRaWAN status query
|User App| -> |Gateway| : Send query command
|Gateway| -> |Hub/Badge| : Broadcast query (Class B downlink)
|Hub/Badge| : Respond with status data (LoRaWAN uplink)
|Hub/Badge| -> |Gateway| : Send status data to Gateway
|Gateway| -> |SDK| : Forward status data to SDK

' 第四阶段流程
|User App| : Periodic BLE beacon scanning
|Hub| : BLE beacon broadcast
|Hub| -> |Gateway| : Send beacon data to Gateway
|Gateway| -> |SDK| : Forward beacon data to SDK

@enduml

## 通信协议（嵌入式端）
本方案仅支持以下嵌入式端协议。

说明：
网关为透传设备。胸牌或集线器发送十六进制（HEX）数据，网关将数据转发至 LNS 服务器。
LNS 服务器必须使用 ChirpStack V4。
ChirpStack V4 与应用服务器之间采用 JSON 格式通信。

## 通信协议（网络服务器端）
待补充

## 参考文档