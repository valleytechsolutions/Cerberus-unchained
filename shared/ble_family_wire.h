#pragma once
#include "ble_family.h"
#include "dual_ble_protocol.h"

/* Version1 accompanies one frozen original, never peer aggregates. Validation
 * proves canonical representation and source consistency, not authenticity of
 * broadcast hints. Failed encode/decode clears output transactionally. */
enum { CERBERUS_DUAL_BLE_FAMILY_WIRE_VERSION = 1U,
       CERBERUS_DUAL_BLE_FAMILY_WIRE_AVAILABLE = 1U,
       CERBERUS_DUAL_BLE_FAMILY_WIRE_FLOCK_OUI = 2U,
       CERBERUS_DUAL_BLE_FAMILY_WIRE_META_BLOCKED = 4U };

static inline bool cerberus_dual_ble_family_evidence_valid(
    const cerberus_dual_ble_family_evidence_t *e)
{
    if (!e) return false;
    if (!e->available) {
        if (e->state != CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN &&
            e->state != CERBERUS_DUAL_BLE_FAMILY_MALFORMED) return false;
        cerberus_dual_ble_family_evidence_t empty = {0};
        empty.state = e->state;
        /* Compare fields, not native padding. */
        return !e->categories && !e->hc_name && !e->ibeacon_result &&
            !e->flock_oui && !e->meta_blocked && !e->findmy.bits &&
            !e->findmy.fmna_uuid_widths && !e->flipper.bits && !e->flipper.flags &&
            !memcmp(&e->flock, &empty.flock, sizeof(e->flock)) &&
            !memcmp(&e->meta, &empty.meta, sizeof(e->meta)) &&
            !memcmp(e->ibeacon.uuid, empty.ibeacon.uuid, sizeof(e->ibeacon.uuid)) &&
            !e->ibeacon.major && !e->ibeacon.minor && !e->ibeacon.measured_power_dbm;
    }
    if (e->state != CERBERUS_DUAL_BLE_FAMILY_COMPLETE ||
        (e->hc_name && e->hc_name != 3U && e->hc_name != 5U && e->hc_name != 6U) ||
        e->ibeacon_result > CERBERUS_IBEACON_MALFORMED ||
        (e->findmy.bits & ~7U) || (e->findmy.fmna_uuid_widths & ~7U) ||
        ((e->findmy.bits & CERBERUS_BLE_FINDMY_HINT_FMNA) != 0U) != (e->findmy.fmna_uuid_widths != 0U) ||
        (e->flipper.bits & ~15U) || (e->flipper.flags & ~31U) ||
        (e->flipper.bits != 0U) != ((e->flipper.flags & 7U) != 0U) ||
        ((e->flipper.bits & CERBERUS_BLE_FLIPPER_HINT_BASE) && !(e->flipper.flags & CERBERUS_BLE_FLIPPER_NAME)) ||
        ((e->flipper.flags & CERBERUS_BLE_FLIPPER_SERVICE_DATA) &&
            (!(e->flipper.flags & CERBERUS_BLE_FLIPPER_NAME) || !e->flipper.bits)) ||
        (e->flock.facts & ~63U) || (e->flock.accessory_contexts & ~36U) ||
        (e->flock.raven_3100_contexts & ~63U) || (e->flock.raven_3200_contexts & ~63U) ||
        ((e->flock.facts & CERBERUS_BLE_FLOCK_MFG_PAYLOAD) && !(e->flock.facts & CERBERUS_BLE_FLOCK_MFG_09C8)) ||
        ((e->flock.facts & 7U) && (e->flock.facts & CERBERUS_BLE_FLOCK_NO_LOCAL_NAME)) ||
        ((e->flock.facts & 7U) && ((e->flock.facts & 7U) & ((e->flock.facts & 7U) - 1U))) ||
        (e->meta.pairs[9] & 0xe0U) ||
        e->meta_blocked != cerberus_ble_meta_evidence_blocked(&e->meta)) return false;
    if (e->ibeacon_result != CERBERUS_IBEACON_FOUND) {
        const uint8_t zero[16] = {0};
        if (memcmp(e->ibeacon.uuid, zero, 16U) || e->ibeacon.major || e->ibeacon.minor ||
            e->ibeacon.measured_power_dbm) return false;
    }
    uint8_t categories = 0U;
    if (e->ibeacon_result == CERBERUS_IBEACON_FOUND) categories |= CERBERUS_DUAL_BLE_FAMILY_IBEACON;
    if (e->findmy.bits) categories |= CERBERUS_DUAL_BLE_FAMILY_FIND_MY;
    if (e->flipper.bits) categories |= CERBERUS_DUAL_BLE_FAMILY_FLIPPER;
    if (cerberus_ble_flock_hints_level(&e->flock) || e->flock_oui) categories |= CERBERUS_DUAL_BLE_FAMILY_FLOCK;
    if (cerberus_ble_meta_evidence_allowed(&e->meta) && !e->meta_blocked) categories |= CERBERUS_DUAL_BLE_FAMILY_META_HINT;
    if (e->hc_name) categories |= CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT;
    return e->categories == categories;
}

static inline bool cerberus_dual_ble_family_wire_encode(
    const cerberus_dual_ble_family_evidence_t *e, uint8_t family_code,
    cerberus_dual_ble_family_wire_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (family_code < 1U || family_code > 6U || !cerberus_dual_ble_family_evidence_valid(e)) return false;
    cerberus_dual_ble_family_wire_t p = {0};
    p.version = CERBERUS_DUAL_BLE_FAMILY_WIRE_VERSION; p.family_code = family_code;
    p.state = e->state; p.categories = e->categories; p.hc_name = e->hc_name;
    p.ibeacon_result = e->ibeacon_result;
    p.flags = (e->available ? CERBERUS_DUAL_BLE_FAMILY_WIRE_AVAILABLE : 0U) |
        (e->flock_oui ? CERBERUS_DUAL_BLE_FAMILY_WIRE_FLOCK_OUI : 0U) |
        (e->meta_blocked ? CERBERUS_DUAL_BLE_FAMILY_WIRE_META_BLOCKED : 0U);
    p.findmy_bits = e->findmy.bits; p.findmy_uuid_widths = e->findmy.fmna_uuid_widths;
    p.flipper_bits = e->flipper.bits; p.flipper_flags = e->flipper.flags;
    p.flock_facts = e->flock.facts; p.flock_accessory_contexts = e->flock.accessory_contexts;
    p.flock_raven_3100_contexts = e->flock.raven_3100_contexts;
    p.flock_raven_3200_contexts = e->flock.raven_3200_contexts;
    memcpy(p.meta_pairs, e->meta.pairs, sizeof(p.meta_pairs));
    memcpy(p.ibeacon_uuid, e->ibeacon.uuid, sizeof(p.ibeacon_uuid));
    p.ibeacon_major_le[0] = (uint8_t)e->ibeacon.major; p.ibeacon_major_le[1] = (uint8_t)(e->ibeacon.major >> 8U);
    p.ibeacon_minor_le[0] = (uint8_t)e->ibeacon.minor; p.ibeacon_minor_le[1] = (uint8_t)(e->ibeacon.minor >> 8U);
    p.ibeacon_power = e->ibeacon.measured_power_dbm;
    *out = p; return true;
}

static inline bool cerberus_dual_ble_family_wire_unpack(
    const cerberus_dual_ble_family_wire_t *w,
    const uint8_t address[6], uint8_t address_type,
    cerberus_dual_ble_family_evidence_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!w || !address || address_type > 3U || w->version != CERBERUS_DUAL_BLE_FAMILY_WIRE_VERSION ||
        w->family_code < 1U || w->family_code > 6U || (w->flags & ~7U) ||
        w->reserved[0] || w->reserved[1]) return false;
    cerberus_dual_ble_family_evidence_t p = {0};
    p.state = w->state; p.categories = w->categories; p.hc_name = w->hc_name;
    p.ibeacon_result = w->ibeacon_result;
    p.available = (w->flags & CERBERUS_DUAL_BLE_FAMILY_WIRE_AVAILABLE) != 0U;
    p.flock_oui = (w->flags & CERBERUS_DUAL_BLE_FAMILY_WIRE_FLOCK_OUI) != 0U;
    p.meta_blocked = (w->flags & CERBERUS_DUAL_BLE_FAMILY_WIRE_META_BLOCKED) != 0U;
    p.findmy.bits = w->findmy_bits; p.findmy.fmna_uuid_widths = w->findmy_uuid_widths;
    p.flipper.bits = w->flipper_bits; p.flipper.flags = w->flipper_flags;
    p.flock.facts = w->flock_facts; p.flock.accessory_contexts = w->flock_accessory_contexts;
    p.flock.raven_3100_contexts = w->flock_raven_3100_contexts;
    p.flock.raven_3200_contexts = w->flock_raven_3200_contexts;
    memcpy(p.meta.pairs, w->meta_pairs, sizeof(p.meta.pairs));
    memcpy(p.ibeacon.uuid, w->ibeacon_uuid, sizeof(p.ibeacon.uuid));
    p.ibeacon.major = (uint16_t)w->ibeacon_major_le[0] | ((uint16_t)w->ibeacon_major_le[1] << 8U);
    p.ibeacon.minor = (uint16_t)w->ibeacon_minor_le[0] | ((uint16_t)w->ibeacon_minor_le[1] << 8U);
    p.ibeacon.measured_power_dbm = w->ibeacon_power;
    if (!cerberus_dual_ble_family_evidence_valid(&p)) return false;
    if (p.available && p.flock_oui != cerberus_ble_flock_oui_hint(address, address_type)) return false;
    *out = p; return true;
}

/* Original-only source gate remains unchanged. Response decoding uses its
 * own strict envelope and only shares the canonical fact unpacker above. */
static inline bool cerberus_dual_ble_family_wire_decode(
    const cerberus_dual_ble_family_wire_t *w,
    const cerberus_dual_ble_receive_metadata_t *receive,
    uint8_t raw_length, uint8_t event_type, uint8_t raw_flags,
    const uint8_t address[6], uint8_t address_type,
    cerberus_dual_ble_family_evidence_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    cerberus_dual_ble_family_evidence_t p;
    if (event_type > 3U || raw_length > 31U || (raw_flags & ~3U) ||
        !cerberus_dual_ble_receive_metadata_valid(receive, raw_length, event_type, raw_flags) ||
        !cerberus_dual_ble_family_wire_unpack(w, address, address_type, &p)) return false;
    if (p.available) {
        if (receive->source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN ||
            receive->ad_validation != CERBERUS_DUAL_BLE_AD_VALID) return false;
    } else if ((p.state == CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN) !=
        (receive->source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN)) return false;
    *out = p; return true;
}
