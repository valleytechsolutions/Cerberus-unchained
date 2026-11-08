#pragma once
#include "ble_archive.h"
#include "ble_response.h"

/* Independent host inputs: an eligible original without family payload,
 * followed by a full255 response whose family fact starts after byte31.
 * No radio/current clock is consulted. Caller supplies frozen C5 identity. */
static inline bool archive_test_response_row(cerberus_ble_archive_row_t *r,
    uint8_t payload_family, uint8_t selected_family)
{
    static const uint8_t samples[6][31] = {
        {26,0xff,0x4c,0,2,0x15,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x12,0x34,0xab,0xcd,0xc5},
        {3,3,0x44,0xfd}, {3,3,0x81,0x30},
        {15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'},
        {3,3,0x5f,0xfd}, {6,9,'H','C','-','0','5'}
    };
    static const uint8_t lengths[] = {27,4,4,16,4,7};
    if (!r || payload_family < 1U || payload_family > 6U || selected_family > 6U) return false;
    memset(r->advertisement, 0, sizeof(r->advertisement));
    memcpy(r->advertisement, (uint8_t[]){2,1,6}, 3U);
    r->advertisement_length = 3U; r->advertisement_event_type = 3U; r->advertisement_flags = 0U;
    r->receive = (cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=2,
        .callback_length=3,.sid=7,.primary_phy=1,.secondary_phy=2,.ad_validation=1};
    cerberus_dual_ble_family_evidence_t original;
    if (!cerberus_dual_ble_family_parse_ad(r->advertisement, 3U, r->address, r->address_type, &original)) return false;
    memset(&r->family, 0, sizeof(r->family));
    if (selected_family && !cerberus_dual_ble_family_wire_encode(&original, selected_family, &r->family)) return false;
    uint8_t full[255] = {39,0x30};
    memcpy(full + 40U, samples[payload_family-1U], lengths[payload_family-1U]);
    cerberus_dual_ble_family_evidence_t response;
    if (!cerberus_dual_ble_family_parse_ad(full, sizeof(full), r->address, r->address_type, &response)) return false;
    cerberus_dual_ble_response_wire_t *s = &r->c5_response;
    memset(s, 0, sizeof(*s));
    s->version=1; s->state=CERBERUS_DUAL_BLE_RESPONSE_PRESENT;
    s->flags=CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID|CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID;
    s->raw_length=31; s->event_type=3; s->rssi=-47; s->raw_flags=1;
    s->original_sequence=UINT64_C(0x0102030405060708); s->response_sequence=s->original_sequence+1U;
    s->age_ms=123U; s->interval_us=2000U;
    s->receive=r->receive; s->receive.properties=10; s->receive.callback_length=255;
    memcpy(s->raw, full, sizeof(s->raw));
    if (samples[payload_family-1U][1] == 9U) {
        s->name_length=(uint8_t)(lengths[payload_family-1U]-2U);
        memcpy(s->name,samples[payload_family-1U]+2U,s->name_length);
        s->flags|=CERBERUS_DUAL_BLE_RESPONSE_NAME_COMPLETE;
    }
    if (!cerberus_dual_ble_response_family_encode(&response, selected_family, &s->family)) return false;
    r->age_upper_valid=true; r->age_upper_at_save_ms=525U;
    r->c5_response_age_upper_valid=true; r->c5_response_age_upper_at_save_ms=523U;
    return cerberus_dual_ble_response_source_valid(s,&r->receive,r->advertisement_length,
        r->advertisement_event_type,r->advertisement_flags,r->address,r->address_type,selected_family);
}
