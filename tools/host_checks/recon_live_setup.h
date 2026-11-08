/* Supporting UI and C5 adapter boundaries for the actual Recon scheduler.
 * The Wi-Fi transfer has its own full transaction tests; this suite tests its
 * independent admission/completion while S3 ownership fails or drains. */
#include "wifi_readout_diagnostics.h"
enum {UI_WIFI_MODE_LIST,UI_WIFI_MODE_STATS,UI_WIFI_MODE_BASELINE,UI_WIFI_MODE_DEFENSE,
    UI_WIFI_MODE_DETAIL,UI_WIFI_MODE_TARGET,UI_WIFI_MODE_PASSWORD,UI_WIFI_MODE_CONNECTION,
    UI_WIFI_MODE_STATIONS,UI_WIFI_MODE_OBSERVED_CLIENTS,UI_WIFI_MODE_AP_JOB};
enum {UI_BLE_MODE_MENU,UI_BLE_MODE_LIST,UI_BLE_MODE_DETAIL};
enum {UI_IEEE802154_MODE_ENERGY,UI_IEEE802154_MODE_BEACONS,UI_IEEE802154_MODE_ACTIVITY,UI_IEEE802154_MODE_ASSESSMENT};
static unsigned wifi_mode,ble_mode,ieee802154_mode;
static struct {bool attack;} field_menu;
static bool wifi_selection_valid;
static struct {struct {uint64_t epoch;} session;} *ble_discovery_live;
static bool ble_discovery_page(void){return current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_LIST;}
static void ble_discovery_tick(uint64_t now,bool asleep){(void)now;(void)asleep;}
static bool cerberus_ble_discovery_session_released(const void *s){(void)s;return true;}
static bool live_status_changed,live_owned_wifi_observation,live_scan_observation_valid,live_ble_observation_valid;
static uint32_t live_capture_pending,live_frame;
static uint16_t live_page[SENTINEL_SCAN_TOOL_COUNT];
static uint8_t live_ble_focus_bits[CERBERUS_BLE_SCAN_MAX_RESULTS];
static bool live_ble_order_dirty;
static int64_t live_ble_published_ms;
static int8_t live_energy[16];
static uint8_t live_activity[16][CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT];
static uint8_t ieee802154_activity_counts[16][CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT];
static uint16_t wifi_defense_loaded_count;
static cerberus_ieee802154_energy_scan_status_payload_t ieee802154_scan_status;
static cerberus_ieee802154_energy_scan_result_payload_t ieee802154_scan_result;
static cerberus_ieee802154_beacon_scan_status_payload_t ieee802154_beacon_status;
static cerberus_ieee802154_activity_scan_status_payload_t ieee802154_activity_status;
static cerberus_wifi_defense_status_payload_t wifi_defense_status;
static esp_err_t wifi_scan_error,ble_scan_error,ieee802154_scan_error,ieee802154_beacon_error,ieee802154_activity_error,wifi_defense_error;
static bool wifi_scan_checked,ble_scan_checked,ieee802154_scan_checked,ieee802154_beacon_checked,ieee802154_activity_checked,wifi_defense_checked;
static unsigned c5_starts,c5_stops,c5_status_reads,c5_captures,ui_redraws;
static esp_err_t c5_start_error,c5_query_error;
static bool c5_held;
static cerberus_wifi_scan_status_payload_t c5_scan;
static void live_wifi_capture_reset_as(cerberus_wifi_readout_cancel_reason_t reason){(void)reason;}
static esp_err_t live_wifi_capture_error(void){return ESP_OK;}
static bool live_wifi_capture_defers_c5(void){return false;}
static bool mesh_ui_busy(void){return false;}
/* This scheduler fixture has no selected-target counter session. */
static bool wifi_packet_count_ui_busy(void){return false;}
static bool channel_analyzer_ui_busy(void){return false;}
static bool wifi_fox_hunt_ui_busy(void){return false;}
static bool saved_ble_lookup_ui_busy(void){return false;}
static bool live_wifi_capture_tick(uint32_t now,bool allowed,cerberus_wifi_readout_cancel_reason_t reason)
{(void)now;(void)allowed;(void)reason;return false;}
static bool live_wifi_capture_begin(uint32_t now)
{
    ++c5_captures;
    live_scan_record_t *r=&live_records[SENTINEL_SCAN_WIFI];
    r->count=c5_scan.stored_count;r->captured=true;r->saved_generation=r->generation;r->captured_ms=now;
    live_capture_pending&=~(UINT32_C(1)<<SENTINEL_SCAN_WIFI);
    if(r->count){strcpy(store.wifi[0].ssid,"Hint AP");store.wifi[0].bssid[5]=42;
        store.wifi[0].rssi=-65;store.wifi[0].channel=36;}
    return true;
}
esp_err_t c5_spi_link_get_wifi_scan_status(cerberus_wifi_scan_status_payload_t *out)
{++c5_status_reads;*out=c5_scan;return c5_query_error;}
esp_err_t c5_spi_link_get_wifi_scan_status_once(uint32_t deadline,cerberus_wifi_scan_status_payload_t *out)
{CHECK(deadline==(uint32_t)((uint64_t)clock_us/1000U)+25U);return c5_spi_link_get_wifi_scan_status(out);}
esp_err_t c5_spi_link_start_wifi_scan(cerberus_wifi_scan_status_payload_t *out)
{++c5_starts;if(c5_start_error)return c5_start_error;++c5_scan.generation;
 c5_scan.state=CERBERUS_WIFI_SCAN_RUNNING;*out=c5_scan;return ESP_OK;}
esp_err_t c5_spi_link_stop_wifi_scan(cerberus_wifi_scan_status_payload_t *out)
{++c5_stops;if(!c5_held)c5_scan.state=CERBERUS_WIFI_SCAN_IDLE;*out=c5_scan;return ESP_OK;}
esp_err_t c5_spi_link_get_wifi_connection_status(cerberus_wifi_connection_status_payload_t *out)
{memset(out,0,sizeof(*out));return ESP_OK;}
#define UNUSED_RADIO(name,type) esp_err_t name(type *out){memset(out,0,sizeof(*out));return ESP_ERR_NOT_SUPPORTED;}
UNUSED_RADIO(c5_spi_link_get_ieee802154_energy_scan_status,cerberus_ieee802154_energy_scan_status_payload_t)
UNUSED_RADIO(c5_spi_link_start_ieee802154_energy_scan,cerberus_ieee802154_energy_scan_status_payload_t)
UNUSED_RADIO(c5_spi_link_get_ieee802154_beacon_scan_status,cerberus_ieee802154_beacon_scan_status_payload_t)
UNUSED_RADIO(c5_spi_link_start_ieee802154_beacon_scan,cerberus_ieee802154_beacon_scan_status_payload_t)
UNUSED_RADIO(c5_spi_link_get_ieee802154_activity_scan_status,cerberus_ieee802154_activity_scan_status_payload_t)
UNUSED_RADIO(c5_spi_link_start_ieee802154_activity_scan,cerberus_ieee802154_activity_scan_status_payload_t)
UNUSED_RADIO(c5_spi_link_get_wifi_defense_status,cerberus_wifi_defense_status_payload_t)
UNUSED_RADIO(c5_spi_link_start_wifi_defense_survey,cerberus_wifi_defense_status_payload_t)
UNUSED_RADIO(c5_spi_link_stop_wifi_defense_survey,cerberus_wifi_defense_status_payload_t)
#undef UNUSED_RADIO
esp_err_t c5_spi_link_get_ieee802154_energy_scan_result(uint8_t g,cerberus_ieee802154_energy_scan_result_payload_t *r)
{(void)g;(void)r;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t c5_spi_link_get_ieee802154_beacon_scan_result(uint8_t g,uint8_t i,cerberus_ieee802154_beacon_scan_result_payload_t *r)
{(void)g;(void)i;(void)r;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t c5_spi_link_get_ieee802154_activity_scan_result(uint8_t g,uint8_t i,cerberus_ieee802154_activity_scan_result_payload_t *r)
{(void)g;(void)i;(void)r;return ESP_ERR_NOT_SUPPORTED;}
static esp_err_t refresh_wifi_defense_survey(void){return ESP_ERR_NOT_SUPPORTED;}
static sentinel_scan_tool_t watch_requested_tool(void);
static bool watch_ble_requested(void);
#include "recon_live_acquire_impl.h"
static bool live_is_view(void){return false;}
static void live_make_view(void){}
static esp_err_t live_paint(bool full){(void)full;return ESP_OK;}
static esp_err_t draw_current_page(void){++ui_redraws;return ESP_OK;}
