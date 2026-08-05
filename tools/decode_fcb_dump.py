#!/usr/bin/env python3
"""
Decodes an Intel HEX dump of a Zephyr FCB (Flash Circular Buffer) partition
containing common/sensor_log.c's records (raw struct sensor_payload bytes,
see common/pawr_protocol.h) into CSV.

Used by tools/Read-StorageFlash.ps1 as a UART-free retrieval path for
gateway_9151's flash log -- see that script's header comment and
NOTES.md 2026-08-05 for why this exists (console UART0 confirmed
hardware-dead, unrelated to firmware).

FCB on-flash format (zephyr/subsys/fs/fcb/fcb.c, fcb_elem_info.c):
  Sector header (8 bytes): fd_magic(u32) fd_ver(u8) _pad(u8) fd_id(u16)
  Then a sequence of entries, each:
    [len byte(s)][len bytes of data][1-byte CRC-8/CCITT over len-field+data]
  len < 0x80 is one byte, stored as (len ^ ~erase_value); our payloads are
  always 8 bytes so this is always the 1-byte form. End of entries in a
  sector is marked by 4+ consecutive erase_value (0xFF) bytes where a
  length byte is expected.

CRC-8/CCITT: poly 0x07, init 0xFF, MSB-first, not reflected (matches
zephyr/subsys/crc/crc8_sw.c's crc8_ccitt()).

Usage: decode_fcb_dump.py <input.hex> <base_address_hex> <output.csv>
"""
import csv
import struct
import sys

SENSOR_PAYLOAD_STRUCT = struct.Struct("<BBHhH")  # node_id, flags, seq, temp_cdeg, humidity_pct10
ERASE_VALUE = 0xFF
SECTOR_HEADER_SIZE = 8
FLAG_TEMP_INVALID = 0x01
FLAG_HUMIDITY_INVALID = 0x02


def crc8_ccitt(data: bytes, crc: int = 0xFF) -> int:
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def parse_intel_hex(path: str) -> tuple[int, bytes]:
    """Returns (base_address, contiguous_bytes). Assumes a single contiguous
    linear dump with no address gaps, extended linear address record(s)
    only setting the upper 16 bits once (matches nrfutil device read's
    output for a single small region)."""
    records = []
    base_upper = 0
    min_addr = None

    with open(path, "r", encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            byte_count = int(line[1:3], 16)
            addr16 = int(line[3:7], 16)
            rec_type = int(line[7:9], 16)
            data_hex = line[9:9 + byte_count * 2]
            data = bytes.fromhex(data_hex)

            if rec_type == 0x04:  # extended linear address
                base_upper = int(data_hex, 16) << 16
            elif rec_type == 0x00:  # data
                full_addr = base_upper + addr16
                if min_addr is None or full_addr < min_addr:
                    min_addr = full_addr
                records.append((full_addr, data))
            elif rec_type == 0x01:  # EOF
                break

    if not records:
        raise ValueError("No data records found in hex file")

    max_addr = max(addr + len(data) for addr, data in records)
    buf = bytearray([ERASE_VALUE] * (max_addr - min_addr))
    for addr, data in records:
        offset = addr - min_addr
        buf[offset:offset + len(data)] = data

    return min_addr, bytes(buf)


def find_sectors(data: bytes) -> list[bytes]:
    """Splits the dump into FCB sectors by looking for valid sector
    headers (fd_magic non-erased). Our sensor_log.c uses f_magic=0x50415752
    ("PAWR"), XORed with ~erase_value per fcb_flash_magic() -- with
    erase_value=0xFF, ~0xFFFFFFFF = 0x00000000, so the on-flash magic is
    the raw value 0x50415752 unchanged. Falls back to a single sector
    (whole dump) if no valid header is found, so a partial/short dump
    (e.g. reading less than a full sector) still gets a best-effort parse.
    """
    magic_bytes = struct.pack("<I", 0x50415752)
    offsets = [i for i in range(0, len(data) - 4) if data[i:i + 4] == magic_bytes]

    if not offsets:
        return [data]

    sectors = []
    for i, off in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
        sectors.append(data[off:end])
    return sectors


def decode_sector(sector: bytes) -> list[dict]:
    if len(sector) <= SECTOR_HEADER_SIZE:
        return []

    entries = []
    pos = SECTOR_HEADER_SIZE

    while pos < len(sector):
        len_byte_raw = sector[pos]
        if len_byte_raw == ERASE_VALUE:
            # 0xFF here (unmodified, since len^~0xFF == len for the raw
            # stored byte only when erase_value is XORed in -- but an
            # actual erased/unwritten length byte reads back as plain
            # 0xFF) means end of written entries in this sector.
            break

        entry_len = len_byte_raw ^ (~ERASE_VALUE & 0xFF)
        if entry_len == 0 or entry_len > 64 or pos + 1 + entry_len + 1 > len(sector):
            # Implausible length (corrupt/partial entry) -- stop, rather
            # than risk misparsing the rest of the sector as garbage.
            break

        len_field = sector[pos:pos + 1]
        payload_bytes = sector[pos + 1:pos + 1 + entry_len]
        stored_crc = sector[pos + 1 + entry_len]

        computed_crc = crc8_ccitt(len_field + payload_bytes)
        crc_ok = (computed_crc == stored_crc)

        if entry_len == SENSOR_PAYLOAD_STRUCT.size:
            node_id, flags, seq, temp_cdeg, humidity_pct10 = \
                SENSOR_PAYLOAD_STRUCT.unpack(payload_bytes)
            entries.append({
                "node_id": node_id,
                "flags": flags,
                "seq": seq,
                "temp_c": temp_cdeg / 100.0,
                "humidity_pct": humidity_pct10 / 10.0,
                "temp_valid": not (flags & FLAG_TEMP_INVALID),
                "humidity_valid": not (flags & FLAG_HUMIDITY_INVALID),
                "crc_ok": crc_ok,
            })

        pos += 1 + entry_len + 1

    return entries


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.hex> <base_address_hex> <output.csv>",
              file=sys.stderr)
        return 1

    hex_path, _base_addr_arg, csv_path = sys.argv[1], sys.argv[2], sys.argv[3]

    _, data = parse_intel_hex(hex_path)
    sectors = find_sectors(data)

    all_entries = []
    for sector in sectors:
        all_entries.extend(decode_sector(sector))

    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["node_id", "seq", "flags", "temp_c", "humidity_pct",
                          "temp_valid", "humidity_valid", "crc_ok"])
        for e in all_entries:
            writer.writerow([e["node_id"], e["seq"], e["flags"],
                              f"{e['temp_c']:.2f}", f"{e['humidity_pct']:.1f}",
                              e["temp_valid"], e["humidity_valid"], e["crc_ok"]])

    bad_crc = sum(1 for e in all_entries if not e["crc_ok"])
    print(f"Decoded {len(all_entries)} record(s) from {len(sectors)} sector(s) "
          f"({bad_crc} CRC mismatch(es)).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
