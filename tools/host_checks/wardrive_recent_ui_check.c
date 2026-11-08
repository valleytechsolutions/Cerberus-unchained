#include "wardrive_recent_framebuffer.h"
#include "privacy.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define TAG "host"
static unsigned checks;
static bool privacy_mask;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static bool privacy_addresses_masked(void) { return privacy_mask; }
static const char *esp_err_to_name(esp_err_t error)
{ return error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":error==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_FAIL"; }
static esp_err_t draw_centered(uint16_t y,const char *text,uint16_t color,uint8_t scale)
{ CHECK(strlen(text)*6U*scale<=320U); return cerberus_display_draw_text((320U-strlen(text)*6U*scale)/2U,y,text,color,UI_BACKGROUND,scale); }
static esp_err_t draw_header(const char *text) { return draw_centered(20,text,UI_TEXT,2); }
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{
    CHECK(strlen(text)*6U<=w);
    esp_err_t error=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
    return error==ESP_OK?cerberus_display_draw_text(x+(w-strlen(text)*6U)/2U,y+(h-7U)/2U,text,UI_TEXT,UI_PANEL,1):error;
}
#include "wardrive_recent_ui.h"
static wardrive_recent_ui_store_t viewer;
static cerberus_wardrive_recent_t model;
static void clear_draw(void) { calls=fail_after=0; drawn_size=0; drawn[0]=0; }
static void seed(unsigned count)
{
    memset(&viewer,0,sizeof(viewer));cerberus_wardrive_recent_init(&model);
    CHECK(cerberus_wardrive_recent_begin(&model,1));
    for(unsigned i=0;i<count;++i) {
        cerberus_wardrive_observation_t o={0};
        o.type=i%2?CERBERUS_WARDRIVE_BLE:CERBERUS_WARDRIVE_WIFI;
        o.radio=i%2?CERBERUS_WARDRIVE_RADIO_S3:CERBERUS_WARDRIVE_RADIO_C5;
        o.address[0]=0xC2;o.address[5]=(uint8_t)(i+1U);o.address_type=i%2?1:0;
        o.channel=i%2?0:36;o.rssi_dbm=-60;o.manufacturer_id=i%2?0x4C:0;
        o.label_length=(uint8_t)snprintf((char *)o.label,sizeof(o.label),"%s %u",i%2?"BLE advertiser":"Wi-Fi network",i+1U);
        cerberus_wardrive_recent_ticket_t ticket;
        CHECK(cerberus_wardrive_recent_observe(&model,1,i+1U,&o,1000U+i*100U,true,&ticket));
        CHECK(cerberus_wardrive_recent_set_outcome(&model,ticket,(cerberus_wardrive_recent_outcome_t)(1U+i%5U)));
    }
    viewer.snapshot.count=(uint8_t)cerberus_wardrive_recent_copy_recent(&model,viewer.snapshot.rows,10);
    viewer.snapshot.session_id=1;viewer.snapshot.revision=model.revision;
    viewer.snapshot.available=true;viewer.snapshot.state=CERBERUS_WARDRIVE_RUNNING;
    viewer.snapshot.worker_active=true;privacy_mask=false;clear_draw();
}
static void preview(const char *directory,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
    for(size_t i=0;i<sizeof(screen)/sizeof(screen[0]);++i) {
        uint16_t p=screen[i];uint8_t rgb[]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    CHECK(fclose(f)==0);
}
static void test_pages(const char *directory)
{
    seed(10);cerberus_wardrive_recent_snapshot_t before=viewer.snapshot;
    CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);
    CHECK(viewer.paint_valid&&viewer.painted_count==4&&viewer.painted_total==10);
    CHECK(!memcmp(&before,&viewer.snapshot,sizeof(before)));
    CHECK(strstr(drawn,"STATE: LIVE")&&strstr(drawn,"PAGE 1 OF 3")&&strstr(drawn,"24 OBSERVATION HISTORY"));
    CHECK(viewer.painted[0].sequence==10&&viewer.painted[3].sequence==7);
    CHECK(viewer.painted_session_id==1&&viewer.painted_revision==model.revision);
    preview(directory,"recent-live");
    const uint16_t gaps[][2]={{7,150},{312,150},{20,200},{20,203},{20,254},{20,308},{20,362},
        {7,374},{156,374},{163,374},{312,374},{20,418},{7,426},{156,426},{163,426},{312,426},{20,470}};
    for(size_t i=0;i<sizeof(gaps)/sizeof(gaps[0]);++i) CHECK(wardrive_recent_ui_tap(&viewer,gaps[i][0],gaps[i][1],true)==WARDRIVE_RECENT_UI_NONE);
    CHECK(wardrive_recent_ui_tap(&viewer,8,374,true)==WARDRIVE_RECENT_UI_NONE);
    for(unsigned page=1;page<3;++page) {
        CHECK(wardrive_recent_ui_tap(&viewer,164,374,true)==WARDRIVE_RECENT_UI_REDRAW);
        CHECK(viewer.page==page&&!viewer.paint_valid);
        CHECK(wardrive_recent_ui_tap(&viewer,8,150,true)==WARDRIVE_RECENT_UI_NONE);
        clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);
    }
    CHECK(viewer.painted_count==2&&viewer.painted[0].sequence==2);
    CHECK(wardrive_recent_ui_tap(&viewer,164,374,true)==WARDRIVE_RECENT_UI_NONE);
    CHECK(wardrive_recent_ui_tap(&viewer,8,258,true)==WARDRIVE_RECENT_UI_NONE);
    preview(directory,"recent-last-page");
    CHECK(wardrive_recent_ui_tap(&viewer,8,374,true)==WARDRIVE_RECENT_UI_REDRAW&&viewer.page==1);
    viewer.snapshot.count=1;clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK&&viewer.page==0);
    CHECK(wardrive_recent_ui_tap(&viewer,8,426,true)==WARDRIVE_RECENT_UI_BACK);
    CHECK(wardrive_recent_ui_tap(&viewer,164,426,true)==WARDRIVE_RECENT_UI_STOP);
    CHECK(wardrive_recent_ui_tap(&viewer,164,426,false)==WARDRIVE_RECENT_UI_NONE);
}
static void test_frozen_detail(const char *directory)
{
    seed(10);CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);
    cerberus_wardrive_recent_row_t visible=viewer.painted[0];uint64_t revision=viewer.painted_revision;
    viewer.snapshot.rows[0].observation.address[5]=99;viewer.snapshot.rows[0].outcome=CERBERUS_WARDRIVE_RECENT_ERROR;
    viewer.snapshot.session_id=2;viewer.snapshot.revision=revision; /* Same revision, different session. */
    CHECK(wardrive_recent_ui_tap(&viewer,8,150,true)==WARDRIVE_RECENT_UI_REDRAW);
    CHECK(viewer.view==WARDRIVE_RECENT_UI_DETAIL&&viewer.selected_valid);
    CHECK(!memcmp(&visible,&viewer.selected,sizeof(visible))&&viewer.selected_revision==revision);
    clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);
    CHECK(strstr(drawn,"FROZEN ROW")&&strstr(drawn,"SESSION 1")&&strstr(drawn,"OBSERVATION 10"));
    CHECK(strstr(drawn,"C2:00:00:00:00:0A")&&strstr(drawn,"BLE RANDOM STATIC ADDRESS"));
    CHECK(strstr(drawn,"PROCESSING TIME IS NOT RF TIME OR UTC"));preview(directory,"recent-ble-detail");
    viewer.snapshot.available=false;viewer.snapshot.view_error=ESP_ERR_NO_MEM;
    clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);
    CHECK(strstr(drawn,"FROZEN ROW")&&strstr(drawn,"VIEW: ESP_ERR_NO_MEM"));
    CHECK(!memcmp(&visible,&viewer.selected,sizeof(visible)));
    privacy_mask=true;clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);
    CHECK(!strstr(drawn,"C2:00:00:00:00:0A")&&strstr(drawn,"BLE-"));preview(directory,"recent-private-detail");
    CHECK(wardrive_recent_ui_tap(&viewer,20,200,true)==WARDRIVE_RECENT_UI_NONE);
    CHECK(wardrive_recent_ui_tap(&viewer,164,426,true)==WARDRIVE_RECENT_UI_STOP);
    CHECK(wardrive_recent_ui_tap(&viewer,8,426,true)==WARDRIVE_RECENT_UI_REDRAW);
    CHECK(viewer.view==WARDRIVE_RECENT_UI_LIST&&!viewer.paint_valid&&!viewer.selected_valid);
}
static void test_errors_and_failures(const char *directory)
{
    seed(4);CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)==ESP_OK);unsigned paints=calls;
    for(unsigned failure=1;failure<=paints;++failure) {
        clear_draw();fail_after=failure;
        CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,true)!=ESP_OK&&!viewer.paint_valid);
        CHECK(wardrive_recent_ui_tap(&viewer,8,150,true)==WARDRIVE_RECENT_UI_NONE);
        CHECK(wardrive_recent_ui_tap(&viewer,164,374,true)==WARDRIVE_RECENT_UI_NONE);
        CHECK(wardrive_recent_ui_tap(&viewer,8,426,true)==WARDRIVE_RECENT_UI_BACK);
        CHECK(wardrive_recent_ui_tap(&viewer,164,426,true)==WARDRIVE_RECENT_UI_STOP);
    }
    clear_draw();CHECK(wardrive_recent_ui_draw(NULL,ESP_ERR_NO_MEM,5000,true)==ESP_OK);
    CHECK(strstr(drawn,"UNAVAILABLE")&&strstr(drawn,"ESP_ERR_NO_MEM"));preview(directory,"recent-unavailable");
    CHECK(wardrive_recent_ui_tap(NULL,8,426,false)==WARDRIVE_RECENT_UI_BACK);
    CHECK(wardrive_recent_ui_tap(NULL,164,426,true)==WARDRIVE_RECENT_UI_STOP);
    CHECK(wardrive_recent_ui_tap(NULL,8,150,true)==WARDRIVE_RECENT_UI_NONE);
    seed(0);viewer.snapshot.available=false;viewer.snapshot.session_id=0;
    viewer.snapshot.state=CERBERUS_WARDRIVE_IDLE;clear_draw();
    CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,0,false)==ESP_OK&&!viewer.paint_valid);
    CHECK(strstr(drawn,"NO RECENT NETWORKS YET")&&strstr(drawn,"START WARDRIVING"));
    CHECK(!strstr(drawn,"UNAVAILABLE")&&!strstr(drawn,"ESP_ERR"));
    CHECK(wardrive_recent_ui_tap(&viewer,8,426,false)==WARDRIVE_RECENT_UI_BACK);
    preview(directory,"recent-not-started");
    viewer.snapshot.session_id=3;viewer.snapshot.state=CERBERUS_WARDRIVE_STOPPED;clear_draw();
    CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,0,false)==ESP_OK&&!viewer.paint_valid);
    CHECK(strstr(drawn,"NO RECENT NETWORKS YET")&&!strstr(drawn,"ESP_ERR"));
    viewer.snapshot.error=ESP_FAIL;clear_draw();
    CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,0,false)==ESP_OK);
    CHECK(strstr(drawn,"SESSION: ESP_FAIL"));
    seed(0);viewer.snapshot.state=CERBERUS_WARDRIVE_STOPPED;clear_draw();
    CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,0,false)==ESP_OK&&viewer.paint_valid);
    CHECK(strstr(drawn,"NO PROCESSED")&&strstr(drawn,"STATE: STOPPED"));preview(directory,"recent-empty");
    seed(2);viewer.snapshot.state=CERBERUS_WARDRIVE_ERROR;viewer.snapshot.error=ESP_FAIL;clear_draw();
    CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,100,false)==ESP_OK);
    CHECK(strstr(drawn,"STATE: ERROR")&&strstr(drawn,"SESSION: ESP_FAIL")&&strstr(drawn,"AGE UNKNOWN"));
    preview(directory,"recent-error");
    viewer.snapshot.count=11;clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,false)==ESP_OK&&!viewer.paint_valid);
    CHECK(strstr(drawn,"UNAVAILABLE"));
}
static void test_extremes(const char *directory)
{
    seed(1);cerberus_wardrive_recent_row_t *entry=&viewer.snapshot.rows[0];
    memset(entry->observation.label,'Z',32);entry->observation.label[0]=1;entry->observation.label_length=32;
    entry->observation.radio=CERBERUS_WARDRIVE_RADIO_R6;entry->session_id=entry->sequence=UINT64_MAX;
    viewer.snapshot.session_id=UINT64_MAX;
    for(unsigned outcome=0;outcome<=CERBERUS_WARDRIVE_RECENT_NOT_LOGGED;++outcome) {
        entry->outcome=(cerberus_wardrive_recent_outcome_t)outcome;clear_draw();
        CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,INT64_MAX,true)==ESP_OK);
        CHECK(strstr(drawn,"PROCESSED >99999s"));
    }
    CHECK(wardrive_recent_ui_tap(&viewer,8,150,true)==WARDRIVE_RECENT_UI_REDRAW);
    clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,INT64_MAX,false)==ESP_OK);
    CHECK(strstr(drawn,"NAME HEX 00: 015A")&&strstr(drawn,"SOURCE R6")&&strstr(drawn,"18446744073709551615"));
    preview(directory,"recent-wifi-detail");
    clear_draw();CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,UINT64_MAX,false)==ESP_OK);
    CHECK(strstr(drawn,"PROCESSING AGE UNAVAILABLE"));
    for(unsigned type=0;type<4;++type)for(unsigned bits=0;bits<4;++bits) {
        viewer.selected.observation.type=CERBERUS_WARDRIVE_BLE;viewer.selected.observation.address_type=type;
        viewer.selected.observation.address[0]=(uint8_t)(bits<<6);clear_draw();
        CHECK(wardrive_recent_ui_draw(&viewer,ESP_OK,5000,false)==ESP_OK);
    }
}
int main(int argc,char **argv)
{
    CHECK(argc==2);test_pages(argv[1]);test_frozen_detail(argv[1]);test_errors_and_failures(argv[1]);test_extremes(argv[1]);
    printf("Wardriving Recent Networks actual UI: %u checks passed; coherent snapshot input, frozen painted selection, paging, all paint failures, Stop/Back and scoped age/log labels. No radio/SD/worker/allocator linked.\n",checks);
    return 0;
}
