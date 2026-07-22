# Multicast Group Parameters — For Firmware Team

*Prepared 2026-07-10*

These are the four ChirpStack multicast groups for SimulAlert Pilot School (CODE RED, BLUE, YELLOW, GREEN). The address and both session keys below are freshly generated, cryptographically random values — not placeholders. These are the exact values being entered into ChirpStack, so please burn these exact strings into device memory to keep both sides in sync.

---

## 1. Group Identity and Keys

Each row below is one required firmware field. Values are hex-encoded (uppercase).

| Group       | Parameter                         | Value                                    |
|-------------|-----------------------------------|------------------------------------------|
| **CODE RED**| Multicast group name              | `mc-red-sch-demoschool-all`              |
|             | Multicast address (MCAddr)        | `83C2A6A8`                               |
|             | Network session key (McNwkSKey)   | `BC6EE98F744528A4422A31F7C3F71635`       |
|             | Application session key (McAppSKey) | `6AA73D8DAB6D8CBFF1AF3B0AC531B493`       |
| **CODE BLUE**| Multicast group name            | `mc-blue-sch-demoschool-nurse-admin`     |
|             | Multicast address (MCAddr)        | `1CF26AA9`                               |
|             | Network session key (McNwkSKey)   | `C2DFF724D5500536E2524BEC84CB41B2`       |
|             | Application session key (McAppSKey) | `B49629D725A9DC4C67D48CC04972BBFF`       |
| **CODE YELLOW**| Multicast group name           | `mc-yellow-sch-demoschool-admin`         |
|             | Multicast address (MCAddr)        | `F59367B5`                               |
|             | Network session key (McNwkSKey)   | `2CD36D9D8B83DD290E8B98654DB8D4F5`       |
|             | Application session key (McAppSKey) | `DDF78EDFB5DED07A8ECEE366310E241B`       |
| **CODE GREEN**| Multicast group name           | `mc-green-sch-demoschool-all`            |
|             | Multicast address (MCAddr)        | `F7C48FB6`                               |
|             | Network session key (McNwkSKey)   | `A151D461B4400E118658B3C1A142BE5B`       |
|             | Application session key (McAppSKey) | `A7EF75C71D98D64B51A1DCF51BBC0309`       |

---

## 2. Shared Radio Parameters (same for all four groups)

| Parameter                           | Value                    |
|-------------------------------------|--------------------------|
| Region                              | US915                    |
| Data rate (DR)                      | DR3 (SF7 / 125 kHz)      |
| Frequency                           | 923300000 Hz (923.3 MHz) |
| Group type                          | Class-B                  |
| Class‑B ping‑slot periodicity       | Every 4 seconds          |

---

## 3. Device‑to‑Group Assignment

For reference — which devices should be members of each group. Devices are already registered in ChirpStack under the SimulAlert Pilot School application.

| Group                     | Devices assigned to this group                              |
|---------------------------|------------------------------------------------------------|
| CODE RED (mc-red-…)       | All badges and all hubs (campus-wide)                      |
| CODE BLUE (mc-blue-…)     | Nurse badges + admin badges                                |
| CODE YELLOW (mc-yellow-…) | Admin badges                                               |
| CODE GREEN (mc-green-…)   | All badges and all hubs (campus-wide)                     |

---

## 4. Handling Note

> These are cryptographic session keys, not just configuration values — please treat this document like a password and send/store it accordingly (avoid pasting into unencrypted chat threads or plain email bodies where avoidable).