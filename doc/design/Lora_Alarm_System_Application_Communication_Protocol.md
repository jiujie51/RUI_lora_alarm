# Lora Alarm System Application Communication Protocol
V1.4

---

## 版本历史
| Ver   | Date      | Revision Author | modify content                                   |
|-------|-----------|-----------------|--------------------------------------------------|
| 1.0   | 2026-5-6  | Mr G            | First version                                    |
| 1.1   | 2026-5-11 | Mr G            | Add SRP related support                          |
| 1.2   |           |                 | Incorporate the feedback received                |
| 1.3   | 2026-5-19 | Mr G            | Modify CMD 0x02, ADD latitude and longitude      |
| 1.4   | 2026-5-21 | Mr G            | Modify the Code and group id related protocol    |

---

### 标注说明
- 蓝色字体：新增
- 紫色字体：修改
- 灰色：未实现

---

## 目录
1. communication interface
   1.1 Upstream and downstream port descriptions
   1.2 Other Features
2. Instruction Details
   2.1 instruction format
   2.3 Instruction Details
3. CRC16

---

# 1 communication interface
## 1.1 Upstream and downstream port descriptions
This protocol is used for badge, hub and LNS.

## 1.2 Other Features

---

# 2 Instruction Details
## 2.1 instruction format
| head   | ver  | control | CMDID | length | CRC16 | data  |
|--------|------|---------|-------|--------|-------|-------|
| 2      | 1    | 1       | 1     | 2      | 2     | N     |
| 0xAA55 | X    | X       | X     | XX     | XX    | X…    |

### head
Length: 2 bytes, fixed to **0xAA55**

### ver
Length: 1 byte, default is 1

### CMDID
| CMDID | CMD NAME        | Instruction Description                                                                 |
|-------|-----------------|-----------------------------------------------------------------------------------------|
| 0x00  | heartbeat       | The gateway and hub report a heartbeat every 5 minutes.                                |
| 0x01  | power           | The badge and hub report any change in battery level when the interval exceeds 5 minutes. |
| 0x02  | Key Event       | After the button is triggered, report the button event.                                |
| 0x03  | Code            | Set operation codes (red code, blue code, yellow code, green code, Hold Code, Secure Code, Evacuate Code, Shelter Code) |
| 0x04  | Code Setting    | Set the display content for the code.                                                   |
| 0x05  | Led control      | Control the led                                                                         |
| 0x06  | Buzzer control   | Control the buzzer                                                                      |
| 0x07  | Vibration control | Control the vibration                                                                 |
| 0x08  | LCD context control | LINE1 & LINE2 Content Control                                                        |
| 0x09  | LCD LINE2 onoff  | Decision whether to display the second line of text                                    |
| 0x0A  | Clear Packet     | Clear all or all clear the status                                                       |
| 0x50  | Set group id     | Set device group id                                                                     |

### control
Length: 1 byte

| Bit7 | Bit6 | Bit5 | Bit4 | Bit3   | Bit4~0        |
|------|------|------|------|--------|---------------|
| RESERVE | RESERVE | RESERVE | RESERVE | RESEND | PACKET_TYPE |

- PACKET_TYPE：0 -- Request packet; others reserved
- RESEND：0
- RESERVE：0

### length
Includes all header information and the length of data fields

### CRC16
The CRC16 check values for both the header (excluding the CRC16 field) and data fields are calculated using the **CRC16/XMODEM** verification algorithm.

### Data Field
See the specific format of each instruction code.

---

## 2.3 Instruction Details
CMDID 0x01~0x02 for uplink  
Other CMDID for downlink

### 2.3.1 command list (CLIST)
#### 2.3.1.1 heartbeat（0x00）
Direction: badge/hub -> Gateway  
Instruction: The device reports a heartbeat packet every 5 minutes.

| field       | length（Byte） | data    | data specification                                                                 |
|-------------|----------------|---------|-------------------------------------------------------------------------------------|
| device type | 1              | 0 or 1  | 0 for badge<br/>1 for hub                                                           |
| Group id    | 1              | 0~255   | Default is 0, 0 means no role. It is set by CMDID 0x50.<br/>Bit0: admin<br/>Bit1：Nurses<br/>Bit2：secure<br/>Bit3：principal<br/>Bit4：......xxx<br/>....<br/>Bit7: (reserve) |

**Example**  
AA55 01 00 01 0A00 xx xx 01

---

#### 2.3.1.2 power（0x01）
Direction: badge/hub -> Gateway  
Instruction: Report a change in battery level once, with an interval of no less than 5 minutes.

| field       | length（Byte） | data    | data specification                         |
|-------------|----------------|---------|---------------------------------------------|
| device type | 1              | 0 or 1  | 0 for badge<br/>1 for hub                   |
| power       | 1              | 0~100   | Battery percent                             |

---

#### 2.3.1.3 Key Event（0x02）
Direction: badge/hub -> Gateway  
Instruction: Report when the button is triggered

| field     | length（Byte） | data                              | data specification                                                                 |
|-----------|----------------|-----------------------------------|-------------------------------------------------------------------------------------|
| Button    | 1              | 0~3                               | 0 green, 1 blue, 2 yellow, 3 red (key)                                             |
| motion    | 1              | 0 or 1                            | 0 short press<br/>1 long press                                                      |
| rssi      | 1              | 0~255                             | Indicates Bluetooth signal strength. -90 dBm converts to 90                        |
| Hub mac   | 6              | XX:XX:XX:XX:XX:XX                 | Hub mac addr                                                                       |
| latitude  | 4              | Bit31:0 N,1S; Bit0~30:0~90000001  | 90000000 means 90.000000<br/>90000001 means invalid                                 |
| longitude | 4              | Bit31:0 E,1W; Bit0~30:0~180000001 | 180000000 means 180.000000<br/>180000001 means invalid                             |

---

#### 2.3.1.4 Code（0x03）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field     | length（Byte） | data    | data specification                                                                 |
|-----------|----------------|---------|-------------------------------------------------------------------------------------|
| Group id  | 1              | 0~255   | Target role.<br/>Bit0: admin<br/>Bit1：Nurses<br/>Bit2：secure<br/>Bit3：principal<br/>...<br/>Bit7: all role |
| Alarm     | 1              | 1~7     | 0 green,1 blue,2 yellow,3 red,4~7 Hold/Secure/Evacuate/Shelter alerts              |

---

#### 2.3.1.5 Code Setting（0x04）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field               | length（Byte） | data          | data specification                                                                 |
|---------------------|----------------|---------------|-------------------------------------------------------------------------------------|
| Group id            | 1              | 0~255         | Target role                                                                         |
| Alarm               | 1              | 0~7           | Alarm type                                                                         |
| Enable switch       | 1              | 0x00~0xFF     | Bit0: lcd line2 enable<br/>Bit1: buzzer enable<br/>Bit2: vibration enable          |
| Mode Switch         | 1              | 0x00~0xFF     | Bit0: buzzer mode(0 normal,1 twinkle)<br/>Bit1: vibration mode                     |
| Volume              | 1              | 0~10          | 0 min,10 max volume                                                                 |
| Buzzer Open duration | 4             | 0~2147483647  | Unit ms                                                                             |
| Buzzer Close duration | 4            | 0~2147483647  | Unit ms                                                                             |
| Vibration Open duration | 4          | 0~2147483647  | Unit ms                                                                             |
| Vibration Close duration | 4         | 0~2147483647  | Unit ms                                                                             |
| LED_R               | 1              | 0~255         |                                                                                     |
| LED_G               | 1              | 0~255         |                                                                                     |
| LED_B               | 1              | 0~255         |                                                                                     |

---

#### 2.3.1.6 Led control（0x05）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field               | length（Byte） | data          | data specification                                                                 |
|---------------------|----------------|---------------|-------------------------------------------------------------------------------------|
| Group id            | 1              | 0~255         | Target role                                                                         |
| Color               | 1              | 0~3           | Alarm color                                                                         |
| Mode Switch         | 1              | 0x00~0xFF     | Bit0: led mode(0 normal,1 twinkle)                                                 |
| Onoff               | 1              | 0 or 1        | mode0:0 off,1 on; mode1: ignore                                                    |
| Led Open duration   | 4              | 0~2147483647  | Unit ms                                                                             |
| Led Close duration  | 4              | 0~2147483647  | Unit ms                                                                             |
| LED_R               | 1              | 0~255         | Color percent                                                                       |
| LED_G               | 1              | 0~255         | Color percent                                                                       |
| LED_B               | 1              | 0~255         | Color percent                                                                       |

---

#### 2.3.1.7 Buzzer control（0x06）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field               | length（Byte） | data          | data specification                                                                 |
|---------------------|----------------|---------------|-------------------------------------------------------------------------------------|
| Group id            | 1              | 0~255         | Target role                                                                         |
| Mode Switch         | 1              | 0x00~0xFF     | Bit0: buzzer mode(0 normal,1 twinkle)                                               |
| Onoff               | 1              | 0 or 1        | mode0:0 off,1 on; mode1: ignore                                                    |
| Buzzer Open duration | 4             | 0~2147483647  | Unit ms                                                                             |
| Buzzer Close duration | 4            | 0~2147483647  | Unit ms                                                                             |
| Volume              | 1              | 0~10          | 0 min,10 max volume                                                                 |

---

#### 2.3.1.8 Vibration control（0x07）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field               | length（Byte） | data          | data specification                                                                 |
|---------------------|----------------|---------------|-------------------------------------------------------------------------------------|
| Group id            | 1              | 0~255         | Target role                                                                         |
| Mode Switch         | 1              | 0x00~0xFF     | Bit0: buzzer mode(0 normal,1 twinkle)                                               |
| Onoff               | 1              | 0 or 1        | mode0:0 off,1 on; mode1: ignore                                                    |
| motion              | 1              | 0 or 1        | 0 action,1 setting                                                                 |
| Vibration Open duration | 4          | 0~2147483647  | Unit ms                                                                             |
| Vibration Close duration | 4         | 0~2147483647  | Unit ms                                                                             |

---

#### 2.3.1.9 LCD context control（0x08）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field               | length（Byte） | data               | data specification               |
|---------------------|----------------|--------------------|-----------------------------------|
| Group id            | 1              | 0~255              | Target role                       |
| First line content  | 20             | U8 str[20]         | First line display content        |
| Second line content | 20             | U8 str[20]         | Second line display content       |

---

#### 2.3.1.10 LCD LINE2 onoff（0x09）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field         | length（Byte） | data    | data specification                     |
|---------------|----------------|---------|-----------------------------------------|
| Group id      | 1              | 0~255   | Target role                             |
| Line 2 enable | 1              | 0 or 1  | 0 disable,1 enable                      |

---

#### 2.3.1.11 Clear Packet（0x0A）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field       | length（Byte） | data    | data specification                                   |
|-------------|----------------|---------|-------------------------------------------------------|
| Group id    | 1              | 0~255   | Target role                                           |
| Clear type  | 1              | 0 or 1  | 0 Clear All Statuses(led close)<br/>1 All clear(green twinkle) |

---

#### 2.3.1.12 Set group id（0x50）
Direction: Gateway -> badge/hub  
Instruction: Cloud-initiated notification

| field     | length（Byte） | data    | data specification                                                                 |
|-----------|----------------|---------|-------------------------------------------------------------------------------------|
| Group id  | 1              | 0~255   | Set device role<br/>Bit0: admin<br/>Bit1：Nurses<br/>Bit2：secure<br/>Bit3：principal<br/>...<br/>Bit7: reserve |

---

# 3. CRC16
CRC verification employs the CRC16/XMODEM checksum code. The C language algorithm code is as follows:

```c
uint16_t xmodem_val;

void crc16_xmodem_init( void )
{
    xmodem_val = 0;
}

void crc16_xmodem_append( uint8_t *pbuf, uint16_t length )
{
    while( length-- ) {
        xmodem_val ^= (uint16_t)(*pbuf++) << 8;
        for( uint8_t i = 0; i < 8; ++i )
        {
            if( xmodem_val & 0x8000 ) {
                xmodem_val = (xmodem_val << 1) ^ 0x1021;
            } else {
                xmodem_val <<= 1;
            }
        }
    }
}

uint16_t crc16_xmodem_end( void )
{
    return xmodem_val;
}
```

### Usage Notes
Before calculating CRC, you must first call `crc16_xmodem_init()`.

### Demo
uint8_t crc_data[] = {
0xAA, 0x55, 0x01, 0x00, 0x00, 0x08, 0x00, 0x03
};
crc16_xmodem_init();
crc16_xmodem_append( crc_data, sizeof(crc_data) );
LOG( "crc16_xmodem value1: %X\r\n", crc16_xmodem_end() );
crc16_xmodem_append( crc_data, sizeof(crc_data) );
LOG( "crc16_xmodem value2: %X\r\n", crc16_xmodem_end() );

### Run result
- crc16_xmodem value1: 58C7
- crc16_xmodem value2: E228

## LED Colors
We need to make sure that the platform supports RGB values instead of defining the colors in a different way. This will enable us to satisfy the requirements for Standard Response protocol (SRP), by sending below colors directly from the dashboard (backend). These are not initiated from a badge:

| Command  | Trigger Source | Broadcast Target   | LED Color | Badge LCD Message |
|----------|----------------|---------------------|-----------|-------------------|
| Hold     | Dashboard only | All Badges & Hubs   | Purple    | Hold Alert        |
| Secure   | Dashboard only | All Badges & Hubs   | Blue      | Secure Alert      |
| Evacuate | Dashboard only | All Badges & Hubs   | Green     | Evacuate Alert    |
| Shelter  | Dashboard only | All Badges & Hubs   | Orange    | Shelter Alert     |

### RGB Proposed values
| Alert Type          | Color  | RGB         | Visibility Reason                                   |
|---------------------|--------|-------------|-----------------------------------------------------|
| Lockdown / Code Red | Red    | 255, 0, 0   | Maximum urgency, universally recognized             |
| Medical / Secure    | Blue   | 0, 100, 255 | Distinct from red and visible through diffusers    |
| Admin Assist        | Yellow | 255, 220, 0 | Very bright, easily seen in daylight               |
| All Clear / Evacuate| Green  | 0, 255, 80  | Bright but clearly different from yellow           |
| Hold                | Purple | 180, 0, 255 | Unique color not confused with others              |
| Shelter             | Orange | 255, 120, 0 | Strong color between red and yellow                |

## LoraWAN Class (A or B)
You selected Class B indicating that “badge and hub need to rec the downlink of the server”. Our main concern is the battery consumption, particularly for the Hub. Have you considered this?

## Offline Fallback
As we continue to review the system design, we wanted to raise a question: how does the system handle an internet outage?

Given that this is an emergency alert system, it is essential that alerts continue to function at the campus level even if all internet connectivity is lost — Ethernet, Wi-Fi, and LTE/4G simultaneously.

Has this been considered and addressed in the current design? Specifically, we would like to understand what the gateway does in the following scenario:
1. All internet connectivity is lost.
2. A badge panic button is pressed on campus.

### Our expectation
1. Detect the connectivity loss and activate a local fallback mode automatically — no manual intervention required.
2. Switch to an embedded local LNS to continue processing uplink alerts from badges and hubs.
3. Apply alert routing rules locally and issue the correct downlink commands to devices — buzzers, LED colours, and LCD messages.
4. Provide a minimal alert dashboard to on-site staff over the local network.
5. Queue all events locally and sync them to the AWS backend once connectivity is restored, preserving original timestamps for the audit trail.
6. Switch back to the primary AWS LNS automatically on reconnection.

Could you please confirm how this is addressed in the current design, or flag if anything above needs further discussion?