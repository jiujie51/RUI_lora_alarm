# Test Downlink Commands (Hex)

FPort=20 (FPORT_COMMON). CRC16/XMODEM polynomial 0x1021.

## Frame Format

```
| head(2) | ver(1) | control(1) | cmdid(1) | total_len(2,BE) | crc(2,BE) | data(N) |
| AA 55   | 01     | 00         | --       | -- --           | -- --     | ...     |
```

`total_len = 9 + data_len` (PROTO_MIN_FRAME_LEN + data_len).
CRC computed over `head..total_len` (7 bytes) + `data` (N bytes).


## Alarm CMD 0x03 - Code

| Broadcast Code Red (ALL group, alarm=3, ALL rooms) | `AA 55 01 00 03 00 0C 84 EB 80 03 FF` |
| Broadcast Code Blue (ALL group, alarm=1, ALL rooms) | `AA 55 01 00 03 00 0C E2 89 80 01 FF` |
| Broadcast Code Yellow (ALL group, alarm=2, ALL rooms) | `AA 55 01 00 03 00 0C B7 DA 80 02 FF` |
| Broadcast Code Green (ALL group, alarm=0, ALL rooms) | `AA 55 01 00 03 00 0C D1 B8 80 00 FF` |
| Admin group Code Red, room=12 | `AA 55 01 00 03 00 0C 57 FD 01 03 0C` |
| Broadcast Shelter (ALL group, alarm=7, ALL rooms) | `AA 55 01 00 03 00 0C 48 2F 80 07 FF` |
| Broadcast Evacuate (ALL group, alarm=6, ALL rooms) | `AA 55 01 00 03 00 0C 7B 1E 80 06 FF` |

## Clear CMD 0x0A - Clear

| Broadcast Clear All (remove all alarms) | `AA 55 01 00 0A 00 0C 99 5A 80 00 FF` |
| Broadcast All Clear (remove Code Red only) | `AA 55 01 00 0A 00 0C AA 6B 80 01 FF` |

## Room ID CMD 0x0B - Set Room ID (Hub only)

| Set Room ID=1 | `AA 55 01 00 0B 00 0A 20 F0 01` |
| Set Room ID=5 | `AA 55 01 00 0B 00 0A 60 74 05` |
| Set Room ID=12 | `AA 55 01 00 0B 00 0A F1 5D 0C` |
| Set Room ID=200 (max) | `AA 55 01 00 0B 00 0A 68 95 C8` |

## Group ID CMD 0x50 - Set Group ID

| Admin (0x01) | `AA 55 01 00 50 00 0A 4B D4 01` |
| Nurse (0x02) | `AA 55 01 00 50 00 0A 7B B7 02` |
| Security (0x04) | `AA 55 01 00 50 00 0A 1B 71 04` |
| Principal (0x08) | `AA 55 01 00 50 00 0A DA FD 08` |
| Admin+Security (0x05) | `AA 55 01 00 50 00 0A 0B 50 05` |
| ALL (0x80) | `AA 55 01 00 50 00 0A CA 7D 80` |

## LED Control CMD 0x05 - LED Control (Hub only)

| Red solid (broadcast) | `AA 55 01 00 05 00 18 ED 73 80 00 00 01 00 00 00 00 00 00 00 00 FF 00 00` |
| Blue blink 500ms (broadcast) | `AA 55 01 00 05 00 18 2C D8 80 00 01 00 00 00 01 F4 00 00 01 F4 00 00 FF` |
| Green solid (broadcast) | `AA 55 01 00 05 00 18 21 EF 80 00 00 01 00 00 00 00 00 00 00 00 00 FF 00` |
| White blink 300ms (broadcast) | `AA 55 01 00 05 00 18 E5 BB 80 00 01 00 00 00 01 2C 00 00 01 2C FF FF FF` |
| LED off (broadcast) | `AA 55 01 00 05 00 18 21 65 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00` |

## Buzzer CMD 0x06 - Buzzer Control (Hub only)

| ON, vol=10 (broadcast) | `AA 55 01 00 06 00 15 33 F9 80 00 01 00 00 00 00 00 00 00 00 0A` |
| OFF (broadcast) | `AA 55 01 00 06 00 15 FD F6 80 00 00 00 00 00 00 00 00 00 00 00` |
| Pattern 500ms, vol=5 (broadcast) | `AA 55 01 00 06 00 15 B2 53 80 01 00 00 00 01 F4 00 00 01 F4 05` |

## Code Setting CMD 0x04 - Configure Alarm LED/Buzzer

| Code Red: red blink 300ms + buzzer ON vol=10 | `AA 55 01 00 04 00 21 0C 34 80 03 01 01 0A 00 00 01 2C 00 00 01 2C 00 00 00 00 00 00 00 00 FF 00 00` |
| Code Blue: blue blink 500ms + buzzer pattern vol=6 | `AA 55 01 00 04 00 21 91 0B 80 01 01 01 06 00 00 01 F4 00 00 01 F4 00 00 00 00 00 00 00 00 00 00 FF` |

---

## Quick Copy (compact hex, no spaces, FPort=20)

```
# -- CMD 0x03 Code --
AA55010003000C84EB8003FF                           # Broadcast Code Red
AA55010003000CE2898001FF                           # Broadcast Code Blue
AA55010003000CB7DA8002FF                           # Broadcast Code Yellow
AA55010003000CD1B88000FF                           # Broadcast Code Green
AA55010003000C57FD01030C                           # Admin Code Red, room=12
AA55010003000C482F8007FF                           # Broadcast Shelter
AA55010003000C7B1E8006FF                           # Broadcast Evacuate
# -- CMD 0x0A Clear --
AA5501000A000C995A8000FF                           # Broadcast Clear All
AA5501000A000CAA6B8001FF                           # Broadcast All Clear (Code Red only)
# -- CMD 0x0B Set Room ID (Hub only) --
AA5501000B000A20F001                               # Room=1
AA5501000B000A607405                               # Room=5
AA5501000B000AF15D0C                               # Room=12
AA5501000B000A6895C8                               # Room=200
# -- CMD 0x50 Set Group ID --
AA55010050000A4BD401                               # Admin (0x01)
AA55010050000A7BB702                               # Nurse (0x02)
AA55010050000A1B7104                               # Security (0x04)
AA55010050000ADAFD08                               # Principal (0x08)
AA55010050000A0B5005                               # Admin+Security (0x05)
AA55010050000ACA7D80                               # ALL (0x80)
# -- CMD 0x05 LED Control (Hub only) --
AA550100050018ED73800000010000000000000000FF0000   # Red solid
AA5501000500182CD880000100000001F4000001F40000FF   # Blue blink 500ms
AA55010005001821EF80000001000000000000000000FF00   # Green solid
AA550100050018E5BB800001000000012C0000012CFFFFFF   # White blink 300ms
AA5501000500182165800000000000000000000000000000   # LED off
# -- CMD 0x06 Buzzer Control (Hub only) --
AA55010006001533F980000100000000000000000A         # ON vol=10
AA550100060015FDF6800000000000000000000000         # OFF
AA550100060015B253800100000001F4000001F405         # Pattern 500ms vol=5
# -- CMD 0x04 Code Setting --
AA5501000400210C34800301010A0000012C0000012C0000000000000000FF0000 # Code Red: red blink 300ms + buzzer ON vol=10
AA550100040021910B8001010106000001F4000001F400000000000000000000FF # Code Blue: blue blink 500ms + buzzer pat vol=6
```

---

## Usage

Copy the compact hex string from the Quick Copy section into ChirpStack/LNS downlink queue, FPort=20.

Verify: `AA 55 01 00 00 00 09` CRC = `3EAC` (empty frame, matches proto_crc16.cpp)

Generated: 2026-08-08
