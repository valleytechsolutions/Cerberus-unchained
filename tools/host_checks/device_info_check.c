#include "device_info_sdk.h"
#include "device_info.h"
#include "fragment_transfer.h"
#include "privacy.h"
#include "display.h"
#include "ui_widgets.h"
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now_us = 1000000;
static unsigned peer_version = 80, model = 5, cores = 2;
static bool flash_ok = true, partition_ok = true, current_mac = true, fallback_mac = true;
static bool allocation_fails, task_fails, cancel_on_sd, cancel_on_delay;
static unsigned allocations, task_creations, transactions, fail_at, transport_fault;
static void (*task_function)(void *);
static void *task_argument;
static cerberus_device_info_job_t *cancel_job;
static char console_log[8192], render_log[16384];
static atomic_bool console_stop_requested;
static void *link_mutex = (void *)1;
static uint8_t sequence;
static esp_app_desc_t app;
static esp_partition_t partition = {.subtype=0, .address=0x10000, .size=0x180000, .label="factory"};

enum { FAULT_NONE, FAULT_INFO_SIZE, FAULT_GENERATION, FAULT_OBJECT_CRC, FAULT_FRAME_CRC,
       FAULT_SEQUENCE, FAULT_LATE, FAULT_CLOCK_BACK, FAULT_METADATA, FAULT_REPLACED };

void esp_chip_info(esp_chip_info_t *out) { *out=(esp_chip_info_t){model,300,cores,0x13}; }
esp_err_t esp_flash_get_size(void *unused, uint32_t *bytes) { (void)unused; *bytes=16U*1024U*1024U; return flash_ok?ESP_OK:ESP_FAIL; }
const esp_partition_t *esp_ota_get_running_partition(void) { return partition_ok?&partition:NULL; }
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
esp_err_t esp_efuse_mac_get_default(uint8_t *out) { const uint8_t mac[6]={2,0,0,0,0,1};memcpy(out,mac,6);return ESP_OK; }
esp_err_t esp_wifi_get_mac(int iface,uint8_t *out) { memset(out,0xaa,6);if(!current_mac)return ESP_ERR_INVALID_STATE;out[0]=2;out[5]=(uint8_t)(iface+2);return ESP_OK; }
esp_err_t esp_read_mac(uint8_t *out,int iface) { memset(out,0xbb,6);if(!fallback_mac)return ESP_FAIL;out[0]=2;out[5]=(uint8_t)(iface+4);return ESP_OK; }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ms) { now_us+=(int64_t)ms*1000;if(cancel_on_delay){cancel_on_delay=false;cerberus_device_info_cancel(cancel_job);} }
void vTaskDelete(void *task) { assert(!task); }
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{
    assert(!strcmp(name,"device_info")&&stack==4096&&priority==2&&!handle&&!task_function);
    if(task_fails)return 0;
    ++task_creations;task_function=fn;task_argument=arg;return pdPASS;
}
size_t heap_caps_get_free_size(unsigned caps) { assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return 1200000; }
size_t heap_caps_get_total_size(unsigned caps) { assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return 2000000; }
void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(allocation_fails)return NULL;
    void *p=calloc(n,size);assert(p);++allocations;return p;
}
void heap_caps_free(void *p) { if(p){assert(allocations);--allocations;free(p);} }
const char *esp_err_to_name(esp_err_t error)
{
    switch(error){case ESP_OK:return "ESP_OK";case ESP_ERR_NO_MEM:return "ESP_ERR_NO_MEM";
    case ESP_ERR_NOT_SUPPORTED:return "ESP_ERR_NOT_SUPPORTED";case ESP_ERR_TIMEOUT:return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FINISHED:return "ESP_ERR_NOT_FINISHED";default:return "ESP_ERR_INVALID_RESPONSE";}
}
void cerberus_system_health_snapshot(cerberus_system_health_t *s)
{
    *s=(cerberus_system_health_t){.reset_reason=1,.uptime_ms=123456,.free_internal_bytes=140000,.minimum_internal_bytes=130000};
}
const char *cerberus_reset_reason_label(esp_reset_reason_t reason) { (void)reason;return "POWERON"; }
void cerberus_battery_get_status(cerberus_battery_status_t *s)
{
    *s=(cerberus_battery_status_t){.valid=true,.percent=75,.voltage_mv=3950,.last_read_ms=1000};
}
esp_err_t cerberus_storage_get_status(cerberus_storage_status_t *s)
{
    *s=(cerberus_storage_status_t){.mounted=true,.total_bytes=32000000000ULL,.free_bytes=30000000000ULL};
    if(cancel_on_sd){cancel_on_sd=false;cerberus_device_info_cancel(cancel_job);}
    return ESP_OK;
}
esp_err_t c5_spi_link_get_wifi_connection_status(cerberus_wifi_connection_status_payload_t *s)
{
    *s=(cerberus_wifi_connection_status_payload_t){.state=CERBERUS_WIFI_CONNECTION_CONNECTED,.channel=36,.ipv4={192,0,2,5}};return ESP_OK;
}
static int take_link_mutex(unsigned wait) { assert(wait<=20);return link_mutex!=NULL; }
static void give_link_mutex(void) { }
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,void *out)
{
    (void)token;(void)deadline;++transactions;now_us+=200;
    if(transactions==fail_at)return ESP_ERR_TIMEOUT;
    *(cerberus_c5_status_t *)out=(cerberus_c5_status_t){.firmware_version=peer_version,.protocol_version=2,
        .free_heap_kib=52,.minimum_free_heap_kib=48,.uptime_ms=500,.boot_count=1};
    return ESP_OK;
}
static esp_err_t spi_request_response_passive(const void *input,void *discard,void *output,size_t size,uint32_t deadline)
{
    (void)discard;(void)deadline;assert(size==32);++transactions;now_us+=200;
    if(transactions==fail_at)return ESP_ERR_TIMEOUT;
    const cerberus_spi_frame_t *request=input;cerberus_spi_frame_t *response=output;
    assert(cerberus_protocol_frame_is_valid(request));
    if(request->command==CERBERUS_SPI_COMMAND_FRAGMENT_OPEN){
        cerberus_fragment_open_request_t q;memcpy(&q,request->payload,sizeof(q));
        assert(q.object_type==CERBERUS_FRAGMENT_OBJECT_DEVICE_IDENTITY);
        cerberus_device_identity_t id;cerberus_fragment_info_payload_t info;
        model=13;cores=1;cerberus_device_identity_collect(&id,peer_version);model=5;cores=2;
        if(transport_fault==FAULT_METADATA)id.version[0]='\n';
        assert(cerberus_c5_fragment_open(q.object_type,&id,sizeof(id),&info)==ESP_OK);
        if(transport_fault==FAULT_INFO_SIZE)++info.total_length;
        assert(cerberus_protocol_prepare_frame(response,request->command,request->sequence,&info,sizeof(info)));
    }else{
        assert(request->command==CERBERUS_SPI_COMMAND_FRAGMENT_READ);
        cerberus_fragment_read_request_t q;cerberus_fragment_read_payload_t reply;
        memcpy(&q,request->payload,sizeof(q));
        if(transport_fault==FAULT_REPLACED){
            uint32_t data=123;cerberus_fragment_info_payload_t info;
            assert(cerberus_c5_fragment_open(1,&data,sizeof(data),&info)==ESP_OK);
        }
        esp_err_t error=cerberus_c5_fragment_read(&q,&reply);
        if(error!=ESP_OK)return error;
        if(transport_fault==FAULT_GENERATION)++reply.generation;
        if(transport_fault==FAULT_OBJECT_CRC)reply.data[0]^=1;
        assert(cerberus_protocol_prepare_frame(response,request->command,request->sequence,&reply,sizeof(reply)));
    }
    if(transport_fault==FAULT_FRAME_CRC)response->crc16^=1;
    if(transport_fault==FAULT_SEQUENCE){++response->sequence;response->crc16=cerberus_protocol_crc16(response,30);}
    if(transport_fault==FAULT_LATE)now_us+=6000000;
    if(transport_fault==FAULT_CLOCK_BACK)now_us-=100000;
    return ESP_OK;
}

#include "c5_device_identity_link_impl.h"

static bool all_zero(const void *p,size_t n) { const unsigned char *b=p;while(n--)if(*b++)return false;return true; }
static void run_task(void) { assert(task_function);void (*fn)(void *)=task_function;void *arg=task_argument;task_function=NULL;fn(arg); }
static void reset_transport(void) { peer_version=80;transactions=fail_at=transport_fault=0;now_us=1000000;link_mutex=(void *)1; }
static esp_err_t read_identity(cerberus_device_identity_t *out)
{
    cerberus_device_identity_reader_t reader;bool complete=false;c5_spi_link_device_identity_begin(&reader,10000);
    esp_err_t error;
    do{
        unsigned before=transactions;memset(out,0xa5,sizeof(*out));
        error=c5_spi_link_device_identity_step(&reader,out,&complete);
        assert(transactions-before<=1);
        if(!complete)assert(all_zero(out,sizeof(*out)));
    }while(error==ESP_OK&&!complete);
    if(error!=ESP_OK){unsigned before=transactions;assert(c5_spi_link_device_identity_step(&reader,out,&complete)==error);assert(before==transactions);}
    return error;
}
static void check_metadata(void)
{
    cerberus_device_identity_t id,copy;cerberus_device_identity_collect(&id,80);
    assert(sizeof(id)==220&&cerberus_device_identity_valid(&id));
    assert((id.flags&(CERBERUS_ID_STA_CURRENT|CERBERUS_ID_AP_CURRENT))==(CERBERUS_ID_STA_CURRENT|CERBERUS_ID_AP_CURRENT));
    copy=id;copy.reserved=1;assert(!cerberus_device_identity_valid(&copy));
    copy=id;copy.version[31]='x';assert(!cerberus_device_identity_valid(&copy));
    copy=id;copy.partition_address=UINT32_MAX;assert(!cerberus_device_identity_valid(&copy));
    copy=id;copy.flags&=~CERBERUS_ID_FLASH;assert(!cerberus_device_identity_valid(&copy));
    copy=id;copy.cores=0;assert(!cerberus_device_identity_valid(&copy));
    current_mac=false;cerberus_device_identity_collect(&copy,80);assert(cerberus_device_identity_valid(&copy));
    assert(!(copy.flags&(CERBERUS_ID_STA_CURRENT|CERBERUS_ID_AP_CURRENT))&&(copy.flags&CERBERUS_ID_STA_MAC));
    flash_ok=partition_ok=fallback_mac=false;cerberus_device_identity_collect(&copy,80);
    assert(cerberus_device_identity_valid(&copy)&&copy.flags==CERBERUS_ID_BASE_MAC);
    assert(all_zero(copy.sta_mac,6)&&all_zero(copy.ap_mac,6));
    flash_ok=partition_ok=current_mac=fallback_mac=true;
}
static void check_transport(void)
{
    cerberus_device_identity_t id;reset_transport();assert(read_identity(&id)==ESP_OK);
    unsigned total=transactions;assert(total==16&&id.chip_model==13&&!strcmp(id.version,app.version));
    for(unsigned fail=1;fail<=total;++fail){reset_transport();fail_at=fail;assert(read_identity(&id)==ESP_ERR_TIMEOUT);}
    for(unsigned fault=FAULT_INFO_SIZE;fault<=FAULT_REPLACED;++fault){reset_transport();transport_fault=fault;assert(read_identity(&id)!=ESP_OK);}
    reset_transport();peer_version=79;assert(read_identity(&id)==ESP_ERR_NOT_SUPPORTED&&transactions==1);
    reset_transport();link_mutex=NULL;assert(read_identity(&id)==ESP_ERR_INVALID_STATE&&transactions==0);
    reset_transport();now_us=-1;assert(read_identity(&id)==ESP_ERR_INVALID_STATE);
    reset_transport();now_us=11000000;assert(read_identity(&id)==ESP_ERR_TIMEOUT);
    reset_transport();
}
static void check_worker(void)
{
    cerberus_device_info_job_t *job=NULL;cerberus_device_info_t value;
    allocation_fails=true;assert(cerberus_device_info_start(&job)==ESP_ERR_NO_MEM&&!job);allocation_fails=false;
    task_fails=true;assert(cerberus_device_info_start(&job)==ESP_ERR_NO_MEM&&!job&&!allocations);task_fails=false;
    for(unsigned mode=0;mode<4;++mode){
        reset_transport();assert(cerberus_device_info_start(&job)==ESP_OK);cancel_job=job;
        memset(&value,0xa5,sizeof(value));assert(!cerberus_device_info_read(job,&value));
        assert(!cerberus_device_info_release(&job)&&allocations==1);
        assert(cerberus_device_info_start(&job)==ESP_ERR_INVALID_STATE);
        if(mode==1)cerberus_device_info_cancel(job);
        if(mode==2)cancel_on_sd=true;
        if(mode==3)cancel_on_delay=true;
        run_task();assert(cerberus_device_info_read(job,&value));
        assert(value.cancelled==(mode!=0));
        if(mode)assert(value.c5_error==ESP_ERR_NOT_FINISHED&&all_zero(&value.c5,sizeof(value.c5)));
        else assert(value.s3_error==ESP_OK&&value.c5_error==ESP_OK&&value.storage_error==ESP_OK&&value.storage.mounted);
        assert(cerberus_device_info_release(&job)&&!job&&!allocations);
    }
}

static void console_printf(const char *format,...)
{
    char text[768];va_list args;va_start(args,format);int n=vsnprintf(text,sizeof(text),format,args);va_end(args);
    assert(n>=0&&(size_t)n<sizeof(text)&&strlen(console_log)+(size_t)n<sizeof(console_log));strcat(console_log,text);
}
#include "device_info_console.h"

static uint8_t pixels[480][320][3];
static bool masked=true;
#define UI_BACKGROUND 0x0841U
#define UI_PANEL 0x2104U
#define UI_HEADER_HEIGHT 48U
enum { UI_PAGE_BLE, UI_PAGE_IEEE802154, UI_PAGE_DASHBOARD };
static unsigned current_page=UI_PAGE_DASHBOARD;
#define UI_TEXT 0xffffU
#define UI_MUTED 0x9cd3U
#define UI_GOOD 0x07efU
#define UI_BAD 0xf981U
#define TAG "test"
#define ESP_RETURN_ON_ERROR(expression, ...) do { esp_err_t error_=(expression);if(error_!=ESP_OK)return error_; }while(0)
#include "device_info_font.h"
static bool privacy_addresses_masked(void) { return masked; }
esp_err_t cerberus_display_fill_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t color)
{
    assert(x+w<=320&&y+h<=480);
    for(unsigned yy=y;yy<y+h;++yy)for(unsigned xx=x;xx<x+w;++xx){pixels[yy][xx][0]=(uint8_t)((color>>11)*255/31);
        pixels[yy][xx][1]=(uint8_t)(((color>>5)&63)*255/63);pixels[yy][xx][2]=(uint8_t)((color&31)*255/31);}
    return ESP_OK;
}
esp_err_t cerberus_display_draw_text(uint16_t x,uint16_t y,const char *text,uint16_t color,uint16_t background,uint8_t scale)
{
    (void)background;assert(x+strlen(text)*6U*scale<=320&&y+8U*scale<=480);
    assert(strlen(render_log)+strlen(text)+2<sizeof(render_log));strcat(render_log,text);strcat(render_log,"\n");
    for(size_t i=0;text[i];++i){const uint8_t *glyph=font5x7(text[i]);for(unsigned col=0;col<5;++col)
        for(unsigned row=0;row<7;++row)if(glyph[row]&(1U<<(4U-col)))
            cerberus_display_fill_rect((uint16_t)(x+(i*6U+col)*scale),(uint16_t)(y+row*scale),scale,scale,color);}
    return ESP_OK;
}
static const cerberus_ui_widget_theme_t theme={UI_BACKGROUND,UI_PANEL,0x4a69,UI_TEXT,UI_MUTED};
#define UI_WIDGET_THEME (&theme)
#include "device_info_widgets.h"
#include "device_info_ui.h"

static void render(const char *directory,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s/info-%s.ppm",directory,name);render_log[0]=0;
    cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(device_info_draw()==ESP_OK);
    draw_button(20,420,280,44,"SYSTEM HEALTH");FILE *f=fopen(path,"wb");assert(f);
    assert(fputs("P6\n320 480\n255\n",f)>=0);assert(fwrite(pixels,1,sizeof(pixels),f)==sizeof(pixels));assert(!fclose(f));
}
static void check_ui(const char *directory)
{
    reset_transport();device_info_open();assert(device_info_tick(1000,true));
    assert(allocations==2&&task_function);unsigned created=task_creations;
    device_info_tap(160,390);assert(task_creations==created);render(directory,"reading");
    run_task();assert(device_info_tick(1100,true)&&device_info_ready&&allocations==1);
    unsigned reads=transactions;render(directory,"s3");assert(!strstr(render_log,"02:aa:aa:aa:aa:02"));
    device_info_tap(150,80);assert(device_info_tab==1);render(directory,"c5");assert(strstr(render_log,"CORES 1"));
    device_info_tap(250,80);assert(device_info_tab==2);render(directory,"system");assert(strstr(render_log,"192.0.2.5"));
    int64_t saved_time=now_us;
    now_us=-1;render(directory,"clock-unavailable");assert(strstr(render_log,"READ AGE UNAVAILABLE"));
    now_us=(int64_t)device_info_snapshot->sampled_ms*1000-1;
    render(directory,"clock-backward");assert(strstr(render_log,"READ AGE UNAVAILABLE"));now_us=saved_time;
    device_info_snapshot->storage.mounted=false;
    render(directory,"no-card");assert(strstr(render_log,"SD NOT MOUNTED"));device_info_snapshot->storage.mounted=true;
    assert(transactions==reads);device_info_tap(160,390);assert(device_info_requested&&!device_info_ready);
    assert(device_info_tick(1200,true));device_info_close();assert(allocations==1&&!device_info_snapshot);
    device_info_open();assert(device_info_discard);assert(!device_info_tick(1250,true));
    run_task();assert(device_info_tick(1300,true));assert(task_function);run_task();assert(device_info_tick(1400,true));
    assert(device_info_ready);assert(!device_info_tick(1500,false)&&!allocations);
    assert(device_info_tick(1600,true));run_task();assert(device_info_tick(1700,true));device_info_close();assert(!allocations);
    allocation_fails=true;device_info_open();render(directory,"no-memory");assert(device_info_error==ESP_ERR_NO_MEM);
    allocation_fails=false;device_info_tap(150,390);assert(device_info_tick(1800,true));run_task();assert(device_info_tick(1900,true));
    device_info_close();reset_transport();peer_version=79;device_info_open();assert(device_info_tick(2000,true));run_task();
    assert(device_info_tick(2100,true));device_info_tab=1;render(directory,"old-peer");assert(strstr(render_log,"80+"));
    device_info_tab=2;render(directory,"old-peer-system");
    assert(strstr(render_log,"C5 RADIO STATUS UNAVAILABLE")&&!strstr(render_log,"C5 RADIO FLAGS 00"));
    device_info_close();assert(!allocations);reset_transport();
}

int main(int argc,char **argv)
{
    assert(argc==2);snprintf(app.project_name,sizeof(app.project_name),"cerberus_test");
    snprintf(app.version,sizeof(app.version),"0.1.0-dev.rev157");snprintf(app.idf_ver,sizeof(app.idf_ver),"v6.0.1");
    snprintf(app.date,sizeof(app.date),"Nov 08 2026");snprintf(app.time,sizeof(app.time),"01:23:45");
    for(unsigned i=0;i<32;++i)app.app_elf_sha256[i]=(uint8_t)(i*7U);
    check_metadata();check_transport();check_worker();check_ui(argv[1]);
    print_builds();assert(strstr(console_log,"s3_build_version=0.1.0-dev.rev157")&&strstr(console_log,"c5_build_version=0.1.0-dev.rev157"));
    assert(strstr(console_log,"c5_build_elf_sha256=")&&!allocations);
    reset_transport();peer_version=79;console_log[0]=0;print_builds();assert(!strstr(console_log,"c5_build_version=")&&!allocations);
    allocation_fails=true;console_log[0]=0;print_builds();assert(strstr(console_log,"builds_error=257")&&!allocations);
    puts("Device Info production metadata/fragment parser/worker/UI/console checks passed.");return 0;
}
