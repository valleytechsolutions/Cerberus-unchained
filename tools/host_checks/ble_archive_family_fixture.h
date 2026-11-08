#pragma once
#include "ble_family_wire.h"

/* Host fixture input: a valid255-byte original with a complete HC name past
 * the retained31-byte boundary. Use the production full-container parser and
 * canonical encoder, never derive evidence from the short preview. */
static bool archive_test_family_original(uint8_t preview[31],
    cerberus_dual_ble_receive_metadata_t *receive, cerberus_dual_ble_family_wire_t *wire,
    const uint8_t address[6], uint8_t address_type, uint8_t suffix)
{
    uint8_t full[255]={39,0xff,0x4c,0};
    memcpy(full+40,(uint8_t[]){6,9,'H','C','-','0',suffix},7);
    cerberus_dual_ble_family_evidence_t evidence;
    if(!cerberus_dual_ble_family_parse_ad(full,sizeof(full),address,address_type,&evidence) ||
        !cerberus_dual_ble_family_wire_encode(&evidence,6,wire))return false;
    memcpy(preview,full,31);
    *receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.callback_length=255,
        .sid=7,.primary_phy=3,.secondary_phy=2,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    return true;
}
