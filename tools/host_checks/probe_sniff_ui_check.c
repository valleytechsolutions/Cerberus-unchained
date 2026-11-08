#define main pure_model_suite
#include "probe_sniff_check.c"
#undef main
#include "probe_framebuffer.h"
#include "ble_archive_io.h"
#include "storage.h"
#include "sentinel_hub.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF471
#define TAG "probe-ui"
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
typedef enum {UI_PAGE_WIFI,UI_PAGE_FILES,UI_PAGE_OTHER} ui_page_t;
typedef enum {UI_WIFI_MODE_MENU,UI_WIFI_MODE_PROBE_SNIFF} ui_wifi_mode_t;
static ui_page_t current_page;
static ui_wifi_mode_t wifi_mode;
static struct {uint32_t firmware_version;} c5_status={71};
static char file_path[CERBERUS_STORAGE_PATH_MAX],opened_name[CERBERUS_STORAGE_NAME_MAX];
static bool oom,masked;
static unsigned allocations,freed,io_saves,io_releases,io_cancels,file_opens;
static uint64_t job_sequence;
static cerberus_ble_archive_io_status_t io;
static cerberus_probe_sniff_snapshot_t io_copy;
static char io_bytes[8193];
static size_t io_length;
static esp_err_t io_start_error;
static bool privacy_addresses_masked(void){return masked;}
static int64_t esp_timer_get_time(void){return clock_us;}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void*p=calloc(n,size);CHECK(p);++allocations;return p;}
static void heap_caps_free(void*p){if(p){++freed;free(p);}}
static const char *esp_err_to_name(esp_err_t error)
{return error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":error==ESP_ERR_INVALID_RESPONSE?"ESP_ERR_INVALID_RESPONSE":error==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":"ESP_FAIL";}
static esp_err_t draw_centered(uint16_t y,const char*text,uint16_t color,uint8_t scale)
{CHECK(strlen(text)*6U*scale<=320U);return cerberus_display_draw_text((320U-strlen(text)*6U*scale)/2U,y,text,color,UI_BACKGROUND,scale);}
static esp_err_t draw_header(const char*text){return draw_centered(20,text,UI_TEXT,2);}
static esp_err_t button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*text,uint8_t scale)
{CHECK(strlen(text)*6U*scale<=w);esp_err_t error=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
 return error?error:cerberus_display_draw_text(x+(w-strlen(text)*6U*scale)/2U,y+(h-7U*scale)/2U,text,UI_TEXT,UI_PANEL,scale);}
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*text)
{return button(x,y,w,h,text,2);}
static esp_err_t draw_small_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*text)
{return button(x,y,w,h,text,1);}
esp_err_t c5_spi_link_probe_sniff_control(uint8_t command,const cerberus_probe_sniff_start_t*q,cerberus_probe_sniff_control_t*r)
{return fake_control(NULL,command,q,r);}
esp_err_t c5_spi_link_probe_sniff_read(const cerberus_probe_sniff_read_t*q,cerberus_probe_sniff_chunk_t*r)
{return fake_read(NULL,q,r);}
esp_err_t cerberus_ble_archive_io_save_probe(const cerberus_probe_sniff_snapshot_t*doc,uint64_t*job)
{++io_saves;*job=0;if(io_start_error)return io_start_error;CHECK(cerberus_probe_sniff_snapshot_valid(doc)&&doc->frozen);
 io_copy=*doc;CHECK(cerberus_probe_sniff_report(&io_copy,io_bytes,sizeof(io_bytes),&io_length));
 io=(cerberus_ble_archive_io_status_t){.job_id=++job_sequence,.operation=CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE,
    .format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT,.worker_active=true,.state=CERBERUS_BLE_ARCHIVE_IO_SAVING};*job=job_sequence;return ESP_OK;}
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t*out){*out=io;}
void cerberus_ble_archive_io_cancel(uint64_t job){CHECK(job==io.job_id);++io_cancels;}
void cerberus_ble_archive_io_release(uint64_t job){CHECK(job==io.job_id);++io_releases;}
static esp_err_t draw_detail_page(void);
static void open_text_file(const char*name){++file_opens;snprintf(opened_name,sizeof(opened_name),"%s",name);}
#include "probe_sniff_ui.h"
static esp_err_t draw_detail_page(void)
{drawn_size=0;drawn[0]=0;return wifi_mode==UI_WIFI_MODE_PROBE_SNIFF?probe_sniff_draw():ESP_OK;}
#include "probe_ui_routes.h"
static void step_ui(unsigned ms,bool visible)
{clock_us+=(int64_t)ms*1000;unsigned before=wire_calls;route_tick((uint64_t)clock_us/1000U,!visible);CHECK(wire_calls-before<=4U);}
static void until_published(bool frozen)
{uint64_t before=probe_sniff_ui->scan.publication;for(unsigned i=0;i<1000U&&probe_sniff_ui&&
    (probe_sniff_ui->scan.publication==before||probe_sniff_ui->scan.published.frozen!=frozen);++i)step_ui(20,true);
 CHECK(probe_sniff_ui&&probe_sniff_ui->scan.publication>before&&probe_sniff_ui->scan.published.frozen==frozen);}
static void reset_ui(void)
{CHECK(!probe_sniff_ui);source_reset();current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_MENU;oom=masked=false;
 io_start_error=ESP_OK;io=(cerberus_ble_archive_io_status_t){0};c5_status.firmware_version=71;fail_after=0;
 drawn_size=0;drawn[0]=0;opened_name[0]=0;}
static void exit_ui(void)
{probe_sniff_close(true);for(unsigned i=0;i<1000U&&probe_sniff_ui;++i)step_ui(20,true);CHECK(!probe_sniff_ui&&!owned&&!exists);}
static void begin_ui_rows(unsigned count)
{reset_ui();route_menu_probe();CHECK(probe_sniff_ui&&!wire_calls&&!starts&&wifi_mode==UI_WIFI_MODE_PROBE_SNIFF);
 probe_sniff_tap(180,392);step_ui(0,true);CHECK(starts==1U&&owned);
 for(unsigned i=0;i<count;++i){uint8_t name[32];for(unsigned j=0;j<32U;++j)name[j]=(uint8_t)(j*7U);
    name[0]=(uint8_t)i;packet((uint8_t)(i+1U),name,(uint8_t)(i==0U?0U:i==1U?32U:8U),i&1U);}
 until_published(false);CHECK(probe_sniff_draw()==ESP_OK);}
static void ui_checks(void)
{
    reset_ui();oom=true;route_menu_probe();CHECK(!probe_sniff_ui&&!wire_calls&&!starts);
    CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NO MEMORY"));route_back();CHECK(wifi_mode==UI_WIFI_MODE_MENU);oom=false;
    reset_ui();c5_status.firmware_version=70;route_menu_probe();probe_sniff_start();step_ui(20,true);
    CHECK(!starts&&!wire_calls);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"FIRMWARE 71"));exit_ui();
    begin_ui_rows(32U);CHECK(probe_sniff_ui->scan.published.count==32U);
    uint8_t extra[]={'N','E','W'};packet(99,extra,3,false);until_published(false);
    CHECK(probe_sniff_ui->scan.published.summary.capacity_dropped_frames==1U);
    unsigned before=wire_calls;CHECK(draw_detail_page()==ESP_OK&&wire_calls==before);
    CHECK(strstr(drawn,"32/32 PAIRS")&&strstr(drawn,"CAPACITY DROPS 1"));
    probe_sniff_tap(20,200);CHECK(probe_sniff_ui->detail&&probe_sniff_ui->selected.row.row.ssid_length==32U);
    probe_sniff_painted_t selected=probe_sniff_ui->selected;
    packet(1,extra,3,false);until_published(false);CHECK(!memcmp(&selected,&probe_sniff_ui->selected,sizeof(selected)));
    masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"**:**:**:**:**:**")&&!strstr(drawn,"02:00:00:00:00:02"));
    CHECK(strstr(drawn,"EXACT HEX")&&strstr(drawn,"C5 CALLBACK UPTIME"));masked=false;
    route_back();CHECK(!probe_sniff_ui->detail);CHECK(draw_detail_page()==ESP_OK);
    fail_after=calls+6U;CHECK(draw_detail_page()!=ESP_OK&&probe_sniff_ui->painted_count==0U);
    probe_sniff_tap(20,155);CHECK(!probe_sniff_ui->detail);fail_after=0;
    for(unsigned group=0;group<3U;++group){probe_sniff_tap((uint16_t)(20U+group*104U),120);CHECK(draw_detail_page()==ESP_OK);
        CHECK(probe_sniff_ui->group==group&&probe_sniff_ui->count==32U);}
    hold_stop=true;probe_sniff_tap(270,392);CHECK(probe_sniff_ui->save_requested);
    step_ui(0,true);CHECK(!io_saves&&owned);hold_stop=false;
    for(unsigned i=0;i<1000U&&!probe_sniff_ui->job;++i)step_ui(20,true);
    CHECK(probe_sniff_ui->job&&io_copy.frozen&&!owned&&io_saves==1U);
    unchanged=io_copy;memset(&table,0,sizeof(table));CHECK(!memcmp(&unchanged,&io_copy,sizeof(io_copy)));
    io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.worker_active=false;snprintf(io.file_name,sizeof(io.file_name),"P00001.CSV");
    step_ui(20,true);CHECK(!probe_sniff_ui->job&&strstr(probe_sniff_ui->message,"SAVED"));
    probe_sniff_tap(270,392);CHECK(current_page==UI_PAGE_FILES&&file_opens&&
        !strcmp(file_path,"/CERBERUS/LOGS")&&!strcmp(opened_name,"P00001.CSV"));
    for(unsigned i=0;i<1000U&&probe_sniff_ui;++i)step_ui(20,false);CHECK(!probe_sniff_ui);

    begin_ui_rows(2U);probe_sniff_start();until_published(true);
    source_error=ESP_FAIL;probe_sniff_ui->scan.published.control.error_code=ESP_FAIL;
    CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SOURCE ERROR"));source_error=ESP_OK;
    probe_sniff_start();CHECK(probe_sniff_ui->suppress_old);CHECK(draw_detail_page()==ESP_OK&&!strstr(drawn,"CALLBACKS / SAMPLED"));
    fail_part=0U;for(unsigned i=0;i<50U;++i)step_ui(20,true);
    CHECK(probe_sniff_ui->suppress_old||probe_sniff_ui->scan.publication>2U);exit_ui();

    begin_ui_rows(1U);hold_stop=true;route_back();CHECK(probe_sniff_ui->closing);
    for(unsigned i=0;i<20U;++i)step_ui(700,false);CHECK(probe_sniff_ui&&owned);
    hold_stop=false;for(unsigned i=0;i<1000U&&probe_sniff_ui;++i)step_ui(20,false);CHECK(!probe_sniff_ui);
    begin_ui_rows(1U);probe_sniff_tap(270,392);for(unsigned i=0;i<1000U&&!probe_sniff_ui->job;++i)step_ui(20,true);
    CHECK(probe_sniff_ui->job);unsigned opened=file_opens;route_back();CHECK(io_cancels);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;snprintf(io.file_name,sizeof(io.file_name),"P00002.CSV");
    for(unsigned i=0;i<1000U&&probe_sniff_ui;++i)step_ui(20,true);CHECK(!probe_sniff_ui&&file_opens==opened);
    CHECK(allocations==freed);
}
static void pending_and_save_retry(void)
{
    reset_ui();route_menu_probe();probe_sniff_start();
    CHECK(probe_sniff_ui->scan.requested&&!probe_sniff_ui->scan.request.session_id&&!wire_calls);
    CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"STARTING C5 CAPTURE")&&strstr(drawn,"STOP\n"));
    probe_sniff_tap(20,80);probe_sniff_tap(250,80);
    CHECK(probe_sniff_ui->mode==CERBERUS_PROBE_SNIFF_FIXED&&!probe_sniff_ui->channel_index);
    probe_sniff_tap(180,392);step_ui(0,true);CHECK(!starts&&!wire_calls);exit_ui();

    begin_ui_rows(2);unsigned saves=io_saves,start_count=starts;
    probe_sniff_tap(270,392);for(unsigned i=0;i<1000U&&!probe_sniff_ui->job;++i)step_ui(20,true);
    CHECK(io_saves==saves+1U);cerberus_probe_sniff_snapshot_t saved=io_copy;
    for(unsigned i=0;i<1000U&&probe_sniff_ui->scan.request.session_id;++i)step_ui(20,true);
    CHECK(!probe_sniff_ui->scan.request.session_id&&!exists&&!owned);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_FAIL;step_ui(20,true);
    CHECK(!probe_sniff_ui->job&&probe_sniff_ui->error==ESP_FAIL&&probe_sniff_ui->scan.published.frozen);
    CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SAVE\n"));
    probe_sniff_tap(270,392);step_ui(20,true);
    CHECK(probe_sniff_ui->job&&io_saves==saves+2U&&starts==start_count&&!exists&&!memcmp(&saved,&io_copy,sizeof(saved)));
    /* A terminal job of the wrong type never offers the report's Open action. */
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI;
    snprintf(io.file_name,sizeof(io.file_name),"WRONG.WFI");step_ui(20,true);
    CHECK(!probe_sniff_ui->job&&!probe_sniff_ui->saved_name[0]&&probe_sniff_ui->error==ESP_ERR_INVALID_RESPONSE);
    probe_sniff_tap(270,392);step_ui(20,true);CHECK(probe_sniff_ui->job&&starts==start_count);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;snprintf(io.file_name,sizeof(io.file_name),"P00003.CSV");step_ui(20,true);
    CHECK(!strcmp(probe_sniff_ui->saved_name,"P00003.CSV"));exit_ui();

    /* A queued capture canceled before source arming provides no measured-zero bank. */
    reset_ui();route_menu_probe();probe_sniff_start();probe_sniff_tap(270,392);step_ui(20,true);
    CHECK(!starts&&!probe_sniff_ui->save_requested&&!probe_sniff_ui->job&&probe_sniff_ui->error==ESP_ERR_INVALID_STATE);exit_ui();
    begin_ui_rows(1U);hold_stop=true;current_page=UI_PAGE_OTHER;step_ui(20,true);
    CHECK(probe_sniff_ui->closing&&owned);hold_stop=false;
    for(unsigned i=0;i<1000U&&probe_sniff_ui;++i)step_ui(20,true);
    CHECK(!probe_sniff_ui&&current_page==UI_PAGE_OTHER);
}
static void ppm(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);
 fprintf(f,"P6\n320 480\n255\n");for(size_t i=0;i<320U*480U;++i){unsigned v=screen[i];
    fputc(((v>>11)&31U)*255U/31U,f);fputc(((v>>5)&63U)*255U/63U,f);fputc((v&31U)*255U/31U,f);}CHECK(!fclose(f));}
static void previews(const char*folder)
{
    begin_ui_rows(32U);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"probe-requests");
    probe_sniff_ui->scan.published.summary.capacity_dropped_frames=UINT32_MAX;
    CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"CAPACITY DROPS >=4294967295"));
    probe_sniff_ui->scan.published.summary.capacity_dropped_frames=0;
    probe_sniff_tap(130,120);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"probe-senders");
    probe_sniff_tap(245,120);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"probe-networks");
    probe_sniff_tap(20,200);masked=true;CHECK(draw_detail_page()==ESP_OK);ppm(folder,"probe-binary-detail");masked=false;
    route_back();hold_stop=true;probe_sniff_start();step_ui(20,true);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"probe-stopping");hold_stop=false;exit_ui();
    reset_ui();oom=true;route_menu_probe();CHECK(draw_detail_page()==ESP_OK);ppm(folder,"probe-no-memory");route_back();oom=false;
    static const cerberus_ui_widget_theme_t theme={.background=UI_BACKGROUND,.panel=UI_PANEL,.text=UI_TEXT,.muted=UI_MUTED,.border=UI_GOOD};
    CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK&&draw_header("Wi-Fi tools")==ESP_OK);
    CHECK(sentinel_hub_draw(SENTINEL_HUB_WIFI,&theme)==ESP_OK&&draw_button(8,430,304,42,"BACK")==ESP_OK);
    const int actions[]={0,1,2,11,10,3,4,5};
    for(unsigned i=0;i<8;++i){CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,30,(uint16_t)(70U+i*36U))==actions[i]);
        CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,30,(uint16_t)(101U+i*36U))==actions[i]);
        CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,30,(uint16_t)(102U+i*36U))==-1);}
    CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,30,225)==10);ppm(folder,"probe-wifi-entry");
}
int main(int argc,char**argv)
{CHECK(argc==2);ui_checks();pending_and_save_retry();previews(argv[1]);printf("Probe UI: %u checks; workspace %zu bytes.\n",checks,sizeof(probe_sniff_ui_store_t));return 0;}
