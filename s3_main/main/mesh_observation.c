#include "mesh_observation.h"

#include <string.h>

static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}

static bool model_valid(const mesh_observation_t *m)
{
    return m && m->session && m->pan_count <= MESH_OBSERVATION_PANS &&
        m->address_count <= MESH_OBSERVATION_ADDRESSES &&
        m->edge_count <= MESH_OBSERVATION_EDGES &&
        m->witness_count <= MESH_OBSERVATION_WITNESSES;
}

static void add(mesh_observation_t *m, uint32_t *counter, uint32_t amount)
{
    if (amount >= UINT32_MAX - *counter) {
        *counter = UINT32_MAX;
        m->counters.saturated = true;
    } else {
        *counter += amount;
    }
}

static bool input_valid(const mesh_observation_input_t *in)
{
    if (!in || !in->sequence || in->sequence > MESH_OBSERVATION_MAX_SEQUENCE ||
        in->channel < 11U || in->channel > 26U ||
        in->length < 2U || in->length > MESH_MAC_MAX_LENGTH || (in->flags & ~7U) ||
        (!(in->flags & MESH_OBSERVATION_TIME_VALID) && in->callback_us) ||
        (!(in->flags & MESH_OBSERVATION_RSSI_VALID) && in->rssi) ||
        (!(in->flags & MESH_OBSERVATION_LQI_VALID) && in->lqi)) return false;
    for (size_t i = 0; i < sizeof(in->reserved); ++i)
        if (in->reserved[i]) return false;
    for (size_t i = in->length; i < sizeof(in->mac); ++i)
        if (in->mac[i]) return false;
    return true;
}

bool mesh_observation_init(mesh_observation_t *m, uint64_t session)
{
    if (!m || !session) return false;
    memset(m, 0, sizeof(*m));
    m->session = session;
    return true;
}

static mesh_observation_pan_key_t pan_key(uint8_t channel, mesh_mac_pan_t pan)
{
    mesh_observation_pan_key_t key = {0};
    key.channel = channel;
    key.known = pan.origin != MESH_MAC_PAN_UNKNOWN;
    key.value = key.known ? pan.value : 0U;
    return key;
}

static bool same_pan(mesh_observation_pan_key_t a, mesh_observation_pan_key_t b)
{
    return a.channel == b.channel && a.known == b.known && a.value == b.value;
}

static uint8_t origin_bit(mesh_mac_pan_t pan)
{
    return (uint8_t)(1U << pan.origin);
}

static void seen(mesh_observation_t *m, mesh_observation_seen_t *s,
                 const mesh_observation_input_t *in, uint8_t witness)
{
    bool timed = (in->flags & MESH_OBSERVATION_TIME_VALID) != 0U;
    if (!s->frames) {
        s->first_sequence = in->sequence;
        s->first_us = in->callback_us;
        s->first_time_valid = timed;
        s->first_witness = witness;
    }
    s->last_sequence = in->sequence;
    s->last_us = in->callback_us;
    s->last_time_valid = timed;
    s->last_witness = witness;
    add(m, &s->frames, 1U);
}

static void record_pan(mesh_observation_t *m, mesh_observation_pan_key_t key,
                       uint8_t origins, const mesh_observation_input_t *in,
                       uint8_t witness)
{
    uint8_t i = 0U;
    while (i < m->pan_count && !same_pan(m->pans[i].key, key)) ++i;
    if (i == m->pan_count) {
        if (i == MESH_OBSERVATION_PANS) {
            add(m, &m->counters.pan_omissions, 1U);
            return;
        }
        ++m->pan_count;
        m->pans[i].key = key;
    }
    m->pans[i].origin_bits |= origins;
    seen(m, &m->pans[i].seen, in, witness);
}

static bool usable(mesh_mac_address_t address)
{
    return address.mode != MESH_MAC_ADDRESS_NONE &&
        !address.broadcast && !address.short_sentinel;
}

static uint8_t address_slot(mesh_observation_t *m, mesh_mac_address_t address,
                            mesh_mac_pan_t pan, uint8_t channel)
{
    if (!usable(address)) return UINT8_MAX;
    mesh_observation_pan_key_t key = pan_key(channel, pan);
    for (uint8_t i = 0; i < m->address_count; ++i) {
        mesh_observation_address_t *row = &m->addresses[i];
        if (row->key.value == address.value && row->key.mode == address.mode &&
            same_pan(row->key.pan, key)) {
            row->origin_bits |= origin_bit(pan);
            return i;
        }
    }
    if (m->address_count == MESH_OBSERVATION_ADDRESSES) {
        add(m, &m->counters.address_omissions, 1U);
        return UINT8_MAX;
    }
    uint8_t i = m->address_count++;
    m->addresses[i].key.value = address.value;
    m->addresses[i].key.mode = address.mode;
    m->addresses[i].key.pan = key;
    m->addresses[i].origin_bits = origin_bit(pan);
    m->addresses[i].last_source_witness = MESH_OBSERVATION_NO_WITNESS;
    return i;
}

static void record_edge(mesh_observation_t *m, const mesh_mac_observation_t *mac,
                        const mesh_observation_input_t *in, uint8_t source,
                        uint8_t destination, uint8_t witness)
{
    /* Even enhanced ACK addressing is not promoted into this traffic graph. */
    if (mac->frame_type == 2U || !usable(mac->source) || !usable(mac->destination)) return;
    if (source == UINT8_MAX || destination == UINT8_MAX) {
        add(m, &m->counters.edge_omissions, 1U);
        return;
    }
    uint8_t i = 0U;
    while (i < m->edge_count && (m->edges[i].source != source ||
           m->edges[i].destination != destination)) ++i;
    if (i == m->edge_count) {
        if (i == MESH_OBSERVATION_EDGES) {
            add(m, &m->counters.edge_omissions, 1U);
            return;
        }
        ++m->edge_count;
        m->edges[i].source = source;
        m->edges[i].destination = destination;
    }
    mesh_observation_edge_t *edge = &m->edges[i];
    seen(m, &edge->seen, in, witness);
    edge->frame_type_bits |= (uint8_t)(1U << mac->frame_type);
    edge->last_frame_type = mac->frame_type;
    edge->last_sequence_present = mac->sequence_present;
    edge->last_sequence = mac->sequence;
    edge->last_signal_flags = in->flags;
    edge->last_source_rssi = in->rssi;
    edge->last_source_lqi = in->lqi;
    if (mac->security_enabled) add(m, &edge->secured_frames, 1U);
    if (mac->encrypted) add(m, &edge->encrypted_frames, 1U);
}

mesh_observation_result_t mesh_observation_ingest(mesh_observation_t *m,
                                               const mesh_observation_input_t *in)
{
    if (!model_valid(m)) return MESH_OBSERVATION_INVALID;
    if (in && overlap(m, sizeof(*m), in, sizeof(*in))) return MESH_OBSERVATION_INVALID;
    if (!input_valid(in)) {
        add(m, &m->counters.invalid_inputs, 1U);
        return MESH_OBSERVATION_INVALID;
    }
    if (in->session != m->session) {
        add(m, &m->counters.wrong_session, 1U);
        return MESH_OBSERVATION_WRONG_SESSION;
    }
    if (in->sequence <= m->last_sequence) {
        add(m, &m->counters.replayed, 1U);
        return MESH_OBSERVATION_REPLAY;
    }
    if ((in->flags & MESH_OBSERVATION_TIME_VALID) && m->time_seen &&
        in->callback_us < m->last_valid_us) {
        add(m, &m->counters.invalid_clock, 1U);
        return MESH_OBSERVATION_INVALID;
    }

    mesh_mac_observation_t mac;
    mesh_mac_result_t result = mesh_mac_parse(in->mac, in->length, &mac);
    add(m, &m->counters.delivery_sequence_gaps, in->sequence - m->last_sequence - 1U);
    m->last_sequence = in->sequence;
    if (in->flags & MESH_OBSERVATION_TIME_VALID) {
        m->last_valid_us = in->callback_us;
        m->time_seen = true;
    }
    add(m, &m->counters.records, 1U);
    uint8_t witness = MESH_OBSERVATION_NO_WITNESS;
    if (m->witness_count < MESH_OBSERVATION_WITNESSES) {
        witness = m->witness_count++;
        m->witnesses[witness].input = *in;
        m->witnesses[witness].parse_result = result;
    } else {
        add(m, &m->counters.witness_omissions, 1U);
    }
    if (result != MESH_MAC_HEADER_OK) {
        if (result == MESH_MAC_UNSUPPORTED) {
            add(m, &m->counters.unsupported, 1U);
            return MESH_OBSERVATION_UNSUPPORTED;
        }
        add(m, &m->counters.malformed, 1U);
        return MESH_OBSERVATION_MALFORMED;
    }
    add(m, &m->counters.headers, 1U);

    mesh_observation_pan_key_t source_pan = pan_key(in->channel, mac.source_pan);
    mesh_observation_pan_key_t destination_pan = pan_key(in->channel, mac.destination_pan);
    bool have_source = mac.source.mode != MESH_MAC_ADDRESS_NONE || source_pan.known;
    bool have_destination = mac.destination.mode != MESH_MAC_ADDRESS_NONE || destination_pan.known;
    if (have_source && have_destination && same_pan(source_pan, destination_pan)) {
        record_pan(m, source_pan, origin_bit(mac.source_pan) | origin_bit(mac.destination_pan), in, witness);
    } else {
        if (have_source) record_pan(m, source_pan, origin_bit(mac.source_pan), in, witness);
        if (have_destination) record_pan(m, destination_pan, origin_bit(mac.destination_pan), in, witness);
    }
    uint8_t source = address_slot(m, mac.source, mac.source_pan, in->channel);
    uint8_t destination = address_slot(m, mac.destination, mac.destination_pan, in->channel);
    if (source != UINT8_MAX) {
        mesh_observation_address_t *row = &m->addresses[source];
        seen(m, &row->seen, in, witness);
        add(m, &row->source_frames, 1U);
        row->last_source_us = in->callback_us;
        row->last_source_sequence = in->sequence;
        row->last_source_witness = witness;
        row->source_flags = in->flags;
        row->source_rssi = in->rssi;
        row->source_lqi = in->lqi;
    }
    if (destination != UINT8_MAX) {
        mesh_observation_address_t *row = &m->addresses[destination];
        if (destination != source) seen(m, &row->seen, in, witness);
        add(m, &row->destination_frames, 1U);
    }
    record_edge(m, &mac, in, source, destination, witness);
    return MESH_OBSERVATION_OK;
}

bool mesh_observation_select(const mesh_observation_t *m, mesh_observation_kind_t kind,
                             uint8_t index, mesh_observation_selection_t *out)
{
    if (!model_valid(m) || !out || overlap(m, sizeof(*m), out, sizeof(*out))) return false;
    uint8_t witness;
    uint32_t sequence = 0U;
    switch (kind) {
    case MESH_OBSERVATION_SELECT_PAN:
        if (index >= m->pan_count) return false;
        witness = m->pans[index].seen.last_witness;
        sequence = m->pans[index].seen.last_sequence;
        break;
    case MESH_OBSERVATION_SELECT_ADDRESS:
        if (index >= m->address_count) return false;
        witness = m->addresses[index].seen.last_witness;
        sequence = m->addresses[index].seen.last_sequence;
        break;
    case MESH_OBSERVATION_SELECT_EDGE:
        if (index >= m->edge_count) return false;
        witness = m->edges[index].seen.last_witness;
        sequence = m->edges[index].seen.last_sequence;
        break;
    case MESH_OBSERVATION_SELECT_WITNESS:
        if (index >= m->witness_count) return false;
        witness = index;
        break;
    default: return false;
    }
    if (witness != MESH_OBSERVATION_NO_WITNESS &&
        (witness >= m->witness_count || m->witnesses[witness].input.session != m->session ||
         (sequence && m->witnesses[witness].input.sequence != sequence))) return false;
    memset(out, 0, sizeof(*out));
    out->session = m->session;
    out->kind = kind;
    out->index = index;
    if (kind == MESH_OBSERVATION_SELECT_PAN) out->record.pan = m->pans[index];
    else if (kind == MESH_OBSERVATION_SELECT_ADDRESS) out->record.address = m->addresses[index];
    else if (kind == MESH_OBSERVATION_SELECT_EDGE) out->record.edge = m->edges[index];
    if (witness != MESH_OBSERVATION_NO_WITNESS) {
        out->witness_valid = true;
        out->witness = m->witnesses[witness];
    }
    return true;
}
