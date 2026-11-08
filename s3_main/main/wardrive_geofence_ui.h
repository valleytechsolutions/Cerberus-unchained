/* The UI owns this optional editor. A recording copies the saved configuration
 * before starting, so editing cannot change an active boundary decision. */
#include "wardrive_geofence.h"
#include <math.h>

enum { WD_AREA_LIST, WD_AREA_EDIT, WD_AREA_INPUT, WD_AREA_CLEAR, WD_AREA_RESET };
typedef struct {
    cerberus_geofence_config_t config;
    cerberus_keyboard_t keyboard;
    char input[48], message[52];
    uint8_t page, slot, field, coordinates;
    bool valid;
} wardrive_area_ui_t;
static wardrive_area_ui_t *wd_area_ui;

static bool wardrive_areas_is_open(void)
{ return wd_area_ui && current_page == UI_PAGE_WARDRIVE; }

static void wardrive_areas_close(void)
{
    if (wd_area_ui) memset(wd_area_ui, 0, sizeof(*wd_area_ui));
    heap_caps_free(wd_area_ui); wd_area_ui = NULL;
}

static void wardrive_areas_release_if_hidden(bool asleep)
{
    if (asleep || current_page != UI_PAGE_WARDRIVE) wardrive_areas_close();
}

static bool wardrive_areas_busy(void)
{
    cerberus_wardrive_status_t status;
    cerberus_wardrive_get_status(&status);
    return wardrive_start_pending || status.state == CERBERUS_WARDRIVE_STARTING ||
        status.state == CERBERUS_WARDRIVE_RUNNING || status.state == CERBERUS_WARDRIVE_STOPPING;
}

static esp_err_t wardrive_areas_start_check(void)
{
    cerberus_geofence_config_t config;
    esp_err_t error = cerberus_settings_geofences_get(&config);
    if (error != ESP_OK || !cerberus_geofence_config_valid(&config)) {
        snprintf(wardrive_blocked_reason, sizeof(wardrive_blocked_reason), "REVIEW GEOFENCE SETTINGS");
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i) {
        if (config.areas[i].enabled && wardrive_include_expansions) {
            snprintf(wardrive_blocked_reason, sizeof(wardrive_blocked_reason), "GEOFENCES REQUIRE R1-R6 OFF");
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

static void wardrive_areas_load(void)
{
    wd_area_ui->valid = cerberus_settings_geofences_get(&wd_area_ui->config) == ESP_OK &&
        cerberus_geofence_config_valid(&wd_area_ui->config);
    if (!wd_area_ui->valid)
        snprintf(wd_area_ui->message, sizeof(wd_area_ui->message), "SETTINGS UNREADABLE / RESET TO REPLACE");
}

static void wardrive_areas_open(void)
{
    if (!wd_area_ui) wd_area_ui = heap_caps_calloc(1, sizeof(*wd_area_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wd_area_ui) {
        wardrive_start_error = ESP_ERR_NO_MEM;
        snprintf(wardrive_blocked_reason, sizeof(wardrive_blocked_reason), "NO MEMORY FOR AREA EDITOR");
    } else {
        wd_area_ui->page = WD_AREA_LIST; wd_area_ui->message[0] = '\0';
        wardrive_areas_load();
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

static bool wardrive_areas_current_fix(void)
{
    cerberus_wardrive_gps_uart_state_t receiver = {0};
    if (cerberus_wardrive_gps_uart_get_state(&receiver) != ESP_OK || receiver.error != ESP_OK ||
        receiver.gps.state != CERBERUS_WARDRIVE_GPS_FRESH || !receiver.gps.coordinates_valid ||
        receiver.gps.fix_age_ms > CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS ||
        !isfinite(receiver.gps.latitude) || !isfinite(receiver.gps.longitude) ||
        fabs(receiver.gps.latitude) > 90.0 || fabs(receiver.gps.longitude) > 180.0) return false;
    cerberus_geofence_area_t *area = &wd_area_ui->config.areas[wd_area_ui->slot];
    area->latitude = receiver.gps.latitude; area->longitude = receiver.gps.longitude;
    wd_area_ui->coordinates = 3U;
    return true;
}

static void wardrive_areas_save(void)
{
    esp_err_t error = cerberus_settings_geofences_set(&wd_area_ui->config);
    if (error == ESP_OK) {
        wd_area_ui->valid = true; wd_area_ui->page = WD_AREA_LIST;
        snprintf(wd_area_ui->message, sizeof(wd_area_ui->message), "SAVED / APPLIES TO NEXT RECORDING");
    } else snprintf(wd_area_ui->message, sizeof(wd_area_ui->message), "SAVE FAILED: %s", esp_err_to_name(error));
}

static esp_err_t wardrive_areas_draw(void)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "area clear");
    ESP_RETURN_ON_ERROR(draw_header("Geofences"), TAG, "area header");
    if (wd_area_ui->page == WD_AREA_INPUT) {
        const char *prompt = wd_area_ui->field == 0U ? "AREA NAME" :
            wd_area_ui->field == 1U ? "LATITUDE / DECIMAL DEGREES" : "LONGITUDE / DECIMAL DEGREES";
        ESP_RETURN_ON_ERROR(cerberus_keyboard_draw(&wd_area_ui->keyboard, prompt), TAG, "area keyboard");
        return draw_small_button(8, 432, 304, 36, "CANCEL FIELD");
    }
    bool busy = wardrive_areas_busy();
    char line[64];
    const char *message = busy ? "STOP WARDRIVING BEFORE EDITING" : wd_area_ui->message;
    if (wd_area_ui->page == WD_AREA_LIST) {
        ESP_RETURN_ON_ERROR(draw_centered(62, "PAUSE BOTH RADIOS INSIDE SAVED AREAS", UI_TEXT, 1), TAG, "area purpose");
        ESP_RETURN_ON_ERROR(draw_centered(82, "RESUME OUTSIDE / KEEP ONE CSV", UI_MUTED, 1), TAG, "area resume");
        for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i) {
            const cerberus_geofence_area_t *area = &wd_area_ui->config.areas[i];
            snprintf(line, sizeof(line), "%u  %-3s  %.32s", i + 1U,
                wd_area_ui->valid && area->enabled ? "ON" : "OFF",
                !wd_area_ui->valid ? "UNREADABLE" : area->name[0] ? area->name : "ADD AREA");
            ESP_RETURN_ON_ERROR(draw_small_button(16, 106 + 48U * i, 288, 38, line), TAG, "area row");
        }
        ESP_RETURN_ON_ERROR(draw_centered(350, message, UI_MUTED, 1), TAG, "area message");
        ESP_RETURN_ON_ERROR(draw_small_button(24, 374, 272, 32, "RESET ALL AREAS"), TAG, "area reset");
        return draw_small_button(8, 432, 304, 36, "BACK TO WARDRIVING");
    }
    if (wd_area_ui->page == WD_AREA_CLEAR || wd_area_ui->page == WD_AREA_RESET) {
        ESP_RETURN_ON_ERROR(draw_centered(134, wd_area_ui->page == WD_AREA_CLEAR ?
            "REMOVE THIS SAVED AREA?" : "REPLACE ALL FIVE AREAS WITH DEFAULTS?", UI_TEXT, 1), TAG, "area confirm");
        ESP_RETURN_ON_ERROR(draw_centered(168, "SAVED AREA SETTINGS WILL BE CLEARED", UI_MUTED, 1), TAG, "area reset scope");
        ESP_RETURN_ON_ERROR(draw_centered(240, message, UI_MUTED, 1), TAG, "area save result");
        ESP_RETURN_ON_ERROR(draw_small_button(24, 342, 272, 36, "CONFIRM CLEAR"), TAG, "area confirm button");
        return draw_small_button(8, 432, 304, 36, "CANCEL");
    }
    const cerberus_geofence_area_t *area = &wd_area_ui->config.areas[wd_area_ui->slot];
    snprintf(line, sizeof(line), "AREA %u / TAP A FIELD TO EDIT", wd_area_ui->slot + 1U);
    ESP_RETURN_ON_ERROR(draw_centered(66, line, UI_MUTED, 1), TAG, "area edit");
    snprintf(line, sizeof(line), "NAME: %.32s", area->name);
    ESP_RETURN_ON_ERROR(draw_small_button(16, 96, 288, 32, line), TAG, "area name");
    if (wd_area_ui->coordinates & 1U) snprintf(line, sizeof(line), "LATITUDE: %.6f", area->latitude);
    else snprintf(line, sizeof(line), "LATITUDE: SET CENTER");
    ESP_RETURN_ON_ERROR(draw_small_button(16, 136, 288, 32, line), TAG, "area latitude");
    if (wd_area_ui->coordinates & 2U) snprintf(line, sizeof(line), "LONGITUDE: %.6f", area->longitude);
    else snprintf(line, sizeof(line), "LONGITUDE: SET CENTER");
    ESP_RETURN_ON_ERROR(draw_small_button(16, 176, 288, 32, line), TAG, "area longitude");
    snprintf(line, sizeof(line), "RADIUS: %.1f MI / %.0f M", area->radius_miles, area->radius_miles * 1609.344);
    ESP_RETURN_ON_ERROR(draw_small_button(16, 216, 288, 32, line), TAG, "area radius");
    ESP_RETURN_ON_ERROR(draw_small_button(16, 256, 288, 32, "USE CURRENT GPS POSITION"), TAG, "area GPS");
    ESP_RETURN_ON_ERROR(draw_small_button(16, 296, 288, 32, area->enabled ? "AREA: ON" : "AREA: OFF"), TAG, "area enabled");
    ESP_RETURN_ON_ERROR(draw_small_button(16, 342, 140, 32, "SAVE AREA"), TAG, "area save");
    ESP_RETURN_ON_ERROR(draw_small_button(164, 342, 140, 32, "CLEAR AREA"), TAG, "area clear");
    ESP_RETURN_ON_ERROR(draw_centered(390, message, UI_MUTED, 1), TAG, "area edit result");
    return draw_small_button(8, 432, 304, 36, "BACK TO AREAS / DISCARD EDITS");
}

static void wardrive_areas_handle_tap(uint16_t x, uint16_t y)
{
    if (!wd_area_ui) return;
    if (y >= 432U && y < 468U && x >= 8U && x < 312U) {
        if (wd_area_ui->page == WD_AREA_LIST) wardrive_areas_close();
        else if (wd_area_ui->page == WD_AREA_INPUT) wd_area_ui->page = WD_AREA_EDIT;
        else {
            wd_area_ui->page = WD_AREA_LIST; wd_area_ui->message[0] = '\0';
            wardrive_areas_load();
        }
        ESP_ERROR_CHECK(draw_detail_page()); return;
    }
    if (wardrive_areas_busy()) return;
    wd_area_ui->message[0] = '\0';
    if (wd_area_ui->page == WD_AREA_INPUT) {
        cerberus_keyboard_action_t action;
        if (cerberus_keyboard_handle_tap(&wd_area_ui->keyboard, x, y, &action) != ESP_OK) return;
        if (action == CERBERUS_KEYBOARD_ACTION_NONE) return;
        if (action == CERBERUS_KEYBOARD_ACTION_SUBMIT) {
            cerberus_geofence_area_t *area = &wd_area_ui->config.areas[wd_area_ui->slot];
            if (!wd_area_ui->field) snprintf(area->name, sizeof(area->name), "%.32s", wd_area_ui->input);
            else {
                char *end; double value = strtod(wd_area_ui->input, &end);
                double limit = wd_area_ui->field == 1U ? 90.0 : 180.0;
                if (!wd_area_ui->input[0] || *end || !isfinite(value) || fabs(value) > limit)
                    snprintf(wd_area_ui->message, sizeof(wd_area_ui->message), "INVALID COORDINATE / VALUE UNCHANGED");
                else {
                    if (wd_area_ui->field == 1U) area->latitude = value; else area->longitude = value;
                    wd_area_ui->coordinates |= 1U << (wd_area_ui->field - 1U);
                }
            }
            memset(wd_area_ui->input, 0, sizeof(wd_area_ui->input)); wd_area_ui->page = WD_AREA_EDIT;
        }
    } else if (wd_area_ui->page == WD_AREA_LIST) {
        if (y >= 374U && y < 406U && x >= 24U && x < 296U) wd_area_ui->page = WD_AREA_RESET;
        else if (wd_area_ui->valid && x >= 16U && x < 304U && y >= 106U && y < 336U && (y - 106U) % 48U < 38U) {
            wd_area_ui->slot = (y - 106U) / 48U;
            cerberus_geofence_area_t *area = &wd_area_ui->config.areas[wd_area_ui->slot];
            wd_area_ui->coordinates = area->name[0] ? 3U : 0U;
            if (!area->name[0]) {
                snprintf(area->name, sizeof(area->name), "AREA %u", wd_area_ui->slot + 1U);
                area->enabled = 1U; (void)wardrive_areas_current_fix();
            }
            wd_area_ui->page = WD_AREA_EDIT;
        }
    } else if (wd_area_ui->page == WD_AREA_CLEAR || wd_area_ui->page == WD_AREA_RESET) {
        if (y >= 342U && y < 378U && x >= 24U && x < 296U) {
            cerberus_geofence_config_t defaults = cerberus_geofence_defaults();
            if (wd_area_ui->page == WD_AREA_RESET) wd_area_ui->config = defaults;
            else wd_area_ui->config.areas[wd_area_ui->slot] = defaults.areas[wd_area_ui->slot];
            wardrive_areas_save();
        }
    } else if (x >= 16U && x < 304U) {
        cerberus_geofence_area_t *area = &wd_area_ui->config.areas[wd_area_ui->slot];
        if (y >= 96U && y < 208U && (y - 96U) % 40U < 32U) {
            wd_area_ui->field = (y - 96U) / 40U;
            char initial[48] = {0};
            if (!wd_area_ui->field) snprintf(initial, sizeof(initial), "%s", area->name);
            else if (wd_area_ui->coordinates & (1U << (wd_area_ui->field - 1U)))
                snprintf(initial, sizeof(initial), "%.6f", wd_area_ui->field == 1U ? area->latitude : area->longitude);
            size_t capacity = wd_area_ui->field ? sizeof(wd_area_ui->input) : sizeof(area->name);
            if (cerberus_keyboard_begin(&wd_area_ui->keyboard, wd_area_ui->input, capacity, initial) == ESP_OK)
                wd_area_ui->page = WD_AREA_INPUT;
        } else if (y >= 216U && y < 248U) {
            unsigned tenths = (unsigned)lround(area->radius_miles * 10.0);
            area->radius_miles = (tenths % 10U + 1U) / 10.0;
        } else if (y >= 256U && y < 288U) {
            if (!wardrive_areas_current_fix()) snprintf(wd_area_ui->message, sizeof(wd_area_ui->message), "WAIT FOR A FRESH GPS POSITION");
        } else if (y >= 296U && y < 328U) area->enabled = !area->enabled;
        else if (y >= 342U && y < 374U) {
            if (x >= 164U) wd_area_ui->page = WD_AREA_CLEAR;
            else if (x < 156U) {
                if (wd_area_ui->coordinates != 3U)
                    snprintf(wd_area_ui->message, sizeof(wd_area_ui->message), "SET BOTH COORDINATES OR USE GPS");
                else wardrive_areas_save();
            }
        }
    }
    ESP_ERROR_CHECK(draw_detail_page());
}
