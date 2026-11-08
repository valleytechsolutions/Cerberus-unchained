/* Share existing real capture + fake driver/PSRAM boundaries. Rename only its
 * old mock observer/scheduler and final-fence stub, then compile the actual
 * production observer/scheduler below. No production control flow is copied. */
#define main scan_store_fixture_main
#define live_observe fixture_observe
#define live_execute fixture_execute
#define live_scan_tick fixture_scan_tick
#define c5_spi_link_get_wifi_scan_status_once fixture_status_once
#define c5_spi_link_get_ieee802154_beacon_scan_status fixture_beacon_status
#define c5_spi_link_get_ieee802154_activity_scan_status fixture_activity_status
#define CERBERUS_SCAN_STORE_EXTERNAL_PASSIVE_CONTROL 1
#include "scan_display_store_check.c"
#undef CERBERUS_SCAN_STORE_EXTERNAL_PASSIVE_CONTROL
#undef main
#undef live_observe
#undef live_execute
#undef live_scan_tick
#undef c5_spi_link_get_wifi_scan_status_once
#undef c5_spi_link_get_ieee802154_beacon_scan_status
#undef c5_spi_link_get_ieee802154_activity_scan_status

static unsigned checks, owned_polls, full_polls, foreign_polls, connection_polls;
static unsigned ble_starts, ble_stops, other_starts;
static uint32_t last_deadline;
static bool live_owned_wifi_observation;
static bool wifi_scan_checked, ble_scan_checked, ieee802154_scan_checked;
static bool ieee802154_beacon_checked, ieee802154_activity_checked, wifi_defense_checked;
static esp_err_t wifi_scan_error, ble_scan_error, ieee802154_scan_error;
static esp_err_t ieee802154_beacon_error, ieee802154_activity_error, wifi_defense_error;
static esp_err_t owned_error, survey_error, connection_error, start_error;
static uint8_t connection_state;
static cerberus_c5_status_t c5_status;
static cerberus_wifi_scan_status_payload_t remote_wifi;
static cerberus_ieee802154_energy_scan_status_payload_t ieee802154_scan_status;
static cerberus_ieee802154_activity_scan_status_payload_t ieee802154_activity_status;
#define CHECK(value) do { ++checks; assert(value); } while (0)

esp_err_t c5_spi_link_get_wifi_scan_status_once(uint32_t deadline,
    cerberus_wifi_scan_status_payload_t *out)
{
    ++owned_polls; last_deadline = deadline;
    /* On error deliberately leave output alone: the adapter must clear its
     * prior COMPLETE before dispatch, independently of transport behavior. */
    if (owned_error) return owned_error;
    *out = remote_wifi; return ESP_OK;
}
esp_err_t c5_spi_link_get_wifi_scan_status(cerberus_wifi_scan_status_payload_t *out)
{ ++full_polls; *out = remote_wifi; return survey_error; }
esp_err_t c5_spi_link_get_ieee802154_energy_scan_status(cerberus_ieee802154_energy_scan_status_payload_t *out)
{ ++foreign_polls; memset(out, 0, sizeof(*out)); return survey_error; }
esp_err_t c5_spi_link_get_ieee802154_beacon_scan_status(cerberus_ieee802154_beacon_scan_status_payload_t *out)
{ ++foreign_polls; memset(out, 0, sizeof(*out)); return survey_error; }
esp_err_t c5_spi_link_get_ieee802154_activity_scan_status(cerberus_ieee802154_activity_scan_status_payload_t *out)
{ ++foreign_polls; memset(out, 0, sizeof(*out)); return survey_error; }
esp_err_t c5_spi_link_get_wifi_defense_status(cerberus_wifi_defense_status_payload_t *out)
{ ++foreign_polls; memset(out, 0, sizeof(*out)); return survey_error; }
esp_err_t c5_spi_link_get_wifi_connection_status(cerberus_wifi_connection_status_payload_t *out)
{ ++connection_polls; memset(out, 0, sizeof(*out)); out->state = connection_state; return connection_error; }

/* Exact S3 boundary for the independent-BLE cases below. Capture and state
 * reads retain the imported bank/fence implementation; controls bind its full
 * token instead of fabricating a global generation-only RUNNING status. */
esp_err_t cerberus_s3_ble_scanner_passive_start(uint64_t previous,cerberus_s3_ble_passive_state_t *out)
{
    memset(out,0,sizeof(*out));
    if(passive_source.attached&&(!previous||previous!=passive_source.epoch||passive_source.owned))
        return ESP_ERR_INVALID_STATE;
    ++ble_starts;++ble_end.generation;ble_end.state=SENTINEL_SCAN_RUNNING;
    passive_source=(cerberus_s3_ble_passive_state_t){.epoch=++passive_next_epoch,
        .scan=ble_end,.scan_epoch=ble_starts,.host_epoch=1U,.attached=true,.owned=true,.receiving=true};
    *out=passive_source;return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_stop(uint64_t epoch,cerberus_s3_ble_passive_state_t *out)
{
    memset(out,0,sizeof(*out));
    if(!epoch||epoch!=passive_source.epoch||!passive_source.attached)return ESP_ERR_NOT_FOUND;
    ++ble_stops;ble_end.state=SENTINEL_SCAN_IDLE;passive_source.scan=ble_end;
    passive_source.owned=passive_source.receiving=passive_source.stopping=false;
    *out=passive_source;return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_release(uint64_t epoch)
{
    if(!epoch||epoch!=passive_source.epoch||!passive_source.attached)return ESP_ERR_NOT_FOUND;
    if(passive_source.owned)return ESP_ERR_INVALID_STATE;
    memset(&passive_source,0,sizeof(passive_source));return ESP_OK;
}

#include "wifi_owned_observe_impl.h"

static esp_err_t live_execute(sentinel_scan_command_t command)
{
    if (command.tool == SENTINEL_SCAN_BLE) {
        return live_ble_execute(command.kind==SENTINEL_SCAN_COMMAND_START);
    }
    if (command.kind == SENTINEL_SCAN_COMMAND_START) {
        ++start_commands;
        if (command.tool == SENTINEL_SCAN_WIFI) {
            remote_wifi.state = SENTINEL_SCAN_RUNNING;
            wifi_scan_status = remote_wifi;
        } else ++other_starts;
        return start_error;
    }
    CHECK(command.tool == SENTINEL_SCAN_WIFI && command.kind == SENTINEL_SCAN_COMMAND_STOP);
    ++stop_commands;
    remote_wifi.state = SENTINEL_SCAN_IDLE; wifi_scan_status = remote_wifi;
    return ESP_OK;
}
#include "scan_display_tick_impl.h"

static void tick(uint32_t now, bool asleep)
{ clock_ms = now; clock_extra_us = 0; live_scan_tick(now, asleep); }
static void seed(void)
{
    mesh_test_busy=false; reset(); sentinel_scan_session_init(&live_session);
    sentinel_scan_session_init(&live_ble_session); live_ble_session_initialized = true;
    owned_polls = full_polls = foreign_polls = connection_polls = 0;
    ble_starts = ble_stops = other_starts = 0;
    owned_error = survey_error = connection_error = start_error = ESP_OK;
    connection_state = 0; live_owned_wifi_observation = false; c5_online = true;
    memset(&ble_scan_status, 0, sizeof(ble_scan_status)); ble_end = ble_scan_status;
    memset(&wifi_defense_status, 0, sizeof(wifi_defense_status));
    c5_status = (cerberus_c5_status_t){.capabilities=UINT32_MAX};
    remote_wifi = (cerberus_wifi_scan_status_payload_t){.state=SENTINEL_SCAN_RUNNING,.generation=7};
    wifi_scan_status = remote_wifi;
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_WIFI);
    live_session.active = SENTINEL_SCAN_WIFI;
    live_session.active_arm_generation = live_session.arm_generation;
    clock_ms = 1000;
}
static void assert_owned_only(void)
{ CHECK(owned_polls && !full_polls && !foreign_polls && !connection_polls); }

static void owned_status_and_clock(void)
{
    const int64_t times[] = {0, 999, 1000, ((int64_t)UINT32_MAX-10)*1000+999,
        ((int64_t)UINT32_MAX+3)*1000+555};
    for (size_t i=0; i<sizeof(times)/sizeof(times[0]); ++i) {
        seed(); clock_ms=times[i]/1000; clock_extra_us=times[i]%1000;
        for (unsigned t=SENTINEL_SCAN_ENERGY; t<SENTINEL_SCAN_TOOL_COUNT; ++t)
            live_records[t]=(live_scan_record_t){.state=SENTINEL_SCAN_COMPLETE,.valid=true,.generation=99};
        (void)live_observe(123);
        CHECK(last_deadline==(uint32_t)((uint64_t)times[i]/1000U)+25U);
        assert_owned_only(); CHECK(live_scan_observation_valid && live_radio_busy);
        CHECK(!result_calls && !allocation_calls && !start_commands && !stop_commands);
    }
    for (unsigned kind=0; kind<4; ++kind) {
        seed(); wifi_scan_status.state=SENTINEL_SCAN_COMPLETE; wifi_scan_status.generation=222;
        if(kind==0)owned_error=ESP_ERR_TIMEOUT;
        if(kind==1)owned_error=ESP_ERR_INVALID_RESPONSE;
        if(kind==2)clock_ms=-1;
        if(kind==3)c5_online=false;
        (void)live_observe(1000);
        CHECK(!live_scan_observation_valid && live_records[SENTINEL_SCAN_WIFI].error!=ESP_OK);
        CHECK(!wifi_scan_status.state && !wifi_scan_status.generation && !live_wifi_capture_busy());
        CHECK(live_session.active==SENTINEL_SCAN_WIFI && !live_session.completed);
        CHECK(end_status_calls==0 && !foreign_polls && !full_polls && !connection_polls);
        CHECK(owned_polls==(kind<2));
    }
}

static void stop_and_navigation(void)
{
    for(unsigned action=0;action<4;++action) {
        seed();
        for(unsigned t=SENTINEL_SCAN_ENERGY;t<SENTINEL_SCAN_TOOL_COUNT;++t)
            live_records[t]=(live_scan_record_t){.valid=false,.state=SENTINEL_SCAN_RUNNING};
        if(action==0)sentinel_scan_session_pause(&live_session,true);
        if(action==1)wifi_mode=UI_WIFI_MODE_DETAIL; /* Inspection drains instead of STOP. */
        if(action==2)current_page=UI_PAGE_FIELD_MENU; /* Back leaves the scan page. */
        tick(1000,action==3);
        assert_owned_only(); CHECK(!start_commands);
        CHECK(stop_commands==(action!=1));
        CHECK(live_session.active==SENTINEL_SCAN_WIFI);
        remote_wifi.state=SENTINEL_SCAN_IDLE;
        tick(1500,action==3);
        CHECK(live_session.active==SENTINEL_SCAN_NONE && !start_commands);
        CHECK(!full_polls && !foreign_polls && !connection_polls);
    }
    seed(); sentinel_scan_session_pause(&live_session,true); owned_error=ESP_ERR_TIMEOUT;
    tick(1000,false); CHECK(!stop_commands && live_session.active==SENTINEL_SCAN_WIFI);
    owned_error=ESP_OK; tick(1500,false);
    CHECK(stop_commands==1 && live_session.stop_requested && !start_commands);
    tick(2000,false); CHECK(live_session.active==SENTINEL_SCAN_NONE && !start_commands);
}

static void admission_and_ambiguous_start(void)
{
    for(unsigned terminal=SENTINEL_SCAN_IDLE;terminal<=SENTINEL_SCAN_ERROR;++terminal) {
        if(terminal==SENTINEL_SCAN_RUNNING)continue;
        seed(); sentinel_scan_session_pause(&live_session,false); /* Explicit new arm. */
        remote_wifi.state=(uint8_t)terminal;
        tick(1000,false);
        CHECK(live_session.active==SENTINEL_SCAN_NONE && !start_commands && live_radio_busy);
        assert_owned_only();
        survey_error=ESP_ERR_TIMEOUT; tick(1500,false);
        CHECK(!start_commands && full_polls==1 && foreign_polls==4 && connection_polls==1);
        survey_error=ESP_OK; connection_state=CERBERUS_WIFI_CONNECTION_CONNECTED;
        tick(2000,false); CHECK(!start_commands);
        connection_state=0; connection_error=ESP_FAIL; tick(2500,false); CHECK(!start_commands);
        connection_error=ESP_OK; tick(3000,false);
        CHECK(start_commands==1 && live_session.active==SENTINEL_SCAN_WIFI && !other_starts);
    }
    seed(); live_session.command_failed=true; live_session.failure_count=1; live_session.waiting=true;
    tick(1000,false);
    CHECK(!live_session.command_failed && !live_session.waiting && !live_session.failure_count);
    CHECK(!start_commands && !stop_commands); assert_owned_only();
    seed(); live_session.command_failed=true; live_session.failure_count=1; live_session.waiting=true;
    live_session.wait_since_ms=1000; live_session.wait_duration_ms=1000;
    remote_wifi.state=SENTINEL_SCAN_ERROR; tick(1000,false);
    CHECK(live_session.active==SENTINEL_SCAN_NONE && live_session.failure_count==1 && !start_commands);
    tick(1500,false); CHECK(!start_commands);
    tick(2000,false); CHECK(start_commands==1 && full_polls==2);
}

static void capture_and_ble(void)
{
    seed(); prepare(SENTINEL_SCAN_WIFI,7,2); remote_wifi=wifi_scan_status;
    prepare(SENTINEL_SCAN_BLE,9,2);
    live_ble_session.desired=SENTINEL_SCAN_BLE;live_ble_session.completed=true;
    (void)live_observe(1000);
    CHECK(live_wifi_capture_busy() && live_records[SENTINEL_SCAN_BLE].captured);
    CHECK(!memcmp(live_store->ble,ble_source,2*sizeof(ble_source[0])));
    cerberus_c5_ble_result_t frozen_ble[2]; memcpy(frozen_ble,live_store->ble,sizeof(frozen_ble));
    unsigned polls=owned_polls; (void)live_observe(1001);
    CHECK(owned_polls==polls && !full_polls && !foreign_polls && !connection_polls);
    for(unsigned n=0;n<30 && live_wifi_capture_busy();++n)tick(1020+20*n,false);
    CHECK(!live_wifi_capture_busy() && live_records[SENTINEL_SCAN_WIFI].captured);
    CHECK(relation_publications==1 && !memcmp(frozen_ble,live_store->ble,sizeof(frozen_ble)));
    tick(2000,false); CHECK(live_session.completed && !start_commands);
    tick(3000,false); CHECK(!start_commands && readout().completed==1);
    seed(); owned_error=ESP_ERR_TIMEOUT; current_page=UI_PAGE_BLE; ble_mode=UI_BLE_MODE_LIST;
    tick(1000,false);
    CHECK(ble_starts==1 && live_ble_session.active==SENTINEL_SCAN_BLE);
    CHECK(live_session.active==SENTINEL_SCAN_WIFI && !stop_commands && !start_commands);
    tick(1500,false); CHECK(ble_starts==1 && live_ble_session.active==SENTINEL_SCAN_BLE);
    CHECK(!foreign_polls && !connection_polls && !full_polls);
}

int main(void)
{
    owned_status_and_clock(); stop_and_navigation(); admission_and_ambiguous_start(); capture_and_ble();
    seed(); mesh_test_busy=true; (void)live_observe(1000);
    CHECK(!owned_polls&&!full_polls&&!foreign_polls&&!connection_polls);
    tick(2000,false); CHECK(!start_commands&&!stop_commands&&!owned_polls);
    mesh_test_busy=false; (void)live_observe(2001); CHECK(owned_polls==1U);
    reset();
    printf("Owned Wi-Fi polling: %u checks; actual observer/capture/scheduler, bounded status dispatch, Stop/navigation/recovery, fresh admission, immutable publication and independent BLE: pass\n",checks);
    return 0;
}
