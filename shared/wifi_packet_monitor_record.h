#pragma once

#include "wifi_capture_ring.h"
#include "wifi_packet_count_protocol.h"

/* Packet Monitor reuses Packet Count's raw transfer encoding, not its target
 * table. Only the owning scanner may allocate/free this optional store. One
 * RX callback produces; scanner_mutex serializes all pin/read/ack operations.
 * Stop closes admission. Detach callbacks and wait for them before freeing. */
#define CERBERUS_MONITOR_RECORD_MIN_FIRMWARE 78U
#define CERBERUS_MONITOR_RECORD_RETENTION_MS 30000U
#define CERBERUS_MONITOR_RECORD_STATS_BYTES 64U
#define CERBERUS_MONITOR_RECORD_START 177U
#define CERBERUS_MONITOR_RECORD_PEEK 178U
#define CERBERUS_MONITOR_RECORD_READ 179U
#define CERBERUS_MONITOR_RECORD_RAW_READ 180U
#define CERBERUS_MONITOR_RECORD_RAW_ACK 181U
#define CERBERUS_MONITOR_RECORD_RELEASE 182U
#define CERBERUS_MONITOR_RECORD_STATUS 183U

typedef struct {
    cerberus_wifi_capture_ring_t ring;
    uint64_t session, last_source_us;
    uint32_t meta_pin, stats_pin, meta_epoch, stats_epoch, last_acked, acked;
    uint8_t metadata[CERBERUS_PACKET_COUNT_RAW_META_BYTES];
    uint8_t stats[CERBERUS_MONITOR_RECORD_STATS_BYTES];
    atomic_bool clock_invalid, exhausted;
} cerberus_monitor_record_store_t;

bool cerberus_monitor_record_init(cerberus_monitor_record_store_t *store, uint64_t session);
cerberus_wifi_capture_push_result_t cerberus_monitor_record_push(
    cerberus_monitor_record_store_t *store, const uint8_t *frame, size_t length,
    uint8_t channel, int8_t rssi, int64_t source_us);
void cerberus_monitor_record_stop(cerberus_monitor_record_store_t *store);
/* Separate metadata/statistics pins cannot invalidate one another. A new pin
 * of the same kind retires its predecessor; pin and record counters never wrap.
 * A metadata pin stays immutable after ACK, but raw reads require current head. */
bool cerberus_monitor_record_pin(cerberus_monitor_record_store_t *store,
    uint64_t session, bool statistics, uint32_t *pin);
bool cerberus_monitor_record_read(const cerberus_monitor_record_store_t *store,
    uint64_t session, uint32_t pin, uint16_t part, bool raw, uint8_t *data);
bool cerberus_monitor_record_ack(cerberus_monitor_record_store_t *store,
    uint64_t session, uint32_t sequence);

/* Statistics v1: version0, flags1 (active1, clock-invalid2, exhausted4),
 * pending2/high-water3; LE32 accepted4, acked8, full12, oversize16, truncated20,
 * invalid24, stopped28, stale32, last-sequence36; reserved40..63 zero.
 * Live counters are sampled, not an atomic RF snapshot. After producer drain,
 * the final projection is stable except for consumer acknowledgements. */
static inline size_t cerberus_monitor_record_request_bytes(uint8_t command)
{
    switch (command) {
    case CERBERUS_MONITOR_RECORD_PEEK: return sizeof(cerberus_packet_count_peek_t);
    case CERBERUS_MONITOR_RECORD_READ:
    case CERBERUS_MONITOR_RECORD_RAW_READ: return sizeof(cerberus_packet_count_read_t);
    case CERBERUS_MONITOR_RECORD_RAW_ACK: return sizeof(cerberus_packet_count_ack_t);
    case CERBERUS_MONITOR_RECORD_RELEASE:
    case CERBERUS_MONITOR_RECORD_STATUS: return sizeof(cerberus_packet_count_request_t);
    default: return SIZE_MAX;
    }
}
