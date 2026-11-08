#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wifi_target_view.h"

enum { UI_PAGE_WIFI=1, UI_WIFI_MODE_MENU=2, UI_WIFI_MODE_RX_TOOLS, UI_WIFI_MODE_DUAL, UI_WIFI_MODE_MULTISSID,
    UI_WIFI_MODE_PINESCAN, UI_WIFI_MODE_FIELD_SURVEY, UI_WIFI_MODE_STATS, UI_WIFI_MODE_PACKET_MONITOR,
    UI_WIFI_MODE_BASELINE, UI_WIFI_MODE_DEFENSE, UI_WIFI_MODE_CONNECTION, UI_WIFI_MODE_CHANNELS_24,
    UI_WIFI_MODE_LIST, UI_WIFI_MODE_TARGET, UI_WIFI_MODE_PROBE_SNIFF, UI_WIFI_MODE_CHANNEL_ANALYZER };
enum { SENTINEL_GUIDE_OPEN_WIFI=1, SENTINEL_GUIDE_OPEN_WIFI_CHANNELS, SENTINEL_GUIDE_OPEN_WIFI_BASELINE };
enum { CERBERUS_WIFI_CONNECTION_STAGING=1, CERBERUS_WIFI_CONNECTION_CONNECTED, CERBERUS_WIFI_CONNECTION_CONNECTING };
#define SENTINEL_HUB_WIFI 1
#define ESP_ERROR_CHECK(value) assert((value)==0)
static unsigned current_page, wifi_mode, menu_item, dual_opens, dual_leaves, tap_count, scan_refreshes, surveys;
static bool ui_capability_blocked, wifi_attack_selecting, pending_start, wifi_selection_valid;
static bool dual_wifi_return_to_view;
static char wifi_target_message[49];
static struct { bool checked; } wifi_baseline;
static struct { bool valid; } wifi_channel_current_identity;
static struct { unsigned state; } wifi_connection_status;
static bool packet_monitor_from_target;
static unsigned packet_monitor_channel;
static char wifi_export_message[10];
static int wifi_connection_error, wifi_scan_error;
static unsigned legacy_loads;
static unsigned probe_opens;
static void channel_analyzer_open(void) { wifi_mode=UI_WIFI_MODE_CHANNEL_ANALYZER; }
static void probe_sniff_open(void) { ++probe_opens; wifi_mode=UI_WIFI_MODE_PROBE_SNIFF; }
static void dual_wifi_live_open(bool start)
{ ++dual_opens; current_page=UI_PAGE_WIFI; wifi_mode=UI_WIFI_MODE_DUAL;
    pending_start=start; dual_wifi_return_to_view=false; }
static void dual_wifi_live_tap(uint16_t x,uint16_t y) { (void)x;(void)y;++tap_count; }
static void dual_wifi_ui_leave(void) { ++dual_leaves; pending_start=false; }
static int sentinel_hub_hit_test(int hub,uint16_t x,uint16_t y) { (void)x;(void)y;assert(hub==SENTINEL_HUB_WIFI);return (int)menu_item; }
static void open_learning_lesson(int value) { (void)value; }
static void multissid_enter(void) {}
static void pinescan_enter(void) {}
static int refresh_wifi_scan_status(void) { ++scan_refreshes; return 0; }
static int refresh_wifi_defense_survey(void) { return 0; }
static int export_wifi_radio_stats(void) { return 0; }
static int load_wifi_results(void) { ++legacy_loads; return -23; }
static int c5_spi_link_get_wifi_connection_status(void *status) { (void)status;return 0; }
static void packet_monitor_start_ui(void) {}
static void wifi_channel_survey_begin_ui(void) { assert(!pending_start);++surveys; }
static int draw_detail_page(void) { return 0; }
#include "dual_wifi_root_routes.h"

static void reset(void)
{
    current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_MENU;menu_item=0;
    dual_opens=dual_leaves=tap_count=scan_refreshes=surveys=0;
    probe_opens=0;
    ui_capability_blocked=wifi_attack_selecting=pending_start=wifi_selection_valid=false;
    wifi_connection_status.state=0;
    wifi_channel_current_identity.valid=false;legacy_loads=0;wifi_scan_error=0;
    dual_wifi_return_to_view=false;wifi_target_message[0]=0;
}
int main(void)
{
    reset();wifi_attack_selecting=true;open_tool_page(UI_PAGE_WIFI);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&pending_start&&dual_opens==1&&!wifi_attack_selecting);
    reset();ui_capability_blocked=true;open_tool_page(UI_PAGE_WIFI);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&!pending_start);
    reset();root_wifi_tap(10,10);assert(dual_opens==1&&pending_start&&wifi_mode==UI_WIFI_MODE_DUAL);
    root_wifi_tap(20,445);assert(tap_count==1);root_touch(20,445);assert(tap_count==2);
    ui_capability_blocked=true;root_touch(20,445);assert(tap_count==2);
    reset();menu_item=12;root_wifi_tap(10,10);
    assert(wifi_mode==UI_WIFI_MODE_RX_TOOLS&&!pending_start&&!dual_opens&&!dual_leaves&&!scan_refreshes&&!surveys);
    reset();menu_item=10;root_wifi_tap(10,10);
    assert(wifi_mode==UI_WIFI_MODE_PROBE_SNIFF&&probe_opens==1&&!pending_start&&
        !dual_opens&&!dual_leaves&&!scan_refreshes&&!surveys);
    reset();root_guide(SENTINEL_GUIDE_OPEN_WIFI);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&pending_start&&dual_opens==1&&!dual_leaves&&!scan_refreshes);
    reset();root_guide(SENTINEL_GUIDE_OPEN_WIFI_CHANNELS);
    assert(wifi_mode==UI_WIFI_MODE_CHANNELS_24&&!pending_start&&dual_leaves==1&&surveys==1);
    reset();root_guide(SENTINEL_GUIDE_OPEN_WIFI_BASELINE);
    assert(wifi_mode==UI_WIFI_MODE_BASELINE&&!pending_start&&dual_leaves==1&&scan_refreshes==1);
    reset();ui_capability_blocked=true;root_guide(SENTINEL_GUIDE_OPEN_WIFI_CHANNELS);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&!pending_start&&!surveys);
    reset();menu_item=5;root_wifi_tap(10,10);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&pending_start);
    reset();menu_item=5;wifi_selection_valid=true;root_wifi_tap(10,10);
    assert(wifi_mode==UI_WIFI_MODE_CONNECTION&&!dual_opens);
    reset();menu_item=5;wifi_connection_status.state=CERBERUS_WIFI_CONNECTION_CONNECTED;root_wifi_tap(10,10);
    assert(wifi_mode==UI_WIFI_MODE_CONNECTION&&!dual_opens);
    reset();menu_item=1;root_wifi_tap(10,10);assert(wifi_mode==UI_WIFI_MODE_STATS&&!dual_opens&&scan_refreshes==1);
    reset();wifi_mode=UI_WIFI_MODE_STATS;wifi_channel_current_identity.valid=true;
    root_wifi_tap(260,380);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&dual_opens==1&&!pending_start&&!legacy_loads&&!wifi_scan_error&&!scan_refreshes);
    /* Actual link invalidation remains an error until an explicit Scan Again;
       opening the retained list neither clears it nor starts another scan. */
    reset();wifi_mode=UI_WIFI_MODE_STATS;wifi_channel_current_identity.valid=true;wifi_scan_error=-17;
    root_wifi_tap(260,380);
    assert(wifi_mode==UI_WIFI_MODE_DUAL&&dual_opens==1&&!pending_start&&!legacy_loads&&wifi_scan_error==-17);
    reset();wifi_mode=UI_WIFI_MODE_STATS;root_wifi_tap(260,380);
    assert(wifi_mode==UI_WIFI_MODE_LIST&&!dual_opens&&legacy_loads==1&&wifi_scan_error==-23);
    /* The actual CHANGE AP / SCAN widget is at y364..393. Both this body
       control and the separate global Back footer must return to the dual
       list after a proved handoff, retaining rows without starting RF. */
    for(unsigned footer=0;footer<2;++footer){
        reset();wifi_mode=UI_WIFI_MODE_TARGET;dual_wifi_return_to_view=true;
        strcpy(wifi_target_message,"PRIOR AP MESSAGE");
        if(footer)root_target_back();else root_target_tap(150,378);
        assert(wifi_mode==UI_WIFI_MODE_DUAL&&dual_opens==1&&!pending_start&&
            !dual_wifi_return_to_view&&!legacy_loads&&!scan_refreshes);
        if(!footer)assert(!wifi_target_message[0]);
        /* A subsequent Scan Again is dispatched to the combined list's own
           control, rather than the legacy C5-only scan handler. */
        root_wifi_tap(240,445);assert(tap_count==1&&!legacy_loads&&!scan_refreshes);
    }
    /* Legacy and explicit AP-select origins keep their legacy list route. */
    for(unsigned selecting=0;selecting<2;++selecting){
        reset();wifi_mode=UI_WIFI_MODE_TARGET;wifi_attack_selecting=selecting!=0;
        strcpy(wifi_target_message,"PRIOR AP MESSAGE");root_target_tap(150,378);
        assert(wifi_mode==UI_WIFI_MODE_LIST&&!dual_opens&&!pending_start&&
            !wifi_target_message[0]&&wifi_attack_selecting==(selecting!=0));
        wifi_mode=UI_WIFI_MODE_TARGET;root_target_back();
        assert(wifi_mode==UI_WIFI_MODE_LIST&&!dual_opens&&!pending_start);
    }
    /* The target row's gap/outer bounds cannot activate a return route. */
    reset();wifi_mode=UI_WIFI_MODE_TARGET;dual_wifi_return_to_view=true;
    root_target_tap(150,394);root_target_tap(11,378);root_target_tap(308,378);
    assert(wifi_mode==UI_WIFI_MODE_TARGET&&dual_wifi_return_to_view&&!dual_opens);
    puts("Dual Wi-Fi root routes: launcher/menu/guide and both AP return controls use split view; legacy/multiselect origins stay legacy; return never rearms RF.");
    return 0;
}
