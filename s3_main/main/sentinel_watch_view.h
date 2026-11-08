#pragma once

#include "esp_err.h"
#include "sentinel_watch_detect.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scene identifiers are independent of classifier enums. Art never determines
 * a detection or implies that a broadcast hint proves a device's identity. */
typedef enum {
    SENTINEL_WATCH_SCENE_GUARDIAN = 0,
    SENTINEL_WATCH_SCENE_AXON,
    SENTINEL_WATCH_SCENE_FLOCK,
    SENTINEL_WATCH_SCENE_WELLNESS,
    SENTINEL_WATCH_SCENE_FINDMY,
    SENTINEL_WATCH_SCENE_TRACKER,
    SENTINEL_WATCH_SCENE_CAMERA,
    SENTINEL_WATCH_SCENE_SMART_GLASSES,
    SENTINEL_WATCH_SCENE_FLIPPER,
    SENTINEL_WATCH_SCENE_MESHTASTIC,
    SENTINEL_WATCH_SCENE_SAMSUNG,
    SENTINEL_WATCH_SCENE_BEACON,
    SENTINEL_WATCH_SCENE_PWNAGOTCHI,
    SENTINEL_WATCH_SCENE_DRONE,
    SENTINEL_WATCH_SCENE_BLE_ACTIVITY,
    SENTINEL_WATCH_SCENE_COUNT,
} sentinel_watch_scene_t;

#define SENTINEL_WATCH_DASHBOARD_ROWS 4U
#define SENTINEL_WATCH_DASHBOARD_BUCKETS 32U
typedef struct {
    char name[39], identity[34], hint[25], age[17];
    sentinel_watch_scene_t scene;
    int8_t rssi;
    bool rssi_valid;
} sentinel_watch_dashboard_row_t;
/* Value-only presentation, owned by the existing optional PSRAM inventory.
 * Rows correspond to separately frozen source/typed-identity proofs. Graph
 * values are inventory additions/expirations, not RF rates or movement. */
typedef struct {
    sentinel_watch_dashboard_row_t rows[SENTINEL_WATCH_DASHBOARD_ROWS];
    uint32_t added[SENTINEL_WATCH_DASHBOARD_BUCKETS];
    uint32_t expired[SENTINEL_WATCH_DASHBOARD_BUCKETS];
    uint8_t count, page, pages, total, buckets;
} sentinel_watch_dashboard_t;

typedef struct {
    sentinel_watch_scene_t scene;
    sentinel_watch_confidence_t confidence;
    bool demo;
    bool paused;
    bool releasing; /* Paused intent still owns cleanup; badge only, not controls. */
    bool radio_online;
    bool has_details;
    uint16_t observation_count;
    uint16_t match_count;
    const char *phase;    /* Actual operation, e.g. "BLE LISTENING / 4S LEFT". */
    const char *title;    /* Optional; at most 24 displayed ASCII characters. */
    const char *evidence; /* Actual broadcast evidence; at most 48 characters. */
    const char *detail;   /* Second evidence/context line; at most 48 characters. */
    const sentinel_watch_dashboard_t *dashboard;
    const char *wifi_status, *ble_status, *sd_status; /* Also available without inventory RAM. */
    bool radar; /* RSSI bands only: never direction, distance or localization. */
} sentinel_watch_view_t;

typedef enum {
    SENTINEL_WATCH_ACTION_NONE = 0,
    SENTINEL_WATCH_ACTION_BACK,
    SENTINEL_WATCH_ACTION_PAUSE, /* Resume when paused. Disabled in demo. */
    SENTINEL_WATCH_ACTION_HISTORY,
    SENTINEL_WATCH_ACTION_DEMO,  /* Next scene when demo. */
    SENTINEL_WATCH_ACTION_DETAILS,
    SENTINEL_WATCH_ACTION_PREV,
    SENTINEL_WATCH_ACTION_NEXT,
    SENTINEL_WATCH_ACTION_ROW_0,
    SENTINEL_WATCH_ACTION_ROW_1,
    SENTINEL_WATCH_ACTION_ROW_2,
    SENTINEL_WATCH_ACTION_ROW_3,
    SENTINEL_WATCH_ACTION_DASHBOARD,
    SENTINEL_WATCH_ACTION_RADAR,
} sentinel_watch_action_t;

/* Owns the complete 320x480 screen. Call draw on entry, update on UI ticks.
 * Live dashboard/radar contains only supplied inventory/signal information.
 * No mascot, inferred bearing or distance is drawn. Both calls repaint without
 * a framebuffer/cache/allocation and never read a clock or radio. The UI bounds
 * periodic refresh to500ms, with immediate touch redraws. The caller freezes
 * row proofs first, publishes their tap map only after a successful paint, and
 * presents the composed frame on the same UI task. now_ms is API compatibility.
 * Main view tabs: Dashboard x100,w124 and Radar x232,w80, y4,h38. Changing
 * view never changes the source session or SD recording. Dashboard rows:
 * x8,y178,49px spacing,46px active height. Radar legend rows:
 * x8,y304,18px spacing. Prev/Next: x8/244,y378,w68,h27.
 * Buttons y426,h44: Back x8,w68; Pause/Resume x84,w68; History x160,w68;
 * Options/Next x236,w76 (legacy ACTION_DEMO id). Demo disables Pause and
 * History. Gaps never activate. The legacy demo flag marks a non-live text
 * preview only; Recon navigation has no gallery or demo route.
 */
esp_err_t sentinel_watch_view_draw(const sentinel_watch_view_t *view, uint32_t now_ms);
esp_err_t sentinel_watch_view_update(const sentinel_watch_view_t *view, uint32_t now_ms);
void sentinel_watch_view_invalidate(void);
sentinel_watch_action_t sentinel_watch_view_hit_test(const sentinel_watch_view_t *view,
                                                   uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif
