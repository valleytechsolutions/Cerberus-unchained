#include "inventory_framebuffer.h"
#include "sentinel_scan_session.h"
#include "recon_recorder.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define TAG "host"
static unsigned checks, start_calls, stop_calls;
static bool privacy_mask;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
typedef enum { WATCH_LIVE,WATCH_HISTORY,WATCH_DETAILS,WATCH_DEMO,WATCH_FILTERS,WATCH_WHY,
    WATCH_OPTIONS,WATCH_ACTIONS,WATCH_SIGNAL,WATCH_INVENTORY,WATCH_INVENTORY_DETAIL,WATCH_INVENTORY_ACTIVITY } watch_page_t;
static watch_page_t watch_page;
static sentinel_watch_t watch_state;
static sentinel_watch_entry_t watch_detail;
static unsigned watch_filters_page,watch_history_page,recon_confirm;
static bool watch_paused,watch_radar,c5_online=true;
static char recon_message[49];
static sentinel_scan_session_t live_session,live_ble_session;
static cerberus_recon_recorder_status_t recorder_status;
esp_err_t cerberus_recon_recorder_start(void) { ++start_calls; return ESP_OK; }
void cerberus_recon_recorder_stop(void) { ++stop_calls; }
void cerberus_recon_recorder_get_status(cerberus_recon_recorder_status_t *out) { *out=recorder_status; }
static const char *esp_err_to_name(esp_err_t error) { return error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":"ESP_FAIL"; }
static bool privacy_addresses_masked(void) { return privacy_mask; }
static void cerberus_privacy_format_ble_identifier(const uint8_t *address,bool mask,char *out,size_t n)
{ (void)address; CHECK(mask); snprintf(out,n,"XX:XX:XX:XX:XX:XX"); }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *address,bool mask,char *out,size_t n)
{ (void)address; CHECK(mask); snprintf(out,n,"XX:XX:XX:XX:XX:XX"); }
static esp_err_t draw_centered(uint16_t y,const char *text,uint16_t color,uint8_t scale)
{ CHECK(strlen(text)*6U*scale<=320U); return cerberus_display_draw_text((320U-strlen(text)*6U*scale)/2U,y,text,color,UI_BACKGROUND,scale); }
static esp_err_t draw_header(const char *text) { return draw_centered(20,text,UI_TEXT,2); }
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{
    CHECK(strlen(text)*6U<=w);
    esp_err_t error=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
    return error==ESP_OK?cerberus_display_draw_text(x+(w-strlen(text)*6U)/2U,y+(h-7U)/2U,text,UI_TEXT,UI_PANEL,1):error;
}
#include "recon_inventory_ui.h"
#include "recon_controls_ui.h"
static recon_inventory_ui_store_t inventory;
static uint64_t publication[2];

static void clear_draw(void) { calls=fail_after=0; drawn_size=0; drawn[0]=0; }
static void seed(void)
{
    memset(&inventory,0,sizeof(inventory)); recon_inventory_ui=&inventory;
    cerberus_recon_inventory_init(&inventory.model,0); inventory.filter=CERBERUS_RECON_INVENTORY_ALL;
    memset(publication,0,sizeof(publication)); sentinel_watch_init(&watch_state);
    sentinel_scan_session_init(&live_session); sentinel_scan_session_init(&live_ble_session);
    watch_page=WATCH_INVENTORY; watch_paused=false; recon_inventory_error=ESP_OK;
    start_calls=stop_calls=0; privacy_mask=false; clear_draw();
}
static cerberus_recon_record_t row(uint8_t radio,unsigned identity,uint64_t time)
{
    cerberus_recon_record_t r={.radio=radio,.consumed_us=time,.rssi=-60,.rssi_valid=true,
        .source_kind=CERBERUS_RECON_SOURCE_NONE,.source_limits=CERBERUS_RECON_LIMIT_SOURCE_UNAVAILABLE};
    r.address[0]=(uint8_t)identity; r.address[5]=0xC0; r.address_type=radio?1:0;
    r.name_length=(uint8_t)snprintf((char *)r.name,sizeof(r.name),"%s %u",radio?"BLE advertiser":"Wi-Fi AP",identity);
    if(!radio) { r.channel=36; r.channel_valid=true; }
    return r;
}
static void batch(uint8_t radio, unsigned first,unsigned count,uint64_t time)
{
    CHECK(cerberus_recon_inventory_begin(&inventory.model,radio,++publication[radio],time));
    for(unsigned i=0;i<count;++i) { cerberus_recon_record_t r=row(radio,first+i,time); CHECK(cerberus_recon_inventory_offer(&inventory.model,&r)); }
    CHECK(cerberus_recon_inventory_end(&inventory.model,time));
}
static void preview(const char *directory,const char *name)
{
    char path[1024]; snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *file=fopen(path,"wb"); CHECK(file); fprintf(file,"P6\n320 480\n255\n");
    for(size_t i=0;i<sizeof(screen)/sizeof(screen[0]);++i) { uint16_t p=screen[i];
        uint8_t rgb[]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,file)==3); }
    CHECK(fclose(file)==0);
}
static void test_list_and_selection(const char *directory)
{
    seed(); batch(0,1,6,1000000); batch(1,20,4,2000000);
    cerberus_recon_inventory_t before=inventory.model;
    CHECK(recon_draw_inventory(3000000)==ESP_OK && inventory.paint_valid && inventory.painted_count==4);
    CHECK(!memcmp(&before,&inventory.model,sizeof(before))); CHECK(inventory.painted[0].record.radio==1);
    CHECK(strstr(drawn,"10 RETAINED ADDRESSES") && strstr(drawn,"NOT DEVICE COUNT")); preview(directory,"inventory-all");
    const uint16_t gaps[][2]={{7,60},{104,60},{111,60},{208,60},{215,60},{312,60},{10,104},
        {7,150},{312,150},{20,200},{20,203},{20,254},{20,308},{20,362},{104,380},{20,418},{63,426},{256,426},{100,470}};
    for(size_t i=0;i<sizeof(gaps)/sizeof(gaps[0]);++i) CHECK(!recon_inventory_handle_tap(gaps[i][0],gaps[i][1],3000000));
    CHECK(recon_inventory_handle_tap(112,60,3000000) && inventory.filter==0 && !inventory.paint_valid);
    CHECK(!recon_inventory_handle_tap(8,150,3000000)); clear_draw();
    CHECK(recon_draw_inventory(3000000)==ESP_OK && inventory.painted[0].record.radio==0);
    CHECK(recon_inventory_handle_tap(112,374,3000000) && inventory.page==1 && !inventory.paint_valid);
    clear_draw(); CHECK(recon_draw_inventory(3000000)==ESP_OK && inventory.painted_count==2);
    CHECK(!recon_inventory_handle_tap(10,258,3000000));
    CHECK(recon_inventory_handle_tap(8,374,3000000) && inventory.page==0);
    clear_draw(); CHECK(recon_draw_inventory(3000000)==ESP_OK);
    cerberus_recon_inventory_entry_t painted=inventory.painted[0];
    batch(0,40,32,4000000); /* Actual capacity replacement after paint cannot retarget a tap. */
    CHECK(recon_inventory_handle_tap(8,150,4000000) && watch_page==WATCH_INVENTORY_DETAIL);
    CHECK(!memcmp(&painted,&inventory.selected,sizeof(painted)));
    batch(0,0,0,UINT64_C(130000000)); /* Completed sweep expiry cannot mutate frozen selection. */
    CHECK(!memcmp(&painted,&inventory.selected,sizeof(painted)));
    clear_draw(); CHECK(recon_draw_inventory_detail(UINT64_C(130000000))==ESP_OK);
    CHECK(strstr(drawn,"FROZEN ROW") && strstr(drawn,"SOURCE TIME UNAVAILABLE"));
    CHECK(recon_inventory_handle_tap(64,426,130000000) && watch_page==WATCH_INVENTORY);
    clear_draw(); CHECK(recon_draw_inventory(130000000)==ESP_OK && inventory.page==0);
    CHECK(strstr(drawn,"NO RETAINED OBSERVATIONS"));
    CHECK(recon_inventory_handle_tap(216,60,130000000) && inventory.filter==1);
    clear_draw(); CHECK(recon_draw_inventory(130000000)==ESP_OK && inventory.painted_count==4);
    CHECK(start_calls==0 && stop_calls==0 && live_session.desired==SENTINEL_SCAN_NONE && live_ble_session.desired==SENTINEL_SCAN_NONE);
}
static void test_details_and_activity(const char *directory)
{
    seed(); cerberus_recon_record_t r=row(1,1,UINT64_C(10000000));
    r.name_length=32; for(unsigned i=0;i<32;++i) r.name[i]=(uint8_t)(i+0x20); r.name[0]=0; r.name[31]=0xFF;
    r.classified=true; r.category=SENTINEL_WATCH_BEACON; r.confidence=SENTINEL_WATCH_CONFIDENCE_HIGH;
    r.mode=SENTINEL_WATCH_MODE_HISTORY; strcpy(r.rule,"ble.ibeacon");
    r.source_kind=CERBERUS_RECON_SOURCE_S3_ADV_MS32; r.source_time=9000;
    r.source_limits=0;
    r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_SEQUENCE_VALID; r.source_sequence=11;
    CHECK(cerberus_recon_inventory_begin(&inventory.model,1,++publication[1],r.consumed_us));
    CHECK(cerberus_recon_inventory_offer(&inventory.model,&r)); CHECK(cerberus_recon_inventory_end(&inventory.model,r.consumed_us));
    CHECK(recon_draw_inventory(11000000)==ESP_OK); CHECK(recon_inventory_handle_tap(8,150,11000000));
    clear_draw(); CHECK(recon_draw_inventory_detail(11000000)==ESP_OK);
    CHECK(strstr(drawn,"002122232425262728292A2B2C2D2E2F") && strstr(drawn,"303132333435363738393A3B3C3D3EFF"));
    CHECK(strstr(drawn,"ORIGINAL SWEEP PEAK") && strstr(drawn,"S3 ORIGINAL ADV CALLBACK"));
    CHECK(strstr(drawn,"AGE BASIS IS INDEPENDENT OF LATEST RSSI") && strstr(drawn,"ble.ibeacon"));
    CHECK(strstr(drawn,"C0:00:00:00:00:01") && strstr(drawn,"BLE RANDOM STATIC")); preview(directory,"inventory-detail");
    privacy_mask=true; clear_draw(); CHECK(recon_draw_inventory_detail(11000000)==ESP_OK);
    CHECK(strstr(drawn,"XX:XX:XX:XX:XX:XX") && !strstr(drawn,"C0:00:00:00:00:01")); privacy_mask=false;
    for(unsigned type=0;type<4;++type) for(unsigned bits=0;bits<4;++bits) {
        inventory.selected.record.address_type=(uint8_t)type; inventory.selected.record.address[5]=(uint8_t)(bits<<6);
        clear_draw(); CHECK(recon_draw_inventory_detail(8000000)==ESP_OK && strstr(drawn,"AGE: UNAVAILABLE"));
    }
    inventory.selected.record.rssi_valid=false; clear_draw(); CHECK(recon_draw_inventory_detail(11000000)==ESP_OK && strstr(drawn,"RSSI UNAVAILABLE"));
    CHECK(recon_inventory_handle_tap(64,426,11000000)); clear_draw(); CHECK(recon_draw_inventory(11000000)==ESP_OK);
    CHECK(recon_inventory_handle_tap(216,374,11000000) && watch_page==WATCH_INVENTORY_ACTIVITY);
    CHECK(cerberus_recon_inventory_tick(&inventory.model,UINT64_C(74000000)));
    batch(0,1,32,UINT64_C(75000000)); batch(0,40,32,UINT64_C(76000000)); batch(1,0,0,UINT64_C(131000000));
    cerberus_recon_inventory_t before=inventory.model; clear_draw(); CHECK(recon_draw_inventory_activity(131000000)==ESP_OK);
    CHECK(!memcmp(&before,&inventory.model,sizeof(before)));
    CHECK(strstr(drawn,"64 SAMPLES") && strstr(drawn,"CAPACITY REPLACED 32") && strstr(drawn,"EXPIRED 1"));
    CHECK(strstr(drawn,"NOT PHYSICAL DEVICES") && strstr(drawn,"PAUSED OR FAILED")); preview(directory,"inventory-activity");
    CHECK(recon_inventory_handle_tap(64,426,131000000) && watch_page==WATCH_INVENTORY);
    CHECK(recon_inventory_handle_tap(64,426,131000000) && watch_page==WATCH_OPTIONS);
    CHECK(start_calls==0 && stop_calls==0);
}
static void test_c5_age_bound(void)
{
    seed(); cerberus_recon_record_t r=row(0,9,UINT64_C(3000000));
    r.source_kind=CERBERUS_RECON_SOURCE_C5_AGE_MS; r.source_time=2000;
    r.source_anchor_us=1000000; r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_ANCHOR_VALID;
    r.source_limits=0;
    CHECK(cerberus_recon_inventory_begin(&inventory.model,0,1,3000000));
    CHECK(cerberus_recon_inventory_offer(&inventory.model,&r)); CHECK(cerberus_recon_inventory_end(&inventory.model,3000000));
    CHECK(recon_draw_inventory(5000000)==ESP_OK && strstr(drawn,"BOUND AGE 6s"));
    CHECK(recon_inventory_handle_tap(8,150,5000000)); clear_draw();
    CHECK(recon_draw_inventory_detail(5000000)==ESP_OK);
    CHECK(strstr(drawn,"AGE UPPER BOUND: 6s") && strstr(drawn,"C5 SOURCE AGE UPPER BOUND"));
    CHECK(strstr(drawn,"RSSI -60 dBm / C5 AP ROW") && !strstr(drawn,"ORIGINAL SWEEP PEAK"));
    privacy_mask=true; clear_draw(); CHECK(recon_draw_inventory_detail(5000000)==ESP_OK && strstr(drawn,"XX:XX:XX:XX:XX:XX"));
}
static void test_paint_failures(const char *directory)
{
    seed(); batch(0,1,4,1000000); CHECK(recon_draw_inventory(2000000)==ESP_OK); unsigned count=calls;
    for(unsigned fail=1;fail<=count;++fail) {
        clear_draw(); fail_after=fail; CHECK(recon_draw_inventory(2000000)!=ESP_OK);
        CHECK(!inventory.paint_valid && !recon_inventory_handle_tap(8,150,2000000));
    }
    clear_draw(); CHECK(recon_draw_inventory(2000000)==ESP_OK && inventory.paint_valid);
    recon_inventory_ui=NULL; recon_inventory_error=ESP_ERR_NO_MEM; clear_draw();
    CHECK(recon_draw_inventory(2000000)==ESP_OK && strstr(drawn,"INVENTORY UNAVAILABLE") && strstr(drawn,"ESP_ERR_NO_MEM"));
    CHECK(!strstr(drawn,"NO RETAINED")); preview(directory,"inventory-unavailable");
    CHECK(!recon_inventory_handle_tap(8,150,2000000));
    CHECK(recon_inventory_handle_tap(64,426,2000000) && watch_page==WATCH_OPTIONS);
    CHECK(start_calls==0 && stop_calls==0);
    recon_inventory_ui=&inventory; recon_inventory_error=ESP_ERR_INVALID_RESPONSE; watch_page=WATCH_INVENTORY;
    clear_draw(); CHECK(recon_draw_inventory(2000000)==ESP_OK && strstr(drawn,"LAST BATCH: ESP_FAIL"));
    CHECK(inventory.paint_valid && inventory.painted_count==4);
    inventory.radio_errors[0]=ESP_ERR_INVALID_RESPONSE; clear_draw();
    CHECK(recon_draw_inventory(2000000)==ESP_OK && strstr(drawn,"WI-FI BATCH: ESP_FAIL"));
    CHECK(recon_inventory_handle_tap(8,150,2000000)); clear_draw();
    CHECK(recon_draw_inventory_detail(2000000)==ESP_OK && strstr(drawn,"WI-FI BATCH: ESP_FAIL"));
    inventory.radio_errors[0]=ESP_OK; inventory.radio_errors[1]=ESP_ERR_INVALID_RESPONSE; clear_draw();
    CHECK(recon_draw_inventory_activity(2000000)==ESP_OK && strstr(drawn,"BLE BATCH: ESP_FAIL"));
    inventory.selected_valid=false; clear_draw();
    CHECK(recon_draw_inventory_detail(2000000)==ESP_OK && strstr(drawn,"BACK TO INVENTORY"));
}
static void test_options(const char *directory)
{
    seed(); watch_page=WATCH_OPTIONS; clear_draw(); CHECK(recon_draw_options()==ESP_OK);
    CHECK(strstr(drawn,"NEARBY INVENTORY") && strstr(drawn,"VIEW: DASHBOARD") && strstr(drawn,"START SD RECORDING"));
    preview(directory,"inventory-options");
    const uint16_t gaps[][2]={{8,164},{8,169},{8,214},{8,223},{8,268},{8,277},{8,322},{8,331},{8,376},{156,332},{164,332}};
    for(size_t i=0;i<sizeof(gaps)/sizeof(gaps[0]);++i) CHECK(!recon_handle_control_tap(gaps[i][0],gaps[i][1],0));
    inventory.filter=1; inventory.page=3; inventory.paint_valid=true;
    CHECK(recon_handle_control_tap(8,332,0) && watch_page==WATCH_INVENTORY);
    CHECK(inventory.filter==CERBERUS_RECON_INVENTORY_ALL && inventory.page==0 && !inventory.paint_valid);
    CHECK(start_calls==0 && stop_calls==0);
    CHECK(recon_inventory_handle_tap(64,426,0) && watch_page==WATCH_OPTIONS);
    CHECK(recon_handle_control_tap(64,426,0) && watch_page==WATCH_LIVE);
    watch_page=WATCH_ACTIONS; watch_detail.radio=SENTINEL_WATCH_RADIO_ENVIRONMENT;
    watch_detail.match.label="Fixture area";
    clear_draw(); CHECK(recon_draw_actions()==ESP_OK); clear_draw(); CHECK(recon_draw_signal(0)==ESP_OK);
    clear_draw(); CHECK(recon_draw_inventory_activity(0)==ESP_OK && strstr(drawn,"0 SAMPLES") && strstr(drawn,"WAITING FOR A COMPLETED SWEEP"));
    inventory.model.counters.added=inventory.model.counters.expired=inventory.model.counters.replaced=inventory.model.counters.invalid=UINT32_MAX;
    clear_draw(); CHECK(recon_draw_inventory_activity(0)==ESP_OK);
    CHECK(strstr(drawn,"ADDED 4294967295+") && strstr(drawn,"EXPIRED 4294967295+") && strstr(drawn,"REPLACED 4294967295+"));
}
int main(int argc,char **argv)
{
    CHECK(argc==2); test_list_and_selection(argv[1]); test_details_and_activity(argv[1]);
    test_paint_failures(argv[1]); test_options(argv[1]); test_c5_age_bound();
    printf("PASS: %u actual inventory/controls UI checks; frozen selection, filters, expiry, bounds, failures and Back.\n",checks);
}
