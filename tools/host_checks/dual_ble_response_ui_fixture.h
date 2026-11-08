#pragma once
/* Synthetic SDK-shaped report pair, using production full-container parsing
 * and response wire validation. No over-air identity assumption. */
static void fixture_response_row(cerberus_dual_ble_row_t *row,
    const uint8_t *full, size_t length, uint8_t family, uint64_t anchor)
{
    assert(length && length <= 255U && family <= 6U);
    memset(row,0,sizeof(*row));memcpy(row->address,"ABCDEF",6);row->address_type=1;
    row->rssi=-70;row->timing_flags=3;row->callback_age_ms=900;row->request_started_ms=anchor;
    row->reports=19;row->event_type=3;row->raw_length=3;
    memcpy(row->raw,(uint8_t[]){2,1,6},3);
    row->receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.properties=2,.callback_length=3,
        .sid=3,.primary_phy=1,.secondary_phy=1,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    cerberus_dual_ble_family_evidence_t evidence;
    if(family){
        assert(cerberus_dual_ble_family_parse_ad(row->raw,3,row->address,row->address_type,&evidence));
        assert(cerberus_dual_ble_family_wire_encode(&evidence,family,&row->family));
    }
    cerberus_dual_ble_response_wire_t *r=&row->response;
    *r=(cerberus_dual_ble_response_wire_t){.version=CERBERUS_DUAL_BLE_RESPONSE_VERSION,
        .state=CERBERUS_DUAL_BLE_RESPONSE_PRESENT,.flags=CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID|
        CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID,.event_type=3,.rssi=-42,
        .raw_length=(uint8_t)(length<31U?length:31U),.raw_flags=length>31U?1U:0U,
        .original_sequence=7,.response_sequence=8,.age_ms=100,.interval_us=800000};
    r->receive=row->receive;r->receive.properties|=8U;r->receive.callback_length=(uint8_t)length;
    memcpy(r->raw,full,r->raw_length);
    assert(cerberus_dual_ble_family_parse_ad(full,length,row->address,row->address_type,&evidence));
    assert(cerberus_dual_ble_response_family_encode(&evidence,family,&r->family));
    assert(cerberus_dual_ble_response_row_valid(row));
}
