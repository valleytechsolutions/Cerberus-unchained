#pragma once
/* Shared C5 evidence presentation only. Callers own source validation, frozen
 * selection, historical/live scope and navigation. No receiver owner, clock,
 * allocation or IO dependency. Coordinates span top..top+59. */
#include "ble_family.h"
#include "s3_ibeacon_hint_view.h"
#include "s3_meta_hint_view.h"

static inline esp_err_t ble_family_hint_ui_draw_source(
    const cerberus_dual_ble_family_evidence_t *evidence, bool full_callback,
    uint8_t full_length, uint8_t preview_length, bool saved, bool response, uint16_t top,
    uint8_t requested_page, uint8_t *painted_pages)
{
    char line[64];
    *painted_pages = 0U;
    bool available = evidence->available;
    uint8_t page_bits = evidence->categories;
    if (available && evidence->meta_blocked) page_bits |= CERBERUS_DUAL_BLE_FAMILY_META_HINT;
    uint8_t meta_count = cerberus_ble_meta_evidence_count(&evidence->meta);
    uint8_t meta_pages = meta_count ? (uint8_t)((meta_count + 1U) / 2U) : 1U;
    bool address_only = evidence->flock_oui && !cerberus_ble_flock_hints_level(&evidence->flock);
    uint8_t pages = 1U, matches = 0U;
    for (unsigned bit = 0U; bit < 6U; ++bit) {
        if (page_bits & (1U << bit)) pages += (1U << bit) == CERBERUS_DUAL_BLE_FAMILY_META_HINT ? meta_pages : 1U;
        if (evidence->categories & (1U << bit)) ++matches;
    }
    uint8_t page = requested_page < pages ? requested_page : 0U;
    snprintf(line, sizeof(line), response ? (saved ? "SAVED C5 RESPONSE / HINTS %u/%u" :
        "C5 MATCHED RESPONSE / HINTS %u/%u") : saved ? "SAVED C5 ORIGINAL / HINTS %u/%u" :
        full_callback ? "C5 FULL ORIGINAL / HINTS %u/%u" :
        address_only ? "C5 ORIGINAL + ADDRESS / HINTS %u/%u" :
        "C5 ORIGINAL ADV / HINTS %u/%u", page + 1U, pages);
    ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 0U), line, UI_MUTED, 1), TAG, "C5 hint source");
    if (!available) {
        const char *reason = evidence->state == CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE ? "SOURCE PROOF UNAVAILABLE" :
            evidence->state == CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN ? "RECEIVE INFO UNAVAILABLE" :
            evidence->state == CERBERUS_DUAL_BLE_FAMILY_TRUNCATED ? (response ? "FULL RESPONSE NOT RETAINED" : "FULL ORIGINAL NOT RETAINED") :
            evidence->state == CERBERUS_DUAL_BLE_FAMILY_MALFORMED ? "AD EVIDENCE NOT CLASSIFIABLE" :
            evidence->state == CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE ? "SOURCE ENVELOPE NOT VALID" :
            saved ? (response ? "RESPONSE HINTS NOT SAVED" : "FULL ORIGINAL HINTS NOT SAVED") : "FAMILY EVIDENCE UNAVAILABLE";
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 16U), reason, UI_MUTED, 1), TAG, "C5 hint unavailable");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 34U), "COMPLETE SOURCE EVIDENCE REQUIRED", UI_MUTED, 1), TAG, "C5 hint limit");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 52U), response ? "RESPONSE RAW / RX INFO IF RETAINED" : "RAW ADV AND RX INFO STILL AVAILABLE", UI_MUTED, 1), TAG, "C5 hint recovery");
    } else if (!page) {
        if (full_callback) snprintf(line, sizeof(line), "FULL %u B / PREVIEW %u B / %u HINTS",
            full_length, preview_length, matches);
        else snprintf(line, sizeof(line), address_only ? "COMPLETE SOURCE / %u FAMILY HINTS" :
            "COMPLETE REPORT / %u FAMILY HINTS", matches);
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), line, UI_TEXT, 1), TAG, "C5 hint count");
        if (!matches) snprintf(line, sizeof(line), "NO SUPPORTED FAMILY HINT");
        else snprintf(line, sizeof(line), "%s%s%s%s%s%s",
            evidence->categories & CERBERUS_DUAL_BLE_FAMILY_IBEACON ? "IBEACON " : "",
            evidence->categories & CERBERUS_DUAL_BLE_FAMILY_FIND_MY ? "FIND MY " : "",
            evidence->categories & CERBERUS_DUAL_BLE_FAMILY_FLIPPER ? "FLIPPER " : "",
            evidence->categories & CERBERUS_DUAL_BLE_FAMILY_FLOCK ? (address_only ? "FLOCK ADDR " : "FLOCK ") : "",
            evidence->categories & CERBERUS_DUAL_BLE_FAMILY_META_HINT ? "META " : "",
            evidence->categories & CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT ? "HC NAME" : "");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U), line, UI_TEXT, 1), TAG, "C5 hint families");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 38U), response ? "MATCHED RESPONSE / ADV KEPT SEPARATE" : "ORIGINAL ADV ONLY / NO RESPONSE DATA", UI_MUTED, 1), TAG, "C5 hint provenance");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), full_callback ? (saved ? "SAVED FORMAT HINTS / NOT VERIFIED ID" :
            "SD SAVES PREVIEW + DECODED HINTS") :
            evidence->meta_blocked ? "META ID RULE SUPPRESSES META HINT" :
            evidence->ibeacon_result == CERBERUS_IBEACON_MALFORMED ? "IBEACON TUPLE AMBIGUOUS / NOT RETAINED" :
            "ADVERTISED HINTS / NOT VERIFIED ID", UI_MUTED, 1), TAG, "C5 hint meaning");
    } else {
        uint8_t family = 0U, remaining = page, meta_offset = 0U;
        for (unsigned bit = 0U; bit < 6U; ++bit) {
            if (!(page_bits & (1U << bit))) continue;
            uint8_t span = (1U << bit) == CERBERUS_DUAL_BLE_FAMILY_META_HINT ? meta_pages : 1U;
            if (remaining <= span) {
                family = (uint8_t)(1U << bit);
                meta_offset = (uint8_t)((remaining - 1U) * 2U);
                break;
            }
            remaining -= span;
        }
        if (family == CERBERUS_DUAL_BLE_FAMILY_IBEACON) {
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), "IBEACON ADVERTISED FORMAT", UI_TEXT, 1), TAG, "C5 iBeacon hint");
            cerberus_ibeacon_uuid_line(&evidence->ibeacon, line, sizeof(line));
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U), line, UI_TEXT, 1), TAG, "C5 iBeacon UUID");
            snprintf(line, sizeof(line), "MAJOR %u / MINOR %u", evidence->ibeacon.major, evidence->ibeacon.minor);
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 38U), line, UI_TEXT, 1), TAG, "C5 iBeacon values");
            snprintf(line, sizeof(line), "MEASURED POWER %d dBm (ADVERTISED)", evidence->ibeacon.measured_power_dbm);
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), line, UI_MUTED, 1), TAG, "C5 iBeacon power");
        } else if (family == CERBERUS_DUAL_BLE_FAMILY_FIND_MY) {
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), "FIND MY FORMAT HINTS", UI_TEXT, 1), TAG, "C5 Find My hint");
            snprintf(line, sizeof(line), "APPLE %s / FMNA %s / DULT %s",
                evidence->findmy.bits & CERBERUS_BLE_FINDMY_HINT_APPLE ? "YES" : "NO",
                evidence->findmy.bits & CERBERUS_BLE_FINDMY_HINT_FMNA ? "YES" : "NO",
                evidence->findmy.bits & CERBERUS_BLE_FINDMY_HINT_DULT ? "YES" : "NO");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U), line, UI_TEXT, 1), TAG, "C5 Find My values");
            snprintf(line, sizeof(line), "FMNA UUID WIDTHS:%s%s%s%s",
                evidence->findmy.fmna_uuid_widths & CERBERUS_BLE_FINDMY_UUID16 ? " 16" : "",
                evidence->findmy.fmna_uuid_widths & CERBERUS_BLE_FINDMY_UUID32 ? " 32" : "",
                evidence->findmy.fmna_uuid_widths & CERBERUS_BLE_FINDMY_UUID128 ? " 128" : "",
                evidence->findmy.fmna_uuid_widths ? "" : " NONE");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 38U), line, UI_TEXT, 1), TAG, "C5 Find My widths");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), "RECEIVED HINTS / NOT A DEVICE ID", UI_MUTED, 1), TAG, "C5 Find My scope");
        } else if (family == CERBERUS_DUAL_BLE_FAMILY_FLIPPER) {
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), "FLIPPER ADVERTISED SERVICE CODES", UI_TEXT, 1), TAG, "C5 Flipper hint");
            snprintf(line, sizeof(line), "CODES:%s%s%s%s",
                evidence->flipper.bits & CERBERUS_BLE_FLIPPER_HINT_BASE ? " 3080" : "",
                evidence->flipper.bits & CERBERUS_BLE_FLIPPER_HINT_BLACK ? " 3081" : "",
                evidence->flipper.bits & CERBERUS_BLE_FLIPPER_HINT_WHITE ? " 3082" : "",
                evidence->flipper.bits & CERBERUS_BLE_FLIPPER_HINT_TRANSPARENT ? " 3083" : "");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U), line, UI_TEXT, 1), TAG, "C5 Flipper codes");
            snprintf(line, sizeof(line), "UUID WIDTHS:%s%s%s",
                evidence->flipper.flags & CERBERUS_BLE_FLIPPER_UUID16 ? " 16" : "",
                evidence->flipper.flags & CERBERUS_BLE_FLIPPER_UUID32 ? " 32" : "",
                evidence->flipper.flags & CERBERUS_BLE_FLIPPER_UUID128 ? " 128" : "");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 38U), line, UI_TEXT, 1), TAG, "C5 Flipper widths");
            snprintf(line, sizeof(line), "SAME-REPORT NAME %s / SERVICE DATA %s",
                evidence->flipper.flags & CERBERUS_BLE_FLIPPER_NAME ? "YES" : "NO",
                evidence->flipper.flags & CERBERUS_BLE_FLIPPER_SERVICE_DATA ? "YES" : "NO");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), line, UI_MUTED, 1), TAG, "C5 Flipper context");
        } else if (family == CERBERUS_DUAL_BLE_FAMILY_FLOCK) {
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), address_only ? "FLOCK ADDRESS PREFIX CLUE" :
                "FLOCK ADVERTISED CLUES", UI_TEXT, 1), TAG, "C5 Flock hint");
            const char *name = evidence->flock.facts & CERBERUS_BLE_FLOCK_FS_BATTERY ? "FS BATTERY" :
                evidence->flock.facts & CERBERUS_BLE_FLOCK_PENGUIN ? "PENGUIN" :
                evidence->flock.facts & CERBERUS_BLE_FLOCK_SERIAL10 ? "10 DIGITS" :
                evidence->flock.facts & CERBERUS_BLE_FLOCK_NO_LOCAL_NAME ? "NONE" : "OTHER";
            snprintf(line, sizeof(line), "NAME %s / MFG 09C8 %s", name,
                evidence->flock.facts & CERBERUS_BLE_FLOCK_MFG_09C8 ? "YES" : "NO");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U), line, UI_TEXT, 1), TAG, "C5 Flock fields");
            snprintf(line, sizeof(line), "SERVICES:%s%s%s%s",
                evidence->flock.accessory_contexts ? " ACCESSORY" : "",
                evidence->flock.raven_3100_contexts ? " 3100" : "",
                evidence->flock.raven_3200_contexts ? " 3200" : "",
                evidence->flock.accessory_contexts || evidence->flock.raven_3100_contexts ||
                    evidence->flock.raven_3200_contexts ? "" : " NONE");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 38U), line, UI_TEXT, 1), TAG, "C5 Flock services");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), evidence->flock_oui ? "PUBLIC ADDRESS PREFIX CLUE / NOT ID" :
                "FORMAT CLUES / NOT VERIFIED ID", UI_MUTED, 1), TAG, "C5 Flock scope");
        } else if (family == CERBERUS_DUAL_BLE_FAMILY_META_HINT) {
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), evidence->meta_blocked ? "META HINT SUPPRESSED BY ID RULE" :
                "META IDENTIFIER HINTS", UI_TEXT, 1), TAG, "C5 Meta hint");
            for (unsigned pair = 0U; pair < 2U; ++pair) {
                uint16_t identifier; cerberus_ble_meta_context_t context;
                if (cerberus_ble_meta_evidence_at(&evidence->meta, meta_offset + pair, &identifier, &context))
                    snprintf(line, sizeof(line), "ID %04X / %s", identifier, cerberus_meta_context_label(context));
                else line[0] = '\0';
                ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U + pair * 12U), line, UI_TEXT, 1), TAG, "C5 Meta pair");
            }
            if (meta_count > 2U) snprintf(line, sizeof(line), "ID/CONTEXT PAIRS %u-%u OF %u / HINTS",
                meta_offset + 1U, meta_offset + 2U <= meta_count ? meta_offset + 2U : meta_count, meta_count);
            else snprintf(line, sizeof(line), "IDENTIFIER HINTS / NOT A MODEL ID");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), line, UI_MUTED, 1), TAG, "C5 Meta scope");
        } else if (family == CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT) {
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 14U), "HC COMPLETE-NAME HINT", UI_TEXT, 1), TAG, "C5 HC hint");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 26U), cerberus_ble_hc_name_label(evidence->hc_name), UI_TEXT, 1), TAG, "C5 HC name");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 38U), response ? "EXACT NAME IN THIS MATCHED RESPONSE" : "EXACT NAME IN THIS ORIGINAL ADV", UI_MUTED, 1), TAG, "C5 HC source");
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(top + 50U), "NAME DOES NOT IDENTIFY HARDWARE", UI_MUTED, 1), TAG, "C5 HC scope");
        }
    }
    *painted_pages = pages;
    return ESP_OK;
}

/* Compatibility for existing original-only live/saved callers. */
static inline esp_err_t ble_family_hint_ui_draw(
    const cerberus_dual_ble_family_evidence_t *evidence, bool full_callback,
    uint8_t full_length, uint8_t preview_length, bool saved, uint16_t top,
    uint8_t requested_page, uint8_t *painted_pages)
{
    return ble_family_hint_ui_draw_source(evidence, full_callback, full_length,
        preview_length, saved, false, top, requested_page, painted_pages);
}
