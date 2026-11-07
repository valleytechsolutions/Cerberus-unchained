#undef NDEBUG
#include "wifi_packet_monitor_record.h"
#include "cerberus_protocol.h"
#include "wifi_pcap.h"
#include <assert.h>
#include <stdio.h>

static cerberus_monitor_record_store_t store;
static uint8_t frame[CERBERUS_WIFI_PCAP_FRAME_MAX + 1U];
static uint8_t recovered[CERBERUS_WIFI_PCAP_FRAME_MAX];

static void transfer(uint64_t session, uint32_t expected_sequence, size_t length)
{
    uint32_t pin = 0U, stats = 0U;
    assert(cerberus_monitor_record_pin(&store, session, false, &pin));
    assert(pin > CERBERUS_PACKET_COUNT_PIN_BASE && !(pin & 1U));
    uint8_t metadata[32], part[41];
    assert(cerberus_monitor_record_pin(&store, session, true, &stats));
    assert(stats > CERBERUS_PACKET_COUNT_PIN_BASE && (stats & 1U));
    for (unsigned i = 0U; i < 4U; ++i) {
        assert(cerberus_monitor_record_read(&store, session, pin, i, false, metadata + i * 8U));
    }
    assert(cerberus_packet_count_get(metadata, 4U) == expected_sequence);
    assert(cerberus_packet_count_get(metadata + 4U, 2U) == length);
    assert(cerberus_packet_count_get(metadata + 6U, 2U) == length);
    assert(cerberus_packet_count_get(metadata + 8U, 8U) == 123456789U);
    assert(metadata[20] == 36U && (int8_t)metadata[21] == -55 && metadata[22] == 3U);
    for (unsigned i = 23U; i < sizeof(metadata); ++i) assert(metadata[i] == 0U);
    for (unsigned offset = 0U; offset < length; offset += 40U) {
        memset(part, 0xa5, sizeof(part));
        assert(cerberus_monitor_record_read(&store, session, expected_sequence,
            (uint16_t)(offset / 40U), true, part));
        assert(part[40] == 0xa5);
        size_t amount = length - offset;
        if (amount > 40U) amount = 40U;
        memcpy(recovered + offset, part, amount);
        for (size_t i = amount; i < 40U; ++i) assert(part[i] == 0U);
    }
    assert(memcmp(recovered, frame, length) == 0);
    assert(cerberus_packet_count_get(metadata + 16U, 4U) == cerberus_protocol_crc32(recovered, length));
    assert(!cerberus_monitor_record_read(&store, session, expected_sequence, UINT16_MAX, true, part));
    assert(!cerberus_monitor_record_ack(&store, session + 1U, expected_sequence));
    assert(!cerberus_monitor_record_ack(&store, session, expected_sequence + 1U));
    assert(cerberus_monitor_record_ack(&store, session, expected_sequence));
    assert(cerberus_monitor_record_ack(&store, session, expected_sequence));
    assert(!cerberus_monitor_record_read(&store, session, expected_sequence, 0U, true, part));
    for (unsigned i = 0U; i < 40U; ++i) assert(part[i] == 0U);
    assert(cerberus_monitor_record_read(&store, session, pin, 0U, false, part));
    assert(memcmp(part, metadata, 8U) == 0); /* Pinned metadata survives its ACK. */
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)i;
    assert(!cerberus_monitor_record_init(NULL, 1U));
    assert(!cerberus_monitor_record_init(&store, 0U));
    const size_t lengths[] = {24U, 41U, 4096U};
    for (unsigned cycle = 0U; cycle < 300U; ++cycle) {
        uint64_t session = UINT64_C(0x1234000000000000) + cycle;
        assert(cerberus_monitor_record_init(&store, session));
        uint32_t pin = UINT32_MAX;
        assert(!cerberus_monitor_record_pin(&store, session, false, &pin) && !pin);
        assert(!cerberus_monitor_record_pin(&store, session + 1U, true, &pin) && !pin);
        for (unsigned n = 0U; n < 3U; ++n) {
            assert(cerberus_monitor_record_push(&store, frame, lengths[n], 36U, -55, 123456789) == CERBERUS_WIFI_CAPTURE_ACCEPTED);
            transfer(session, n + 1U, lengths[n]);
        }
        assert(store.acked == 3U);
        cerberus_monitor_record_stop(&store);
        assert(cerberus_monitor_record_push(&store, frame, 24U, 36U, -55, 123456789) == CERBERUS_WIFI_CAPTURE_DROP_STOPPED);
        assert(cerberus_monitor_record_pin(&store, session, true, &pin));
        assert(store.stats[0] == 1U && store.stats[1] == 0U && store.stats[2] == 0U);
        assert(cerberus_packet_count_get(store.stats + 4U, 4U) == 3U);
        assert(cerberus_packet_count_get(store.stats + 8U, 4U) == 3U);
        assert(cerberus_packet_count_get(store.stats + 28U, 4U) == 1U);
    }
    assert(cerberus_monitor_record_init(&store, 9U));
    assert(cerberus_monitor_record_push(&store, frame, 4097U, 1U, -55, 1) == CERBERUS_WIFI_CAPTURE_DROP_OVERSIZE);
    assert(cerberus_monitor_record_push(&store, frame, 23U, 1U, -55, 2) == CERBERUS_WIFI_CAPTURE_DROP_INVALID);
    assert(cerberus_monitor_record_push(&store, frame, 24U, 37U, -55, 2) == CERBERUS_WIFI_CAPTURE_DROP_INVALID);
    for (unsigned n = 0U; n < 2U; ++n)
        assert(cerberus_monitor_record_push(&store, frame, 24U, 1U, INT8_MIN, 3) == CERBERUS_WIFI_CAPTURE_ACCEPTED);
    assert(cerberus_monitor_record_push(&store, frame, 24U, 1U, -55, 4) == CERBERUS_WIFI_CAPTURE_DROP_FULL);
    uint32_t pin, old_pin;
    assert(cerberus_monitor_record_pin(&store, 9U, false, &old_pin));
    assert(store.metadata[21] == 0U && store.metadata[22] == 1U);
    assert(cerberus_monitor_record_pin(&store, 9U, true, &pin));
    assert(cerberus_packet_count_get(store.stats + 12U, 4U) == 1U);
    assert(cerberus_packet_count_get(store.stats + 16U, 4U) == 1U);
    assert(cerberus_packet_count_get(store.stats + 24U, 4U) == 2U);
    assert(store.stats[2] == 2U && store.stats[3] == 2U);
    assert(cerberus_monitor_record_pin(&store, 9U, false, &pin) && pin != old_pin);
    uint8_t data[40];
    assert(!cerberus_monitor_record_read(&store, 9U, old_pin, 0U, false, data));
    store.meta_epoch = UINT32_MAX - 1U;
    assert(!cerberus_monitor_record_pin(&store, 9U, false, &pin) && pin == 0U);
    assert(cerberus_monitor_record_push(&store, frame, 24U, 1U, -55, 2) == CERBERUS_WIFI_CAPTURE_DROP_INVALID);
    assert(atomic_load(&store.clock_invalid));
    assert(cerberus_monitor_record_push(&store, frame, 24U, 1U, -55, 10) == CERBERUS_WIFI_CAPTURE_DROP_INVALID);
    assert(cerberus_monitor_record_init(&store, 10U));
    atomic_store(&store.ring.next_sequence, CERBERUS_PACKET_COUNT_SEQUENCE_MAX);
    assert(cerberus_monitor_record_push(&store, frame, 24U, 1U, -55, 1) == CERBERUS_WIFI_CAPTURE_DROP_INVALID);
    assert(atomic_load(&store.exhausted));
    assert(cerberus_monitor_record_init(&store, 11U));
    assert(cerberus_monitor_record_push(&store, frame, 24U, 1U, -55, -1) == CERBERUS_WIFI_CAPTURE_DROP_INVALID);
    assert(atomic_load(&store.clock_invalid));
    puts("Monitor record store: 300 session/transfer cycles, immutable pins, exact ACK and loss boundaries passed");
    return 0;
}
