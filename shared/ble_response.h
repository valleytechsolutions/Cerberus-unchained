#pragma once
#include "ble_family_wire.h"

/* Response metadata deliberately has its own validator. The original-only
 * envelope cannot accept scan-response properties, event4, or response facts. */
static inline bool cerberus_dual_ble_response_receive_valid(
    const cerberus_dual_ble_receive_metadata_t *m, uint8_t length,
    uint8_t event, uint8_t flags)
{
    if (!m || !m->callback_length || length != (m->callback_length < 31U ? m->callback_length : 31U) ||
        flags != (m->callback_length > 31U ? 1U : 0U) || m->ad_validation != CERBERUS_DUAL_BLE_AD_VALID)
        return false;
    if (m->source_kind == CERBERUS_DUAL_BLE_RECEIVE_LEGACY)
        return event == 4U && m->callback_length <= 31U && !m->properties &&
            m->sid == 0xffU && !m->primary_phy && !m->secondary_phy && m->data_status == 0xffU;
    if (m->source_kind != CERBERUS_DUAL_BLE_RECEIVE_EXTENDED ||
        (m->properties & ~0x1fU) || !(m->properties & 8U) || !(m->properties & 2U) ||
        (m->properties & 4U) || m->data_status ||
        (m->primary_phy != 1U && m->primary_phy != 3U) || m->secondary_phy > 3U) return false;
    if (m->properties & 16U)
        return (m->properties == 0x1aU || m->properties == 0x1bU) && event == 4U &&
            m->callback_length <= 31U && m->sid == 0xffU && m->primary_phy == 1U && !m->secondary_phy;
    return m->sid <= 15U && !(m->properties & 1U) && event == 3U;
}

static inline bool cerberus_dual_ble_response_original_eligible(
    const cerberus_dual_ble_receive_metadata_t *m, uint8_t length, uint8_t event, uint8_t flags)
{
    if (!cerberus_dual_ble_receive_metadata_valid(m,length,event,flags) ||
        m->source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN || m->ad_validation != CERBERUS_DUAL_BLE_AD_VALID)
        return false;
    if (m->source_kind == CERBERUS_DUAL_BLE_RECEIVE_LEGACY) return event == 0U || event == 2U;
    if (!(m->properties & 2U)) return false;
    return (m->properties & 16U) ? (event == 0U || event == 2U) :
        (m->sid <= 15U && !(m->properties & (1U|4U)));
}

static inline bool cerberus_dual_ble_response_context_matches(
    const cerberus_dual_ble_receive_metadata_t *a,
    const cerberus_dual_ble_receive_metadata_t *b)
{
    /* Legacy callbacks and legacy-PDU EXT callbacks are separate source
     * contexts. No cross-source guess or unknown-SID advertising-set join. */
    if (a->source_kind != b->source_kind) return false;
    if (a->source_kind == CERBERUS_DUAL_BLE_RECEIVE_LEGACY) return true;
    return (uint8_t)(b->properties & ~8U) == a->properties && a->sid == b->sid &&
        a->primary_phy == b->primary_phy && a->secondary_phy == b->secondary_phy;
}

static inline bool cerberus_dual_ble_response_family_encode(
    const cerberus_dual_ble_family_evidence_t *e, uint8_t family,
    cerberus_dual_ble_family_wire_t *out)
{
    if (!out) return false;
    if (family > 6U) { memset(out,0,sizeof(*out)); return false; }
    if (!cerberus_dual_ble_family_wire_encode(e,family ? family : 1U,out)) return false;
    out->family_code=family; return true;
}

static inline bool cerberus_dual_ble_response_family(
    const cerberus_dual_ble_response_wire_t *r, const uint8_t address[6],
    uint8_t type, cerberus_dual_ble_family_evidence_t *out)
{
    if (!out) return false;
    memset(out,0,sizeof(*out));
    if (!r || r->version != CERBERUS_DUAL_BLE_RESPONSE_VERSION || r->state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT ||
        !cerberus_dual_ble_response_receive_valid(&r->receive,r->raw_length,r->event_type,r->raw_flags) ||
        r->family.family_code > 6U) {
        memset(out,0,sizeof(*out)); return false;
    }
    cerberus_dual_ble_family_wire_t wire=r->family;
    if (!wire.family_code) wire.family_code=1U;
    if (!cerberus_dual_ble_family_wire_unpack(&wire,address,type,out) || !out->available) {
        memset(out,0,sizeof(*out)); return false;
    }
    return true;
}

static inline bool cerberus_dual_ble_response_source_valid(
    const cerberus_dual_ble_response_wire_t *r,
    const cerberus_dual_ble_receive_metadata_t *original_receive,
    uint8_t original_length, uint8_t original_event, uint8_t original_flags,
    const uint8_t address[6], uint8_t address_type, uint8_t selected_family)
{
    if (!r || !address || address_type > 3U || selected_family > 6U) return false;
    cerberus_dual_ble_response_wire_t empty = {0};
    if (r->state == CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN) return !memcmp(r,&empty,sizeof(empty));
    empty.version = CERBERUS_DUAL_BLE_RESPONSE_VERSION; empty.state = CERBERUS_DUAL_BLE_RESPONSE_NONE;
    if (r->state == CERBERUS_DUAL_BLE_RESPONSE_NONE) {
        empty.original_sequence=r->original_sequence;
        return r->original_sequence && !memcmp(r,&empty,sizeof(empty)) &&
            cerberus_dual_ble_response_original_eligible(original_receive,original_length,original_event,original_flags);
    }
    if (r->version != CERBERUS_DUAL_BLE_RESPONSE_VERSION || r->state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT ||
        (r->flags & ~15U) || r->reserved_tail || r->name_length > 32U ||
        !r->original_sequence || r->response_sequence <= r->original_sequence ||
        r->interval_us > CERBERUS_DUAL_BLE_RESPONSE_INTERVAL_US ||
        ((r->flags & CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID) ? r->age_ms == UINT32_MAX : r->age_ms != UINT32_MAX) ||
        ((r->flags & CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID) != 0U) != (r->rssi >= -127 && r->rssi <= 20) ||
        (!(r->flags & CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID) && r->rssi != 127) ||
        (!r->name_length && (r->flags & (CERBERUS_DUAL_BLE_RESPONSE_NAME_COMPLETE|CERBERUS_DUAL_BLE_RESPONSE_NAME_TRUNCATED))) ||
        ((r->flags & CERBERUS_DUAL_BLE_RESPONSE_NAME_TRUNCATED) && r->name_length != 32U) ||
        r->family.family_code != selected_family ||
        !cerberus_dual_ble_response_original_eligible(original_receive,original_length,original_event,original_flags) ||
        !cerberus_dual_ble_response_context_matches(original_receive,&r->receive)) return false;
    for (size_t i=r->raw_length;i<sizeof(r->raw);++i) if (r->raw[i]) return false;
    for (size_t i=r->name_length;i<sizeof(r->name);++i) if (r->name[i]) return false;
    cerberus_dual_ble_family_evidence_t evidence;
    return cerberus_dual_ble_response_family(r,address,address_type,&evidence);
}

static inline bool cerberus_dual_ble_response_valid(const cerberus_dual_ble_response_wire_t *r,
    const cerberus_dual_ble_snapshot_family_row_t *original, uint8_t selected_family)
{
    if (!original) return false;
    const cerberus_dual_ble_snapshot_row_t *a=&original->metadata.row;
    return cerberus_dual_ble_response_source_valid(r,&original->metadata.receive,a->raw_length,
        a->event_type,a->raw_flags,a->address,a->address_type,selected_family);
}

/* Portable bytes for the archive and projected SPI suffix. Multibyte integers
 * are explicitly little endian; no native padding/endian assumptions. */
static inline void cerberus_dual_ble_response_encode_bytes(
    const cerberus_dual_ble_response_wire_t *r, uint8_t out[152])
{
    memcpy(out,r,8U);
    for (unsigned i=0;i<8U;++i) { out[8U+i]=(uint8_t)(r->original_sequence>>(8U*i)); out[16U+i]=(uint8_t)(r->response_sequence>>(8U*i)); }
    for (unsigned i=0;i<4U;++i) { out[24U+i]=(uint8_t)(r->age_ms>>(8U*i)); out[28U+i]=(uint8_t)(r->interval_us>>(8U*i)); }
    memcpy(out+32U,&r->receive,8U); memcpy(out+40U,r->raw,31U);
    memcpy(out+71U,r->name,32U); memcpy(out+103U,&r->family,48U); out[151]=r->reserved_tail;
}
static inline void cerberus_dual_ble_response_decode_bytes(
    const uint8_t in[152], cerberus_dual_ble_response_wire_t *r)
{
    memset(r,0,sizeof(*r)); memcpy(r,in,8U);
    for (unsigned i=0;i<8U;++i) { r->original_sequence|=(uint64_t)in[8U+i]<<(8U*i); r->response_sequence|=(uint64_t)in[16U+i]<<(8U*i); }
    for (unsigned i=0;i<4U;++i) { r->age_ms|=(uint32_t)in[24U+i]<<(8U*i); r->interval_us|=(uint32_t)in[28U+i]<<(8U*i); }
    memcpy(&r->receive,in+32U,8U); memcpy(r->raw,in+40U,31U);
    memcpy(r->name,in+71U,32U); memcpy(&r->family,in+103U,48U); r->reserved_tail=in[151];
}
