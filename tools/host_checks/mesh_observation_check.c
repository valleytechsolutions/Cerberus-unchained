/* Synthetic frames only. Exercise the production parser and observation model. */
#include "mesh_observation.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static mesh_observation_t model, before;
static const uint64_t session = UINT64_C(0x1234567800000001);

static void put(mesh_observation_input_t *in, uint64_t value, unsigned count)
{
    CHECK(count <= sizeof(in->mac) - in->length);
    for (unsigned i = 0; i < count; ++i)
        in->mac[in->length++] = (uint8_t)(value >> (8U * i));
}

static mesh_observation_input_t frame(uint32_t sequence, unsigned version,
    unsigned type, unsigned dm, unsigned sm, bool compressed, bool dp, bool sp,
    uint16_t dpan, uint16_t span, uint64_t destination, uint64_t source)
{
    mesh_observation_input_t in = {0};
    in.session = session;
    in.sequence = sequence;
    in.callback_us = (uint64_t)sequence * 100U;
    in.channel = 11U;
    in.flags = 7U;
    in.rssi = -60;
    in.lqi = 100U;
    put(&in, type | (compressed ? 0x40U : 0U) | (dm << 10U) |
        (version << 12U) | (sm << 14U), 2U);
    put(&in, 0x35U, 1U); /* Same MAC sequence is intentionally repeated. */
    if (dp) put(&in, dpan, 2U);
    if (dm) put(&in, destination, dm == 2U ? 2U : 8U);
    if (sp) put(&in, span, 2U);
    if (sm) put(&in, source, sm == 2U ? 2U : 8U);
    return in;
}

static mesh_observation_input_t pair(uint32_t seq, uint16_t source, uint16_t dest)
{
    return frame(seq, 1U, 1U, 2U, 2U, true, true, false,
                 0x1234U, 0U, dest, source);
}

static mesh_observation_input_t source_only(uint32_t seq, uint16_t pan, uint16_t source)
{
    return frame(seq, 1U, 1U, 0U, 2U, false, false, true, 0U, pan, 0U, source);
}

static void reset(void)
{
    CHECK(mesh_observation_init(&model, session));
}

static void reject_only(const mesh_observation_input_t *in,
                        mesh_observation_result_t expected, size_t offset)
{
    before = model;
    uint32_t *counter = (uint32_t *)((uint8_t *)&before.counters + offset);
    ++*counter;
    CHECK(mesh_observation_ingest(&model, in) == expected);
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
}

static void basic_and_selection(void)
{
    reset();
    mesh_observation_input_t in = pair(1U, 0x1111U, 0x2222U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.pan_count == 1U && model.address_count == 2U && model.edge_count == 1U);
    CHECK(model.pans[0].seen.frames == 1U && model.pans[0].origin_bits == 6U);
    CHECK(model.addresses[0].source_frames == 1U && !model.addresses[0].destination_frames);
    CHECK(model.addresses[0].source_rssi == -60 && model.addresses[0].source_flags == 7U);
    CHECK(model.addresses[1].destination_frames == 1U && !model.addresses[1].source_frames);
    CHECK(!model.addresses[1].source_flags && !model.addresses[1].last_source_sequence);
    CHECK(model.addresses[1].last_source_witness == MESH_OBSERVATION_NO_WITNESS);
    CHECK(model.edges[0].source == 0U && model.edges[0].destination == 1U);
    CHECK(model.edges[0].last_sequence == 0x35U && model.edges[0].last_sequence_present);

    mesh_observation_selection_t selected, retained;
    CHECK(mesh_observation_select(&model, MESH_OBSERVATION_SELECT_EDGE, 0U, &selected));
    retained = selected;
    CHECK(selected.witness_valid && selected.witness.input.sequence == 1U);
    in = pair(2U, 0x1111U, 0x2222U);
    in.rssi = -80;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.address_count == 2U && model.edge_count == 1U && model.edges[0].seen.frames == 2U);
    CHECK(model.addresses[0].source_rssi == -80 && model.addresses[0].last_source_sequence == 2U);
    CHECK(!memcmp(&selected, &retained, sizeof(selected)));
    CHECK(model.counters.headers == 2U && model.witness_count == 2U);
    reject_only(&in, MESH_OBSERVATION_REPLAY, offsetof(mesh_observation_counters_t, replayed));
    in.sequence = 1U; in.mac[3] ^= 1U;
    reject_only(&in, MESH_OBSERVATION_REPLAY, offsetof(mesh_observation_counters_t, replayed));
    in = pair(3U, 0x2222U, 0x1111U);
    in.rssi = -30;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.edge_count == 2U && model.edges[1].source == 1U && model.edges[1].destination == 0U);
    CHECK(model.addresses[0].source_rssi == -80 && model.addresses[0].last_source_sequence == 2U);
    CHECK(model.addresses[0].last_source_witness == 1U && model.addresses[0].seen.last_sequence == 3U);
    CHECK(model.addresses[1].source_rssi == -30 && model.addresses[1].source_frames == 1U);
    CHECK(mesh_observation_select(&model, MESH_OBSERVATION_SELECT_ADDRESS, 0U, &selected));
    CHECK(selected.witness.input.sequence == 3U && selected.record.address.last_source_sequence == 2U);
    CHECK(mesh_observation_select(&model, MESH_OBSERVATION_SELECT_WITNESS,
          selected.record.address.last_source_witness, &selected));
    CHECK(selected.witness.input.rssi == -80 && selected.witness.input.sequence == 2U);

    retained = selected;
    CHECK(!mesh_observation_select(&model, MESH_OBSERVATION_SELECT_ADDRESS, 64U, &selected));
    CHECK(!memcmp(&selected, &retained, sizeof(selected)));
    CHECK(!mesh_observation_select(&model, (mesh_observation_kind_t)99, 0U, &selected));
    CHECK(!memcmp(&selected, &retained, sizeof(selected)));
    before = model;
    CHECK(!mesh_observation_select(&model, MESH_OBSERVATION_SELECT_PAN, 0U,
          (mesh_observation_selection_t *)&model.pans[0]));
    CHECK(!memcmp(&before, &model, sizeof(model)));
    CHECK(mesh_observation_ingest(&model, &model.witnesses[0].input) == MESH_OBSERVATION_INVALID);
    CHECK(!memcmp(&before, &model, sizeof(model)));
    CHECK(!mesh_observation_init(&model, 0U) && !memcmp(&before, &model, sizeof(model)));
    CHECK(mesh_observation_init(&model, session + 1U));
    reject_only(&in, MESH_OBSERVATION_WRONG_SESSION, offsetof(mesh_observation_counters_t, wrong_session));
    CHECK(!model.address_count && !model.edge_count && !model.witness_count);
}

static void metadata_and_clock(void)
{
    reset();
    mesh_observation_input_t good = pair(1U, 1U, 2U), in;
    for (unsigned which = 0; which < 13U; ++which) {
        in = good;
        switch (which) {
        case 0: in.sequence = 0U; break;
        case 1: in.sequence = UINT32_C(0x80000000); break;
        case 2: in.channel = 10U; break;
        case 3: in.channel = 27U; break;
        case 4: in.flags = 8U; break;
        case 5: in.flags &= ~MESH_OBSERVATION_TIME_VALID; break;
        case 6: in.flags &= ~MESH_OBSERVATION_RSSI_VALID; break;
        case 7: in.flags &= ~MESH_OBSERVATION_LQI_VALID; break;
        case 8: in.length = 0U; break;
        case 9: in.length = 1U; break;
        case 10: in.length = 126U; break;
        case 11: in.reserved[2] = 1U; break;
        default: in.mac[124] = 1U; break;
        }
        reject_only(&in, MESH_OBSERVATION_INVALID, offsetof(mesh_observation_counters_t, invalid_inputs));
    }
    reject_only(NULL, MESH_OBSERVATION_INVALID, offsetof(mesh_observation_counters_t, invalid_inputs));
    good.callback_us = 0U; /* Zero is a valid first timestamp. */
    CHECK(mesh_observation_ingest(&model, &good) == MESH_OBSERVATION_OK);
    CHECK(model.addresses[0].seen.first_time_valid && !model.addresses[0].seen.first_us);
    in = pair(2U, 1U, 2U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    in.sequence = 3U; in.callback_us = 199U;
    reject_only(&in, MESH_OBSERVATION_INVALID, offsetof(mesh_observation_counters_t, invalid_clock));
    in.callback_us = 0U; in.rssi = 0; in.lqi = 0U; in.flags = 0U;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(!model.addresses[0].source_flags && !model.addresses[0].seen.last_time_valid);
    CHECK(model.addresses[0].seen.first_time_valid && model.last_valid_us == 200U);
    in.sequence = 4U; in.callback_us = 199U; in.flags = MESH_OBSERVATION_TIME_VALID;
    reject_only(&in, MESH_OBSERVATION_INVALID, offsetof(mesh_observation_counters_t, invalid_clock));
    in.callback_us = 200U;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    in.sequence = 7U;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.counters.delivery_sequence_gaps == 2U && model.counters.records == 5U);
    in.sequence = MESH_OBSERVATION_MAX_SEQUENCE;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    reject_only(&in, MESH_OBSERVATION_REPLAY, offsetof(mesh_observation_counters_t, replayed));
    in.sequence++;
    reject_only(&in, MESH_OBSERVATION_INVALID, offsetof(mesh_observation_counters_t, invalid_inputs));
}

static void identities_and_non_edges(void)
{
    reset();
    mesh_observation_input_t in = source_only(1U, 0U, 0x1234U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    in = frame(2U, 2U, 1U, 0U, 2U, true, false, false, 0U, 0U, 0U, 0x1234U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.pan_count == 2U && model.address_count == 2U);
    CHECK(model.addresses[0].key.pan.known && !model.addresses[1].key.pan.known);
    in = source_only(3U, 0U, 0x1234U); in.channel = 12U;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    in = source_only(4U, 1U, 0x1234U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.address_count == 4U && model.pan_count == 4U);
    in = frame(5U, 1U, 1U, 0U, 3U, false, false, true, 0U, 0U, 0U, 0x1234U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.address_count == 5U && model.addresses[4].key.mode == MESH_MAC_ADDRESS_EXT64);
    in = frame(6U, 2U, 1U, 3U, 3U, false, true, false, 0x1234U, 0U,
        UINT64_C(0x0102030405060708), UINT64_C(0xfedcba9876543210));
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.addresses[5].key.value == UINT64_C(0xfedcba9876543210));
    CHECK(!model.addresses[5].key.pan.known && model.addresses[6].key.pan.known);
    CHECK(model.addresses[6].key.value == UINT64_C(0x0102030405060708));
    CHECK(model.edge_count == 1U);

    reset();
    in = pair(1U, 7U, 8U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    in = source_only(2U, 0x1234U, 7U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.address_count == 2U && model.addresses[0].origin_bits == 6U);
    CHECK(model.pans[0].seen.frames == 2U); /* Same PAN twice in one packet counted once. */
    in = pair(3U, 7U, 7U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.addresses[0].seen.frames == 3U && model.addresses[0].source_frames == 3U);
    CHECK(model.addresses[0].destination_frames == 1U && model.edge_count == 2U);

    reset();
    in = pair(1U, 0xffffU, 0xfffeU);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(!model.address_count && !model.edge_count && model.pan_count == 1U);
    in = pair(2U, 7U, 0xffffU);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.address_count == 1U && !model.edge_count);
    in = frame(3U, 0U, 2U, 0U, 0U, false, false, false, 0U, 0U, 0U, 0U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.address_count == 1U && !model.edge_count && model.pans[0].seen.frames == 2U);
    in = frame(4U, 2U, 1U, 0U, 0U, true, true, false, 0x7777U, 0U, 0U, 0U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.pan_count == 2U && model.address_count == 1U && !model.edge_count);
}

static void parser_outcomes(void)
{
    reset();
    mesh_observation_input_t in = pair(1U, 1U, 2U);
    memset(in.mac, 0, sizeof(in.mac)); in.length = 2U;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_MALFORMED);
    CHECK(model.last_sequence == 1U && model.counters.malformed == 1U && model.witness_count == 1U);
    CHECK(model.witnesses[0].parse_result == MESH_MAC_MALFORMED);
    reject_only(&in, MESH_OBSERVATION_REPLAY, offsetof(mesh_observation_counters_t, replayed));
    in = frame(2U, 3U, 1U, 0U, 0U, false, false, false, 0U, 0U, 0U, 0U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_UNSUPPORTED);
    CHECK(model.counters.unsupported == 1U && !model.address_count);
    in = pair(3U, 1U, 2U);
    in.mac[0] |= 8U; /* Valid v1 encrypted header; payload remains opaque. */
    put(&in, 5U, 1U); put(&in, UINT32_C(0x12345678), 4U);
    put(&in, 0xabcdefU, 3U); put(&in, 0U, 4U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.edges[0].secured_frames == 1U && model.edges[0].encrypted_frames == 1U);
    CHECK(model.counters.records == 3U && model.counters.headers == 1U);
    CHECK(model.witnesses[2].input.length == in.length &&
          !memcmp(model.witnesses[2].input.mac, in.mac, sizeof(in.mac)));
}

static void capacities_and_saturation(void)
{
    reset();
    mesh_observation_input_t in;
    for (unsigned i = 0; i < 17U; ++i) {
        in = source_only(i + 1U, (uint16_t)i, (uint16_t)i);
        CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    }
    CHECK(model.pan_count == 16U && model.address_count == 17U && model.counters.pan_omissions == 1U);
    in = source_only(18U, 0U, 0U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.pans[0].seen.frames == 2U && model.pans[15].key.value == 15U);

    reset();
    for (unsigned i = 0; i < 32U; ++i) {
        in = pair(i + 1U, (uint16_t)(2U * i), (uint16_t)(2U * i + 1U));
        CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    }
    CHECK(model.address_count == 64U && model.edge_count == 32U);
    in = pair(33U, 100U, 1U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.counters.address_omissions == 1U && model.counters.edge_omissions == 1U);
    CHECK(model.addresses[63].key.value == 63U && model.addresses[1].destination_frames == 2U);
    in = pair(34U, 0U, 1U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK && model.edges[0].seen.frames == 2U);

    reset();
    uint32_t sequence = 0U;
    for (uint16_t s = 0; s < 9U; ++s) for (uint16_t d = 0; d < 9U; ++d) {
        if (s == d) continue;
        in = pair(++sequence, s, d);
        CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    }
    CHECK(sequence == 72U && model.edge_count == 64U && model.address_count == 9U);
    CHECK(model.counters.edge_omissions == 8U && model.counters.witness_omissions == 8U);
    CHECK(model.witness_count == 64U && model.witnesses[63].input.sequence == 64U);
    in = pair(73U, 0U, 1U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.edges[0].seen.frames == 2U && model.edges[0].seen.first_witness == 0U);
    CHECK(model.edges[0].seen.last_witness == MESH_OBSERVATION_NO_WITNESS);
    mesh_observation_selection_t selected;
    CHECK(mesh_observation_select(&model, MESH_OBSERVATION_SELECT_EDGE, 0U, &selected));
    CHECK(!selected.witness_valid && !selected.witness.input.session);
    CHECK(mesh_observation_select(&model, MESH_OBSERVATION_SELECT_WITNESS, 0U, &selected));
    CHECK(selected.witness_valid && selected.witness.input.sequence == 1U);

    model.counters.records = UINT32_MAX - 1U;
    model.edges[0].seen.frames = UINT32_MAX - 1U;
    model.addresses[0].source_frames = UINT32_MAX;
    in = pair(74U, 0U, 1U);
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.counters.saturated && model.counters.records == UINT32_MAX);
    CHECK(model.edges[0].seen.frames == UINT32_MAX && model.addresses[0].source_frames == UINT32_MAX);
    in.sequence = 75U;
    CHECK(mesh_observation_ingest(&model, &in) == MESH_OBSERVATION_OK);
    CHECK(model.counters.records == UINT32_MAX && model.edges[0].seen.last_sequence == 75U);
}

static void random_boundaries(void)
{
    reset();
    uint32_t random = UINT32_C(0x12345678);
    for (uint32_t sequence = 1U; sequence <= 4000U; ++sequence) {
        mesh_observation_input_t in = pair(sequence, 1U, 2U);
        memset(in.mac, 0, sizeof(in.mac));
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        in.length = (uint8_t)(2U + random % 124U);
        for (unsigned j = 0; j < in.length; ++j) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            in.mac[j] = (uint8_t)(random >> 24U);
        }
        mesh_mac_observation_t parsed;
        mesh_mac_result_t result = mesh_mac_parse(in.mac, in.length, &parsed);
        mesh_observation_result_t expected = result == MESH_MAC_HEADER_OK ? MESH_OBSERVATION_OK :
            result == MESH_MAC_UNSUPPORTED ? MESH_OBSERVATION_UNSUPPORTED : MESH_OBSERVATION_MALFORMED;
        CHECK(mesh_observation_ingest(&model, &in) == expected);
        CHECK(model.last_sequence == sequence && model.counters.records == sequence);
        CHECK(model.counters.headers + model.counters.malformed + model.counters.unsupported == sequence);
        CHECK(model.pan_count <= 16U && model.address_count <= 64U && model.edge_count <= 64U);
        CHECK(model.witness_count == (sequence < 64U ? sequence : 64U));
        for (unsigned i = 0; i < model.edge_count; ++i) {
            CHECK(model.edges[i].source < model.address_count && model.edges[i].destination < model.address_count);
            for (unsigned j = 0; j < i; ++j)
                CHECK(model.edges[i].source != model.edges[j].source || model.edges[i].destination != model.edges[j].destination);
        }
        if (sequence % 100U == 0U)
            reject_only(&in, MESH_OBSERVATION_REPLAY, offsetof(mesh_observation_counters_t, replayed));
    }
    CHECK(model.counters.witness_omissions == 3936U && !model.counters.delivery_sequence_gaps);
    CHECK(model.witnesses[0].input.sequence == 1U && model.witnesses[63].input.sequence == 64U);
}

int main(void)
{
    CHECK(!mesh_observation_init(NULL, session));
    CHECK(mesh_observation_ingest(NULL, NULL) == MESH_OBSERVATION_INVALID);
    basic_and_selection();
    metadata_and_clock();
    identities_and_non_edges();
    parser_outcomes();
    capacities_and_saturation();
    random_boundaries();
    printf("PASS: %lu mesh observation checks; model=%zu input=%zu selection=%zu witness=%zu bytes.\n",
        checks, sizeof(model), sizeof(mesh_observation_input_t), sizeof(mesh_observation_selection_t),
        sizeof(mesh_observation_witness_t));
    return 0;
}
