#pragma once

// ======================================================
// NODE ID  — change this before uploading to each node
// ======================================================

#ifndef NODE_ID
#define NODE_ID  20
#endif

// ======================================================
// TIMING — milliseconds
// SENSOR_READ_INTERVAL_MS : how often sensors are read and a new broadcast begins.
// ADV_INTERVAL_MS         : BLE advertising repeat rate.
//                           100 ms → < 3 % channel occupation for 17 nodes.
//                           TODO: re-derive this occupancy figure — the wire
//                           payload has changed size more than once since
//                           this was last computed (see DataTypes.h) and
//                           the target node count for scalability testing
//                           may exceed 17; needs an airtime-per-advertisement
//                           recalculation or an empirical measurement, not a
//                           guess.
// ======================================================
#define SENSOR_READ_INTERVAL_MS  1000
#define ADV_INTERVAL_MS            100

// Wire payload: AdvChannelPayload is now 8 bytes (was 18, then 24 before
// that) as of the 2026-08-20 change to match PAwR-ncs's own struct
// sensor_payload byte-for-byte for a fair thesis comparison -- see
// DataTypes.h's comment on AdvChannelPayload for the full rationale.

#define _STRINGIFY(x) #x
#define _TOSTRING(x)  _STRINGIFY(x)
#define BLE_DEVICE_NAME "WearableNode_" _TOSTRING(NODE_ID)

// ======================================================
// DEBUG MACRO
// ======================================================

#define DEBUG 1

#if DEBUG
    #define DEBUG_PRINT(x)    Serial.print(x)
    #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
#endif