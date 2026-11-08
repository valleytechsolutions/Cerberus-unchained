#pragma once
#include "ble_response.h"
/* Only allocated for explicitly requested response scans. No extra permanent
 * inventory bank: eight short-lived original candidates support promotion. */
typedef struct {
    cerberus_dual_ble_response_wire_t wire;
    uint64_t original_us, response_us;
} c5_response_live_t;
typedef struct {
    cerberus_ble_record_t original;
    cerberus_dual_ble_family_wire_t family;
    uint64_t sequence, original_us;
} c5_response_candidate_t;
typedef struct {
    c5_response_live_t live[32];
    uint8_t frozen[32][152];
    c5_response_candidate_t candidates[8];
    uint64_t sequence, clock_highwater;
    bool clock_valid;
} c5_response_context_t;
