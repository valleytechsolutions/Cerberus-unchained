#pragma once
/* Live wrapper: evidence comes only from the frozen C5 original and copied
 * provenance. The common renderer owns no live receiver or clock. */
#include "dual_ble_family.h"
#include "ble_family_hint_ui.h"

static esp_err_t dual_ble_hint_ui_draw(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *proof, uint8_t requested_page, uint8_t *painted_pages)
{
    cerberus_dual_ble_family_evidence_t evidence;
    *painted_pages = 0U;
    bool available = cerberus_dual_ble_family_classify(row, proof, &evidence);
    uint8_t pages = 0U;
    ESP_RETURN_ON_ERROR(ble_family_hint_ui_draw(&evidence, available && row->family.version != 0U,
        row->receive.callback_length, row->raw_length, false, 230U, requested_page, &pages), TAG, "C5 hints");
    if (pages > 1U)
        ESP_RETURN_ON_ERROR(draw_small_button(8, 294, 304, 18, "NEXT HINT PAGE >"), TAG, "C5 hint page");
    *painted_pages = pages;
    return ESP_OK;
}

static esp_err_t dual_ble_response_hint_ui_draw(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *proof, uint8_t requested_page, uint8_t *painted_pages)
{
    cerberus_dual_ble_family_evidence_t evidence;
    *painted_pages = 0U;
    bool available = cerberus_dual_ble_response_classify(row, proof, &evidence);
    uint8_t pages = 0U;
    ESP_RETURN_ON_ERROR(ble_family_hint_ui_draw_source(&evidence, available,
        row->response.receive.callback_length, row->response.raw_length, false, true,
        230U, requested_page, &pages), TAG, "C5 response hints");
    if (pages > 1U)
        ESP_RETURN_ON_ERROR(draw_small_button(8, 294, 304, 18, "NEXT HINT PAGE >"), TAG, "C5 response page");
    *painted_pages = pages;
    return ESP_OK;
}
