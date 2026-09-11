#include "BleManager.h"
#include "Config.h"
#include <math.h>

static const uint16_t COMPANY_ID = 0xFFFF;

// Persistent advertising state — the SoftDevice holds a pointer to these for
// the lifetime of the advertising set, so they must NOT be stack-allocated.
static uint8_t            s_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t            s_adv_buf[31];
static ble_gap_adv_data_t s_gap_adv_data;

void setupBLE() {
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    Bluefruit.setTxPower(8);
    Bluefruit.setName(BLE_DEVICE_NAME);

    // Advertising is NOT started here. updateAdvertising() owns the advertising
    // set and drives it via direct SoftDevice calls, bypassing the Adafruit
    // wrapper whose start() fails when called from a FreeRTOS task context.
    DEBUG_PRINTLN("[BLE] Initialized");
}

void updateAdvertising(float bodyTemp, float humidity,
                       uint32_t timestamp, uint16_t seq, uint8_t flags) {
    // timestamp is accepted (unchanged call signature, so Tasks.cpp/
    // SensorData don't need reworking) but no longer placed on the wire --
    // see DataTypes.h's 2026-08-20 comment on AdvChannelPayload for why
    // (matching PAwR-ncs's own struct sensor_payload/reliability model
    // byte-for-byte). (void)-cast to make the intentional non-use explicit
    // rather than leaving an unused-parameter warning to wonder about.
    (void)timestamp;

    AdvChannelPayload payload;
    payload.nodeID         = NODE_ID;
    payload.flags          = flags;
    payload.seq            = seq;
    // Same scale/rounding as PAwR-ncs's max30205_read_temp_cdeg()
    // (raw * 100 / 256, i.e. multiply-by-100 on a 1/256 C raw value) and
    // its SHT4x humidity_pct10 (percent * 10) -- applied here to the
    // already-computed float rather than the raw register, since
    // MAX30205Sensor/SHT41Sensor only expose the float, but it's the same
    // underlying value at the same scale factor.
    payload.tempCdeg       = (int16_t)lroundf(bodyTemp * 100.0f);
    payload.humidityPct10  = (uint16_t)lroundf(humidity * 10.0f);

    // Build raw BLE AD packet directly into the static buffer.
    uint8_t idx = 0;
    s_adv_buf[idx++] = 2;
    s_adv_buf[idx++] = BLE_GAP_AD_TYPE_FLAGS;
    s_adv_buf[idx++] = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    s_adv_buf[idx++] = 1 + 2 + (uint8_t)sizeof(AdvChannelPayload);
    s_adv_buf[idx++] = BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
    s_adv_buf[idx++] = COMPANY_ID & 0xFF;
    s_adv_buf[idx++] = (COMPANY_ID >> 8) & 0xFF;
    memcpy(&s_adv_buf[idx], &payload, sizeof(payload));
    idx += sizeof(payload);

    s_gap_adv_data.adv_data.p_data      = s_adv_buf;
    s_gap_adv_data.adv_data.len         = idx;
    s_gap_adv_data.scan_rsp_data.p_data = NULL;
    s_gap_adv_data.scan_rsp_data.len    = 0;

    ble_gap_adv_params_t params;
    memset(&params, 0, sizeof(params));
    params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    params.interval        = (uint32_t)(ADV_INTERVAL_MS / 0.625f);
    params.duration        = 0;
    params.primary_phy     = BLE_GAP_PHY_1MBPS;

    if (s_adv_handle != BLE_GAP_ADV_SET_HANDLE_NOT_SET) {
        sd_ble_gap_adv_stop(s_adv_handle);
    }

    uint32_t err = sd_ble_gap_adv_set_configure(&s_adv_handle, &s_gap_adv_data, &params);
    if (err != NRF_SUCCESS) {
#if DEBUG
        Serial.print("[ADV] configure err=0x"); Serial.println(err, HEX);
#endif
        return;
    }

    err = sd_ble_gap_adv_start(s_adv_handle, BLE_CONN_CFG_TAG_DEFAULT);
    if (err != NRF_SUCCESS) {
#if DEBUG
        Serial.print("[ADV] start err=0x"); Serial.println(err, HEX);
#endif
        return;
    }

#if DEBUG
    Serial.print("[TX] "); Serial.print(millis());
    Serial.print(',');     Serial.print(NODE_ID);
    Serial.print(',');     Serial.print(seq);
    Serial.print(',');     Serial.print(timestamp);
    Serial.print(',');     Serial.print(bodyTemp, 4);
    Serial.print(',');     Serial.print(humidity, 4);
    Serial.print(',');     Serial.println(flags);
#endif
}
