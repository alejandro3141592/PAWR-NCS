#include <bluefruit.h>
#include "DataTypes.h"

static const uint16_t COMPANY_ID = 0xFFFF;

// ======================================================
// Per-node state — bounds-checked linear-scan table (replaces the old
// fixed-size s_lastPrint[18] array indexed directly by nodeID, which had
// no bounds check and would corrupt memory for any nodeID >= 18).
//
// 2026-08-20: firstSeenHub/offsetMs (clock-offset-corrected latency
// tracking) removed along with AdvChannelPayload's timestamp field -- see
// DataTypes.h's comment on that change. PDR is now purely a sequence-gap
// count, matching PAwR-ncs's own PDR methodology exactly (received /
// (seq_max - seq_min + 1) from gaps in the free-running sequence counter
// -- see THESIS_TECHNICAL_REPORT.md §3's methodology notes).
// ======================================================

// 2026-08-26: raised 32 -> 64 -- the 32 cap silently dropped any node beyond
// the first 32 seen after each hub boot (findOrCreateNode() returns nullptr
// once full, no crash/warning visible in a raw byte capture since
// s_tableFullWarned only prints once), which surfaced as a *different*
// random 8-of-40 subset going missing after every hub reset during the
// 40-node scalability tier -- not a per-board hardware fault as first
// suspected. 64 gives headroom above the 40-node tier, the largest planned
// for this scalability axis.
#define MAX_NODES 64

struct NodeState {
    bool     inUse       = false;
    uint8_t  nodeID       = 0;
    bool     haveLastSeq  = false;
    uint16_t lastSeq      = 0;
};

static NodeState s_nodes[MAX_NODES];
static uint8_t   s_nodeCount = 0;
static bool      s_tableFullWarned = false;

void scan_callback(ble_gap_evt_adv_report_t* report);

// Returns pointer to existing or newly-inserted entry, or nullptr if the
// table is full (bounded — never writes out of s_nodes[]).
NodeState* findOrCreateNode(uint8_t nodeID) {
    for (uint8_t i = 0; i < s_nodeCount; i++) {
        if (s_nodes[i].nodeID == nodeID) return &s_nodes[i];
    }
    if (s_nodeCount >= MAX_NODES) {
        if (!s_tableFullWarned) {
            Serial.println("[WARN] node table full, dropping unseen node");
            s_tableFullWarned = true;
        }
        return nullptr;
    }
    NodeState* n = &s_nodes[s_nodeCount++];
    n->inUse = true;
    n->nodeID = nodeID;
    return n;
}

// ======================================================
// Setup
// ======================================================

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(LED_RED,     OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    digitalWrite(LED_RED,     HIGH);

    Serial.begin(115200);

    Serial.println("\nBLE Hub — passive scan");
    Serial.print("sizeof(AdvChannelPayload) = ");
    Serial.println(sizeof(AdvChannelPayload));

    Bluefruit.begin(0, 1);
    Bluefruit.setName("AdvHub");

    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Scanner.setInterval(160, 80);
    Bluefruit.Scanner.useActiveScan(false);

    Bluefruit.Scanner.start(0);
    Serial.println("Scanning...");

    Serial.println("hub_rx_millis,nodeID,seq,rssi,temp_cdeg,humidity_pct10,flags,dup,gap_count");
}

// ======================================================
// Main loop — heartbeat only
// ======================================================

void loop() {
    static uint32_t lastToggle = 0;
    static bool     ledOn      = false;
    uint32_t now = millis();
    if (now - lastToggle >= (uint32_t)(ledOn ? 100 : 1900)) {
        lastToggle = now;
        ledOn = !ledOn;
        digitalWrite(LED_BUILTIN, ledOn ? LOW : HIGH);
    }
}

// ======================================================
// Scan callback
// ======================================================

void scan_callback(ble_gap_evt_adv_report_t* report) {
    int8_t rssi = report->rssi;

    uint8_t buf[2 + sizeof(AdvChannelPayload)];
    uint8_t len = Bluefruit.Scanner.parseReportByType(
        report,
        BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
        buf, sizeof(buf));

    if (len != sizeof(buf)) { Bluefruit.Scanner.resume(); return; }

    uint16_t cid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (cid != COMPANY_ID) { Bluefruit.Scanner.resume(); return; }

    AdvChannelPayload pkt;
    memcpy(&pkt, &buf[2], sizeof(pkt));

    uint32_t now = millis();

    NodeState* node = findOrCreateNode(pkt.nodeID);
    if (node == nullptr) { Bluefruit.Scanner.resume(); return; }

    bool     isDup       = false;
    uint16_t missingCount = 0;

    if (!node->haveLastSeq) {
        // First packet from this node: establish the baseline sequence
        // number. No gap/dup/out-of-order classification is possible yet.
        node->lastSeq      = pkt.seq;
        node->haveLastSeq  = true;
    } else {
        int16_t delta = (int16_t)(pkt.seq - node->lastSeq);
        if (delta == 1) {
            node->lastSeq = pkt.seq;
        } else if (delta > 1) {
            missingCount = (uint16_t)(delta - 1);
            node->lastSeq = pkt.seq;
        } else if (delta == 0) {
            isDup = true;
        } else {
            // delta < 0: out-of-order / late arrival — log it, don't regress
            // lastSeq and don't count it as a gap.
        }
    }

    digitalWrite(LED_RED, LOW);
    delay(30);
    digitalWrite(LED_RED, HIGH);

    // temp_cdeg/humidity_pct10 printed as the raw fixed-point wire values
    // (not converted back to float here), matching PAwR-ncs central's own
    // "EVT DOWNLOAD_DATA ... TEMP %d HUM %u ..." printk format exactly
    // (central/src/main.c) -- keeps both architectures' captured data in
    // the same units for direct comparison/analysis without a conversion
    // step introducing extra rounding on either side.
    Serial.print(now);                Serial.print(',');
    Serial.print(pkt.nodeID);         Serial.print(',');
    Serial.print(pkt.seq);            Serial.print(',');
    Serial.print(rssi);               Serial.print(',');
    Serial.print(pkt.tempCdeg);       Serial.print(',');
    Serial.print(pkt.humidityPct10);  Serial.print(',');
    Serial.print(pkt.flags);          Serial.print(',');
    Serial.print(isDup ? 1 : 0);      Serial.print(',');
    Serial.println(missingCount);

    Bluefruit.Scanner.resume();
}
