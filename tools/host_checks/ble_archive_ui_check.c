#include "archive_framebuffer.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define TAG "host"
static unsigned checks;
static bool privacy_mask;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static bool privacy_addresses_masked(void) { return privacy_mask; }
static void cerberus_privacy_format_ble_identifier(const uint8_t *a,bool mask,char *out,size_t n)
{
    if(mask) snprintf(out,n,"XX:XX:XX:XX:XX:XX");
    else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[5],a[4],a[3],a[2],a[1],a[0]);
}
static const char *esp_err_to_name(esp_err_t error)
{
    return error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":error==ESP_ERR_INVALID_RESPONSE?
        "ESP_ERR_INVALID_RESPONSE":error==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_FAIL";
}
static esp_err_t draw_centered(uint16_t y,const char *text,uint16_t color,uint8_t scale)
{ CHECK(strlen(text)*6U*scale<=320U); return cerberus_display_draw_text((320U-strlen(text)*6U*scale)/2U,y,text,color,UI_BACKGROUND,scale); }
static esp_err_t draw_header(const char *text) { return draw_centered(20,text,UI_TEXT,2); }
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{
    CHECK(strlen(text)*6U<=w);
    esp_err_t error=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
    return error==ESP_OK?cerberus_display_draw_text(x+(w-strlen(text)*6U)/2U,y+(h-7U)/2U,text,UI_TEXT,UI_PANEL,1):error;
}
#include "ble_archive_ui.h"
#include "ble_archive_response_fixture.h"
static ble_archive_ui_store_t viewer;
static cerberus_ble_archive_t source;

static void clear_draw(void) { calls=fail_after=0; drawn_size=0; drawn[0]=0; }
static void seed(unsigned count)
{
    memset(&viewer,0,sizeof(viewer)); memset(&source,0,sizeof(source));
    source.saved_uptime_ms=1000000; source.count=(uint8_t)count;
    for(unsigned i=0;i<count;++i) {
        cerberus_ble_archive_row_t *r=&source.rows[i];
        r->receiver=i<32?CERBERUS_BLE_ARCHIVE_RECEIVER_S3:CERBERUS_BLE_ARCHIVE_RECEIVER_C5;
        r->address[0]=(uint8_t)(i%32U+1U);r->address[5]=0xC0;r->address_type=1;
        snprintf(r->name,sizeof(r->name),"Saved advertiser %u",i+1U);
        r->rssi=-50;r->rssi_valid=true;r->manufacturer_id=0xFFFF;
        r->source_time_valid=true;r->original_source_uptime_ms=999000;r->copy_ceiling_ms=999500;
        r->scan_epoch_valid=true;r->scan_epoch=123;r->generation=4;r->index=(uint8_t)(i%32U);r->reports_seen=5;
        if(i>=32){
            r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;r->receiver_boot_id=UINT64_C(0xABCDEF0123456789);
            r->source_time_valid=false;r->original_source_uptime_ms=r->copy_ceiling_ms=0;
            r->age_upper_valid=true;r->age_upper_at_save_ms=1700;
        }
        const uint8_t data[]={2,1,6,3,3,0x0F,0x18,4,0x09,'t','a','g',3,0xFF,0x4C,0};
        memcpy(r->advertisement,data,sizeof(data));r->advertisement_length=sizeof(data);
    }
    CHECK(cerberus_ble_archive_validate(&source));privacy_mask=false;clear_draw();
}
static void load(unsigned count)
{
    seed(count);CHECK(ble_archive_ui_begin(&viewer,1,false));
    CHECK(ble_archive_ui_complete(&viewer,1,&source,"B00001.BLE"));
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
static void repaint(void)
{
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
}
/* Interleaved receiver observations deliberately share typed address bytes
 * across S3/C5/LEGACY. These are separate saved observations, not devices. */
static void seed_mixed(unsigned count)
{
    seed(count);cerberus_ble_archive_row_t base=source.rows[0];
    for(unsigned i=0;i<count;++i) {
        cerberus_ble_archive_row_t *r=&source.rows[i];*r=base;
        r->receiver=i%3U==0?CERBERUS_BLE_ARCHIVE_RECEIVER_S3:
            i%3U==1?CERBERUS_BLE_ARCHIVE_RECEIVER_C5:CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY;
        r->address[0]=(uint8_t)(i/12U+1U);r->address_type=(uint8_t)(i/3U%4U);
        r->index=(uint8_t)(i/3U);r->generation=(uint8_t)(i+1U);
        r->scan_epoch=UINT64_C(0x1234567800000000)+i+1U;
        snprintf(r->name,sizeof(r->name),"Observation %02u",i);
        r->rssi=(int8_t)(-30-(int)i);r->original_source_uptime_ms=999000U-i;
        if(r->receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
            r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
            r->receiver_boot_id=UINT64_C(0xABCDEF0100000000)+i+1U;
            r->source_time_valid=false;r->original_source_uptime_ms=r->copy_ceiling_ms=0;
            r->age_upper_valid=true;r->age_upper_at_save_ms=1700U+i;
            r->advertisement_event_type=3;
            r->receive=(cerberus_dual_ble_receive_metadata_t){
                .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=r->advertisement_length,
                .sid=255,.data_status=255,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
        } else if(r->receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_S3) {
            r->response_state=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
            r->response_length=r->response_original_length=5;r->response_event_type=4;
            memcpy(r->response,"\004\011RSP",5);
            r->response_original_sequence=11;r->response_sequence=12;
            r->response_original_observed_ms=(uint32_t)r->original_source_uptime_ms;
            r->response_observed_ms=999100U-i;
            r->response_time_valid=true;r->response_source_uptime_ms=999100U-i;
        }
    }
    CHECK(cerberus_ble_archive_validate(&source));
}
static void load_mixed(unsigned count)
{
    seed_mixed(count);CHECK(ble_archive_ui_begin(&viewer,1,false));
    CHECK(ble_archive_ui_complete(&viewer,1,&source,"mixed-observations.BLE"));repaint();
}
static void source_control(cerberus_ble_archive_list_source_t wanted)
{
    for(unsigned attempts=0;viewer.source_filter!=wanted;++attempts) {
        CHECK(attempts<CERBERUS_BLE_ARCHIVE_LIST_SOURCE_COUNT);
        CHECK(ble_archive_ui_tap(&viewer,8,112)==BLE_ARCHIVE_UI_REDRAW);
        CHECK(viewer.page==0&&!viewer.paint_valid&&viewer.painted_count==0);
        CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_NONE);repaint();
    }
}
static void sort_control(cerberus_ble_archive_list_sort_t wanted)
{
    for(unsigned attempts=0;viewer.sort!=wanted;++attempts) {
        CHECK(attempts<CERBERUS_BLE_ARCHIVE_LIST_SORT_COUNT);
        CHECK(ble_archive_ui_tap(&viewer,164,112)==BLE_ARCHIVE_UI_REDRAW);
        CHECK(viewer.page==0&&!viewer.paint_valid&&viewer.painted_count==0);
        CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_NONE);repaint();
    }
}
static void test_jobs(const char *directory)
{
    seed(5); CHECK(!ble_archive_ui_begin(NULL,1,false));CHECK(!ble_archive_ui_begin(&viewer,0,false));
    CHECK(ble_archive_ui_begin(&viewer,1,true));CHECK(!ble_archive_ui_begin(&viewer,2,false));
    CHECK(!ble_archive_ui_error(&viewer,ESP_ERR_NO_MEM));CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"SAVING SNAPSHOT")&&strstr(drawn,"A SAVE MAY FINISH"));preview(directory,"archive-saving");
    CHECK(!ble_archive_ui_complete(&viewer,2,&source,"B00001.BLE"));
    CHECK(!ble_archive_ui_fail(&viewer,2,ESP_FAIL));CHECK(!ble_archive_ui_fail(&viewer,1,ESP_OK));
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_CANCEL&&!viewer.job_pending);
    CHECK(!ble_archive_ui_complete(&viewer,1,&source,"late.BLE"));
    CHECK(!ble_archive_ui_fail(&viewer,1,ESP_FAIL));CHECK(!ble_archive_ui_begin(&viewer,1,false));
    CHECK(ble_archive_ui_begin(&viewer,2,false));clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"LOADING SNAPSHOT")&&strstr(drawn,"BACK CANCELS THIS LOAD"));
    CHECK(ble_archive_ui_complete(&viewer,2,&source,"B00002.BLE"));
    CHECK(!ble_archive_ui_complete(&viewer,2,&source,"duplicate.BLE"));
    CHECK(ble_archive_ui_begin(&viewer,3,false));
    cerberus_ble_archive_t prior=viewer.document;
    source.count=0;CHECK(!ble_archive_ui_complete(&viewer,3,&source,"invalid.BLE"));
    CHECK(viewer.view==BLE_ARCHIVE_UI_ERROR&&viewer.error==ESP_ERR_INVALID_RESPONSE);
    CHECK(!memcmp(&prior,&viewer.document,sizeof(prior))&&viewer.has_document);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"PREVIOUS SNAPSHOT RETAINED")&&strstr(drawn,"ESP_ERR_INVALID_RESPONSE"));preview(directory,"archive-error");
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_LIST);
    CHECK(ble_archive_ui_error(&viewer,ESP_ERR_NO_MEM));CHECK(!memcmp(&prior,&viewer.document,sizeof(prior)));
    CHECK(ble_archive_ui_begin(&viewer,UINT64_MAX,false));CHECK(ble_archive_ui_fail(&viewer,UINT64_MAX,ESP_FAIL));
    CHECK(!ble_archive_ui_begin(&viewer,1,false));
    clear_draw();CHECK(ble_archive_ui_draw(NULL,ESP_ERR_NO_MEM)==ESP_OK);
    CHECK(strstr(drawn,"ARCHIVE UNAVAILABLE")&&strstr(drawn,"ESP_ERR_NO_MEM")&&!strstr(drawn,"0 OBSERVATIONS"));
    CHECK(ble_archive_ui_tap(NULL,64,426)==BLE_ARCHIVE_UI_BACK&&ble_archive_ui_tap(NULL,8,150)==BLE_ARCHIVE_UI_NONE);
    preview(directory,"archive-unavailable");
}
static void test_list(const char *directory)
{
    load(32);cerberus_ble_archive_t prior=viewer.document;
    CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&viewer.paint_valid&&viewer.painted_count==4);
    CHECK(!memcmp(&prior,&viewer.document,sizeof(prior)));CHECK(strstr(drawn,"32/32 OBS / P1/8")&&strstr(drawn,"AGE AT SAVE 1000ms"));
    preview(directory,"archive-list");
    const uint16_t gaps[][2]={{7,150},{312,150},{8,200},{8,203},{8,254},{8,308},{8,362},{156,374},{163,374},{8,418},{7,426},{104,426},{111,426},{312,426},{64,470}};
    for(unsigned i=0;i<sizeof(gaps)/sizeof(gaps[0]);++i) CHECK(ble_archive_ui_tap(&viewer,gaps[i][0],gaps[i][1])==BLE_ARCHIVE_UI_NONE);
    CHECK(ble_archive_ui_tap(&viewer,112,426)==BLE_ARCHIVE_UI_EXPORT&&viewer.view==BLE_ARCHIVE_UI_LIST);
    CHECK(!memcmp(&prior,&viewer.document,sizeof(prior)));
    for(unsigned p=1;p<8;++p) {
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.page==p&&!viewer.paint_valid);
        CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_NONE);clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
        CHECK(viewer.painted[0].address[0]==p*4U+1U);
    }
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.page==7);
    for(unsigned p=7;p>0;--p) CHECK(ble_archive_ui_tap(&viewer,8,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.page==p-1U);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    cerberus_ble_archive_row_t painted=viewer.painted[0];
    viewer.document.rows[0]=viewer.document.rows[1];viewer.document.saved_uptime_ms+=500000;
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
    CHECK(!memcmp(&painted,&viewer.selected,sizeof(painted)));CHECK(viewer.selected_saved_uptime_ms==1000000&&viewer.selected_age_ms==1000);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"C0:00:00:00:00:01")&&strstr(drawn,"AGE AT SAVE: 1000 ms"));preview(directory,"archive-detail");
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_LIST);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_BACK);
    load(5);CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&viewer.painted_count==1);
    CHECK(ble_archive_ui_tap(&viewer,8,204)==BLE_ARCHIVE_UI_NONE);
}
static void test_paint_and_privacy(void)
{
    load(4);CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);unsigned total=calls;
    for(unsigned failure=1;failure<=total;++failure) {
        clear_draw();fail_after=failure;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)!=ESP_OK);
        CHECK(!viewer.paint_valid&&ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_NONE);
    }
    clear_draw();privacy_mask=true;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"XX:XX:XX:XX:XX:XX")&&!strstr(drawn,"C0:00:00:00:00:01"));
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"XX:XX:XX:XX:XX:XX")&&!strstr(drawn,"C0:00:00:00:00:01"));
    for(unsigned type=0;type<4;++type)for(unsigned bits=0;bits<4;++bits) {
        viewer.selected.address_type=(uint8_t)type;viewer.selected.address[5]=(uint8_t)(bits<<6);
        clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    }
    memset(viewer.selected.name,'\xFE',32);viewer.selected.name[32]=0;
    viewer.selected.rssi_valid=false;viewer.selected.source_time_valid=false;viewer.selected.scan_epoch_valid=false;
    viewer.selected_age_valid=false;viewer.selected_saved_uptime_ms=UINT64_MAX;
    viewer.selected.uuid_length=16;memset(viewer.selected.uuid,0xFF,16);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"FEFEFEFEFEFEFEFEFEFEFEFEFEFEFEFE")&&strstr(drawn,"AGE AT SAVE: UNKNOWN"));
    CHECK(strstr(drawn,"RSSI UNAVAILABLE")&&strstr(drawn,"NO LOCATION OR WALL CLOCK"));
}
static void test_receivers(const char *directory)
{
    load(64);CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"64/64 OBS / P1/16")&&strstr(drawn,"S3 32 C5 32 L0"));
    CHECK(strstr(drawn,"S3 / Saved advertiser 1")&&strstr(drawn,"PEAK -50"));
    preview(directory,"archive-both-first-page");
    for(unsigned i=0;i<8;++i)CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"C5 / Saved advertiser 33")&&strstr(drawn,"LAST -50")&&
        strstr(drawn,"AGE AT SAVE <=1700ms"));
    preview(directory,"archive-c5-list");
    cerberus_ble_archive_row_t frozen=viewer.painted[0];
    viewer.document.rows[32]=viewer.document.rows[0];viewer.document.saved_uptime_ms=3;
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(!memcmp(&frozen,&viewer.selected,sizeof(frozen))&&viewer.selected_age_ms==1700&&
        viewer.selected_saved_uptime_ms==1000000);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"C5 / Saved advertiser 33")&&strstr(drawn,"LATEST ORIGINAL ADV")&&
        strstr(drawn,"AGE AT SAVE: <= 1700 ms")&&strstr(drawn,"C5 BOOT ID: ABCDEF0123456789")&&
        strstr(drawn,"NO LOCAL RX TIMESTAMP"));
    CHECK(!strstr(drawn,"RAW ADV UPTIME: 0")&&!strstr(drawn,"ORIGINAL SWEEP PEAK"));
    preview(directory,"archive-c5-detail");
    viewer.selected_age_ms=UINT64_MAX;clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    viewer.selected_age_valid=false;clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"AGE AT SAVE: UNKNOWN"));
    load(64);viewer.document.saved_uptime_ms=UINT64_MAX;
    viewer.document.rows[0].original_source_uptime_ms=viewer.document.rows[0].copy_ceiling_ms=0;
    viewer.document.rows[32].age_upper_at_save_ms=UINT64_MAX;
    viewer.document.rows[33].age_upper_valid=false;viewer.document.rows[33].age_upper_at_save_ms=0;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"AGE AT SAVE 18446744073709551615ms"));
    preview(directory,"archive-max-exact-age");
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"AGE AT SAVE: 18446744073709551615 ms"));
    preview(directory,"archive-max-exact-detail");
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);viewer.page=8;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"AGE AT SAVE <=18446744073709551615ms")&&strstr(drawn,"AGE AT SAVE UNKNOWN"));
    preview(directory,"archive-max-upper-age");
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"AGE AT SAVE: <= 18446744073709551615 ms"));
    preview(directory,"archive-max-upper-detail");
    load(1);viewer.document.rows[0].receiver=CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"LEGACY / Saved advertiser 1"));
    CHECK(strstr(drawn,"S3 0 C5 0 L1"));preview(directory,"archive-legacy-receiver");
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"LEGACY /"));
    CHECK(!strstr(drawn,"C5 BOOT ID"));
    viewer.view=BLE_ARCHIVE_UI_JOB;viewer.release_pending=true;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"SD WRITE NOT STARTED"));
    preview(directory,"archive-releasing");
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_NONE);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_CANCEL&&!viewer.release_pending);
}
static void test_raw(const char *directory)
{
    load(1);CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_RAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"ADVERTISEMENT *")&&strstr(drawn,"RESPONSE")&&strstr(drawn,"FLAGS")&&strstr(drawn,"UUID16 LIST"));
    CHECK(strstr(drawn,"02 01 06 03 03 0F 18")&&strstr(drawn,"4 COMPLETE FIELDS / PAGE 1 OF 2"));
    preview(directory,"archive-raw");
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_page==1);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"MANUFACTURER DATA"));
    CHECK(ble_archive_ui_tap(&viewer,8,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_page==0);
    viewer.selected.advertisement_length=31;memset(viewer.selected.advertisement,0,31);
    for(unsigned i=0;i<30;i+=2){viewer.selected.advertisement[i]=1;viewer.selected.advertisement[i+1]=0xFE;}
    for(unsigned p=0;p<5;++p) {
        clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"15 COMPLETE FIELDS"));
        CHECK(strstr(drawn,"UNKNOWN TYPE"));CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);
        CHECK(viewer.raw_page==(p<4?p+1U:4U));
    }
    memset(viewer.selected.advertisement,0xFF,31);viewer.selected.advertisement[0]=30;viewer.raw_page=0;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"1 COMPLETE FIELDS"));
    viewer.selected.advertisement[0]=31;viewer.selected.advertisement_flags=0x83;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"MALFORMED OR CUT AD AT BYTE 0")&&strstr(drawn,"NO COMPLETE AD FIELDS")&&strstr(drawn,"RAW FLAGS 83"));
    CHECK(strstr(drawn,"SOURCE: TRUNCATED + MALFORMED"));
    preview(directory,"archive-malformed");
    viewer.selected.advertisement_flags=1;clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"SOURCE: TRUNCATED / MISSING BYTES UNKNOWN"));
    viewer.selected.advertisement_flags=2;clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"SOURCE MARKED MALFORMED / RAW RETAINED"));
    uint8_t offsets[15],bad;viewer.selected.advertisement_length=3;
    viewer.selected.advertisement[0]=0;viewer.selected.advertisement[1]=1;viewer.selected.advertisement[2]=0;
    CHECK(ble_archive_ui_fields(&viewer.selected,offsets,&bad)==0&&bad==0);
    viewer.selected.advertisement[1]=0;CHECK(ble_archive_ui_fields(&viewer.selected,offsets,&bad)==0&&bad==UINT8_MAX);
    viewer.selected.advertisement_length=0;clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"NO COMPLETE AD FIELDS"));
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_LIST);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_BACK);
}
static void test_response(const char *directory)
{
    seed(1);cerberus_ble_archive_row_t *r=&source.rows[0];
    r->response_state=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
    r->response_length=r->response_original_length=5;r->response_event_type=4;
    memcpy(r->response,"\004\011RSP",5);
    r->response_original_sequence=11;r->response_sequence=12;
    r->response_original_observed_ms=999000;r->response_observed_ms=999100;
    r->response_time_valid=true;r->response_source_uptime_ms=999100;
    CHECK(cerberus_ble_archive_validate(&source));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&ble_archive_ui_complete(&viewer,1,&source,"with-response.BLE"));
    CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"MATCHED RESPONSE: 5 OF 5 BYTES")&&strstr(drawn,"RSP AGE AT SAVE: 900 ms"));
    preview(directory,"archive-response-detail");
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_response);
    CHECK(ble_archive_ui_tap(&viewer,160,60)==BLE_ARCHIVE_UI_NONE);
    CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_response);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"SAVED RESPONSE")&&strstr(drawn,"04 09 52 53 50")&&strstr(drawn,"COMPLETE NAME"));
    CHECK(!strstr(drawn,"02 01 06 03 03 0F 18"));preview(directory,"archive-response-raw");
    memset(viewer.document.rows[0].response,0,sizeof(viewer.document.rows[0].response));
    CHECK(ble_archive_ui_tap(&viewer,8,54)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_response);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"02 01 06 03 03 0F 18"));
    CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"04 09 52 53 50"));
    /* Frozen bytes survive a replacement of the underlying committed row. */
    CHECK(viewer.selected.response_sequence==12&&viewer.selected.response_source_uptime_ms==999100);
    viewer.selected.response_length=31;viewer.selected.response_original_length=255;
    viewer.selected.response_flags=CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;
    memset(viewer.selected.response,0xA5,31);viewer.selected.response[0]=254;viewer.selected.response[1]=0xFF;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"31 OF 255 BYTES")&&
        strstr(drawn,"MALFORMED OR CUT AD AT BYTE 0")&&strstr(drawn,"SOURCE: TRUNCATED"));
    preview(directory,"archive-response-prefix");
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
    viewer.selected.response_source_uptime_ms=0;viewer.selected_saved_uptime_ms=UINT64_MAX;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"RSP AGE AT SAVE: 18446744073709551615 ms"));
    preview(directory,"archive-response-max-age");
    for(unsigned state=0;state<2;++state){
        load(1);viewer.document.rows[0].response_state=(uint8_t)state;
        CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
        CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
        CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW);
        clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
        CHECK(strstr(drawn,state?"NO MATCHED RESPONSE RETAINED":"RESPONSE BYTES / CORRELATION UNAVAILABLE"));
        CHECK(strstr(drawn,"THIS DOES NOT PROVE NO RESPONSE WAS SENT"));
        CHECK(!strstr(drawn,"COMPLETE FIELDS")&&!strstr(drawn,"PREV FIELDS"));
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_NONE);
        preview(directory,state?"archive-response-none":"archive-response-unknown");
        CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);
        CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_response);
    }
}
static void test_receive_info(const char *directory)
{
    seed(33);cerberus_ble_archive_row_t *r=&source.rows[32];
    r->advertisement_event_type=3;r->advertisement_length=31;r->advertisement_flags=3;
    memset(r->advertisement,0xA5,31);r->advertisement[0]=254;r->advertisement[1]=0xFF;
    r->receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.callback_length=255,.sid=15,
        .primary_phy=3,.secondary_phy=2,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    CHECK(cerberus_ble_archive_validate(&source));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&ble_archive_ui_complete(&viewer,1,&source,"receive-v4.BLE"));
    viewer.page=8;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    cerberus_ble_archive_row_t frozen=viewer.painted[0];
    /* Selection follows the painted snapshot even if its document is replaced. */
    viewer.document.rows[32]=viewer.document.rows[0];viewer.document.saved_uptime_ms=7;
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(!memcmp(&viewer.selected,&frozen,sizeof(frozen)));
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"C5 RECEIVE INFO RETAINED")&&strstr(drawn,"C5 RESPONSE BYTES UNAVAILABLE")&&
        strstr(drawn,"RAW DATA")&&strstr(drawn,"FIND LIVE"));
    preview(directory,"archive-c5-receive-detail");
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_response);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"MALFORMED OR CUT AD AT BYTE 0")&&strstr(drawn,"RX INFO"));
    CHECK(ble_archive_ui_tap(&viewer,108,60)==BLE_ARCHIVE_UI_NONE);
    CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_response);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(strstr(drawn,"SAVED RECEIVE INFO")&&strstr(drawn,"RX INFO *")&&strstr(drawn,"C5 EXTENDED CALLBACK"));
    CHECK(strstr(drawn,"CALLBACK 255 B / PREVIEW 31 B")&&strstr(drawn,"PHY CODED PRIMARY / 2M SECONDARY")&&
        strstr(drawn,"SID 15 / PROPS 00 / CTRL COMPLETE")&&strstr(drawn,"FULL AD LAYOUT VALID / PREVIEW CUT"));
    CHECK(strstr(drawn,"HISTORICAL EVIDENCE")&&strstr(drawn,"NOT CURRENT RADIO STATE")&&
        strstr(drawn,"RESPONSE HAS ITS OWN SOURCE TAB")&&strstr(drawn,"MISSING BYTES ARE NOT RECONSTRUCTED"));
    CHECK(!strstr(drawn,"PREV FIELDS")&&!strstr(drawn,"COMPLETE FIELDS")&&!strstr(drawn,"SAVED RESPONSE"));
    CHECK(ble_archive_ui_tap(&viewer,8,374)==BLE_ARCHIVE_UI_NONE&&
        ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_NONE&&viewer.raw_page==0);
    preview(directory,"archive-c5-receive-extended");
    unsigned total=calls;
    for(unsigned failure=1;failure<=total;++failure){
        clear_draw();fail_after=failure;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)!=ESP_OK);
        CHECK(!memcmp(&viewer.selected,&frozen,sizeof(frozen)));
    }
    clear_draw();privacy_mask=true;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(!strstr(drawn,"C0:00:00:00:00:01"));
    viewer.selected.receive.ad_validation=CERBERUS_DUAL_BLE_AD_INVALID;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"FULL AD LAYOUT INVALID / PREVIEW CUT"));
    preview(directory,"archive-c5-receive-invalid");
    viewer.selected.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=31,.sid=255,
        .data_status=255,.ad_validation=CERBERUS_DUAL_BLE_AD_INVALID};
    viewer.selected.advertisement_flags=2;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"C5 LEGACY CALLBACK")&&
        strstr(drawn,"SID N/A / PROPS N/A / CTRL STATUS N/A")&&strstr(drawn,"PHY N/A PRIMARY / N/A SECONDARY"));
    CHECK(!strstr(drawn,"PREVIEW CUT"));preview(directory,"archive-c5-receive-legacy");
    viewer.selected.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.properties=0x10,.sid=255,
        .primary_phy=1,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    viewer.selected.advertisement_length=0;viewer.selected.advertisement_flags=0;
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"C5 EXT CALLBACK / LEGACY PDU")&&
        strstr(drawn,"CALLBACK 0 B / PREVIEW 0 B")&&strstr(drawn,"SID N/A / PROPS 10 / CTRL COMPLETE"));
    preview(directory,"archive-c5-receive-legacy-pdu");
    /* Pre-v4 reports and older peers are represented as canonical UNKNOWN. */
    memset(&viewer.selected.receive,0,sizeof(viewer.selected.receive));
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"RECEIVE INFO UNAVAILABLE")&&
        strstr(drawn,"NO METADATA FOR THIS OBSERVATION")&&strstr(drawn,"RAW BYTES REMAIN AVAILABLE"));
    CHECK(!strstr(drawn,"CTRL COMPLETE")&&!strstr(drawn,"FULL AD LAYOUT")&&!strstr(drawn,"PREV FIELDS"));
    preview(directory,"archive-c5-receive-unknown");
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"C5 RECEIVE INFO UNAVAILABLE"));
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_response);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_LIST);
    viewer.page=0;clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK);
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_response);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW);
    clear_draw();CHECK(ble_archive_ui_draw(&viewer,ESP_OK)==ESP_OK&&strstr(drawn,"SAVED RESPONSE")&&
        !strstr(drawn,"RECEIVE INFO"));
}
static void test_filtered_pages(const char *directory)
{
    load_mixed(64);cerberus_ble_archive_t prior=viewer.document;
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_ALL&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_FILE);
    CHECK(viewer.filtered_count==64&&strstr(drawn,"64/64 OBS / P1/16"));
    CHECK(strstr(drawn,"S3 22 C5 21 L21"));
    CHECK(strstr(drawn,"S3 / Observation 00")&&strstr(drawn,"C5 / Observation 01")&&
        strstr(drawn,"LEGACY / Observation 02"));
    CHECK(strstr(drawn,"C0:00:00:00:00:01 T0")&&strstr(drawn,"C0:00:00:00:00:01 T1"));
    CHECK(!memcmp(viewer.painted[0].address,viewer.painted[1].address,6)&&
        !memcmp(viewer.painted[0].address,viewer.painted[2].address,6));
    CHECK(viewer.painted[0].address_type==viewer.painted[1].address_type&&
        viewer.painted[1].address_type==viewer.painted[2].address_type);
    CHECK(viewer.painted[0].receiver==viewer.painted[3].receiver&&
        !memcmp(viewer.painted[0].address,viewer.painted[3].address,6)&&
        viewer.painted[0].address_type!=viewer.painted[3].address_type);
    preview(directory,"archive-mixed-controls");
    const uint16_t gaps[][2]={{7,112},{156,112},{163,112},{312,112},{8,111},{8,142},{164,142},{8,149}};
    for(unsigned i=0;i<sizeof(gaps)/sizeof(gaps[0]);++i)
        CHECK(ble_archive_ui_tap(&viewer,gaps[i][0],gaps[i][1])==BLE_ARCHIVE_UI_NONE);
    const unsigned counts[]={64,22,21,21};
    for(unsigned filter=0;filter<CERBERUS_BLE_ARCHIVE_LIST_SOURCE_COUNT;++filter) {
        source_control((cerberus_ble_archive_list_source_t)filter);
        CHECK(viewer.filtered_count==counts[filter]);
        bool seen[64]={false};unsigned found=0;
        unsigned pages=(counts[filter]+3U)/4U;
        for(unsigned p=0;p<pages;++p) {
            CHECK(viewer.page==p&&viewer.paint_valid);
            for(unsigned row=0;row<viewer.painted_count;++row) {
                unsigned position=p*4U+row;
                unsigned expected=filter?3U*position+(filter-1U):position;
                CHECK(expected<64&&!seen[expected]);seen[expected]=true;++found;
                CHECK(viewer.order[position]==expected);
                CHECK(!memcmp(&viewer.painted[row],&prior.rows[expected],sizeof(viewer.painted[row])));
                CHECK(viewer.painted_age_valid[row]);
                CHECK(viewer.painted_age_ms[row]==(prior.rows[expected].receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5?
                    1700U+expected:1000U+expected));
            }
            CHECK(!memcmp(&viewer.document,&prior,sizeof(prior)));
            if(p+1U<pages) {
                CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
            }
        }
        CHECK(found==counts[filter]);
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.page==pages-1U);repaint();
        if(filter==CERBERUS_BLE_ARCHIVE_LIST_C5)preview(directory,"archive-c5-filter-last-page");
        if(filter==CERBERUS_BLE_ARCHIVE_LIST_LEGACY)preview(directory,"archive-legacy-filter-last-page");
    }
    source_control(CERBERUS_BLE_ARCHIVE_LIST_ALL);
    CHECK(viewer.filtered_count==64&&viewer.page==0);
    CHECK(!memcmp(&viewer.document,&prior,sizeof(prior)));
}
static void test_sort_and_empty(const char *directory)
{
    seed_mixed(7);
    const char *names[]={"zulu","ALPHA","alpha","","Bravo","alpha","zz"};
    const int8_t signals[]={-20,-80,-20,127,-20,-20,127};
    for(unsigned i=0;i<7;++i){strcpy(source.rows[i].name,names[i]);source.rows[i].rssi=signals[i];
        source.rows[i].rssi_valid=i!=3U&&i!=6U;}
    CHECK(cerberus_ble_archive_validate(&source));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&ble_archive_ui_complete(&viewer,1,&source,"sort.BLE"));repaint();
    cerberus_ble_archive_t prior=viewer.document;
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.page==1);repaint();
    sort_control(CERBERUS_BLE_ARCHIVE_LIST_NAME);
    const uint8_t by_name[]={3,1,2,5,4,0,6};
    CHECK(!memcmp(viewer.order,by_name,sizeof(by_name)));
    CHECK(strstr(drawn,"[NO RETAINED NAME]")&&strstr(drawn,"RSSI UNKNOWN"));
    preview(directory,"archive-name-sort");
    sort_control(CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);
    const uint8_t by_signal[]={2,5,4,0,1,3,6};
    CHECK(!memcmp(viewer.order,by_signal,sizeof(by_signal)));
    CHECK(strstr(drawn,"PEAK -20")&&strstr(drawn,"LAST -20"));
    preview(directory,"archive-signal-sort");
    source_control(CERBERUS_BLE_ARCHIVE_LIST_C5);
    CHECK(viewer.filtered_count==2&&viewer.order[0]==4&&viewer.order[1]==1);
    CHECK(viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);
    source_control(CERBERUS_BLE_ARCHIVE_LIST_ALL);sort_control(CERBERUS_BLE_ARCHIVE_LIST_FILE);
    for(unsigned i=0;i<7;++i)CHECK(viewer.order[i]==i);
    CHECK(!memcmp(&viewer.document,&prior,sizeof(prior)));
    /* An empty source is a presentation of the existing valid file. It is
     * not a newly published zero-row archive and cannot select stale slots. */
    load(2);repaint();source_control(CERBERUS_BLE_ARCHIVE_LIST_C5);
    CHECK(viewer.filtered_count==0&&viewer.painted_count==0&&viewer.page==0&&viewer.paint_valid);
    CHECK(strstr(drawn,"0/2 OBS / P1/1"));
    for(unsigned row=0;row<4;++row)CHECK(ble_archive_ui_tap(&viewer,8,(uint16_t)(150U+54U*row))==BLE_ARCHIVE_UI_NONE);
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW&&viewer.page==0);repaint();
    sort_control(CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);
    CHECK(!viewer.filtered_count&&!viewer.painted_count&&viewer.document.count==2);
    preview(directory,"archive-empty-source");source_control(CERBERUS_BLE_ARCHIVE_LIST_ALL);
    CHECK(viewer.filtered_count==2&&viewer.painted_count==2);
}
static void test_frozen_filtered_and_jobs(const char *directory)
{
    load_mixed(64);source_control(CERBERUS_BLE_ARCHIVE_LIST_C5);
    for(unsigned p=0;p<2;++p)CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);
    repaint();CHECK(viewer.page==2&&viewer.order[8]==25);
    cerberus_ble_archive_row_t frozen=viewer.painted[0];
    CHECK(frozen.receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5&&frozen.index==8&&frozen.generation==26);
    CHECK(frozen.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_LEGACY);
    viewer.document.rows[25]=source.rows[0];viewer.document.saved_uptime_ms=UINT64_MAX;
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(!memcmp(&viewer.selected,&frozen,sizeof(frozen))&&viewer.selected_saved_uptime_ms==1000000&&
        viewer.selected_age_valid&&viewer.selected_age_ms==1725);
    repaint();CHECK(strstr(drawn,"C5 / Observation 25")&&strstr(drawn,"AGE AT SAVE: <= 1725 ms"));
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"C5 LEGACY CALLBACK")&&strstr(drawn,"CALLBACK 16 B / PREVIEW 16 B"));
    preview(directory,"archive-filtered-frozen-receive");
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_LIST);
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_C5&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_FILE&&viewer.page==2);
    viewer.document=source;repaint();
    source_control(CERBERUS_BLE_ARCHIVE_LIST_S3);sort_control(CERBERUS_BLE_ARCHIVE_LIST_NAME);
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
    frozen=viewer.painted[1];unsigned slot=viewer.order[5];uint64_t age=viewer.painted_age_ms[1];
    CHECK(frozen.receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_S3&&frozen.response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT);
    memset(&viewer.document.rows[slot],0,sizeof(viewer.document.rows[slot]));viewer.document.saved_uptime_ms=1;
    CHECK(ble_archive_ui_tap(&viewer,8,204)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(!memcmp(&viewer.selected,&frozen,sizeof(frozen))&&viewer.selected_age_ms==age&&
        viewer.selected_saved_uptime_ms==1000000);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"04 09 52 53 50")&&strstr(drawn,"SAVED RESPONSE"));
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_S3&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_NAME&&viewer.page==1);
    viewer.document=source;repaint();sort_control(CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
    cerberus_ble_archive_t prior=viewer.document;
    CHECK(ble_archive_ui_begin(&viewer,2,false)&&ble_archive_ui_fail(&viewer,2,ESP_FAIL));
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_S3&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_SIGNAL&&viewer.page==1);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(!memcmp(&viewer.document,&prior,sizeof(prior)));
    CHECK(ble_archive_ui_begin(&viewer,3,false));source.count=0;
    CHECK(!ble_archive_ui_complete(&viewer,3,&source,"bad.BLE"));
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_S3&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_SIGNAL&&viewer.page==1);
    source=prior;CHECK(ble_archive_ui_begin(&viewer,4,false)&&ble_archive_ui_complete(&viewer,4,&source,"new.BLE"));
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_ALL&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_FILE&&viewer.page==0);
    repaint();CHECK(!memcmp(&viewer.document,&prior,sizeof(prior)));
}
static void test_control_recovery_and_history(const char *directory)
{
    load_mixed(7);unsigned total=calls;
    for(unsigned failure=1;failure<=total;++failure) {
        clear_draw();fail_after=failure;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)!=ESP_OK);
        CHECK(!viewer.paint_valid);
        for(unsigned row=0;row<4;++row)CHECK(ble_archive_ui_tap(&viewer,8,(uint16_t)(150U+54U*row))==BLE_ARCHIVE_UI_NONE);
        CHECK(ble_archive_ui_tap(&viewer,8,112)==BLE_ARCHIVE_UI_REDRAW&&!viewer.selected_valid);
        CHECK(!viewer.paint_valid&&!viewer.painted_count);repaint();
        source_control(CERBERUS_BLE_ARCHIVE_LIST_ALL);
        CHECK(ble_archive_ui_tap(&viewer,164,112)==BLE_ARCHIVE_UI_REDRAW);repaint();
        sort_control(CERBERUS_BLE_ARCHIVE_LIST_FILE);
    }
    cerberus_ble_archive_t prior=viewer.document;
    uint64_t saved_ages[4];memcpy(saved_ages,viewer.painted_age_ms,sizeof(saved_ages));
    for(unsigned repeat=0;repeat<10;++repeat) {
        source_control(CERBERUS_BLE_ARCHIVE_LIST_C5);sort_control(CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);
        source_control(CERBERUS_BLE_ARCHIVE_LIST_ALL);sort_control(CERBERUS_BLE_ARCHIVE_LIST_FILE);repaint();
        CHECK(!memcmp(saved_ages,viewer.painted_age_ms,sizeof(saved_ages)));
        CHECK(viewer.painted_saved_uptime_ms==1000000&&!memcmp(&viewer.document,&prior,sizeof(prior)));
    }
    char long_name[256];memset(long_name,'N',255);long_name[40]='\n';long_name[80]=(char)0xFE;long_name[255]=0;
    memset(source.rows[0].name,'Q',32);source.rows[0].name[12]=(char)0xFE;source.rows[0].name[32]=0;
    CHECK(ble_archive_ui_begin(&viewer,2,false)&&ble_archive_ui_complete(&viewer,2,&source,long_name));
    CHECK(strlen(viewer.file_name)==255&&viewer.file_name[40]=='.'&&viewer.file_name[80]=='.');
    privacy_mask=true;repaint();
    CHECK(strstr(drawn,"XX:XX:XX:XX:XX:XX")&&!strstr(drawn,"C0:00:00:00:00:01"));
    CHECK(strstr(drawn,"SOURCE ALL")&&strstr(drawn,"FILE ORDER"));
    CHECK(strstr(drawn,"QQQQQQQQQQQQ.QQQQQQQQQQQQQQQQQQQ"));
    preview(directory,"archive-long-file-private-controls");
}
static void test_v1_filtered_read(const char *directory)
{
    /* Literal v1 file predates the current codec/writer; do not construct it
     * with today's encoder and accidentally test a v4 round trip instead. */
    static const char hex[]=
        "43424c45415243000100200080000100a0000000807060504030201031ca8e79"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000";
    uint8_t bytes[160];CHECK(strlen(hex)==2U*sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);++i){unsigned value;CHECK(sscanf(hex+2U*i,"%2x",&value)==1);bytes[i]=(uint8_t)value;}
    memset(&viewer,0,sizeof(viewer));memset(&source,0,sizeof(source));privacy_mask=false;
    CHECK(cerberus_ble_archive_decode(bytes,sizeof(bytes),&source));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&ble_archive_ui_complete(&viewer,1,&source,"old-v1.BLE"));repaint();
    CHECK(viewer.filtered_count==1&&strstr(drawn,"LEGACY / A.."));
    source_control(CERBERUS_BLE_ARCHIVE_LIST_S3);CHECK(!viewer.filtered_count);
    source_control(CERBERUS_BLE_ARCHIVE_LIST_C5);CHECK(!viewer.filtered_count);
    source_control(CERBERUS_BLE_ARCHIVE_LIST_LEGACY);CHECK(viewer.filtered_count==1&&viewer.order[0]==0);
    sort_control(CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);preview(directory,"archive-v1-filtered");
    CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(viewer.selected.receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY&&!viewer.selected.receive.source_kind&&
        viewer.selected.response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN);
    repaint();CHECK(strstr(drawn,"RAW ADV UPTIME")&&!strstr(drawn,"C5 BOOT ID"));
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(viewer.source_filter==CERBERUS_BLE_ARCHIVE_LIST_LEGACY&&viewer.sort==CERBERUS_BLE_ARCHIVE_LIST_SIGNAL);
}
static void full_family_source(const uint8_t *full,size_t length,uint8_t family)
{
    seed(33);source.rows[0]=source.rows[32];source.count=1;
    cerberus_ble_archive_row_t *row=&source.rows[0];
    memset(row->advertisement,0,sizeof(row->advertisement));
    row->advertisement_length=(uint8_t)(length>31U?31U:length);
    memcpy(row->advertisement,full,row->advertisement_length);
    row->advertisement_event_type=3;row->advertisement_flags=length>31U?3U:0U;
    row->receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.callback_length=(uint8_t)length,
        .sid=7,.primary_phy=3,.secondary_phy=2,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    cerberus_dual_ble_family_evidence_t evidence;
    CHECK(cerberus_dual_ble_family_parse_ad(full,length,row->address,row->address_type,&evidence));
    CHECK(cerberus_dual_ble_family_wire_encode(&evidence,family,&row->family));
    CHECK(cerberus_ble_archive_validate(&source));
}

static void reopen_family(void)
{
    static uint8_t bytes[CERBERUS_BLE_ARCHIVE_MAX_SIZE];size_t written=0;
    CHECK(cerberus_ble_archive_encode(&source,bytes,sizeof(bytes),&written));
    CHECK(written==32U+432U*source.count);
    CHECK(cerberus_ble_archive_decode(bytes,written,&viewer.incoming));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&
        ble_archive_ui_complete(&viewer,1,&viewer.incoming,"saved-family-v5.BLE"));
    repaint();CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,250,54)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_hints&&!viewer.raw_response);
    repaint();
}

static void test_saved_full_families(const char *directory)
{
    static const uint8_t beacon[27]={26,0xff,0x4c,0,2,0x15,1,2,3,4};
    static const uint8_t findmy[]={3,3,0x44,0xfd},flipper[]={3,3,0x82,0x30};
    static const uint8_t flock[]={4,0xff,0xc8,9,1},meta[]={3,0xff,0x5f,0xfd};
    static const uint8_t hc[]={6,9,'H','C','-','0','5'};
    const struct {const uint8_t *bytes;size_t size;const char *detail;} cases[]={
        {beacon,sizeof(beacon),"IBEACON ADVERTISED FORMAT"},
        {findmy,sizeof(findmy),"FIND MY FORMAT HINTS"},
        {flipper,sizeof(flipper),"CODES: 3082"},
        {flock,sizeof(flock),"FLOCK ADVERTISED CLUES"},
        {meta,sizeof(meta),"META IDENTIFIER HINTS"},
        {hc,sizeof(hc),"HC-05"}};
    for(unsigned i=0;i<6;++i){
        uint8_t full[255]={0};size_t prefix=sizeof(full)-cases[i].size;
        full[0]=(uint8_t)(prefix-1U);full[1]=0xff;
        memcpy(full+prefix,cases[i].bytes,cases[i].size);
        full_family_source(full,sizeof(full),(uint8_t)(i+1U));reopen_family();
        CHECK(strstr(drawn,"SAVED C5 ORIGINAL")&&strstr(drawn,"FULL 255 B / PREVIEW 31 B"));
        CHECK(strstr(drawn,"HISTORICAL / NO LIVE RADIO")&&strstr(drawn,"AGE AT SAVE: <= 1700 ms"));
        CHECK(strstr(drawn,"DECODED HINTS / RAW PREVIEW IS SEPARATE")&&!strstr(drawn,"BEEP")&&!strstr(drawn,"FOX"));
        if(i==2U)preview(directory,"archive-family-full255-summary");
        cerberus_ble_archive_row_t frozen=viewer.selected;
        memset(&viewer.document.rows[0].family,0,sizeof(viewer.document.rows[0].family));
        viewer.document.rows[0].receiver_boot_id=7;viewer.document.rows[0].scan_epoch=8;
        viewer.document.saved_uptime_ms=9;
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(strstr(drawn,cases[i].detail)&&strstr(drawn,"AGE AT SAVE: <= 1700 ms"));
        CHECK(strstr(drawn,"SAVED BOOT UPTIME: 1000000 ms")&&strstr(drawn,"C5 BOOT ID: ABCDEF0123456789"));
        CHECK(!memcmp(&frozen,&viewer.selected,sizeof(frozen)));
        if(i==2U)preview(directory,"archive-family-frozen-flipper");
        const uint16_t gaps[][2]={{7,54},{104,54},{108,54},{208,54},{212,54},{312,54},{250,82}};
        for(unsigned gap=0;gap<sizeof(gaps)/sizeof(gaps[0]);++gap)
            CHECK(ble_archive_ui_tap(&viewer,gaps[gap][0],gaps[gap][1])==BLE_ARCHIVE_UI_NONE);
        CHECK(ble_archive_ui_tap(&viewer,8,54)==BLE_ARCHIVE_UI_REDRAW&&!viewer.raw_hints);repaint();
        CHECK(strstr(drawn,"31 BYTES / EVENT 3")&&!strstr(drawn,"FULL 255 B"));
        CHECK(ble_archive_ui_tap(&viewer,164,54)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_response);repaint();
        CHECK(strstr(drawn,"CALLBACK 255 B / PREVIEW 31 B")&&!strstr(drawn,"CODES: 3082"));
        CHECK(ble_archive_ui_tap(&viewer,250,54)==BLE_ARCHIVE_UI_REDRAW&&viewer.raw_hints&&!viewer.hint_page);repaint();
        unsigned total=calls;
        for(unsigned failure=1;failure<=total;++failure){
            clear_draw();fail_after=failure;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)!=ESP_OK);
            CHECK(!viewer.hint_pages&&ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_NONE);
            CHECK(!memcmp(&frozen,&viewer.selected,sizeof(frozen)));
        }
        repaint();CHECK(viewer.hint_pages>1U);
        CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
        CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_LIST);
    }
}

static void test_saved_hint_states(const char *directory)
{
    uint8_t full[255]={254,0xff};
    full_family_source(full,sizeof(full),3);reopen_family();
    CHECK(strstr(drawn,"NO SUPPORTED FAMILY HINT")&&strstr(drawn,"/ 0 HINTS"));
    CHECK(viewer.hint_pages==1U);preview(directory,"archive-family-complete-nohint");
    cerberus_dual_ble_family_evidence_t unavailable={.state=CERBERUS_DUAL_BLE_FAMILY_MALFORMED};
    full_family_source(full,sizeof(full),3);
    CHECK(cerberus_dual_ble_family_wire_encode(&unavailable,3,&source.rows[0].family));reopen_family();
    CHECK(strstr(drawn,"AD EVIDENCE NOT CLASSIFIABLE")&&!strstr(drawn,"NO SUPPORTED FAMILY HINT"));
    preview(directory,"archive-family-unclassifiable");
    full_family_source(full,sizeof(full),3);unavailable.state=CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN;
    memset(&source.rows[0].receive,0,sizeof(source.rows[0].receive));
    CHECK(cerberus_dual_ble_family_wire_encode(&unavailable,3,&source.rows[0].family));reopen_family();
    CHECK(strstr(drawn,"RECEIVE INFO UNAVAILABLE")&&!strstr(drawn,"FULL ORIGINAL HINTS NOT SAVED"));
    preview(directory,"archive-family-receive-unknown");
    full_family_source(full,sizeof(full),3);memset(&source.rows[0].family,0,sizeof(source.rows[0].family));
    source.rows[0].age_upper_valid=false;source.rows[0].age_upper_at_save_ms=0;reopen_family();
    CHECK(strstr(drawn,"FULL ORIGINAL HINTS NOT SAVED")&&strstr(drawn,"AGE AT SAVE: UNKNOWN"));
    CHECK(!strstr(drawn,"NO SUPPORTED FAMILY HINT"));preview(directory,"archive-family-absent-proof");
    full_family_source(full,sizeof(full),3);source.rows[0].age_upper_at_save_ms=UINT64_MAX;
    source.saved_uptime_ms=UINT64_MAX;reopen_family();
    CHECK(strstr(drawn,"AGE AT SAVE: <= 18446744073709551615 ms"));
    preview(directory,"archive-family-extreme-historical-age");
}

static void test_saved_meta_pages(const char *directory)
{
    uint8_t full[255]={0};size_t used=0;
    const uint8_t base[12]={0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0};
    const unsigned widths[]={2,4,16};
    for(unsigned w=0;w<3;++w){
        unsigned width=widths[w];full[used++]=(uint8_t)(11U*width+1U);full[used++]=(uint8_t)(3U+2U*w);
        for(unsigned id=0;id<11;++id){
            uint16_t value=cerberus_ble_meta_id_value(id);
            if(width==16U){memcpy(full+used,base,12);full[used+12]=(uint8_t)value;full[used+13]=(uint8_t)(value>>8);}
            else {full[used]=(uint8_t)value;full[used+1]=(uint8_t)(value>>8);}
            used+=width;
        }
    }
    CHECK(used==248U);full_family_source(full,sizeof(full),5);reopen_family();
    CHECK(viewer.hint_pages==18U);preview(directory,"archive-family-meta-summary");
    for(unsigned page=1;page<18;++page){
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(viewer.hint_page==page&&viewer.hint_pages==18U);
        CHECK(strstr(drawn,"META HINT SUPPRESSED BY ID RULE"));
        for(unsigned pair=0;pair<2&&2U*(page-1U)+pair<33U;++pair){
            unsigned index=2U*(page-1U)+pair;char expected[40];
            snprintf(expected,sizeof(expected),"ID %04X / %s",cerberus_ble_meta_id_value(index/3U),
                index%3U==0U?"LIST16":index%3U==1U?"LIST32":"LIST128");
            CHECK(strstr(drawn,expected));
        }
    }
    CHECK(strstr(drawn,"ID/CONTEXT PAIRS 33-33 OF 33"));preview(directory,"archive-family-meta-last-pair");
    CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();CHECK(viewer.hint_page==17U);
    for(unsigned page=17;page;--page){CHECK(ble_archive_ui_tap(&viewer,8,374)==BLE_ARCHIVE_UI_REDRAW);repaint();CHECK(viewer.hint_page==page-1U);}
}

static void test_v4_missing_hints(const char *directory)
{
    static const char hex[]=
        "43424c454152430004002000d8000100f8000000807060504030201045e6bb85"
        "010203040506020d03c9a5024c00030300fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000200030703010001";
    uint8_t bytes[248];CHECK(strlen(hex)==2U*sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);++i){unsigned value;CHECK(sscanf(hex+2U*i,"%2x",&value)==1);bytes[i]=(uint8_t)value;}
    memset(&viewer,0,sizeof(viewer));CHECK(cerberus_ble_archive_decode(bytes,sizeof(bytes),&source));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&ble_archive_ui_complete(&viewer,1,&source,"old-v4.BLE"));
    repaint();CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,250,54)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"FULL ORIGINAL HINTS NOT SAVED")&&!strstr(drawn,"NO SUPPORTED FAMILY HINT"));
    CHECK(strstr(drawn,"AGE AT SAVE: <= 18364758544493064720 ms"));preview(directory,"archive-family-v4-unavailable");
    CHECK(ble_archive_ui_tap(&viewer,200,94)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"C5 RESPONSE EVIDENCE UNKNOWN")&&!viewer.hint_pages);
}

static void saved_response_source(uint8_t family,uint8_t selected_family)
{
    seed(33);source.rows[0]=source.rows[32];source.count=1;
    CHECK(archive_test_response_row(&source.rows[0],family,selected_family));
    CHECK(cerberus_ble_archive_validate(&source));
}
static void open_saved_response(void)
{
    reopen_family();CHECK(!viewer.c5_response);
    CHECK(ble_archive_ui_tap(&viewer,200,94)==BLE_ARCHIVE_UI_REDRAW&&viewer.c5_response);
    repaint();
}
static void test_saved_c5_responses(const char *directory)
{
    const char *details[]={"IBEACON ADVERTISED FORMAT","FIND MY FORMAT HINTS","CODES: 3081",
        "FLOCK ADVERTISED CLUES","META IDENTIFIER HINTS","HC-05"};
    for(uint8_t family=1;family<=6U;++family){
        saved_response_source(family,family==6U?0U:family);open_saved_response();
        CHECK(strstr(drawn,"SAVED C5 RESPONSE")&&strstr(drawn,"FULL 255 B / PREVIEW 31 B"));
        CHECK(strstr(drawn,"AGE AT SAVE: <= 523 ms")&&strstr(drawn,"MATCHED RESPONSE / ORIGINAL KEPT SEPARATE"));
        CHECK(strstr(drawn,"HISTORICAL / NO LIVE RADIO")&&!strstr(drawn,"BEEP")&&!strstr(drawn,"FOX"));
        if(family==6U)preview(directory,"archive-response-full255-summary");
        cerberus_ble_archive_row_t frozen=viewer.selected;
        memset(&viewer.document.rows[0],0,sizeof(viewer.document.rows[0]));
        viewer.document.saved_uptime_ms=0;
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(strstr(drawn,details[family-1U])&&strstr(drawn,"AGE AT SAVE: <= 523 ms"));
        CHECK(strstr(drawn,"SAVED BOOT UPTIME: 1000000 ms")&&strstr(drawn,"C5 BOOT ID: ABCDEF0123456789"));
        CHECK(!memcmp(&viewer.selected,&frozen,sizeof(frozen)));
        if(family==6U){CHECK(strstr(drawn,"EXACT NAME IN THIS MATCHED RESPONSE"));preview(directory,"archive-response-full255-name");}
        CHECK(ble_archive_ui_tap(&viewer,8,54)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(strstr(drawn,"31 OF 255 BYTES / EVENT 3")&&strstr(drawn,"MALFORMED OR CUT AD AT BYTE 0"));
        CHECK(!strstr(drawn,details[family-1U]));
        if(family==6U)preview(directory,"archive-response-raw-preview");
        CHECK(ble_archive_ui_tap(&viewer,160,65)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(strstr(drawn,"CALLBACK 255 B / PREVIEW 31 B")&&strstr(drawn,"PROPS 0A"));
        CHECK(strstr(drawn,"SAVED RESPONSE RSSI -47 dBm")&&strstr(drawn,"RSP AGE AT SAVE: <= 523 ms"));
        if(family==6U){CHECK(strstr(drawn,"RSP NAME: HC-05"));preview(directory,"archive-response-receive-info");}
        CHECK(ble_archive_ui_tap(&viewer,40,94)==BLE_ARCHIVE_UI_REDRAW&&!viewer.c5_response);repaint();
        CHECK(strstr(drawn,"CALLBACK 3 B / PREVIEW 3 B")&&!strstr(drawn,"SAVED RESPONSE RSSI"));
        CHECK(ble_archive_ui_tap(&viewer,250,65)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(strstr(drawn,"AGE AT SAVE: <= 525 ms")&&!strstr(drawn,"AGE AT SAVE: <= 523 ms"));
        CHECK(!strstr(drawn,details[family-1U]));
        CHECK(ble_archive_ui_tap(&viewer,200,94)==BLE_ARCHIVE_UI_REDRAW);repaint();
        unsigned total=calls;
        for(unsigned failure=1;failure<=total;++failure){
            clear_draw();fail_after=failure;CHECK(ble_archive_ui_draw(&viewer,ESP_OK)!=ESP_OK);
            CHECK(!viewer.hint_pages&&ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_NONE);
            CHECK(!memcmp(&frozen,&viewer.selected,sizeof(frozen)));
        }
        /* Local tabs can request a repaint even after a failed paint. */
        CHECK(ble_archive_ui_tap(&viewer,40,94)==BLE_ARCHIVE_UI_REDRAW&&!viewer.c5_response);repaint();
        CHECK(ble_archive_ui_tap(&viewer,200,94)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(viewer.hint_pages>1U);
        const uint16_t gaps[][2]={{7,94},{156,94},{160,94},{312,94},{200,83},{200,108}};
        for(unsigned i=0;i<sizeof(gaps)/sizeof(gaps[0]);++i)
            CHECK(ble_archive_ui_tap(&viewer,gaps[i][0],gaps[i][1])==BLE_ARCHIVE_UI_NONE);
        CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW&&viewer.view==BLE_ARCHIVE_UI_DETAIL);
        CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW&&!viewer.c5_response);
    }
}
static void test_saved_c5_response_states(const char *directory)
{
    saved_response_source(6,0);source.rows[0].c5_response_age_upper_valid=false;
    source.rows[0].c5_response_age_upper_at_save_ms=0;open_saved_response();
    CHECK(strstr(drawn,"AGE AT SAVE: UNKNOWN")&&strstr(drawn,"FULL 255 B"));
    preview(directory,"archive-response-age-unknown");
    saved_response_source(6,6);source.rows[0].age_upper_valid=false;source.rows[0].age_upper_at_save_ms=0;
    source.rows[0].c5_response_age_upper_at_save_ms=UINT64_MAX;source.saved_uptime_ms=UINT64_MAX;
    open_saved_response();CHECK(strstr(drawn,"AGE AT SAVE: <= 18446744073709551615 ms"));
    preview(directory,"archive-response-extreme-age");
    CHECK(ble_archive_ui_tap(&viewer,160,65)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"RSP AGE AT SAVE: <= 18446744073709551615 ms"));
    preview(directory,"archive-response-extreme-age-receive");
    /* Name storage is bounded bytes, not necessarily a terminated C string. */
    saved_response_source(6,0);cerberus_dual_ble_response_wire_t *r=&source.rows[0].c5_response;
    r->name_length=32;memset(r->name,'N',32);r->name[2]=0;r->name[4]=0xff;
    open_saved_response();CHECK(ble_archive_ui_tap(&viewer,160,65)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"RSP NAME: NN.N.NNNNNNNNNNNNNNNNNNNNNNNNNNN"));
    preview(directory,"archive-response-bounded-name");
    for(unsigned state=0;state<=1;++state){
        saved_response_source(6,6);r=&source.rows[0].c5_response;memset(r,0,sizeof(*r));
        if(state){r->version=1;r->state=CERBERUS_DUAL_BLE_RESPONSE_NONE;r->original_sequence=7;}
        source.rows[0].c5_response_age_upper_valid=false;source.rows[0].c5_response_age_upper_at_save_ms=0;
        open_saved_response();CHECK(strstr(drawn,state?"NO ADMITTED MATCHED RESPONSE":"C5 RESPONSE EVIDENCE UNKNOWN"));
        CHECK(strstr(drawn,"THIS DOES NOT PROVE NO RESPONSE WAS SENT")&&!viewer.hint_pages);
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_NONE);
        preview(directory,state?"archive-response-none":"archive-response-unknown");
    }
    saved_response_source(6,6);open_saved_response();viewer.selected.c5_response.response_sequence=0;
    repaint();CHECK(strstr(drawn,"C5 RESPONSE EVIDENCE UNKNOWN")&&!strstr(drawn,"HC-05")&&!viewer.hint_pages);
    CHECK(ble_archive_ui_tap(&viewer,64,426)==BLE_ARCHIVE_UI_REDRAW);
}
static void test_saved_response_meta_pages(const char *directory)
{
    uint8_t full[255]={0};size_t used=0;
    const uint8_t base[12]={0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0};
    const unsigned widths[]={2,4,16};
    for(unsigned w=0;w<3;++w){
        unsigned width=widths[w];full[used++]=(uint8_t)(11U*width+1U);full[used++]=(uint8_t)(3U+2U*w);
        for(unsigned id=0;id<11;++id){
            uint16_t value=cerberus_ble_meta_id_value(id);
            if(width==16U){memcpy(full+used,base,12);full[used+12]=(uint8_t)value;full[used+13]=(uint8_t)(value>>8);}
            else {full[used]=(uint8_t)value;full[used+1]=(uint8_t)(value>>8);}
            used+=width;
        }
    }
    CHECK(used==248U);saved_response_source(5,5);
    cerberus_ble_archive_row_t *r=&source.rows[0];cerberus_dual_ble_family_evidence_t evidence;
    CHECK(cerberus_dual_ble_family_parse_ad(full,sizeof(full),r->address,r->address_type,&evidence));
    memcpy(r->c5_response.raw,full,31);
    CHECK(cerberus_dual_ble_response_family_encode(&evidence,5,&r->c5_response.family));
    open_saved_response();CHECK(viewer.hint_pages==18U);preview(directory,"archive-response-meta-summary");
    for(unsigned page=1;page<18;++page){
        CHECK(ble_archive_ui_tap(&viewer,164,374)==BLE_ARCHIVE_UI_REDRAW);repaint();
        CHECK(viewer.hint_page==page&&viewer.hint_pages==18U);
        CHECK(strstr(drawn,"META HINT SUPPRESSED BY ID RULE")&&strstr(drawn,"AGE AT SAVE: <= 523 ms"));
        for(unsigned pair=0;pair<2&&2U*(page-1U)+pair<33U;++pair){
            unsigned index=2U*(page-1U)+pair;char expected[40];
            snprintf(expected,sizeof(expected),"ID %04X / %s",cerberus_ble_meta_id_value(index/3U),
                index%3U==0U?"LIST16":index%3U==1U?"LIST32":"LIST128");CHECK(strstr(drawn,expected));
        }
    }
    CHECK(strstr(drawn,"ID/CONTEXT PAIRS 33-33 OF 33"));preview(directory,"archive-response-meta-last-pair");
}
static void test_v5_missing_response(const char *directory)
{
    /* Immutable independent v5 vector, not produced by the new v6 encoder. */
    static const char hex[]=
        "43424c4541524300050020000801010028010000807060504030201048ff8a9f"
        "010203040506020d03c9a5024c00030300fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002000307030100010000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000";
    uint8_t bytes[296];CHECK(strlen(hex)==2U*sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);++i){unsigned value;CHECK(sscanf(hex+2U*i,"%2x",&value)==1);bytes[i]=(uint8_t)value;}
    memset(&viewer,0,sizeof(viewer));CHECK(cerberus_ble_archive_decode(bytes,sizeof(bytes),&source));
    CHECK(ble_archive_ui_begin(&viewer,1,false)&&ble_archive_ui_complete(&viewer,1,&source,"old-v5.BLE"));
    repaint();CHECK(ble_archive_ui_tap(&viewer,8,150)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,64,374)==BLE_ARCHIVE_UI_REDRAW);
    CHECK(ble_archive_ui_tap(&viewer,200,94)==BLE_ARCHIVE_UI_REDRAW);repaint();
    CHECK(strstr(drawn,"C5 RESPONSE EVIDENCE UNKNOWN")&&!strstr(drawn,"NO ADMITTED MATCHED RESPONSE"));
    CHECK(!viewer.selected.c5_response.version&&!viewer.selected.c5_response_age_upper_valid);
    preview(directory,"archive-response-v5-unknown");
}

int main(int argc,char **argv)
{
    CHECK(argc==2);test_jobs(argv[1]);test_list(argv[1]);test_paint_and_privacy();test_raw(argv[1]);test_receivers(argv[1]);test_response(argv[1]);test_receive_info(argv[1]);
    test_filtered_pages(argv[1]);test_sort_and_empty(argv[1]);test_frozen_filtered_and_jobs(argv[1]);
    test_control_recovery_and_history(argv[1]);test_v1_filtered_read(argv[1]);
    test_saved_full_families(argv[1]);test_saved_hint_states(argv[1]);test_saved_meta_pages(argv[1]);test_v4_missing_hints(argv[1]);
    test_saved_c5_responses(argv[1]);test_saved_c5_response_states(argv[1]);test_saved_response_meta_pages(argv[1]);test_v5_missing_response(argv[1]);
    printf("BLE archive UI PASS %u assertions; PSRAM model=%zu bytes; no clock, heap, SD, IO worker or radio linked\n",
        checks,sizeof(ble_archive_ui_store_t));
    return 0;
}
