#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wardrive_service.h"
#include "wardrive_log_manager.h"
#include "wardrive_upload_settings.h"
#include "keyboard.h"
#include "ui_widgets.h"
#include "display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "gps_recorder_ui_stub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_font.h"
#define UI_HEADER_HEIGHT 48U
enum { UI_PAGE_BLE=1, UI_PAGE_IEEE802154=2, UI_PAGE_WARDRIVE=3 };
static unsigned current_page=UI_PAGE_WARDRIVE;
static const char *TAG="upload_ui_test";
static uint16_t pixels[480][320];
static char drawn[8192];
static bool wardrive_start_pending;
static uint32_t wardrive_last_draw_ms;
static esp_err_t wardrive_start_error;
static char wardrive_blocked_reason[64];
static cerberus_wardrive_status_t runtime;
static cerberus_log_manager_status_t manager;
static cerberus_upload_config_t saved;
static unsigned requests,saves,forgets,cancels,closes,allocs,retries;
static bool current_names;
static cerberus_log_action_t last_action;
static int last_index;
static void wardrive_gps_view_close(void){}
static void wardrive_recent_close(void){}
void cerberus_wardrive_get_status(cerberus_wardrive_status_t *out){*out=runtime;}
const char *esp_err_to_name(esp_err_t error){return error==ESP_OK?"ESP_OK":"TEST_ERROR";}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){assert(caps==6);void *p=calloc(n,size);if(p)++allocs;return p;}
void heap_caps_free(void *p){if(p){assert(allocs);--allocs;free(p);}}
esp_err_t cerberus_log_manager_open(void){manager.active=true;manager.session=19;manager.catalog_revision=7;manager.count=7;return ESP_OK;}
void cerberus_log_manager_close(void){++closes;manager.active=false;}
void cerberus_log_manager_cancel(void){++cancels;}
void cerberus_log_manager_status(cerberus_log_manager_status_t *out){*out=manager;}
bool cerberus_log_manager_row(unsigned index,cerberus_ward_log_entry_t *out){if(index>=manager.count)return false;
    memset(out,0,sizeof(*out));snprintf(out->name,sizeof(out->name),"%s%03u.CSV",current_names?"CERBWardrive":"WIGLE",index+1);out->bytes=123456;return true;}
esp_err_t cerberus_log_manager_request(uint64_t session,uint32_t revision,cerberus_log_action_t action,int index)
{assert(session==manager.session&&revision==manager.catalog_revision);++requests;last_action=action;last_index=index;return ESP_OK;}
esp_err_t cerberus_log_manager_retry_service(uint64_t session,uint32_t revision,int index,cerberus_upload_service_t service)
{assert(session==manager.session&&revision==manager.catalog_revision&&index==0&&service==CERBERUS_UPLOAD_WDGWARS);++retries;return ESP_OK;}
esp_err_t cerberus_upload_settings_load(cerberus_upload_config_t *out){*out=saved;return ESP_OK;}
esp_err_t cerberus_upload_settings_save(const cerberus_upload_config_t *c){saved=*c;++saves;return ESP_OK;}
esp_err_t cerberus_upload_settings_forget(void){memset(&saved,0,sizeof(saved));++forgets;return ESP_OK;}
esp_err_t cerberus_display_fill_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t c)
{assert((unsigned)x+w<=320&&(unsigned)y+h<=480);for(unsigned iy=y;iy<(unsigned)y+h;++iy)for(unsigned ix=x;ix<(unsigned)x+w;++ix)pixels[iy][ix]=c;return ESP_OK;}
esp_err_t cerberus_display_draw_text(uint16_t x,uint16_t y,const char *text,uint16_t fg,uint16_t bg,uint8_t scale)
{
    assert(x+strlen(text)*6U*scale<=320&&(unsigned)y+7U*scale<=480);assert(strlen(drawn)+strlen(text)+2<sizeof(drawn));
    strcat(drawn,text);strcat(drawn,"\n");
    for(size_t i=0;text[i];++i){const uint8_t *font=font5x7((unsigned char)text[i]);
        for(unsigned cy=0;cy<7;++cy)for(unsigned cx=0;cx<6;++cx)for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
            pixels[y+cy*scale+sy][x+(i*6+cx)*scale+sx]=cx<5&&(font[cy]&(1U<<(4-cx)))?fg:bg;}
    return ESP_OK;
}
#include "upload_draw_helpers.h"
#include "wardrive_logs_ui.h"
static void paint(const char *prefix,const char *name)
{
    drawn[0]=0;assert(wardrive_logs_draw()==ESP_OK);char path[768];snprintf(path,sizeof(path),"%s-%s.ppm",prefix,name);
    FILE *f=fopen(path,"wb");assert(f);fputs("P6\n320 480\n255\n",f);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<320;++x){uint16_t p=pixels[y][x];unsigned char rgb[3]={(unsigned char)(((p>>11)&31)*255/31),(unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};assert(fwrite(rgb,1,3,f)==3);}assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);runtime.state=CERBERUS_WARDRIVE_RUNNING;wardrive_logs_open();assert(!wardrive_logs_is_open());
    runtime.state=CERBERUS_WARDRIVE_STOPPED;gps_log_runtime.busy=true;wardrive_logs_open();
    assert(!wardrive_logs_is_open()&&wardrive_start_error==ESP_ERR_INVALID_STATE&&strstr(wardrive_blocked_reason,"STOP GPX"));
    gps_log_runtime.busy=false;
    runtime.state=CERBERUS_WARDRIVE_STOPPED;wardrive_logs_open();assert(wardrive_logs_is_open()&&allocs==1);
    paint(argv[1],"list");assert(strstr(drawn,"HOME WI-FI / API SETUP")&&strstr(drawn,"WIGLE005.CSV"));
    wardrive_logs_tap(250,320);paint(argv[1],"page2");assert(strstr(drawn,"WIGLE007.CSV")&&!strstr(drawn,"WIGLE001.CSV"));
    wardrive_logs_tap(50,320);wardrive_logs_tap(50,140);assert(last_action==CERBERUS_LOG_INSPECT&&last_index==0&&requests==1);
    strcpy(manager.name,"WIGLE001.CSV");manager.summary=(cerberus_ward_log_summary_t){.kind=CERBERUS_WARD_LOG_WIGLE,.complete=true,.rows=1436,.wifi_24=263,.wifi_5=121,.ble=1052,.missing_accuracy=1436,.missing_altitude=2,.bytes=139042};
    strcpy(manager.summary.first_utc,"2026-11-04 12:00:00");paint(argv[1],"detail");
    manager.destination[0].result=CERBERUS_UPLOAD_QUEUED;manager.destination[1].result=CERBERUS_UPLOAD_UNKNOWN;
    manager.destination_complete[0]=true;
    strcpy(manager.stage,"WDGWARS / CHECK ACCOUNT FIRST");manager.error=ESP_ERR_INVALID_STATE;
    manager.transfer=(cerberus_upload_transfer_t){.attempted=true,.http_status=200,.response_bytes=18};
    paint(argv[1],"uncertain");assert(strstr(drawn,"UPLOAD NEEDS RETRY CONFIRMATION")&&strstr(drawn,"WIGLE: UPLOADED")&&strstr(drawn,"HTTP 200 / NO COMPLETE REPLY"));
    assert(strstr(drawn,"WDGWARS: CHECK ACCOUNT")&&strstr(drawn,"UPLOAD VERIFIED; IMPORT MAY BE PENDING")&&!strstr(drawn,"WDGWARS: UPLOADED"));
    manager.error=ESP_OK;manager.stage[0]=0;memset(&manager.transfer,0,sizeof(manager.transfer));
    manager.destination[1]=(cerberus_upload_reply_t){.result=CERBERUS_UPLOAD_ACCEPTED,.counts_valid=true,.imported=1400,.rejected_rows=36};
    manager.destination_complete[1]=true;
    manager.destination[2].result=CERBERUS_UPLOAD_REJECTED;
    paint(argv[1],"uploaded");assert(strstr(drawn,"WIGLE: UPLOADED")&&strstr(drawn,"WDGWARS: UPLOADED")&&strstr(drawn,"WARDRIFT: REJECTED"));
    assert(strstr(drawn,"WDGW IMPORTED 1400 / REJECTED ROWS 36")&&strstr(drawn,"IMPORT MAY BE PENDING"));
    manager.destination[0].result=CERBERUS_UPLOAD_NEW;
    paint(argv[1],"accepted");assert(strstr(drawn,"WDGWARS: UPLOADED")&&!strstr(drawn,"IMPORT MAY BE PENDING"));
    manager.destination[1]=(cerberus_upload_reply_t){.result=CERBERUS_UPLOAD_UNKNOWN};
    manager.destination[2].result=CERBERUS_UPLOAD_SENDING;
    paint(argv[1],"not-confirmed");assert(!strstr(drawn,"UPLOADED")&&!strstr(drawn,"IMPORT MAY BE PENDING"));
    manager.destination[0].result=CERBERUS_UPLOAD_QUEUED;manager.destination[2].result=CERBERUS_UPLOAD_NEW;
    manager.destination_complete[0]=false;manager.destination_complete[1]=false;
    paint(argv[1],"incomplete");assert(strstr(drawn,"WIGLE: INCOMPLETE")&&!strstr(drawn,"UPLOADED")&&!strstr(drawn,"IMPORT MAY BE PENDING"));
    manager.busy=true;paint(argv[1],"multipart-progress");assert(strstr(drawn,"WIGLE: IN PROGRESS")&&!strstr(drawn,"UPLOADED"));
    manager.busy=false;manager.destination_complete[0]=true;
    cerberus_ward_log_summary_t located=manager.summary;
    manager.summary=(cerberus_ward_log_summary_t){.kind=CERBERUS_WARD_LOG_UNLOCATED,.complete=true,.rows=2,.bytes=717};
    paint(argv[1],"unlocated");assert(strstr(drawn,"RADIO SUBTOTALS NOT AVAILABLE")&&strstr(drawn,"LEGACY LOG / NO GPS / CANNOT UPLOAD"));
    assert(!strstr(drawn,"BLE 0")&&!strstr(drawn,"NO ACCURACY 0"));
    wardrive_logs_tap(50,360);assert(wd_log_ui->page==WD_LOG_DETAIL&&requests==1);
    manager.summary=located;
    wardrive_logs_tap(50,400);assert(wd_log_ui->page==WD_LOG_CONFIRM_DELETE&&requests==1);
    paint(argv[1],"delete-confirm");wardrive_logs_tap(50,450);assert(wd_log_ui->page==WD_LOG_DETAIL&&requests==1);
    wardrive_logs_tap(50,360);assert(wd_log_ui->page==WD_LOG_CONFIRM_UPLOAD&&requests==1);
    paint(argv[1],"upload-confirm");wardrive_logs_tap(50,395);assert(last_action==CERBERUS_LOG_UPLOAD&&last_index==0&&requests==2);
    manager.busy=true;paint(argv[1],"progress");assert(strstr(drawn,"WIGLE: UPLOADED")&&strstr(drawn,"IMPORT MAY BE PENDING")&&strstr(drawn,"WDGWARS: CHECK ACCOUNT"));
    wardrive_logs_tap(50,395);assert(cancels==1&&requests==2);manager.busy=false;
    wardrive_logs_tap(50,450);wardrive_logs_tap(50,105);assert(wd_log_ui->page==WD_LOG_SETUP);
    paint(argv[1],"setup");wardrive_logs_tap(50,148);assert(wd_log_ui->page==WD_LOG_INPUT&&wd_log_ui->keyboard.masked);
    strcpy(wd_log_ui->input,"SyntheticSecret123");wd_log_ui->keyboard.length=strlen(wd_log_ui->input);
    paint(argv[1],"password");assert(!strstr(drawn,"SyntheticSecret123"));
    wardrive_logs_tap(270,366);assert(wd_log_ui->page==WD_LOG_SETUP&&!strcmp(wd_log_ui->draft.password,"SyntheticSecret123")&&!wd_log_ui->input[0]);
    wardrive_logs_tap(50,360);assert(saves==1&&wd_log_ui->page==WD_LOG_LIST&&!wd_log_ui->draft.password[0]);
    wardrive_logs_tap(50,105);wardrive_logs_tap(50,400);assert(wd_log_ui->page==WD_LOG_CONFIRM_FORGET&&!forgets);
    wardrive_logs_tap(50,395);assert(forgets==1&&!saved.password[0]);wardrive_logs_tap(50,450);
    wardrive_logs_tap(50,400);assert(wd_log_ui->confirmed_count==7&&last_action==CERBERUS_LOG_UPLOAD);wardrive_logs_tap(50,395);
    assert(last_action==CERBERUS_LOG_DELETE&&last_index==-1&&requests==3);
    wardrive_logs_tap(50,450);wardrive_logs_tick(false);assert(!wardrive_logs_is_open()&&!allocs&&closes);
    wardrive_logs_open();wardrive_logs_tap(50,105);wardrive_logs_tap(50,336);
    assert(wd_log_ui->page==WD_LOG_CONFIRM_IMPORT&&requests==3);
    paint(argv[1],"import-confirm");assert(strstr(drawn,CERBERUS_WARD_UPLOAD_PROFILE_PATH)&&strstr(drawn,"REPLACES HOME AP"));
    wardrive_logs_tap(50,450);assert(wd_log_ui->page==WD_LOG_SETUP&&requests==3);
    wardrive_logs_tap(50,336);wardrive_logs_tap(50,395);
    assert(last_action==CERBERUS_LOG_IMPORT_PROFILE&&last_index==-1&&requests==4&&wd_log_ui->page==WD_LOG_LIST);
    wardrive_logs_tap(50,450);wardrive_logs_tick(false);assert(!wardrive_logs_is_open()&&!allocs);
    wardrive_logs_open();strcpy(wd_log_ui->draft.password,"SyntheticSecret123");wardrive_logs_tick(true);assert(!wd_log_ui&&!allocs);
    memset(&manager,0,sizeof(manager));wardrive_logs_open();wardrive_logs_tap(50,140);
    manager.summary=located;strcpy(manager.name,"WIGLE001.CSV");saved.destinations=3;
    manager.destination[0].result=CERBERUS_UPLOAD_QUEUED;manager.destination[1].result=CERBERUS_UPLOAD_UNKNOWN;
    manager.destination_complete[0]=true;
    unsigned before=requests;paint(argv[1],"retry-direct");assert(strstr(drawn,"RETRY WDGWARS UPLOAD"));
    wardrive_logs_tap(50,360);assert(wd_log_ui->page==WD_LOG_CONFIRM_RETRY);paint(argv[1],"retry-confirm");
    assert(strstr(drawn,"THE EARLIER ATTEMPT MAY HAVE ARRIVED")&&strstr(drawn,"OTHER SERVICES ARE NOT SENT"));
    wardrive_logs_tap(50,450);assert(wd_log_ui->page==WD_LOG_DETAIL&&!retries);
    wardrive_logs_tap(50,360);wardrive_logs_tap(50,395);
    assert(wd_log_ui->page==WD_LOG_DETAIL&&retries==1&&requests==before);
    /* Multiple uncertain destinations require one selected service. The main
     * confirmation button must not reissue the normal action and hit the hold. */
    manager.destination[0].result=CERBERUS_UPLOAD_UNKNOWN;
    wardrive_logs_tap(50,360);paint(argv[1],"retry-review");
    assert(wd_log_ui->page==WD_LOG_CONFIRM_UPLOAD&&strstr(drawn,"CHOOSE A SERVICE ABOVE"));
    wardrive_logs_tap(50,395);assert(wd_log_ui->page==WD_LOG_CONFIRM_UPLOAD&&requests==before);
    wardrive_logs_tap(50,264);assert(wd_log_ui->page==WD_LOG_CONFIRM_RETRY);
    wardrive_logs_tap(50,450);assert(wd_log_ui->page==WD_LOG_CONFIRM_UPLOAD);
    manager.destination[0].result=CERBERUS_UPLOAD_QUEUED;
    wardrive_logs_tap(50,395);assert(wd_log_ui->page==WD_LOG_CONFIRM_RETRY&&requests==before);
    wardrive_logs_tap(50,450);wardrive_logs_tap(50,450);assert(wd_log_ui->page==WD_LOG_DETAIL);
    saved.destinations=1;wardrive_logs_tap(50,360);paint(argv[1],"retry-disabled");
    assert(!strstr(drawn,"REVIEW RETRY"));wardrive_logs_tap(50,264);assert(wd_log_ui->page==WD_LOG_CONFIRM_UPLOAD&&retries==1);
    wardrive_logs_tap(50,450);wardrive_logs_tap(50,450);saved.destinations=3;
    wardrive_logs_tap(50,360);assert(wd_log_ui->confirmed_index==-1);paint(argv[1],"retry-all-excluded");
    assert(!strstr(drawn,"REVIEW RETRY"));wardrive_logs_tap(50,264);assert(wd_log_ui->page==WD_LOG_CONFIRM_UPLOAD&&retries==1);
    wardrive_logs_tap(50,450);wardrive_logs_tap(50,140);strcpy(manager.name,"WIGLE007.CSV");
    wardrive_logs_tap(50,360);paint(argv[1],"retry-wrong-file");assert(!strstr(drawn,"REVIEW RETRY"));
    wardrive_logs_tap(50,450);wardrive_logs_tap(50,450);current_names=true;
    paint(argv[1],"current-list");assert(strstr(drawn,"CERBWardrive001.CSV"));
    wardrive_logs_tap(50,140);strcpy(manager.name,"CERBWardrive001.CSV");
    paint(argv[1],"current-detail");assert(strstr(drawn,"CERBWardrive001.CSV")&&strstr(drawn,"RETRY WDGWARS UPLOAD"));
    wardrive_logs_tick(true);assert(!wd_log_ui&&!allocs);
    wardrive_logs_open();manager.count=0;manager.error=ESP_OK;manager.stage[0]=0;
    runtime=(cerberus_wardrive_status_t){.state=CERBERUS_WARDRIVE_STOPPED,.gps_requested=true,.skipped_no_fix=138};
    paint(argv[1],"empty-no-fix");assert(strstr(drawn,"LAST SESSION: NO CSV SAVED")&&
        strstr(drawn,"NO VALID GPS FIX + UTC DURING CAPTURE")&&strstr(drawn,"WAIT FOR GPS: FIX + UTC"));
    manager.count=7;paint(argv[1],"old-logs-no-new-fix");
    assert(strstr(drawn,"CERBWardrive001.CSV")&&strstr(drawn,"LAST SESSION: NO CSV SAVED"));
    runtime.files_saved=1;runtime.sd_rows=10;paint(argv[1],"saved-session");assert(!strstr(drawn,"NO CSV SAVED"));
    manager.count=0;manager.error=ESP_ERR_INVALID_STATE;paint(argv[1],"catalog-error");
    assert(strstr(drawn,"LOG LIST UNAVAILABLE")&&!strstr(drawn,"NO SAVED CSV LOGS"));
    wardrive_logs_tap(50,450);wardrive_logs_tick(false);assert(!wd_log_ui&&!allocs);
    puts("Actual upload UI: pagination, masked setup, explicit confirmations, Stop/Back/sleep passed");return 0;
}
