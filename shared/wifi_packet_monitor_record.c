#include "wifi_packet_monitor_record.h"
#include "cerberus_protocol.h"

static void drop_invalid(cerberus_monitor_record_store_t *s)
{
    unsigned n = atomic_load_explicit(&s->ring.dropped_invalid, memory_order_relaxed);
    if (n != UINT32_MAX)
        atomic_store_explicit(&s->ring.dropped_invalid, n + 1U, memory_order_relaxed);
}

bool cerberus_monitor_record_init(cerberus_monitor_record_store_t *s, uint64_t session)
{
    if (!s || !session) return false;
    memset(s, 0, sizeof(*s));
    s->session = session;
    s->meta_epoch = CERBERUS_PACKET_COUNT_PIN_BASE;
    s->stats_epoch = CERBERUS_PACKET_COUNT_PIN_BASE - 1U;
    atomic_init(&s->clock_invalid, false);
    atomic_init(&s->exhausted, false);
    cerberus_wifi_capture_ring_init(&s->ring);
    return cerberus_wifi_capture_ring_start(&s->ring, 1U);
}

cerberus_wifi_capture_push_result_t cerberus_monitor_record_push(
    cerberus_monitor_record_store_t *s, const uint8_t *frame, size_t length,
    uint8_t channel, int8_t rssi, int64_t source_us)
{
    if (!s) return CERBERUS_WIFI_CAPTURE_DROP_INVALID;
    if (source_us < 0 || (uint64_t)source_us < s->last_source_us)
        atomic_store_explicit(&s->clock_invalid, true, memory_order_release);
    if (atomic_load_explicit(&s->ring.next_sequence, memory_order_acquire) >=
        CERBERUS_PACKET_COUNT_SEQUENCE_MAX)
        atomic_store_explicit(&s->exhausted, true, memory_order_release);
    if (atomic_load_explicit(&s->clock_invalid, memory_order_acquire) ||
        atomic_load_explicit(&s->exhausted, memory_order_acquire) ||
        !frame || length < 24U || !cerberus_wifi_packet_count_channel_bit(channel)) {
        drop_invalid(s);
        return CERBERUS_WIFI_CAPTURE_DROP_INVALID;
    }
    s->last_source_us = (uint64_t)source_us;
    bool signal_valid = rssi >= -127 && rssi <= 0;
    cerberus_wifi_pcap_metadata_t metadata = {
        .channel = channel, .rssi_dbm = signal_valid ? rssi : 0,
        .rssi_known = signal_valid, .time_kind = CERBERUS_WIFI_PCAP_TIME_MONOTONIC,
        .capture_time_us = (uint64_t)source_us,
    };
    return cerberus_wifi_capture_ring_push(&s->ring, 1U, &metadata, frame, length, length);
}

void cerberus_monitor_record_stop(cerberus_monitor_record_store_t *s)
{ if (s) cerberus_wifi_capture_ring_stop(&s->ring); }

bool cerberus_monitor_record_pin(cerberus_monitor_record_store_t *s,
    uint64_t session, bool statistics, uint32_t *pin)
{
    if (pin) *pin = 0U;
    if (!s || !pin || !session || session != s->session) return false;
    uint32_t *epoch = statistics ? &s->stats_epoch : &s->meta_epoch;
    if (*epoch > UINT32_MAX - 2U) return false;
    if (statistics) {
        cerberus_wifi_capture_ring_stats_t r;
        cerberus_wifi_capture_ring_stats(&s->ring, &r);
        memset(s->stats, 0, sizeof(s->stats));
        s->stats[0] = 1U;
        s->stats[1] = (r.active ? 1U : 0U) |
            (atomic_load_explicit(&s->clock_invalid, memory_order_acquire) ? 2U : 0U) |
            (atomic_load_explicit(&s->exhausted, memory_order_acquire) ? 4U : 0U);
        s->stats[2] = r.pending;
        s->stats[3] = r.high_watermark;
        const uint32_t values[] = {r.accepted, s->acked, r.dropped_full,
            r.dropped_oversize, r.dropped_truncated, r.dropped_invalid,
            r.dropped_stopped, r.dropped_stale,
            atomic_load_explicit(&s->ring.next_sequence, memory_order_acquire)};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            cerberus_packet_count_put(s->stats + 4U + i * 4U, values[i], 4U);
        s->stats_pin = *epoch += 2U;
        *pin = s->stats_pin;
    } else {
        const cerberus_wifi_capture_record_t *r = cerberus_wifi_capture_ring_peek(&s->ring);
        if (!r) return false;
        memset(s->metadata, 0, sizeof(s->metadata));
        cerberus_packet_count_put(s->metadata, r->sequence, 4U);
        cerberus_packet_count_put(s->metadata + 4U, r->captured_length, 2U);
        cerberus_packet_count_put(s->metadata + 6U, r->original_length, 2U);
        cerberus_packet_count_put(s->metadata + 8U, r->metadata.capture_time_us, 8U);
        cerberus_packet_count_put(s->metadata + 16U,
            cerberus_protocol_crc32(r->frame, r->captured_length), 4U);
        s->metadata[20] = r->metadata.channel;
        s->metadata[21] = (uint8_t)r->metadata.rssi_dbm;
        s->metadata[22] = 1U | (r->metadata.rssi_known ? 2U : 0U);
        s->meta_pin = *epoch += 2U;
        *pin = s->meta_pin;
    }
    return true;
}

bool cerberus_monitor_record_read(const cerberus_monitor_record_store_t *s,
    uint64_t session, uint32_t pin, uint16_t part, bool raw, uint8_t *data)
{
    size_t chunk = raw ? CERBERUS_PACKET_COUNT_RAW_CHUNK_BYTES : CERBERUS_PACKET_COUNT_CHUNK_BYTES;
    if (!data) return false;
    memset(data, 0, chunk);
    if (!s || !session || session != s->session || !pin) return false;
    const uint8_t *bytes = NULL;
    size_t size = 0U;
    if (raw) {
        const cerberus_wifi_capture_record_t *r = cerberus_wifi_capture_ring_peek(&s->ring);
        if (r && r->sequence == pin) { bytes = r->frame; size = r->captured_length; }
    } else if (pin == s->meta_pin) { bytes = s->metadata; size = sizeof(s->metadata); }
    else if (pin == s->stats_pin) { bytes = s->stats; size = sizeof(s->stats); }
    size_t offset = (size_t)part * chunk;
    if (!bytes || offset >= size) return false;
    size_t amount = size - offset;
    if (amount > chunk) amount = chunk;
    memcpy(data, bytes + offset, amount);
    return true;
}

bool cerberus_monitor_record_ack(cerberus_monitor_record_store_t *s,
    uint64_t session, uint32_t sequence)
{
    if (!s || !session || session != s->session || !sequence ||
        sequence > CERBERUS_PACKET_COUNT_SEQUENCE_MAX) return false;
    if (sequence == s->last_acked) return true;
    if (!cerberus_wifi_capture_ring_ack(&s->ring, 1U, sequence)) return false;
    s->last_acked = sequence;
    if (s->acked != UINT32_MAX) ++s->acked;
    return true;
}
