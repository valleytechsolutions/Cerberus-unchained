#pragma once
#include "probe_sniff_protocol.h"

/* Pure complete-frame parser. Explicit results; no allocation/SDK/radio. */
typedef enum {
    CERBERUS_PROBE_SNIFF_DECODE_IGNORE=0,
    CERBERUS_PROBE_SNIFF_DECODE_VALID,
    CERBERUS_PROBE_SNIFF_DECODE_MALFORMED,
    CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED,
} cerberus_probe_sniff_decode_result_t;
typedef struct {
    cerberus_probe_sniff_row_t rows[CERBERUS_PROBE_SNIFF_CAPACITY];
    cerberus_probe_sniff_summary_t summary;
    uint64_t session_id,armed_us,clock_high_water_us;
    bool accepting;
} cerberus_probe_sniff_table_t;

bool cerberus_probe_sniff_control_valid(const cerberus_probe_sniff_control_t *control,
    bool info,bool release,uint64_t requested_session);
bool cerberus_probe_sniff_channel_valid(uint8_t channel);
bool cerberus_probe_sniff_mac_valid(const uint8_t mac[6]);
bool cerberus_probe_sniff_row_valid(const cerberus_probe_sniff_row_t *row);
bool cerberus_probe_sniff_projection_valid(const cerberus_probe_sniff_projection_t *row);
bool cerberus_probe_sniff_summary_valid(const cerberus_probe_sniff_summary_t *summary);
/* Canonical map bits0..13=1..14; bits14..41=listed 5GHz channels36..177. */
uint8_t cerberus_probe_sniff_channel_at(uint8_t index);
cerberus_probe_sniff_decode_result_t cerberus_probe_sniff_decode(
    const uint8_t *frame,size_t length,uint8_t rx_channel,int rssi,
    uint64_t callback_us,cerberus_probe_sniff_row_t *out);
void cerberus_probe_sniff_table_begin(cerberus_probe_sniff_table_t *table,
    uint64_t session_id,uint64_t armed_us);
/* Caller serializes admission/commit. Stale epoch never mutates any counter. */
bool cerberus_probe_sniff_table_commit(cerberus_probe_sniff_table_t *table,
    uint64_t session_id,const cerberus_probe_sniff_row_t *row,uint64_t callback_us);
void cerberus_probe_sniff_table_rejected(cerberus_probe_sniff_table_t *table,
    uint64_t session_id,cerberus_probe_sniff_decode_result_t result,uint64_t callback_us);
void cerberus_probe_sniff_table_finish(cerberus_probe_sniff_table_t *table,
    uint64_t session_id,int64_t stopped_us);
bool cerberus_probe_sniff_table_copy_row(const cerberus_probe_sniff_table_t *table,
    uint64_t session_id,uint8_t index,int64_t copied_us,cerberus_probe_sniff_projection_t *out);
bool cerberus_probe_sniff_table_copy_summary(const cerberus_probe_sniff_table_t *table,
    uint64_t session_id,int64_t copied_us,cerberus_probe_sniff_summary_t *out);
