"""
Device Identity Tool - generate device_identity binary for flash programming.

Production flow:
  1. nrfjprog --program firmware.hex --sectorerase       # Flash firmware
  2. Power on device → firmware auto-persists BLE MAC to 0xB4000
  3. python gen_identity.py --read-mac                   # Read MAC from identity flash
  4. python gen_identity.py --read-mac --deveui ... --appeui ... --appkey ...   # Generate identity
  5. python gen_identity.py --flash ...                  # Flash identity (or nrfjprog directly)

Usage:
  # Auto-read BLE MAC from device_identity flash at 0xB4000 (recommended, production)
  python gen_identity.py --deveui 2026061800000001 --appeui B6AC3C8700677DD6 --appkey AF96095F9889BE31261274B69ED7FCDE --read-mac

  # Read BLE MAC from RUI3 NVM (legacy, 0xD0032)
  python gen_identity.py --read-mac --read-nvm

  # Read BLE MAC from FICR (IEEE public address, for reference)
  python gen_identity.py --read-mac --ficr

  # Manual BLE MAC
  python gen_identity.py --deveui 2026061800000001 --appeui B6AC3C8700677DD6 --appkey AF96095F9889BE31261274B69ED7FCDE --mac AB:56:34:12:EF:CD

  # Read BLE MAC only
  python gen_identity.py --read-mac

Output:
  identity.bin (46 bytes), flash to 0xB4000 with:
  nrfjprog --program identity.hex --sectorerase --verify

BLE MAC source:
  --read-mac (default) → device_identity flash at 0xB4000 (firmware auto-persisted)
    Validates magic + CRC32. Requires firmware to have booted once with BLE enabled.
  --read-nvm           → RUI3 system config NVM at 0xD0032 (legacy, may be zero on fresh device)
  --ficr               → FICR DEVICEADDR (IEEE public address, always available)
"""

import struct
import argparse
import subprocess
import re
import sys

# CRC32 lookup table — exact copy from firmware src/utils/crc32.cpp
# (IEEE 802.3 / Ethernet, poly=0xEDB88320)
_CRC32_TABLE = [
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
    0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
    0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
    0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
    0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
    0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
    0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
    0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
    0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
    0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
    0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
    0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
    0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
    0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
    0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
    0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB30A04,
    0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
    0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
    0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
    0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
    0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
    0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
]


def crc32_update(crc: int, data: bytes) -> int:
    """CRC32 update — exact copy from firmware crc32_update()"""
    crc ^= 0xFFFFFFFF
    for b in data:
        crc = _CRC32_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def crc32_compute(data: bytes) -> int:
    """CRC32 compute — exact copy from firmware crc32_compute()"""
    return crc32_update(0, data)


def parse_hex_bytes(hex_str: str, expected_len: int) -> bytes:
    """Parse hex string like "AABBCC" or "AA:BB:CC" to bytes"""
    hex_str = hex_str.replace(":", "").replace("-", "").replace(" ", "").strip()
    if len(hex_str) != expected_len * 2:
        raise ValueError(
            f"Expected {expected_len} bytes ({expected_len*2} hex chars), "
            f"got {len(hex_str)}"
        )
    return bytes.fromhex(hex_str)


def parse_mac(mac_str: str) -> bytes:
    """Parse MAC "AA:BB:CC:DD:EE:FF" to 6 bytes (LSBF)"""
    return parse_hex_bytes(mac_str, 6)


def read_flash_bytes(addr: int, size: int) -> bytes:
    """Read bytes from device flash via nrfjprog.

    SoftDevice may lock the debug port (APPROTECT) while running,
    preventing nrfjprog --memrd from accessing memory.
    We try a soft read first; if it fails, halt the core via debugreset,
    read, then release the core.
    """
    def _try_memrd():
        out = subprocess.check_output(
            ["nrfjprog", "--memrd", f"0x{addr:X}", "--n", str(size)],
            stderr=subprocess.STDOUT, text=True
        )
        return out

    def _parse_output(out):
        words = []
        for line in out.splitlines():
            m = re.match(r"0x[0-9A-Fa-f]+:\s+([0-9A-Fa-f\s]+)", line)
            if m:
                for w in m.group(1).split():
                    words.append(int(w, 16))
        data = b"".join(struct.pack("<I", w) for w in words)
        return data[:size]

    # Attempt 1: direct read (works when device is in reset or bootloader)
    try:
        return _parse_output(_try_memrd())
    except subprocess.CalledProcessError as e:
        if e.returncode != 54:
            raise
        print("  (debug port locked by SoftDevice, retrying with debugreset...)")
    except Exception:
        pass  # fall through to debugreset path

    # Attempt 2: halt core via debugreset, read, then ALWAYS release
    try:
        subprocess.check_call(
            ["nrfjprog", "--debugreset", "--timeout", "5"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        try:
            return _parse_output(_try_memrd())
        finally:
            # Always release the core so firmware can continue
            subprocess.check_call(
                ["nrfjprog", "--reset", "--timeout", "5"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
    except subprocess.CalledProcessError:
        raise RuntimeError(
            "Cannot read flash at 0x{:X} — debug port locked by SoftDevice. "
            "Power-cycle the device and try again, or use --mac to specify manually.".format(addr)
        )


def read_mac_from_identity_flash() -> bytes:
    """
    Read BLE MAC from device_identity struct in user flash at 0xB4004.

    The struct layout at 0xB4000 (46 bytes, packed):
      magic(4) + ble_mac(6) + dev_eui(8) + app_eui(8) + app_key(16) + crc32(4)
    ble_mac is at offset 4, 6 raw bytes.

    Validates magic (0x4C534449) and CRC32 before returning.
    Requires firmware to have called device_identity_persist() (auto on first boot).

    Raises RuntimeError if read fails or data is invalid.
    Reads via nrfjprog — may fail if SoftDevice has locked the debug port.
    """
    MAGIC = 0x4C534449
    raw = read_flash_bytes(0xB4000, 46)

    if len(raw) < 46:
        raise RuntimeError("Incomplete data (len={})".format(len(raw)))

    magic = struct.unpack('<I', raw[0:4])[0]
    if magic != MAGIC:
        if magic == 0xFFFFFFFF:
            raise RuntimeError(
                "device_identity area is blank (all 0xFF). "
                "Firmware hasn't persisted yet. Boot device once first."
            )
        raise RuntimeError(
            "Bad magic: 0x{:08X} (expected 0x{:08X})".format(magic, MAGIC)
        )

    expected_crc = struct.unpack('<I', raw[42:46])[0]
    computed_crc = crc32_compute(raw[0:42])
    if expected_crc != computed_crc:
        raise RuntimeError(
            "CRC32 mismatch (expected=0x{:08X}, computed=0x{:08X})".format(
                expected_crc, computed_crc)
        )

    return raw[4:10]  # ble_mac: 6 bytes at offset 4


def read_mac_from_rui3_nvm() -> bytes:
    """
    Read BLE MAC from RUI3 system config NVM (legacy, 0xD0032).

    RUI3 stores BLE MAC as a 12-char hex string ("CB59E77979BE") in the
    PRE_rui_cfg_t struct at MCU_SYS_CONFIG_NVM_ADDR (0xD0000).
    The MAC is at g_ble_cfg_t.mac[12] — offset 50 within the struct → 0xD0032.

    NOTE: On fresh devices this is all zeros. Prefer read_mac_from_identity_flash().
    """
    try:
        out = subprocess.check_output(
            ["nrfjprog", "--memrd", "0xD0032", "--n", "12"],
            stderr=subprocess.STDOUT, text=True
        )
    except FileNotFoundError:
        sys.exit("ERROR: nrfjprog not found. "
                 "Install nRF Command Line Tools or use --mac to specify manually.")
    except subprocess.CalledProcessError as e:
        sys.exit(f"ERROR: nrfjprog failed. Is J-Link connected?\n{e.output}")

    # Parse nrfjprog output: "0x000D0032: 43423539 45373739 37394245" = "CB59E77979BE"
    # nrfjprog --memrd reads 32-bit words (little-endian), so bytes within each word
    # are reversed relative to the in-memory ASCII string order.
    words = []
    for line in out.splitlines():
        m = re.match(r"0x[0-9A-Fa-f]+:\s+([0-9A-Fa-f\s]+)", line)
        if m:
            for w in m.group(1).split():
                words.append(int(w, 16))

    if not words:
        sys.exit(f"ERROR: Cannot parse nrfjprog output:\n{out}")

    # Convert 32-bit little-endian words to byte stream
    raw_bytes = b"".join(struct.pack("<I", w) for w in words)
    # Take the first 12 bytes (ASCII hex string)
    hex_str = raw_bytes[:12].decode('ascii', errors='replace')

    # Validate hex string
    if not re.match(r'^[0-9A-Fa-f]{12}$', hex_str):
        if hex_str == '\x00' * 12 or hex_str.strip('\x00') == '':
            sys.exit(
                "ERROR: BLE MAC in RUI3 NVM is all zeros.\n"
                "  The device hasn't saved a BLE MAC yet (first boot with BLE\n"
                "  not initialized, or api.ble.mac.set() was never called).\n"
                "  Options:\n"
                "    1. Run 'api.ble.mac.get()' on the device to get the MAC,\n"
                "       then pass it with --mac XX:XX:XX:XX:XX:XX\n"
                "    2. Use --ficr to read the IEEE public address from FICR instead"
            )
        sys.exit(f"ERROR: Invalid BLE MAC hex string at 0xD0032: '{hex_str}'")

    # Parse 12-char hex string → 6 bytes
    mac = bytes.fromhex(hex_str)
    return mac


def read_mac_from_ficr() -> bytes:
    """
    Read factory BLE MAC from nRF52840 FICR via nrfjprog (IEEE public address).

    FICR registers:
      0x100000A4: DEVICEADDR[0] (32-bit) - MAC[0..3]
      0x100000A8: DEVICEADDR[1] (32-bit) - MAC[4..5] (low 16 bits)

    Returns: 6 bytes (little-endian)
    """
    try:
        out = subprocess.check_output(
            ["nrfjprog", "--memrd", "0x100000A4", "--n", "8"],
            stderr=subprocess.STDOUT, text=True
        )
    except FileNotFoundError:
        sys.exit("ERROR: nrfjprog not found. "
                 "Install nRF Command Line Tools or use --mac to specify manually.")
    except subprocess.CalledProcessError as e:
        sys.exit(f"ERROR: nrfjprog failed. Is J-Link connected?\n{e.output}")

    # Parse output: "0x100000A4: 123456AB FFFFCDEF"
    match = re.search(r"0x100000A4:\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})", out)
    if not match:
        sys.exit(f"ERROR: Cannot parse nrfjprog output:\n{out}")

    addr0 = int(match.group(1), 16)
    addr1 = int(match.group(2), 16)

    # nRF52840 DEVICEADDR is little-endian
    mac = struct.pack("<IH", addr0, addr1 & 0xFFFF)
    return mac


def generate_identity(ble_mac: bytes, dev_eui: bytes, app_eui: bytes, app_key: bytes) -> bytes:
    """
    Generate 46-byte device_identity binary.

    Layout: magic(4) + ble_mac(6) + dev_eui(8) + app_eui(8) + app_key(16) + crc32(4)
    """
    MAGIC = 0x4C534449  # "IDSL"

    header = struct.pack('<I', MAGIC)
    payload = header + ble_mac + dev_eui + app_eui + app_key
    crc = crc32_compute(payload)
    return payload + struct.pack('<I', crc)


def bin_to_intel_hex(data: bytes, base_addr: int) -> str:
    """Convert binary data to Intel HEX format (supports 32-bit addresses)."""
    lines = []

    # Extended Linear Address record (type 04): upper 16 bits of the 32-bit address
    # base_addr=0xB4000 => upper=0x000B, data records use lower 16 bits (0x4000)
    upper = (base_addr >> 16) & 0xFFFF
    if upper != 0:
        ela = bytearray([0x02, 0x00, 0x00, 0x04, (upper >> 8) & 0xFF, upper & 0xFF])
        cksum = (-sum(ela)) & 0xFF
        lines.append(":" + ela.hex().upper() + f"{cksum:02X}")

    base_low = base_addr & 0xFFFF
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        addr = (base_low + offset) & 0xFFFF
        record = bytearray([len(chunk), (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + chunk
        cksum = (-sum(record)) & 0xFF
        hex_line = ":" + record.hex().upper() + f"{cksum:02X}"
        lines.append(hex_line)
    lines.append(":00000001FF")  # EOF
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Generate LoRa device identity binary for flash programming"
    )
    parser.add_argument("--deveui", default=None, help="DevEUI (16 hex chars)")
    parser.add_argument("--appeui", default=None, help="AppEUI/JoinEUI (16 hex chars)")
    parser.add_argument("--appkey", default=None, help="AppKey (32 hex chars)")
    parser.add_argument("--mac", default=None,
                        help="BLE MAC (12 hex), FF:FF:FF:FF:FF:FF placeholder if omitted")
    parser.add_argument("--read-mac", action="store_true",
                        help="Auto-read BLE MAC from device_identity flash (0xB4000, production default)")
    parser.add_argument("--read-nvm", action="store_true",
                        help="Read BLE MAC from RUI3 system config NVM (0xD0032, legacy)")
    parser.add_argument("--ficr", action="store_true",
                        help="Read BLE MAC from FICR DEVICEADDR (IEEE public address)")
    parser.add_argument("--output", default="identity",
                        help="Output file basename (default: identity, generates .bin + .hex)")
    parser.add_argument("--flash-addr", default="0xB4000",
                        help="Target flash address (default: 0xB4000)")
    parser.add_argument("--flash", action="store_true",
                        help="Automatically run nrfjprog to program the device after generation")

    args = parser.parse_args()

    # No arguments -> print help
    if not any([args.deveui, args.appeui, args.appkey, args.read_mac, args.mac, args.ficr, args.read_nvm]):
        parser.print_help()
        return

    # Read MAC only mode
    if (args.read_mac or args.ficr or args.read_nvm) and not args.deveui:
        if args.ficr:
            try:
                mac = read_mac_from_ficr()
                print(f"FICR (IEEE public): {':'.join(f'{b:02X}' for b in mac)}")
            except Exception as e:
                sys.exit(f"ERROR: Cannot read FICR: {e}")
        elif args.read_nvm:
            try:
                mac = read_mac_from_rui3_nvm()
                print(f"RUI3 NVM (api.ble.mac.get): {':'.join(f'{b:02X}' for b in mac)}")
            except Exception as e:
                sys.exit(f"ERROR: Cannot read RUI3 NVM: {e}")
        else:
            try:
                mac = read_mac_from_identity_flash()
                print(f"Identity flash (0xB4004): {':'.join(f'{b:02X}' for b in mac)}")
            except Exception as e:
                sys.exit(
                    f"ERROR: Cannot read BLE MAC: {e}\n"
                    "  The SoftDevice may have locked the debug port.\n"
                    "  Options:\n"
                    "    1. Power-cycle the device and try again\n"
                    "    2. Use --ficr to read the IEEE public address\n"
                    "    3. Use --mac to specify manually"
                )
        return

    # Generate identity mode
    if not args.deveui or not args.appeui or not args.appkey:
        sys.exit("ERROR: --deveui, --appeui, --appkey are required. Use -h for help.")

    dev_eui = parse_hex_bytes(args.deveui, 8)
    app_eui = parse_hex_bytes(args.appeui, 8)
    app_key = parse_hex_bytes(args.appkey, 16)

    if args.ficr:
        ble_mac = read_mac_from_ficr()
        print(f"  BLE MAC from FICR (IEEE public): {':'.join(f'{b:02X}' for b in ble_mac)}")
    elif args.read_nvm:
        ble_mac = read_mac_from_rui3_nvm()
        print(f"  BLE MAC from RUI3 NVM (api.ble.mac.get): {':'.join(f'{b:02X}' for b in ble_mac)}")
    elif args.read_mac:
        try:
            ble_mac = read_mac_from_identity_flash()
            print(f"  BLE MAC from identity flash (0xB4004): {':'.join(f'{b:02X}' for b in ble_mac)}")
        except (RuntimeError, subprocess.CalledProcessError) as e:
            ble_mac = b'\xFF\xFF\xFF\xFF\xFF\xFF'
            print(f"  WARNING: Cannot read MAC from identity flash: {e}")
            print(f"  Using FF:FF:FF:FF:FF:FF — Hub firmware will auto-correct MAC on next boot")
    elif args.mac:
        ble_mac = parse_mac(args.mac)
    else:
        ble_mac = b'\xFF\xFF\xFF\xFF\xFF\xFF'
        print("WARNING: BLE MAC not provided, using FF:FF:FF:FF:FF:FF "
              "(firmware will use api.ble.mac.get() on first boot)")

    flash_addr = int(args.flash_addr, 16)

    # Generate
    data = generate_identity(ble_mac, dev_eui, app_eui, app_key)

    # Write .bin
    bin_path = args.output + ".bin"
    with open(bin_path, 'wb') as f:
        f.write(data)

    # Write .hex (Intel HEX format for nrfjprog)
    hex_path = args.output + ".hex"
    with open(hex_path, 'w') as f:
        f.write(bin_to_intel_hex(data, flash_addr))

    print(f"Generated: {bin_path} + {hex_path} ({len(data)} bytes)")
    print(f"  BLE MAC : {'%02X:%02X:%02X:%02X:%02X:%02X' % tuple(ble_mac)}")
    print(f"  DevEUI  : {dev_eui.hex().upper()}")
    print(f"  AppEUI  : {app_eui.hex().upper()}")
    print(f"  AppKey  : {app_key.hex().upper()}")
    print(f"  CRC32   : 0x{struct.unpack('<I', data[-4:])[0]:08X}")
    print()
    print(f"  Flash command:")
    print(f"  nrfjprog --program {hex_path} --sectorerase --verify")

    if args.flash:
        print()
        print("--- Programming device ---")
        try:
            subprocess.check_call(
                ["nrfjprog", "--program", hex_path, "--sectorerase", "--verify", "--reset"],
                stdout=sys.stdout, stderr=sys.stderr
            )
            print("Flash done (nrfjprog built-in verify passed, device reset).")
        except FileNotFoundError:
            sys.exit("ERROR: nrfjprog not found. Install nRF Command Line Tools.")
        except subprocess.CalledProcessError as e:
            sys.exit(f"ERROR: nrfjprog failed (exit code {e.returncode}). Is J-Link connected?")


if __name__ == "__main__":
    main()
