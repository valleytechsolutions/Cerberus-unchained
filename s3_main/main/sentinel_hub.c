#include "sentinel_hub.h"
#include "display.h"
#include "esp_check.h"

#include <stddef.h>

typedef struct {
    const char *title;
    const char *description;
} hub_item_t;

static const hub_item_t wifi_items[] = {
    {"Nearby networks", "2.4 + 5 GHZ AP DISCOVERY"},
    {"Survey summary", "COUNTS, SECURITY AND SCAN HEALTH"},
    {"Channel view", "BEACON / DISCONNECT / PROBE COUNTS"},
    {"Channel Analyzer", "TOTAL CHANNEL TRAFFIC / RECENT BEACON"},
    {"Probe Sniff", "PASSIVE REQUESTS / SAVE EVIDENCE"},
    {"Trusted baseline", "REVIEW YOUR SAVED NETWORKS"},
    {"Activity review", "BOUNDED DEFENSIVE OBSERVATIONS"},
    {"My connection", "CONNECT, INSPECT AND RUN CHECKS"},
};
static const hub_item_t ble_items[] = {
    {"Nearby devices", "PASSIVE ADVERTISEMENT DISCOVERY"},
    {"Device + signal", "INSPECT A DEVICE AND ITS HISTORY"},
    {"Services + values", "USER-INITIATED GATT CONNECTION"},
    {"Sensor viewer", "TEMPERATURE, HUMIDITY, PRESSURE"},
    {"Bluetooth Analyzer", "LIVE ADVERTISING RATE + EVIDENCE"},
};
static const hub_item_t radio_items[] = {
    {"Mesh Detection", "PASSIVE PAN / ADDRESS / TRAFFIC MAP"},
    {"Channel energy", "EXPLORE THE 2.4 GHZ AIRSPACE"},
    {"Zigbee beacons", "OBSERVED CLASSIC ZIGBEE BEACONS"},
    {"Frame activity", "PASSIVE 802.15.4 FRAME COUNTS"},
    {"Compare snapshots", "ENERGY + ACTIVITY EXPLAINED"},
};

static const hub_item_t *items_for(sentinel_hub_t hub, size_t *count)
{
    if (hub == SENTINEL_HUB_WIFI) {
        *count = sizeof(wifi_items) / sizeof(wifi_items[0]);
        return wifi_items;
    }
    if (hub == SENTINEL_HUB_BLE) {
        *count = sizeof(ble_items) / sizeof(ble_items[0]);
        return ble_items;
    }
    *count = sizeof(radio_items) / sizeof(radio_items[0]);
    return radio_items;
}

static uint16_t item_y(sentinel_hub_t hub, size_t index)
{
    return hub != SENTINEL_HUB_WIFI ? (uint16_t)(67U + index * 54U) :
        (uint16_t)(70U + index * 36U);
}

esp_err_t sentinel_hub_draw(sentinel_hub_t hub, const cerberus_ui_widget_theme_t *theme)
{
    if (theme == NULL || hub > SENTINEL_HUB_RADIO) return ESP_ERR_INVALID_ARG;
    size_t count;
    const hub_item_t *items = items_for(hub, &count);
    uint16_t accent = hub == SENTINEL_HUB_WIFI ? 0x2EFD :
        (hub == SENTINEL_HUB_BLE ? 0xBBBF : 0xFDAB);
    const char *subtitle = hub == SENTINEL_HUB_WIFI ? "WATCH / UNDERSTAND YOUR NETWORK" :
        (hub == SENTINEL_HUB_BLE ? "SCOUT / FOLLOW THE ADVERTISEMENTS" :
                                 "SENSE / EXPLORE SHARED AIRSPACE");
    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, 52, subtitle,
        theme->muted, theme->background, 1), "hub", "subtitle");
    for (size_t i = 0; i < count; ++i) {
        uint16_t y = item_y(hub, i);
        uint16_t height = hub == SENTINEL_HUB_WIFI ? 32U :
            48U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(12, y, 296, height,
            theme->panel), "hub", "panel");
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(12, y, 3, height,
            accent), "hub", "accent");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(24, y + (hub == SENTINEL_HUB_WIFI ? 3U : 6U), items[i].title,
            theme->text, theme->panel, 2), "hub", "title");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(24, y + (hub == SENTINEL_HUB_WIFI ? 21U : 28U), items[i].description,
            theme->muted, theme->panel, 1), "hub", "description");
    }
    if (hub != SENTINEL_HUB_WIFI) {
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(24,
            342U,
            hub == SENTINEL_HUB_BLE ? "RSSI SHOWS A TREND, NOT EXACT DISTANCE" :
                                     "OBSERVATIONS DO NOT PROVE ROUTING",
            theme->muted, theme->background, 1), "hub", "note");
    }
    if (hub == SENTINEL_HUB_WIFI) {
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){72, 364, 56, 42},
            "PINE", 1, true, theme), "hub", "pinescan");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){132, 364, 56, 42},
            "MULTI", 1, true, theme), "hub", "multissid");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){192, 364, 56, 42},
            "SURVEY", 1, true, theme), "hub", "survey");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){252, 364, 56, 42},
            "SAE", 1, true, theme), "hub", "SAE monitor");
        return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12, 364, 56, 42}, "LEARN", 1, true, theme);
    }
    if (hub == SENTINEL_HUB_BLE) {
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12, 364, 88, 42},
            "LEARN", 1, true, theme), "hub", "learn");
        return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){108, 364, 200, 42},
            "SD SNAPSHOTS", 2, true, theme);
    }
    return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12, 364, 296, 42},
        "LEARN WITH CERBERUS", 2, true, theme);
}

int sentinel_hub_hit_test(sentinel_hub_t hub, uint16_t x, uint16_t y)
{
    if (hub > SENTINEL_HUB_RADIO || x < 12 || x >= 308) return -1;
    if (y >= 364 && y < 406) {
        if (hub == SENTINEL_HUB_BLE) return x < 100 ? 6 : x >= 108 ? 7 : -1;
        if (hub != SENTINEL_HUB_WIFI) return 6;
        if (x < 68) return 6;
        if (x >= 72 && x < 128) return 9;
        if (x >= 132 && x < 188) return 8;
        if (x >= 192 && x < 248) return 7;
        if (x >= 252) return 12;
        return -1;
    }
    size_t count;
    (void)items_for(hub, &count);
    for (size_t i = 0; i < count; ++i) {
        uint16_t top = item_y(hub, i);
        uint16_t height = hub == SENTINEL_HUB_WIFI ? 32U :
            48U;
        if (y >= top && y < top + height) {
            static const uint8_t wifi_actions[] = {0, 1, 2, 11, 10, 3, 4, 5};
            return hub == SENTINEL_HUB_WIFI ? wifi_actions[i] : (int)i;
        }
    }
    return -1;
}
