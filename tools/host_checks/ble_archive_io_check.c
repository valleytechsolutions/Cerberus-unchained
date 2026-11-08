#undef NDEBUG
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include "ble_archive_io.h"
#include "archive_storage_types.h"
#include "ble_archive_family_fixture.h"
#include "ble_archive_response_fixture.h"
#include "archive_compat_vectors.h"
#include "wifi_archive_io_fixture.h"
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"Archive IO %u: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static bool locked;
#define portENTER_CRITICAL(p) do{(void)(p);CHECK(!locked);locked=true;}while(0)
#define portEXIT_CRITICAL(p) do{(void)(p);CHECK(locked);locked=false;}while(0)
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
#define pdPASS 1
static size_t free_internal=100000,largest_internal=50000;
static unsigned allocations,frees,outstanding,tasks,deletes;
static void *allocation_pointer;
static size_t allocation_size;
static bool fail_alloc,fail_task,immediate_task,release_in_alloc,storage_active;
static void (*pending_task)(void*);
static void *pending_argument;
static uint64_t last_job;
static void (*storage_hook)(void);
static cerberus_ble_archive_io_status_t status(void)
{cerberus_ble_archive_io_status_t s;cerberus_ble_archive_io_get_status(&s);return s;}
static size_t heap_caps_get_free_size(unsigned caps){CHECK(!locked&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return free_internal;}
static size_t heap_caps_get_largest_free_block(unsigned caps){CHECK(!locked&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return largest_internal;}
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{
    CHECK(!locked&&count==1&&caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));++allocations;
    CHECK(!allocation_pointer);
    void*p=fail_alloc?NULL:calloc(count,size+64U);
    if(p){++outstanding;allocation_pointer=p;allocation_size=size;memset((uint8_t*)p+size,0xCD,64U);}
    if(release_in_alloc)cerberus_ble_archive_io_release(status().job_id);
    return p;
}
static void heap_caps_free(void *p){CHECK(!locked&&!storage_active);if(p){
    CHECK(outstanding&&p==allocation_pointer);for(unsigned i=0;i<64;++i)CHECK(((uint8_t*)p)[allocation_size+i]==0xCD);
    allocation_pointer=NULL;--outstanding;++frees;}free(p);}
static int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void*arg,unsigned priority,void*handle)
{
    CHECK(!locked&&!pending_task&&!strcmp(name,"ble_archive")&&stack==6144&&priority==2&&!handle);++tasks;
    if(fail_task)return 0;
    if(immediate_task)fn(arg);else{pending_task=fn;pending_argument=arg;}return pdPASS;
}
static void vTaskDelete(void *handle){CHECK(!handle&&!locked);++deletes;}
static uint8_t storage_bytes[CERBERUS_BLE_ARCHIVE_MAX_SIZE];
static size_t storage_length;
static unsigned saves,loads;
static esp_err_t storage_error;
static bool partial_save;
static char last_path[512],last_name[256];
static esp_err_t cerberus_storage_ble_archive_save(const uint8_t*data,size_t length,char*name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length<=sizeof(storage_bytes)&&capacity==256);++saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(storage_bytes,data,length);storage_length=length;strcpy(name,"B00001.BLE");}
    else if(partial_save)strcpy(name,"B00001.TMP");
    storage_active=false;return storage_error;
}
static esp_err_t cerberus_storage_ble_archive_load(const char*path,const char*name,uint8_t*data,size_t capacity,size_t*length)
{
    CHECK(!locked&&!storage_active&&capacity==sizeof(storage_bytes));++loads;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_LOADING);
    strcpy(last_path,path);strcpy(last_name,name);if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(data,storage_bytes,storage_length);*length=storage_length;}else *length=0;
    storage_active=false;return storage_error;
}
static uint8_t json_bytes[CERBERUS_AIRTAG_JSON_MAX_BYTES];
static size_t json_length;
static unsigned json_saves,json_loads;
static esp_err_t cerberus_storage_airtag_json_save(const uint8_t*data,size_t length,char*name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length<=sizeof(json_bytes)&&capacity==256);++json_saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(json_bytes,data,length);json_length=length;strcpy(name,"A00001.LOG");}
    else if(partial_save)strcpy(name,"A00001.TMP");
    storage_active=false;return storage_error;
}
static esp_err_t cerberus_storage_airtag_json_load(const char*path,const char*name,uint8_t*data,size_t capacity,size_t*length)
{
    CHECK(!locked&&!storage_active&&capacity==sizeof(json_bytes));++json_loads;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_LOADING);
    strcpy(last_path,path);strcpy(last_name,name);if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(data,json_bytes,json_length);*length=json_length;}else *length=0;
    storage_active=false;return storage_error;
}
static uint8_t wifi_bytes[CERBERUS_WIFI_ARCHIVE_MAX_SIZE];
static size_t wifi_length;
static unsigned wifi_saves,wifi_loads;
static esp_err_t cerberus_storage_wifi_archive_save(const uint8_t*data,size_t length,char*name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length<=sizeof(wifi_bytes)&&capacity==256);++wifi_saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(wifi_bytes,data,length);wifi_length=length;strcpy(name,"W00001.WFI");}
    else if(partial_save)strcpy(name,"W00001.TMP");
    storage_active=false;return storage_error;
}
static esp_err_t cerberus_storage_wifi_archive_load(const char*path,const char*name,uint8_t*data,size_t capacity,size_t*length)
{
    CHECK(!locked&&!storage_active&&capacity==sizeof(wifi_bytes));++wifi_loads;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_LOADING);
    strcpy(last_path,path);strcpy(last_name,name);if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(data,wifi_bytes,wifi_length);*length=wifi_length;}else *length=0;
    storage_active=false;return storage_error;
}
static uint8_t ap_json_bytes[CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES];
static size_t ap_json_length;
static unsigned ap_json_saves,ap_json_loads;
static esp_err_t cerberus_storage_ap_json_save(const uint8_t *data,size_t length,char *name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length<=sizeof(ap_json_bytes)&&capacity==256);
    ++ap_json_saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(ap_json_bytes,data,length);ap_json_length=length;strcpy(name,"P00001.LOG");}
    else if(partial_save)strcpy(name,"P00001.TMP");
    storage_active=false;return storage_error;
}
static esp_err_t cerberus_storage_ap_json_load(const char *path,const char *name,uint8_t *data,size_t capacity,size_t *length)
{
    CHECK(!locked&&!storage_active&&capacity==sizeof(ap_json_bytes));++ap_json_loads;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_LOADING);
    strcpy(last_path,path);strcpy(last_name,name);if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(data,ap_json_bytes,ap_json_length);*length=ap_json_length;}else *length=0;
    storage_active=false;return storage_error;
}
static uint8_t ssid_text_bytes[CERBERUS_WIFI_MARAUDER_SSID_MAX_BYTES];
static size_t ssid_text_length;
static unsigned ssid_text_saves,ssid_text_loads;
static esp_err_t cerberus_storage_ssid_text_save(const uint8_t *data,size_t length,char *name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length<=sizeof(ssid_text_bytes)&&capacity==256);
    ++ssid_text_saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(ssid_text_bytes,data,length);ssid_text_length=length;strcpy(name,"S00001.TXT");}
    else if(partial_save)strcpy(name,"S00001.TMP");
    storage_active=false;return storage_error;
}
static esp_err_t cerberus_storage_ssid_text_load(const char *path,const char *name,uint8_t *data,size_t capacity,size_t *length)
{
    CHECK(!locked&&!storage_active&&capacity==sizeof(ssid_text_bytes));++ssid_text_loads;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_LOADING);
    strcpy(last_path,path);strcpy(last_name,name);if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(data,ssid_text_bytes,ssid_text_length);*length=ssid_text_length;}else *length=0;
    storage_active=false;return storage_error;
}
static uint8_t probe_bytes[CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES + 1U];
static size_t probe_length;
static unsigned probe_saves,probe_formats;
static void (*probe_format_hook)(void);
static esp_err_t cerberus_storage_probe_report_save(const uint8_t *data,size_t length,char *name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length>0&&length<=CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES&&capacity==256);
    ++probe_saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    CHECK(status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(probe_bytes,data,length);probe_bytes[length]=0;probe_length=length;strcpy(name,"P00001.CSV");}
    else if(partial_save)strcpy(name,"P00001.TMP");
    storage_active=false;return storage_error;
}
static bool checked_probe_report(const cerberus_probe_sniff_snapshot_t *snapshot,
    char *bytes,size_t capacity,size_t *length)
{
    CHECK(!locked);++probe_formats;
    bool okay=cerberus_probe_sniff_report(snapshot,bytes,capacity,length);
    void(*hook)(void)=probe_format_hook;probe_format_hook=NULL;if(hook)hook();
    return okay;
}
static uint8_t mesh_bytes[CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES + 1U];
static size_t mesh_length;
static unsigned mesh_saves,mesh_formats;
static void (*mesh_format_hook)(void);
static unsigned locate_formats;
static void (*locate_format_hook)(void);
static esp_err_t cerberus_storage_mesh_report_save(const uint8_t *data,size_t length,char *name,size_t capacity)
{
    CHECK(!locked&&!storage_active&&length>0&&length<=CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES&&capacity==256);
    ++mesh_saves;storage_active=true;
    CHECK(status().worker_active&&status().state==CERBERUS_BLE_ARCHIVE_IO_SAVING);
    CHECK(status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT ||
        status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT);
    if(storage_hook)storage_hook();
    if(storage_error==ESP_OK){memcpy(mesh_bytes,data,length);mesh_bytes[length]=0;mesh_length=length;strcpy(name,"M00001.CSV");}
    else if(partial_save)strcpy(name,"M00001.TMP");
    storage_active=false;return storage_error;
}
static bool checked_mesh_report_encode(const mesh_scan_t *snapshot,
    char *bytes,size_t capacity,size_t *length)
{
    CHECK(!locked);++mesh_formats;
    bool okay=mesh_report_encode(snapshot,bytes,capacity,length);
    void(*hook)(void)=mesh_format_hook;mesh_format_hook=NULL;if(hook)hook();
    return okay;
}
static bool checked_locate_report_encode(const mesh_locate_t *snapshot,
    char *bytes,size_t capacity,size_t *length)
{
    CHECK(!locked);++locate_formats;
    bool okay=mesh_locate_report_encode(snapshot,bytes,capacity,length);
    void(*hook)(void)=locate_format_hook;locate_format_hook=NULL;if(hook)hook();
    return okay;
}
#include "mesh_archive_io_boundary.h"
/* Interleave Release and nested readers at the actual large copy. No document
 * bytes may be copied while interrupts are disabled, nor freed while pinned. */
static void (*copy_hook)(void);
static unsigned document_copies;
static void *archive_checked_copy(void *out,const void *in,size_t bytes)
{
    if(bytes==sizeof(mesh_archive_t)||bytes==sizeof(cerberus_ble_archive_t)||bytes==sizeof(cerberus_airtag_json_t)||bytes==sizeof(cerberus_wifi_archive_t)||bytes==sizeof(cerberus_wifi_marauder_ap_t)||bytes==sizeof(cerberus_wifi_marauder_ssid_t)){
        CHECK(!locked);++document_copies;
        void(*hook)(void)=copy_hook;copy_hook=NULL;if(hook)hook();
    }
    return memcpy(out,in,bytes);
}
static void (*conversion_hook)(void);
static cerberus_airtag_json_result_t checked_conversion(const cerberus_ble_archive_t *doc,
    cerberus_airtag_json_t *out,cerberus_airtag_json_export_stats_t *stats)
{
    CHECK(!locked);
    cerberus_airtag_json_result_t result=cerberus_airtag_json_from_archive(doc,out,stats);
    void(*hook)(void)=conversion_hook;conversion_hook=NULL;if(hook)hook();
    return result;
}
static cerberus_wifi_marauder_result_t checked_ap_conversion(const cerberus_wifi_archive_t *doc,
    cerberus_wifi_marauder_ap_t *out,cerberus_wifi_marauder_export_stats_t *stats)
{
    CHECK(!locked);cerberus_wifi_marauder_result_t result=cerberus_wifi_marauder_ap_from_archive(doc,out,stats);
    void(*hook)(void)=conversion_hook;conversion_hook=NULL;if(hook)hook();return result;
}
static cerberus_wifi_marauder_result_t checked_ssid_conversion(const cerberus_wifi_archive_t *doc,
    cerberus_wifi_marauder_ssid_t *out,cerberus_wifi_marauder_export_stats_t *stats)
{
    CHECK(!locked);cerberus_wifi_marauder_result_t result=cerberus_wifi_marauder_ssid_from_archive(doc,out,stats);
    void(*hook)(void)=conversion_hook;conversion_hook=NULL;if(hook)hook();return result;
}
#define cerberus_wifi_marauder_ap_from_archive checked_ap_conversion
#define cerberus_wifi_marauder_ssid_from_archive checked_ssid_conversion
#define cerberus_airtag_json_from_archive checked_conversion
#define memcpy archive_checked_copy
#define cerberus_probe_sniff_report checked_probe_report
#define mesh_report_encode checked_mesh_report_encode
#define mesh_locate_report_encode checked_locate_report_encode
#define mesh_archive_encode checked_map_encode
#define mesh_archive_decode checked_map_decode
#define mesh_archive_report_encode checked_map_report
#include "archive_io_impl.h"
#undef mesh_archive_encode
#undef mesh_archive_decode
#undef mesh_archive_report_encode
#undef mesh_locate_report_encode
#undef mesh_report_encode
#undef cerberus_probe_sniff_report
#undef memcpy
#undef cerberus_airtag_json_from_archive
#undef cerberus_wifi_marauder_ap_from_archive
#undef cerberus_wifi_marauder_ssid_from_archive

static char console_text[1600];
static size_t console_length;
static void console_printf(const char *format,...)
{
    va_list args;va_start(args,format);
    int n=vsnprintf(console_text+console_length,sizeof(console_text)-console_length,format,args);
    va_end(args);CHECK(n>=0&&(size_t)n<sizeof(console_text)-console_length);console_length+=(size_t)n;
}
#include "archive_console_impl.h"
static void console_check(cerberus_ble_archive_io_state_t expected_state)
{
    cerberus_ble_archive_io_status_t before=status();CHECK(before.state==expected_state);
    unsigned saved_allocations=allocations,saved_frees=frees,saved_tasks=tasks,saved_saves=saves,saved_loads=loads;
    console_length=0;console_text[0]=0;print_ble_archive();
    cerberus_ble_archive_io_status_t after=status();CHECK(!memcmp(&before,&after,sizeof(before)));
    CHECK(allocations==saved_allocations&&frees==saved_frees&&tasks==saved_tasks&&saves==saved_saves&&loads==saved_loads);
    char line[600];
#define FIELD(format,value) do{CHECK(snprintf(line,sizeof(line),format "\r\n",value)>0);CHECK(strstr(console_text,line));}while(0)
    FIELD("ble_archive_state=%s",cerberus_ble_archive_io_state_name(expected_state));
    FIELD("ble_archive_active=%u",before.worker_active?1U:0U);
    FIELD("ble_archive_error=%" PRId32,(int32_t)before.error);
    FIELD("ble_archive_job_id=%" PRIu64,before.job_id);
    FIELD("ble_archive_operation=%u",(unsigned)before.operation);
    FIELD("ble_archive_cancel_requested=%u",before.cancel_requested?1U:0U);
    FIELD("ble_archive_result_ready=%u",before.result_ready?1U:0U);
    FIELD("ble_archive_bytes=%" PRIu32,before.bytes);
    FIELD("ble_archive_rows=%u",(unsigned)before.count);
    FIELD("ble_archive_path=%s",before.relative_path);
    FIELD("ble_archive_file=%s",before.file_name);
#undef FIELD
}

static cerberus_ble_archive_t input,expected,output,untouched,nested_output;
static void fill(unsigned count)
{
    memset(&input,0,sizeof(input));input.saved_uptime_ms=100000;input.count=(uint8_t)count;
    for(unsigned i=0;i<count;++i){
        cerberus_ble_archive_row_t *r=&input.rows[i];r->address[0]=(uint8_t)(i%32);r->address[5]=0xA1;r->address_type=i%4;
        snprintf(r->name,sizeof(r->name),"test-%u",i);r->rssi=-42;r->rssi_valid=true;
        r->advertisement_length=31;for(unsigned j=0;j<31;++j)r->advertisement[j]=(uint8_t)(i+j);
        r->copy_ceiling_ms=99999;r->source_time_valid=true;r->original_source_uptime_ms=90000;
        r->receiver=CERBERUS_BLE_ARCHIVE_RECEIVER_S3;r->index=(uint8_t)(i%32);
        if(i<32&&!(i%2)){
            r->scan_epoch_valid=true;r->scan_epoch=55;
            memset(r->advertisement,0,sizeof(r->advertisement));r->advertisement_length=3;
            r->advertisement[0]=2;r->advertisement[1]=1;r->advertisement[2]=6;
            r->response_state=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
            r->response_length=r->response_original_length=5;r->response_event_type=4;
            memcpy(r->response,"\004\011RSP",5);
            r->response_original_sequence=1;r->response_sequence=2;
            r->response_original_observed_ms=90000;r->response_observed_ms=90010;
            r->response_time_valid=true;r->response_source_uptime_ms=90010;
        }
        if(i>=32){
            r->receiver=CERBERUS_BLE_ARCHIVE_RECEIVER_C5;r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
            r->source_time_valid=false;r->copy_ceiling_ms=r->original_source_uptime_ms=0;
            r->receiver_boot_id=0xC501;r->scan_epoch_valid=true;r->scan_epoch=55;r->generation=7;
            r->age_upper_valid=true;r->age_upper_at_save_ms=123+i;
            if(i%3==1){
                r->receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,
                    .callback_length=31,.sid=0xff,.data_status=0xff,.ad_validation=CERBERUS_DUAL_BLE_AD_INVALID};
                r->advertisement_flags=2;
            } else if(i%3==2||i==63){
                r->advertisement_event_type=3;r->advertisement_flags=3;
                CHECK(archive_test_family_original(r->advertisement,&r->receive,&r->family,
                    r->address,r->address_type,(i%2)?'5':'6'));
            }
            CHECK(archive_test_response_row(r,i==63?6:(uint8_t)((i-32)%6+1),
                (i%2)?6:0));
        }
    }
    CHECK(cerberus_ble_archive_validate(&input));expected=input;
}
static void reset(void)
{
    CHECK(!locked&&!pending_task&&!storage_active&&!status().worker_active&&!archive_io_readers);
    cerberus_ble_archive_io_release(status().job_id);CHECK(!outstanding&&!archive_io_context);
    free_internal=100000;largest_internal=50000;fail_alloc=fail_task=immediate_task=release_in_alloc=false;
    storage_hook=copy_hook=conversion_hook=NULL;storage_error=ESP_OK;partial_save=false;saves=loads=json_saves=json_loads=wifi_saves=wifi_loads=ap_json_saves=ap_json_loads=ssid_text_saves=ssid_text_loads=0;last_path[0]=last_name[0]=0;
    probe_saves=probe_formats=0;probe_length=0;probe_format_hook=NULL;
    mesh_saves=mesh_formats=0;mesh_length=0;mesh_format_hook=NULL;
    locate_formats=0;locate_format_hook=NULL;
    map_saves=map_loads=map_encodes=map_decodes=map_exports=0;map_codec_hook=NULL;
    memset(&output,0xA5,sizeof(output));untouched=output;
}
static void run_worker(void)
{
    CHECK(pending_task);void(*fn)(void*)=pending_task;void*arg=pending_argument;pending_task=NULL;pending_argument=NULL;
    unsigned before=deletes;fn(arg);CHECK(deletes==before+1&&!status().worker_active&&!storage_active&&!locked);
}
static uint64_t start_save(void)
{uint64_t id;CHECK(cerberus_ble_archive_io_save(&input,&id)==ESP_OK&&id>last_job);last_job=id;return id;}
static uint64_t start_load(void)
{uint64_t id;CHECK(cerberus_ble_archive_io_load("/COPIED","saved.ble",&id)==ESP_OK&&id>last_job);last_job=id;return id;}
static void release_hook(void)
{
    console_check(archive_io_loading(status().operation)?CERBERUS_BLE_ARCHIVE_IO_LOADING:CERBERUS_BLE_ARCHIVE_IO_SAVING);
    uint64_t id=status().job_id;unsigned before=frees;cerberus_ble_archive_io_release(id);CHECK(frees==before&&outstanding==1);
    uint64_t refused=99;CHECK(cerberus_ble_archive_io_load("/","x.BLE",&refused)==ESP_ERR_INVALID_STATE&&refused==0);
    CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_ERR_INVALID_STATE&&!memcmp(&output,&untouched,sizeof(output)));
}
static void cancel_hook(void){cerberus_ble_archive_io_cancel(status().job_id);CHECK(status().cancel_requested&&outstanding==1);}
static void release_during_copy(void)
{
    CHECK(!locked&&archive_io_readers>0&&outstanding==1);
    unsigned before=frees;uint64_t id=status().job_id;
    cerberus_ble_archive_io_release(id);
    CHECK(frees==before&&outstanding==1&&archive_io_context&&!status().result_ready);
    uint64_t refused=99;
    CHECK(cerberus_ble_archive_io_save(&input,&refused)==ESP_ERR_INVALID_STATE&&refused==0);
    CHECK(cerberus_ble_archive_io_copy_result(id,&untouched)==ESP_ERR_INVALID_STATE);
}
static void nested_copy_then_release(void)
{
    CHECK(archive_io_readers==1&&outstanding==1);
    copy_hook=release_during_copy;
    CHECK(cerberus_ble_archive_io_copy_result(status().job_id,&nested_output)==ESP_OK);
    CHECK(!memcmp(&nested_output,&expected,sizeof(expected))&&archive_io_readers==1&&outstanding==1);
}

static void check_old_file_loads(void)
{
    const char *vectors[]={archive_io_hex,archive_io_v2_hex,archive_io_v3_hex,archive_io_v4_hex,archive_io_v5_golden};
    const size_t lengths[]={160,176,240,248,296};
    for(unsigned version=0;version<5;++version){
        reset();const char *hex=vectors[version];storage_length=strlen(hex)/2;
        CHECK(storage_length==lengths[version]);
        for(size_t i=0;i<storage_length;++i){unsigned byte=0;
            CHECK(sscanf(hex+2*i,"%2x",&byte)==1);storage_bytes[i]=(uint8_t)byte;}
        uint64_t id=start_load();run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&status().result_ready);
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_OK&&output.count==1);
        CHECK(output.rows[0].receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY&&output.rows[0].address_type==2);
        CHECK(output.rows[0].scan_epoch==UINT64_C(0x0102030405060708)&&
            output.rows[0].original_source_uptime_ms==UINT64_C(0x100000001));
        const cerberus_dual_ble_family_wire_t absent={0};
        const cerberus_dual_ble_response_wire_t no_response={0};
        CHECK(!memcmp(&output.rows[0].family,&absent,sizeof(absent)));
        CHECK(!memcmp(&output.rows[0].c5_response,&no_response,sizeof(no_response))&&
            !output.rows[0].c5_response_age_upper_valid&&!output.rows[0].c5_response_age_upper_at_save_ms);
        CHECK(output.rows[0].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN&&!output.rows[0].receive.source_kind);
        cerberus_ble_archive_io_release(id);CHECK(!outstanding&&!archive_io_context);
    }
}


static cerberus_airtag_json_t json_input,json_expected,json_output,json_untouched,json_nested;
static void fill_json(unsigned count,unsigned total)
{
    memset(&json_input,0,sizeof(json_input));json_input.count=(uint8_t)count;json_input.payload_used=(uint16_t)total;
    unsigned offset=0;
    for(unsigned i=0;i<count;++i){
        unsigned length=(total-offset)/(count-i);
        json_input.records[i]=(cerberus_airtag_json_record_t){.address={1,2,3,4,5,6},.offset=(uint16_t)offset,.length=(uint16_t)length};
        offset+=length;
    }
    for(unsigned i=0;i<total;++i)json_input.payload[i]=(uint8_t)i;
    CHECK(cerberus_airtag_json_validate(&json_input)==CERBERUS_AIRTAG_JSON_OK);json_expected=json_input;
    memset(&json_output,0xA5,sizeof(json_output));json_untouched=json_output;
}
static void export_source(unsigned count,bool response)
{
    memset(&input,0,sizeof(input));input.count=(uint8_t)count;input.saved_uptime_ms=100;
    for(unsigned i=0;i<count;++i){
        cerberus_ble_archive_row_t*r=&input.rows[i];r->address[0]=(uint8_t)(i%32+1);r->address[5]=0xA1;
        r->index=(uint8_t)(i%32);r->rssi=CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE;r->scan_epoch_valid=true;r->scan_epoch=UINT64_C(0x100000005);
        r->receiver=i<32?CERBERUS_BLE_ARCHIVE_RECEIVER_S3:CERBERUS_BLE_ARCHIVE_RECEIVER_C5;
        r->advertisement_length=4;memcpy(r->advertisement,(uint8_t[]){3,3,0x44,0xfd},4);
        if(i>=32){r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;r->receiver_boot_id=0xC501;
            r->receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=1,.callback_length=4,.sid=255,.data_status=255,.ad_validation=1};}
        else if(response){r->response_state=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
            r->response_length=r->response_original_length=4;r->response_event_type=4;
            memcpy(r->response,r->advertisement,4);r->response_original_sequence=1;r->response_sequence=2;
            r->response_original_observed_ms=10;r->response_observed_ms=11;}
    }
    CHECK(cerberus_ble_archive_validate(&input));expected=input;
}
static uint64_t start_json(unsigned operation)
{
    uint64_t id=0;esp_err_t err;
    if(operation==0)err=cerberus_ble_archive_io_load_airtag_json("/", "Airtags_0.log",&id);
    else if(operation==1)err=cerberus_ble_archive_io_save_airtag_json(&json_input,&id);
    else err=cerberus_ble_archive_io_export_airtag_json(&input,&id);
    CHECK(err==ESP_OK&&id>last_job);last_job=id;
    CHECK(status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON);
    CHECK(allocation_size==archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON));
    return id;
}
static void json_copy_release(void)
{
    CHECK(archive_io_readers>0&&!locked&&outstanding==1);
    cerberus_ble_archive_io_release(status().job_id);CHECK(outstanding==1);
    CHECK(cerberus_ble_archive_io_copy_airtag_json(status().job_id,&json_untouched)==ESP_ERR_INVALID_STATE);
}
static void json_nested_release(void)
{
    CHECK(archive_io_readers==1);copy_hook=json_copy_release;
    CHECK(cerberus_ble_archive_io_copy_airtag_json(status().job_id,&json_nested)==ESP_OK);
    CHECK(!memcmp(&json_nested,&json_expected,sizeof(json_expected))&&archive_io_readers==1&&outstanding==1);
}
static void check_json_operations(void)
{
    /* Exact selected-arm allocations, trailing canaries and cross-format copy
     * rejection protect the discriminated context, including the smaller BLE arm. */
    reset();fill(1);uint64_t id=start_save();
    CHECK(allocation_size==768U+sizeof(cerberus_ble_archive_t)+CERBERUS_BLE_ARCHIVE_MAX_SIZE);
    run_worker();fill_json(1,1);
    CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_ERR_INVALID_STATE&&!memcmp(&json_output,&json_untouched,sizeof(json_output)));
    for(unsigned n=0;n<3;++n){
        reset();fill_json(n==0?0:n==1?2:64,n==0?0:n==1?3:8192);
        id=start_json(1);memset(&json_input,0xCC,sizeof(json_input));run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&json_saves==1&&!saves&&!loads&&!json_loads);
        CHECK(status().count==json_expected.count&&!strcmp(status().file_name,"A00001.LOG"));
        CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_OK&&!memcmp(&json_output,&json_expected,sizeof(json_output)));
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_ERR_INVALID_STATE&&!memcmp(&output,&untouched,sizeof(output)));
        CHECK(cerberus_ble_archive_io_copy_airtag_json(id+1,&json_output)==ESP_ERR_INVALID_STATE);
        cerberus_ble_archive_io_release(id);id=start_json(0);run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&json_loads==1&&status().count==json_expected.count);
        CHECK(!strcmp(last_path,"/")&&!strcmp(last_name,"Airtags_0.log"));
        CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_OK&&!memcmp(&json_output,&json_expected,sizeof(json_output)));
        if(n==2)CHECK(json_length>40000U&&json_length<=49152U);
    }
    for(unsigned nested=0;nested<2;++nested){
        reset();fill_json(64,8192);id=start_json(1);run_worker();unsigned before=frees;
        copy_hook=nested?json_nested_release:json_copy_release;
        CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_OK);
        CHECK(!memcmp(&json_output,&json_expected,sizeof(json_output))&&!outstanding&&!archive_io_context&&frees==before+1);
    }
    reset();fill_json(1,1);id=start_json(1);run_worker();archive_io_readers=UINT16_MAX;
    CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_ERR_INVALID_STATE&&!memcmp(&json_output,&json_untouched,sizeof(json_output)));
    archive_io_readers=0;
    for(unsigned count=1;count<=64;count+=63){
        reset();export_source(count,false);id=start_json(2);memset(&input,0xCC,sizeof(input));
        CHECK(!json_saves);run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&json_saves==1);
        CHECK(status().export_stats.eligible==count&&!status().export_stats.incomplete&&!status().export_stats.filtered);
        CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_OK&&json_output.count==count);
        for(unsigned i=0;i<count;++i){CHECK(!memcmp(json_output.records[i].address,expected.rows[i].address,6));
            CHECK(json_output.records[i].length==4&&!memcmp(json_output.payload+json_output.records[i].offset,expected.rows[i].advertisement,4));}
    }
    reset();export_source(1,true);id=start_json(2);run_worker();CHECK(status().export_stats.eligible==2&&status().count==2);
    CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_OK);
    CHECK(!memcmp(json_output.records[0].address,json_output.records[1].address,6)&&json_output.records[1].offset==4);
    reset();export_source(33,true);id=start_json(2);run_worker();
    CHECK(status().error==ESP_ERR_INVALID_SIZE&&status().export_stats.eligible==65&&!json_saves&&!outstanding);
    reset();export_source(2,false);memcpy(input.rows[0].advertisement,(uint8_t[]){3,3,0x12,0x18},4);
    input.rows[1].advertisement_flags=1;id=start_json(2);run_worker();
    CHECK(status().error==ESP_ERR_NOT_FOUND&&!status().export_stats.eligible&&status().export_stats.filtered==1&&status().export_stats.incomplete==1&&!json_saves&&!outstanding);
    reset();export_source(1,false);input.rows[0].address_type=99;id=start_json(2);run_worker();
    CHECK(status().error==ESP_ERR_INVALID_RESPONSE&&!json_saves&&!outstanding);
    /* Cancel before dispatch performs no FS; while inside FS, SAVE/EXPORT
     * reports actual publication while LOAD never exposes a canceled result. */
    for(unsigned operation=0;operation<3;++operation)for(unsigned release=0;release<2;++release){
        for(unsigned during=0;during<2;++during){
            reset();fill_json(2,3);export_source(1,true);
            CHECK(cerberus_airtag_json_encode(&json_input,json_bytes,sizeof(json_bytes),&json_length)==CERBERUS_AIRTAG_JSON_OK);
            if(during)storage_hook=release?release_hook:cancel_hook;
            id=start_json(operation);
            if(!during){if(release)cerberus_ble_archive_io_release(id);else cerberus_ble_archive_io_cancel(id);}
            run_worker();CHECK(status().cancel_requested);
            CHECK(status().state==((during&&operation)?CERBERUS_BLE_ARCHIVE_IO_COMPLETE:CERBERUS_BLE_ARCHIVE_IO_CANCELED));
            CHECK(status().result_ready==(during&&operation&&!release));
            CHECK(json_saves+json_loads==during&&!saves&&!loads);
            cerberus_ble_archive_io_release(id);CHECK(!outstanding);
        }
    }
    /* Conversion is real; cancellation injected at its return must prevent
     * storage of the staging result and free exactly once. */
    for(unsigned release=0;release<2;++release){
        reset();export_source(1,true);conversion_hook=release?release_hook:cancel_hook;
        id=start_json(2);run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_CANCELED&&status().export_stats.eligible==2);
        CHECK(!json_saves&&!json_loads&&!outstanding&&!status().result_ready);
    }
    for(unsigned operation=0;operation<3;++operation)for(unsigned kind=0;kind<5;++kind){
        reset();fill_json(1,1);export_source(1,false);
        if(kind==0)free_internal=24575;if(kind==1)largest_internal=8191;if(kind==2)fail_alloc=true;if(kind==3)fail_task=true;
        if(kind==4){immediate_task=true;release_in_alloc=true;}
        esp_err_t err=operation==0?cerberus_ble_archive_io_load_airtag_json("/","x.LOG",&id):operation==1?
            cerberus_ble_archive_io_save_airtag_json(&json_input,&id):cerberus_ble_archive_io_export_airtag_json(&input,&id);
        CHECK(id>last_job);last_job=id;CHECK(err==(kind==4?ESP_OK:ESP_ERR_NO_MEM));
        CHECK(!outstanding&&!pending_task&&!json_saves&&!json_loads&&!status().worker_active);
    }
    reset();fill_json(1,1);json_input.records[0].offset=1;id=start_json(1);run_worker();CHECK(status().error==ESP_ERR_INVALID_RESPONSE&&!json_saves&&!outstanding);
    reset();fill_json(1,1);json_input.count=65;id=start_json(1);run_worker();CHECK(status().error==ESP_ERR_INVALID_SIZE&&!json_saves&&!outstanding);
    const char *bad[]={"", "[", "[{}]", "[{\"mac\":\"FF\"}]"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){reset();json_length=strlen(bad[i]);memcpy(json_bytes,bad[i],json_length);
        id=start_json(0);run_worker();CHECK(status().error==ESP_ERR_INVALID_RESPONSE&&!outstanding&&!status().result_ready);}
    reset();fill_json(1,1);storage_error=ESP_FAIL;partial_save=true;id=start_json(1);run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!strcmp(status().file_name,"A00001.TMP")&&!outstanding);
    reset();storage_error=ESP_ERR_TIMEOUT;id=start_json(0);run_worker();CHECK(status().error==ESP_ERR_TIMEOUT&&!outstanding);
    reset();fill_json(0,0);json_length=2;memcpy(json_bytes,"[]",2);char path[20]="/COPY",name[20]="before.json";
    CHECK(cerberus_ble_archive_io_load_airtag_json(path,name,&id)==ESP_OK);path[1]='!';name[0]='!';run_worker();
    CHECK(!strcmp(last_path,"/COPY")&&!strcmp(last_name,"before.json"));
    uint64_t stale=id;cerberus_ble_archive_io_release(id);id=start_json(1);cerberus_ble_archive_io_cancel(stale);cerberus_ble_archive_io_release(stale);
    CHECK(!status().cancel_requested&&outstanding==1);run_worker();CHECK(status().result_ready);reset();
    CHECK(cerberus_ble_archive_io_save_airtag_json(NULL,&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_export_airtag_json(NULL,&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_copy_airtag_json(0,NULL)==ESP_ERR_INVALID_ARG);
    char badpath[512];memset(badpath,'X',sizeof(badpath));
    CHECK(cerberus_ble_archive_io_load_airtag_json(badpath,"x.LOG",&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_save_airtag_json(&json_input,NULL)==ESP_ERR_INVALID_ARG);
}

static cerberus_wifi_archive_t wifi_input,wifi_expected,wifi_output,wifi_untouched,wifi_nested;
static void fill_wifi(unsigned count)
{
    archive_test_wifi_document(&wifi_input,count);
    CHECK(cerberus_wifi_archive_validate(&wifi_input));wifi_expected=wifi_input;
    memset(&wifi_output,0xA5,sizeof(wifi_output));wifi_untouched=wifi_output;
}
static uint64_t start_wifi(bool save)
{
    uint64_t id=0;
    esp_err_t e=save?cerberus_ble_archive_io_save_wifi(&wifi_input,&id):
        cerberus_ble_archive_io_load_wifi("/COPIED","saved.wfi",&id);
    CHECK(e==ESP_OK&&id>last_job);last_job=id;
    CHECK(status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI);
    CHECK(allocation_size==offsetof(ble_archive_io_context_t,payload)+sizeof(archive_io_context->payload.wifi));
    CHECK(allocation_size<archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE));
    return id;
}
static void seed_wifi_bytes(void)
{CHECK(cerberus_wifi_archive_encode(&wifi_input,wifi_bytes,sizeof(wifi_bytes),&wifi_length));}
static void nested_wifi_then_release(void)
{
    CHECK(archive_io_readers==1&&outstanding==1);
    copy_hook=release_during_copy;
    CHECK(cerberus_ble_archive_io_copy_wifi(status().job_id,&wifi_nested)==ESP_OK);
    CHECK(!memcmp(&wifi_nested,&wifi_expected,sizeof(wifi_expected))&&archive_io_readers==1&&outstanding==1);
}
static void wifi_failed_result(uint64_t id)
{
    CHECK(!status().result_ready&&!outstanding);
    CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE);
    CHECK(!memcmp(&wifi_output,&wifi_untouched,sizeof(wifi_output)));
}
static void check_wifi_operations(void)
{
    /* These fixed host allocations were measured before adding the Wi-Fi arm.
     * The tagged union must never make existing jobs allocate its largest arm. */
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE)==57648);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON)==58760);
    CHECK(!archive_io_json(CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI)&&!archive_io_json(CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI));
    for(unsigned count=0;count<=64;++count){
        reset();fill_wifi(count);uint64_t id=start_wifi(true);
        CHECK(!saves&&!loads&&!json_saves&&!json_loads&&!wifi_saves&&!wifi_loads);
        CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE);
        CHECK(!memcmp(&wifi_output,&wifi_untouched,sizeof(wifi_output)));
        /* Input may change immediately after tap: publication and every row
         * came from the owned immutable copy, including source age evidence. */
        memset(&wifi_input,0xCC,sizeof(wifi_input));run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&status().count==count&&status().result_ready);
        CHECK(wifi_saves==1&&!wifi_loads&&!saves&&!loads&&!json_saves&&!json_loads);
        CHECK(status().bytes==96+80*count&&!strcmp(status().relative_path,"/CERBERUS/WIFI")&&!strcmp(status().file_name,"W00001.WFI"));
        CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_OK&&!memcmp(&wifi_output,&wifi_expected,sizeof(wifi_output)));
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_ERR_INVALID_STATE&&!memcmp(&output,&untouched,sizeof(output)));
        memset(&json_output,0xA5,sizeof(json_output));json_untouched=json_output;
        CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_ERR_INVALID_STATE&&!memcmp(&json_output,&json_untouched,sizeof(json_output)));
        cerberus_ble_archive_io_release(id);CHECK(!outstanding);
        id=start_wifi(false);run_worker();
        CHECK(wifi_loads==1&&wifi_saves==1&&status().count==count&&status().result_ready);
        CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_OK&&!memcmp(&wifi_output,&wifi_expected,sizeof(wifi_output)));
        CHECK(!strcmp(last_path,"/COPIED")&&!strcmp(last_name,"saved.wfi"));
    }
    reset();fill_wifi(64);
    CHECK(cerberus_wifi_archive_subset(&wifi_input,UINT64_C(1)|(UINT64_C(1)<<63),&wifi_expected));
    wifi_input=wifi_expected;uint64_t id=start_wifi(true);run_worker();
    CHECK(status().count==2&&status().bytes==256);
    CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_OK&&!memcmp(&wifi_output,&wifi_expected,sizeof(wifi_output)));
    CHECK(wifi_output.subset&&wifi_output.publication.s3_count==32&&wifi_output.publication.c5_count==32);
    for(unsigned nested=0;nested<2;++nested){
        reset();fill_wifi(64);id=start_wifi(true);run_worker();
        unsigned before=frees,copies=document_copies;copy_hook=nested?nested_wifi_then_release:release_during_copy;
        CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_OK&&!memcmp(&wifi_output,&wifi_expected,sizeof(wifi_output)));
        CHECK(!archive_io_readers&&!archive_io_context&&!outstanding&&frees==before+1&&document_copies==copies+1+nested);
    }
    reset();fill_wifi(64);id=start_wifi(true);run_worker();archive_io_readers=UINT16_MAX;
    CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE&&!memcmp(&wifi_output,&wifi_untouched,sizeof(wifi_output)));
    archive_io_readers=0;
    for(unsigned save=0;save<2;++save){
        for(unsigned release=0;release<2;++release){
            reset();fill_wifi(64);seed_wifi_bytes();id=start_wifi(save!=0);
            if(release)cerberus_ble_archive_io_release(id);else cerberus_ble_archive_io_cancel(id);
            CHECK(outstanding==1&&status().worker_active);run_worker();
            CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_CANCELED&&!wifi_saves&&!wifi_loads);wifi_failed_result(id);
        }
        for(unsigned release=0;release<2;++release){
            reset();fill_wifi(64);seed_wifi_bytes();storage_hook=release?release_hook:cancel_hook;id=start_wifi(save!=0);run_worker();
            CHECK(status().cancel_requested&&status().state==(save?CERBERUS_BLE_ARCHIVE_IO_COMPLETE:CERBERUS_BLE_ARCHIVE_IO_CANCELED));
            CHECK(status().result_ready==(save&&!release));
            if(save&&!release)CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_OK&&!memcmp(&wifi_output,&wifi_expected,sizeof(wifi_output)));
            CHECK((save?wifi_saves:wifi_loads)==1);cerberus_ble_archive_io_release(id);CHECK(!outstanding);
        }
        for(unsigned kind=0;kind<5;++kind){
            reset();fill_wifi(64);seed_wifi_bytes();
            if(kind==0)free_internal=24575;if(kind==1)largest_internal=8191;if(kind==2)fail_alloc=true;if(kind==3)fail_task=true;
            if(kind==4){release_in_alloc=true;immediate_task=true;}
            esp_err_t e=save?cerberus_ble_archive_io_save_wifi(&wifi_input,&id):cerberus_ble_archive_io_load_wifi("/","x.WFI",&id);
            CHECK(id>last_job);last_job=id;
            CHECK(e==(kind==4?ESP_OK:ESP_ERR_NO_MEM));
            CHECK(status().state==(kind==4?CERBERUS_BLE_ARCHIVE_IO_CANCELED:CERBERUS_BLE_ARCHIVE_IO_ERROR));
            CHECK(!pending_task&&!wifi_saves&&!wifi_loads&&!outstanding);
        }
    }
    for(unsigned failure=0;failure<6;++failure){
        reset();fill_wifi(64);seed_wifi_bytes();
        if(failure==0){wifi_input.rows[63].source_index=0;id=start_wifi(true);}
        else if(failure==1){storage_error=ESP_FAIL;partial_save=true;storage_hook=cancel_hook;id=start_wifi(true);}
        else {if(failure==2)storage_error=ESP_ERR_TIMEOUT;
            if(failure==3)wifi_bytes[sizeof(wifi_bytes)-1]^=1;
            if(failure==4)memset(wifi_bytes,0,wifi_length);
            if(failure==5){fill(1);CHECK(cerberus_ble_archive_encode(&input,wifi_bytes,sizeof(wifi_bytes),&wifi_length));}
            id=start_wifi(false);}
        run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR);wifi_failed_result(id);
        if(failure==0)CHECK(!wifi_saves);
        if(failure==1)CHECK(!strcmp(status().file_name,"W00001.TMP")&&status().error==ESP_FAIL);
    }
    const size_t short_files[]={0,95,96,175,5215};
    for(unsigned n=0;n<sizeof(short_files)/sizeof(short_files[0]);++n){
        reset();fill_wifi(64);seed_wifi_bytes();wifi_length=short_files[n];id=start_wifi(false);run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&status().error==ESP_ERR_INVALID_RESPONSE);wifi_failed_result(id);
    }
    reset();fill_wifi(64);seed_wifi_bytes();char path[512]="/COPY",name[256]="before.WFI";
    CHECK(cerberus_ble_archive_io_load_wifi(path,name,&id)==ESP_OK);path[1]='!';name[0]='!';run_worker();
    CHECK(!strcmp(last_path,"/COPY")&&!strcmp(last_name,"before.WFI"));
    uint64_t stale=id;cerberus_ble_archive_io_release(id);id=start_wifi(true);
    cerberus_ble_archive_io_cancel(stale);cerberus_ble_archive_io_release(stale);
    CHECK(!status().cancel_requested&&outstanding==1);run_worker();CHECK(status().result_ready);
    CHECK(cerberus_ble_archive_io_copy_wifi(stale,&wifi_output)==ESP_ERR_INVALID_STATE);
    reset();fill(1);id=start_save();run_worker();
    CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE&&!memcmp(&wifi_output,&wifi_untouched,sizeof(wifi_output)));
    reset();fill_json(1,1);id=start_json(1);run_worker();
    CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE&&!memcmp(&wifi_output,&wifi_untouched,sizeof(wifi_output)));
    reset();fill_wifi(1);immediate_task=true;id=start_wifi(true);CHECK(status().result_ready&&!pending_task);reset();
    char invalid_path[512],invalid_name[256];memset(invalid_path,'X',sizeof(invalid_path));memset(invalid_name,'X',sizeof(invalid_name));
    CHECK(cerberus_ble_archive_io_load_wifi(invalid_path,"x.WFI",&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_load_wifi("/",invalid_name,&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_load_wifi(NULL,"x.WFI",&id)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ble_archive_io_load_wifi("/",NULL,&id)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ble_archive_io_save_wifi(NULL,&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_save_wifi(&wifi_input,NULL)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ble_archive_io_copy_wifi(0,NULL)==ESP_ERR_INVALID_ARG);
    CHECK(archive_io_start((cerberus_ble_archive_io_operation_t)99,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(!archive_io_allocation_size((cerberus_ble_archive_io_operation_t)99)&&!outstanding&&!pending_task);
}

#include "wifi_interchange_io_check.h"
#include "probe_report_io_check.h"
#include "mesh_report_io_check.h"
#include "mesh_locate_report_io_check.h"
#include "mesh_archive_io_check.h"

int main(void)
{
    CHECK(CERBERUS_STORAGE_BLE_ARCHIVE_MAX_BYTES==27680U&&CERBERUS_BLE_ARCHIVE_MAX_SIZE==27680U);
    console_check(CERBERUS_BLE_ARCHIVE_IO_IDLE);
    CHECK(!strcmp(console_text,"ble_archive_state=IDLE\r\nble_archive_active=0\r\nble_archive_error=0\r\n"
        "ble_archive_job_id=0\r\nble_archive_operation=0\r\nble_archive_cancel_requested=0\r\n"
        "ble_archive_result_ready=0\r\nble_archive_bytes=0\r\nble_archive_rows=0\r\nble_archive_format=BLE\r\n"
        "ble_archive_path=\r\nble_archive_file=\r\n"));
    check_old_file_loads();
    for(unsigned count=1;count<=64;++count){
        reset();fill(count);uint64_t id=start_save();CHECK(!saves&&!loads&&status().worker_active&&outstanding==1);
        console_check(CERBERUS_BLE_ARCHIVE_IO_STARTING);
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_ERR_INVALID_STATE&&!memcmp(&output,&untouched,sizeof(output)));
        input.rows[0].name[0]='!';
        for(unsigned i=32;i<count;++i){
            memset(&input.rows[i].receive,0,sizeof(input.rows[i].receive));
            memset(&input.rows[i].family,0,sizeof(input.rows[i].family));
            memset(&input.rows[i].c5_response,0,sizeof(input.rows[i].c5_response));
            input.rows[i].c5_response_age_upper_at_save_ms=UINT64_MAX;
        }
        uint64_t unused;CHECK(cerberus_ble_archive_io_save(&input,&unused)==ESP_ERR_INVALID_STATE);
        run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&status().result_ready&&status().count==count);
        console_check(CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
        CHECK(status().bytes==32+432*count&&!strcmp(status().relative_path,"/CERBERUS/BLE")&&!strcmp(status().file_name,"B00001.BLE"));
        if(count==64){
            CHECK(storage_length==27680U&&storage_bytes[32+63*432+216]==1);
            CHECK(storage_bytes[32+63*432+264]==1&&storage_bytes[32+63*432+265]==CERBERUS_DUAL_BLE_RESPONSE_PRESENT);
        }
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_OK&&!memcmp(&output,&expected,sizeof(output)));
        if(count==64)CHECK(output.rows[63].c5_response.family.hc_name==CERBERUS_BLE_HC_05&&
            output.rows[63].c5_response.receive.callback_length==255&&
            output.rows[63].age_upper_at_save_ms==525&&output.rows[63].c5_response_age_upper_at_save_ms==523);
        cerberus_ble_archive_io_release(id);CHECK(!outstanding&&!status().result_ready);
        console_check(CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
        memset(&output,0xA5,sizeof(output));untouched=output;CHECK(cerberus_ble_archive_io_copy_result(id,&output)!=ESP_OK&&!memcmp(&output,&untouched,sizeof(output)));
        id=start_load();run_worker();CHECK(loads==1&&saves==1&&status().result_ready&&status().count==count);
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_OK&&!memcmp(&output,&expected,sizeof(output)));
        CHECK(!strcmp(last_path,"/COPIED")&&!strcmp(last_name,"saved.ble"));
    }
    for(unsigned nested=0;nested<2;++nested){
        reset();fill(64);uint64_t copy_id=start_save();run_worker();
        unsigned before=frees,copies=document_copies;
        copy_hook=nested?nested_copy_then_release:release_during_copy;
        CHECK(cerberus_ble_archive_io_copy_result(copy_id,&output)==ESP_OK);
        CHECK(!memcmp(&output,&expected,sizeof(output))&&!archive_io_readers&&!archive_io_context&&!outstanding);
        CHECK(frees==before+1&&document_copies==copies+1+nested);
        cerberus_ble_archive_io_release(copy_id);CHECK(frees==before+1);
    }
    reset();fill(64);uint64_t saturated_id=start_save();run_worker();archive_io_readers=UINT16_MAX;
    CHECK(cerberus_ble_archive_io_copy_result(saturated_id,&output)==ESP_ERR_INVALID_STATE&&!memcmp(&output,&untouched,sizeof(output)));
    archive_io_readers=0;cerberus_ble_archive_io_release(saturated_id);
    reset();fill(1);input.count=0;uint64_t id=start_save();run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!saves&&!outstanding);
    for(unsigned kind=0;kind<5;++kind){
        reset();fill(2);if(kind==0)free_internal=24575;if(kind==1)largest_internal=8191;if(kind==2)fail_alloc=true;if(kind==3)fail_task=true;
        if(kind==4){immediate_task=true;release_in_alloc=true;}
        esp_err_t e=cerberus_ble_archive_io_save(&input,&id);CHECK(id>last_job);last_job=id;
        if(kind==4)CHECK(e==ESP_OK&&status().state==CERBERUS_BLE_ARCHIVE_IO_CANCELED);
        else CHECK(e==ESP_ERR_NO_MEM&&status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR);
        CHECK(!saves&&!loads&&!outstanding&&!pending_task&&!status().worker_active);
    }
    for(unsigned operation=0;operation<2;++operation){
        for(unsigned release=0;release<2;++release){
            reset();fill(64);CHECK(cerberus_ble_archive_encode(&input,storage_bytes,sizeof(storage_bytes),&storage_length));
            id=operation?start_load():start_save();if(release)cerberus_ble_archive_io_release(id);else cerberus_ble_archive_io_cancel(id);
            CHECK(outstanding==1&&status().worker_active);run_worker();
            CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_CANCELED&&!saves&&!loads&&!outstanding&&!status().result_ready);
        }
        for(unsigned release=0;release<2;++release){
            reset();fill(64);CHECK(cerberus_ble_archive_encode(&input,storage_bytes,sizeof(storage_bytes),&storage_length));
            storage_hook=release?release_hook:cancel_hook;id=operation?start_load():start_save();run_worker();
            CHECK(status().cancel_requested&&status().state==(operation?CERBERUS_BLE_ARCHIVE_IO_CANCELED:CERBERUS_BLE_ARCHIVE_IO_COMPLETE));
            CHECK(status().result_ready==(!operation&&!release));
            if(!operation&&!release){
                CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_OK&&!memcmp(&output,&expected,sizeof(output)));
            }
            if(!operation){CHECK(saves==1&&!strcmp(status().file_name,"B00001.BLE"));}
            cerberus_ble_archive_io_release(id);CHECK(!outstanding);
        }
    }
    reset();fill(64);storage_error=ESP_FAIL;partial_save=true;id=start_save();run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!strcmp(status().file_name,"B00001.TMP")&&!outstanding);
    console_check(CERBERUS_BLE_ARCHIVE_IO_ERROR);
    CHECK(cerberus_ble_archive_io_copy_result(id,&output)!=ESP_OK&&!memcmp(&output,&untouched,sizeof(output)));
    reset();fill(64);CHECK(cerberus_ble_archive_encode(&input,storage_bytes,sizeof(storage_bytes),&storage_length));storage_bytes[27679]^=1;
    id=start_load();run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&status().error==ESP_ERR_INVALID_RESPONSE&&!outstanding);
    CHECK(cerberus_ble_archive_io_copy_result(id,&output)!=ESP_OK&&!memcmp(&output,&untouched,sizeof(output)));
    /* Truncated v6 headers, rows and response suffix never expose a partial
     * document. Worker-owned buffers are released after each failed decode. */
    const size_t truncated[]={31,32,295,463,16927,27679};
    for(unsigned n=0;n<sizeof(truncated)/sizeof(truncated[0]);++n){
        reset();fill(64);CHECK(cerberus_ble_archive_encode(&input,storage_bytes,sizeof(storage_bytes),&storage_length));
        storage_length=truncated[n];id=start_load();run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&status().error==ESP_ERR_INVALID_RESPONSE&&!outstanding);
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)!=ESP_OK&&!memcmp(&output,&untouched,sizeof(output)));
    }
    reset();fill(64);input.rows[63].family.reserved[1]=1;
    id=start_save();run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!saves&&!outstanding&&!status().result_ready);
    for(unsigned fault=0;fault<5;++fault){
        reset();fill(64);
        cerberus_ble_archive_row_t *r=&input.rows[63];
        if(fault==0)r->c5_response.reserved_tail=1;
        if(fault==1)r->c5_response.original_sequence=r->c5_response.response_sequence;
        if(fault==2)r->c5_response.family.family_code=1;
        if(fault==3)r->c5_response_age_upper_at_save_ms=r->age_upper_at_save_ms+1;
        if(fault==4)r->c5_response_age_upper_valid=false;
        id=start_save();run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!saves&&!outstanding&&!status().result_ready);
        CHECK(cerberus_ble_archive_io_copy_result(id,&output)!=ESP_OK&&!memcmp(&output,&untouched,sizeof(output)));
    }
    reset();storage_error=ESP_ERR_TIMEOUT;id=start_load();run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!outstanding);
    reset();fill(1);char path[512]="/COPY",name[256]="before.BLE";CHECK(cerberus_ble_archive_encode(&input,storage_bytes,sizeof(storage_bytes),&storage_length));
    CHECK(cerberus_ble_archive_io_load(path,name,&id)==ESP_OK);path[1]='!';name[0]='!';run_worker();
    CHECK(!strcmp(last_path,"/COPY")&&!strcmp(last_name,"before.BLE"));
    uint64_t stale=id;cerberus_ble_archive_io_release(id);id=start_save();cerberus_ble_archive_io_cancel(stale);cerberus_ble_archive_io_release(stale);
    CHECK(!status().cancel_requested&&outstanding==1);run_worker();CHECK(status().result_ready);
    reset();fill(1);immediate_task=true;id=start_save();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&!pending_task);reset();
    char badpath[512];memset(badpath,'X',sizeof(badpath));CHECK(cerberus_ble_archive_io_load(badpath,"x.BLE",&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_save(NULL,&id)==ESP_ERR_INVALID_ARG&&id==0);CHECK(cerberus_ble_archive_io_load(NULL,"x.BLE",&id)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ble_archive_io_save(&input,NULL)==ESP_ERR_INVALID_ARG);CHECK(cerberus_ble_archive_io_copy_result(0,NULL)==ESP_ERR_INVALID_ARG);
    check_json_operations();
    check_wifi_operations();
    check_wifi_interchange_operations();
    check_probe_operations();
    check_mesh_operations();
    check_mesh_archive_operations();
    check_locate_operations();
    archive_io_last_job=UINT64_MAX;CHECK(cerberus_ble_archive_io_save(&input,&id)==ESP_ERR_INVALID_STATE&&id==0&&!pending_task);
    CHECK(cerberus_ble_archive_io_save_airtag_json(&json_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_load_airtag_json("/","x.LOG",&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_export_airtag_json(&input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_save_wifi(&wifi_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_load_wifi("/","x.WFI",&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_save_probe(&probe_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(!strcmp(cerberus_ble_archive_io_state_name(CERBERUS_BLE_ARCHIVE_IO_CANCELED),"CANCELED"));
    CHECK(!strcmp(cerberus_ble_archive_io_state_name((cerberus_ble_archive_io_state_t)99),"UNKNOWN"));
    cerberus_ble_archive_io_get_status(NULL);cerberus_ble_archive_io_release(0);cerberus_ble_archive_io_cancel(0);
    CHECK(!outstanding&&allocations>=frees);printf("Archive async lifecycle passed %u checks; host native allocation=%zu JSON allocation=%zu Wi-Fi allocation=%zu AP JSON allocation=%zu SSID text allocation=%zu status=%zu.\n",checks,archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE),archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON),archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI),archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON),archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT),sizeof(cerberus_ble_archive_io_status_t));
    return 0;
}
