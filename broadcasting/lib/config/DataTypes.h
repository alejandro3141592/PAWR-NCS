#pragma once

#include <Arduino.h>


#define FLAG_LOW_BATTERY  0x02   // reserved for future battery-sensing support; not currently set by any code path
#define FLAG_SENSOR_FAIL  0x04


// Internal queue item carrying one set of sensor readings
struct SensorData {
    float    bodyTemp;       // MAX30205
    float    humidity;       // SHT41
    uint32_t timestamp;
};



// ======================================================
// Advertising payload inside the manufacturer-specific AD type.
// Bytes 0-1 are the company ID (0xFFFF), followed by this struct.
// Total on-air: 3 (Flags) + 1+1+2+8 (MSD) = 15 bytes
// (16 bytes below the 31-byte legacy ADV max; headroom intentionally unused).
//
// 2026-08-20: changed to be byte-for-byte identical to PAwR-ncs's own
// struct sensor_payload (common/pawr_protocol.h, the original 8-byte
// PAwR-era definition -- NOT the current 16-byte GATT-era one, which
// carries millis_since_init/init_epoch fields specific to that
// architecture's incremental-download bookkeeping and have no broadcasting
// equivalent) -- so the PAwR-vs-Broadcasting thesis comparison is measuring
// the same on-air payload size/shape in both architectures, not an
// apples-to-oranges comparison where one side simply has less to send.
// That also means matching PAwR-ncs's own reliability model: no app-level
// CRC field (BLE's own link-layer CRC is the only integrity check, same as
// PAwR-ncs -- see THESIS_TECHNICAL_REPORT.md's methodology notes) and no
// per-packet timestamp (PDR is computed hub-side from sequence-number
// gaps, exactly like PAwR-ncs's distance-vs-PDR sweeps, not from
// timestamp deltas).
//
// bodyTemp/humidity are now fixed-point (centi-degrees C / tenths-of-a-
// percent) instead of float, matching PAwR-ncs's temp_cdeg/humidity_pct10
// encoding exactly (see that struct's own comments for the scale: 3612 =
// 36.12C). BleManager.cpp does the float->fixed-point conversion at pack
// time; hub_main.cpp prints the raw fixed-point values as-is (no
// conversion back to float), matching PAwR-ncs central's own
// "EVT DOWNLOAD_DATA ... TEMP %d HUM %u ..." printk format exactly, so
// captured data from both architectures is in the same units.
//
// Shared directly by both the sender (PlatformIO env `xiao_nrf52840`) and
// the hub (PlatformIO env `hub`, src/hub_main.cpp) via this header — no
// hand-duplicated copy exists anymore.
// ======================================================

typedef struct __attribute__((packed)) {
    uint8_t  nodeID;           // offset 0, 1 byte  — human-readable label, not used for assignment
    uint8_t  flags;            // offset 1, 1 byte
    uint16_t seq;               // offset 2, 2 bytes — rolling broadcast counter, wraps at 65536
    int16_t  tempCdeg;          // offset 4, 2 bytes — skin temp, centi-degrees C (3612 = 36.12C)
    uint16_t humidityPct10;     // offset 6, 2 bytes — relative humidity, tenths of a percent
} AdvChannelPayload;            // sizeof == 8 bytes

static_assert(sizeof(AdvChannelPayload) == 8, "AdvChannelPayload size mismatch");
