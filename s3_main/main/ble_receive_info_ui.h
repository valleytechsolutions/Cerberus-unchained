#pragma once
#include "dual_ble_protocol.h"

/* Shared presentation of already validated, frozen receive evidence. The
 * caller supplies live-selected or saved bytes and the page's vertical origin.
 * No device state, clocks, allocation, transport or action eligibility. */
static inline const char *ble_receive_info_ui_phy(uint8_t phy)
{
    switch (phy) {
    case 1U: return "1M";
    case 2U: return "2M";
    case 3U: return "CODED";
    default: return "N/A";
    }
}

static inline esp_err_t ble_receive_info_ui_draw(
    const cerberus_dual_ble_receive_metadata_t *receive, uint8_t preview_length,
    uint16_t top)
{
    char line[64], sid[4];
    const bool legacy = receive->source_kind == CERBERUS_DUAL_BLE_RECEIVE_LEGACY;
    if (!legacy && receive->source_kind != CERBERUS_DUAL_BLE_RECEIVE_EXTENDED) {
        ESP_RETURN_ON_ERROR(draw_centered(top + 4U, "RECEIVE INFO UNAVAILABLE", UI_MUTED, 1), TAG, "receive metadata missing");
        ESP_RETURN_ON_ERROR(draw_centered(top + 24U, "NO METADATA FOR THIS OBSERVATION", UI_MUTED, 1), TAG, "receive metadata scope");
        return draw_centered(top + 48U, "RAW BYTES REMAIN AVAILABLE", UI_TEXT, 1);
    }
    ESP_RETURN_ON_ERROR(draw_centered(top, legacy ? "C5 LEGACY CALLBACK" :
        (receive->properties & 0x10U) ? "C5 EXT CALLBACK / LEGACY PDU" :
        "C5 EXTENDED CALLBACK", UI_TEXT, 1), TAG, "receive callback kind");
    snprintf(line, sizeof(line), "CALLBACK %u B / PREVIEW %u B", receive->callback_length, preview_length);
    ESP_RETURN_ON_ERROR(draw_centered(top + 18U, line, UI_MUTED, 1), TAG, "receive callback bytes");
    if (receive->sid <= 15U) snprintf(sid, sizeof(sid), "%u", receive->sid);
    else memcpy(sid, "N/A", sizeof(sid));
    snprintf(line, sizeof(line), "PHY %s PRIMARY / %s SECONDARY", ble_receive_info_ui_phy(receive->primary_phy),
        ble_receive_info_ui_phy(receive->secondary_phy));
    ESP_RETURN_ON_ERROR(draw_centered(top + 36U, line, UI_MUTED, 1), TAG, "receive physical metadata");
    if (legacy) snprintf(line, sizeof(line), "SID N/A / PROPS N/A / CTRL STATUS N/A");
    else snprintf(line, sizeof(line), "SID %s / PROPS %02X / CTRL COMPLETE", sid, receive->properties);
    ESP_RETURN_ON_ERROR(draw_centered(top + 54U, line, UI_MUTED, 1), TAG, "receive report status");
    snprintf(line, sizeof(line), "FULL AD LAYOUT %s%s", receive->ad_validation == CERBERUS_DUAL_BLE_AD_VALID ?
        "VALID" : receive->ad_validation == CERBERUS_DUAL_BLE_AD_INVALID ? "INVALID" : "UNKNOWN",
        receive->callback_length > preview_length ? " / PREVIEW CUT" : "");
    return draw_centered(top + 72U, line, UI_MUTED, 1);
}
