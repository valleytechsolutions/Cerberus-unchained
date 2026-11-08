#pragma once

#include "wardrive_log.h"

#define CERBERUS_WARDRIVE_RSSI_SETS 128U
#define CERBERUS_WARDRIVE_RSSI_WAYS 4U

typedef struct {
    uint8_t address[6], type; /* Zero type marks an unused entry. */
    int8_t weakest, strongest;
} cerberus_wardrive_rssi_entry_t;

typedef struct {
    cerberus_wardrive_rssi_entry_t entries[CERBERUS_WARDRIVE_RSSI_SETS][CERBERUS_WARDRIVE_RSSI_WAYS];
    uint8_t next[CERBERUS_WARDRIVE_RSSI_SETS];
} cerberus_wardrive_rssi_filter_t;

/* Zero-initialize once per recording. The sole SD writer calls these only
 * after GPS/time/CSV validation. Identity matches final compaction: address
 * plus WIFI/BLE, independent of radio, BLE address type, name or channel.
 * A miss, eviction or absent cache permits another append; it cannot discard
 * a new extreme. Final compaction still decides the published rows. */
bool cerberus_wardrive_rssi_needs_append(const cerberus_wardrive_rssi_filter_t *filter,
    const cerberus_ward_log_identity_t *identity);

/* Call only after the complete row was accepted by the journal. This records
 * accepted appends, not a guarantee of persistence before the next SD sync.
 * No concurrent callers, file I/O or allocation occurs inside the filter. */
void cerberus_wardrive_rssi_record_append(cerberus_wardrive_rssi_filter_t *filter,
    const cerberus_ward_log_identity_t *identity);
