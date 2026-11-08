#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <inttypes.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif
#include "wardrive_log_manager.h"
#include "wardrive_upload_net.h"
#include "wardrive_upload_settings.h"
#include "wardrive_upload_receipt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "psa/crypto.h"

static unsigned critical,storage_locked,allocations,phase,sends,begins,ends,deleted;
static bool fail_alloc,fail_task,fail_sync,sd_busy,unknown_reply,network_fail,request_cancel,wdgwars_unknown;
static const char *import_mode;
static const char *retry_mode;
static unsigned service_sends[3];
static bool retry_multipart;
static bool multipart_cancel;
static bool current_names;
static unsigned part_sends[3][2];
static unsigned saves;
static void (*queued)(void *);
static cerberus_upload_config_t saved;
static char console_output[1024];
static void console_printf(const char *format,...)
{size_t used=strlen(console_output);va_list args;va_start(args,format);
    int n=vsnprintf(console_output+used,sizeof(console_output)-used,format,args);va_end(args);
    assert(n>0&&(size_t)n<sizeof(console_output)-used);}
#include "upload_status_impl.h"
static atomic_bool console_stop_requested;
#include "ward_logs_console_impl.h"
struct tm *test_gmtime_r(const time_t *t,struct tm *out){struct tm *p=gmtime(t);if(p)*out=*p;return p?out:NULL;}
void test_enter(void){assert(!critical);critical=1;}
void test_exit(void){assert(critical);critical=0;}
void *heap_caps_malloc(size_t n,unsigned caps){(void)caps;assert(!critical);if(fail_alloc)return NULL;void *p=malloc(n);if(p)++allocations;return p;}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){void *p=heap_caps_malloc(n*size,caps);if(p)memset(p,0,n*size);return p;}
void heap_caps_free(void *p){assert(!critical);if(p){--allocations;free(p);}}
int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{(void)name;(void)arg;(void)priority;(void)handle;assert(!critical&&stack==16384);if(fail_task)return 0;queued=fn;return 1;}
void vTaskDelete(void *task){assert(!task&&!critical);++deleted;}
/* Controlled PSA boundary, not a cryptography test. Every byte supplied by the
 * actual worker changes this fixture hash, so receipt binding can be tested. */
psa_status_t psa_hash_setup(psa_hash_operation_t *h,unsigned algorithm){assert(algorithm==PSA_ALG_SHA_256);h->v=2166136261U;h->bytes=0;return 0;}
psa_status_t psa_hash_update(psa_hash_operation_t *h,const uint8_t *p,size_t n){while(n--){h->v=(h->v^*p++)*16777619U;++h->bytes;}return 0;}
psa_status_t psa_hash_finish(psa_hash_operation_t *h,uint8_t *out,size_t cap,size_t *n){assert(cap==32);for(unsigned i=0;i<32;++i)out[i]=(uint8_t)((h->v>>((i%4)*8))^i);*n=32;return 0;}
psa_status_t psa_hash_abort(psa_hash_operation_t *h){memset(h,0,sizeof(*h));return 0;}

#define CERBERUS_STORAGE_MOUNT_PATH "card"
#define CERBERUS_STORAGE_WARDRIVE_PATH "/CERBERUS/WARD"
#define ESP_RETURN_ON_ERROR(expr,tag,msg) do{esp_err_t e_=(expr);if(e_!=ESP_OK)return e_;}while(0)
static uint64_t sd_ward_log_lease,sd_ward_log_sequence;
static FILE *sd_wifi_capture_file;
static struct {bool active;} sd_recon;
static void *sd_packet_count;
static bool wardrive_storage_busy(void){return sd_busy||sd_ward_log_lease;}
static esp_err_t take_storage_mutex(void){assert(!critical&&!storage_locked);storage_locked=1;return ESP_OK;}
static void release_storage_mutex(void){assert(storage_locked);storage_locked=0;}
static esp_err_t require_firmware_owner_locked(void){assert(storage_locked);return ESP_OK;}
static esp_err_t ensure_directory_exists(const char *p){assert(storage_locked);struct stat st;
    if(!stat(p,&st))return S_ISDIR(st.st_mode)?ESP_OK:ESP_FAIL;
#ifdef _WIN32
    return _mkdir(p)==0?ESP_OK:ESP_FAIL;
#else
    return mkdir(p,0700)==0?ESP_OK:ESP_FAIL;
#endif
}
static int test_fsync(int fd){assert(storage_locked);if(fail_sync)return -1;
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}
#define fsync test_fsync
static int test_remove(const char *path)
{if(import_mode&&!strcmp(import_mode,"import_remove_fail")&&strstr(path,"/CONFIG/UPLOAD.JSON")){errno=EIO;return -1;}return remove(path);}
#define remove test_remove
#include "storage_wardrive_logs_impl.h"
#undef fsync
#undef remove
esp_err_t cerberus_upload_settings_load(cerberus_upload_config_t *out)
{if(import_mode&&!strcmp(import_mode,"import_readback_fail"))return ESP_FAIL;*out=saved;return ESP_OK;}
esp_err_t cerberus_upload_settings_save(const cerberus_upload_config_t *config)
{++saves;if(import_mode&&!strcmp(import_mode,"import_save_fail"))return ESP_FAIL;saved=*config;
    if(import_mode&&!strcmp(import_mode,"import_cancel_after_save"))cerberus_log_manager_cancel();return ESP_OK;}
esp_err_t cerberus_upload_net_begin(const cerberus_upload_config_t *c,cerberus_upload_cancel_fn cancel,cerberus_upload_progress_fn progress,void *context)
{assert(!critical&&!storage_locked&&sd_ward_log_lease&&c->destinations==7&&!cancel(context));++begins;progress(context,"CONNECTING",0,0);return network_fail?ESP_FAIL:ESP_OK;}
esp_err_t cerberus_upload_net_end(void){assert(!critical&&!storage_locked);++ends;return ESP_OK;}
cerberus_upload_reply_t cerberus_upload_net_send(const cerberus_upload_config_t *c,cerberus_upload_service_t service,
    FILE *f,const cerberus_upload_file_t *plan,unsigned part,const char *name,cerberus_upload_cancel_fn cancel,
    cerberus_upload_progress_fn progress,void *context,cerberus_upload_transfer_t *transfer)
{
    (void)c;assert(!critical&&!storage_locked&&f&&plan->parts&&part<plan->parts&&!cancel(context));
    if(current_names)assert(!strncmp(name,"CERBWardrive001-",sizeof("CERBWardrive001-")-1U));
    ++sends;++service_sends[service];assert(part<2);++part_sends[service][part];
    if(retry_multipart)assert(plan->parts==2);
    cerberus_log_manager_status_t current;cerberus_log_manager_status(&current);
    assert(!current.destination_complete[service]); /* Never complete before the final POST reply. */
    progress(context,"SENDING",10,20);*transfer=(cerberus_upload_transfer_t){.http_status=200,.attempted=true,.response_complete=true,.response_bytes=48};
    if(request_cancel)cerberus_log_manager_cancel();
    cerberus_upload_reply_t r={.result=unknown_reply||request_cancel||(wdgwars_unknown&&service==CERBERUS_UPLOAD_WDGWARS&&(!retry_multipart||part==1))?CERBERUS_UPLOAD_UNKNOWN:
        service==CERBERUS_UPLOAD_WIGLE||(service==CERBERUS_UPLOAD_WDGWARS&&retry_mode&&!strcmp(retry_mode,"retry_queued"))?
        CERBERUS_UPLOAD_QUEUED:CERBERUS_UPLOAD_ACCEPTED};
    if(r.result==CERBERUS_UPLOAD_QUEUED)strcpy(r.transaction,service==CERBERUS_UPLOAD_WDGWARS?"1234":"synthetic-transaction");return r;
}

static void issue(cerberus_log_action_t action,int index)
{cerberus_log_manager_status_t s;cerberus_log_manager_status(&s);
    assert(cerberus_log_manager_request(s.session+1,s.catalog_revision,action,index)!=ESP_OK);
    assert(cerberus_log_manager_request(s.session,s.catalog_revision+1,action,index)!=ESP_OK);
    assert(cerberus_log_manager_request(s.session,s.catalog_revision,action,index)==ESP_OK);
    assert(cerberus_log_manager_request(s.session,s.catalog_revision,action,index)!=ESP_OK);
}
/* Simulate damaged media or a newly durable acknowledgement between review
 * and worker execution. This touches only the synthetic temporary card. */
static void change_uncertain_receipt(bool corrupt)
{
    const char *folder="card/CERBERUS/WARD/UPLOADS";DIR *dir=opendir(folder);assert(dir);
    struct dirent *entry;unsigned changed=0;
    while((entry=readdir(dir))!=NULL){
        if(!strstr(entry->d_name,".RCT"))continue;
        char path[512];snprintf(path,sizeof(path),"%s/%s",folder,entry->d_name);
        FILE *f=fopen(path,"r+b");assert(f);assert(!fseek(f,-128,SEEK_END));
        uint8_t bytes[128];assert(fread(bytes,1,128,f)==128);cerberus_upload_reply_t reply;
        assert(cerberus_upload_receipt_decode(bytes,&reply));
        if(reply.result==CERBERUS_UPLOAD_UNKNOWN){
            if(corrupt){bytes[127]^=1;assert(!fseek(f,-128,SEEK_END));}
            else{reply.result=CERBERUS_UPLOAD_ACCEPTED;assert(cerberus_upload_receipt_encode(&reply,bytes));assert(!fseek(f,0,SEEK_END));}
            assert(fwrite(bytes,1,128,f)==128);++changed;
        }assert(!fclose(f));
    }assert(!closedir(dir)&&changed==1);
}
void vTaskDelay(unsigned ms)
{
    assert(!critical&&!storage_locked);cerberus_log_manager_status_t s;cerberus_log_manager_status(&s);
    if(s.busy||ms!=50){
        if(multipart_cancel&&phase==2&&ms==100&&sends==1)cerberus_log_manager_cancel();
        return;
    }
    if(sd_busy){assert(s.error!=ESP_OK&&!sd_ward_log_lease);cerberus_log_manager_close();return;}
    if(multipart_cancel){
        switch(phase++){
        case 0:issue(CERBERUS_LOG_INSPECT,0);break;
        case 1:assert(s.summary.rows==14000&&s.error==ESP_OK);issue(CERBERUS_LOG_UPLOAD,0);break;
        case 2:assert(sends==1&&ends==1&&strstr(s.stage,"CANCELED"));
            assert(s.destination[0].result==CERBERUS_UPLOAD_QUEUED&&!s.destination_complete[0]);
            issue(CERBERUS_LOG_INSPECT,0);break;
        case 3:assert(s.error==ESP_OK&&sends==1&&s.destination[0].result==CERBERUS_UPLOAD_NEW);
            assert(!s.destination_complete[0]);issue(CERBERUS_LOG_UPLOAD,0);break;
        case 4:assert(s.error==ESP_OK&&sends==6&&begins==2&&ends==2);
            for(unsigned service=0;service<3;++service){
                assert(s.destination_complete[service]);
                assert(part_sends[service][0]==1&&part_sends[service][1]==1);
            }
            cerberus_log_manager_close();break;
        default:assert(!"unexpected multipart cancellation phase");
        }return;
    }
    if(retry_multipart){
        switch(phase++){
        case 0:issue(CERBERUS_LOG_INSPECT,0);break;
        case 1:assert(s.summary.rows==14000&&s.error==ESP_OK);issue(CERBERUS_LOG_UPLOAD,0);break;
        case 2:assert(s.error!=ESP_OK&&sends==4&&begins==1&&ends==1&&s.destination[0].result==CERBERUS_UPLOAD_QUEUED);
            assert(s.destination_complete[0]&&!s.destination_complete[1]&&!s.destination_complete[2]);
            assert(service_sends[0]==2&&service_sends[1]==2&&!service_sends[2]);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,0,CERBERUS_UPLOAD_WDGWARS)==ESP_OK);
            wdgwars_unknown=false;break;
        case 3:assert(s.error==ESP_OK&&sends==5&&begins==2&&ends==2);
            assert(part_sends[0][0]==1&&part_sends[0][1]==1&&part_sends[1][0]==1&&part_sends[1][1]==2&&!service_sends[2]);
            assert(s.destination[0].result==CERBERUS_UPLOAD_QUEUED&&s.destination[1].result==CERBERUS_UPLOAD_ACCEPTED);
            assert(s.destination_complete[0]&&s.destination_complete[1]&&!s.destination_complete[2]);
            issue(CERBERUS_LOG_UPLOAD,0);break;
        case 4:assert(s.error==ESP_OK&&sends==7&&begins==3&&ends==3&&service_sends[2]==2);
            assert(s.destination_complete[0]&&s.destination_complete[1]&&s.destination_complete[2]);
            issue(CERBERUS_LOG_INSPECT,0);break;
        case 5:assert(s.error==ESP_OK&&sends==7&&begins==3&&ends==3);
            assert(s.destination_complete[0]&&s.destination_complete[1]&&s.destination_complete[2]);
            cerberus_log_manager_close();break;
        default:assert(!"unexpected multipart phase");
        }return;
    }
    if(import_mode){
        if(phase++==0){assert(s.active&&s.count==2);issue(CERBERUS_LOG_IMPORT_PROFILE,-1);return;}
        assert(phase==2&&!begins&&!sends&&!ends);
        bool invalid=!strcmp(import_mode,"import_invalid"),save_fail=!strcmp(import_mode,"import_save_fail");
        bool read_fail=!strcmp(import_mode,"import_readback_fail"),remove_fail=!strcmp(import_mode,"import_remove_fail");
        assert(s.profile_saved==!(invalid||save_fail||read_fail));
        assert((s.error==ESP_OK)==!(invalid||save_fail||read_fail||remove_fail));
        assert(saves==(unsigned)!invalid);
        assert(!strcmp(saved.ssid,invalid||save_fail?"fixture":"JSON fixture AP"));
        struct stat st;assert((stat("card/CERBERUS/CONFIG/UPLOAD.JSON",&st)==0)==(invalid||save_fail||read_fail||remove_fail));
        if(s.profile_saved)assert(strstr(s.stage,"PROFILE SAVED"));
        cerberus_log_manager_close();return;
    }
    switch(phase++){
    case 0:{assert(s.count==2&&s.active&&s.error==ESP_OK&&sd_ward_log_lease);
        uint64_t token=1;assert(cerberus_ward_log_storage_begin(&token)!=ESP_OK&&!token);
        assert(cerberus_ward_log_storage_end(sd_ward_log_lease+1)!=ESP_OK);
        cerberus_ward_log_entry_t entry;assert(cerberus_log_manager_row(0,&entry)&&!strcmp(entry.name,current_names?"CERBWardrive001.CSV":"WIGLE001.CSV"));
        cerberus_ward_log_entry_t modern={.name="CERBWardrive001.CSV"},legacy={.name="WIGLE999.CSV"};
        assert(ward_log_compare(&modern,&legacy)<0&&ward_log_compare(&legacy,&modern)>0);
        assert(!cerberus_log_manager_row(2,&entry));issue(CERBERUS_LOG_INSPECT,0);break;}
    case 1:assert(s.error==ESP_OK&&s.summary.rows==10&&s.summary.complete&&begins==0);issue(CERBERUS_LOG_UPLOAD,-1);break;
    case 2:
        assert(ends==1&&begins==1);
        if(network_fail||fail_sync)assert(s.error!=ESP_OK&&sends==0);
        else if(unknown_reply||request_cancel||wdgwars_unknown)assert(s.error!=ESP_OK&&sends==(wdgwars_unknown?2U:1U));
        else assert(s.error==ESP_OK&&sends==3&&s.queued[0]==1&&s.accepted[1]==1&&s.accepted[2]==1);
        if(network_fail){network_fail=false;issue(CERBERUS_LOG_UPLOAD,0);phase=6;break;}
        fail_sync=false;request_cancel=false;issue(CERBERUS_LOG_UPLOAD,0);break;
    case 3:
        console_output[0]=0;print_upload_status();
        assert(strstr(console_output,"upload_busy=0")&&!strstr(console_output,"fixture-token")&&!strstr(console_output,"synthetic-transaction"));
        if(wdgwars_unknown){
            assert(s.error==ESP_ERR_INVALID_STATE&&sends==2&&s.destination[0].result==CERBERUS_UPLOAD_QUEUED);
            assert(s.destination[1].result==CERBERUS_UPLOAD_UNKNOWN);
            assert(!strcmp(s.stage,"WDGWARS / CHECK ACCOUNT FIRST"));
            assert(strstr(console_output,"upload_service=2")&&strstr(console_output,"upload_destination_1_result=5"));
        }else if(unknown_reply||s.error!=ESP_OK){
            assert(sends<=1&&strstr(s.stage,"CHECK ACCOUNT FIRST"));
        }else assert(sends==3);
        assert(begins==1&&ends==1);
        if(retry_mode){
            assert(cerberus_log_manager_request(s.session,s.catalog_revision,CERBERUS_LOG_RETRY_SERVICE,0)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session+1,s.catalog_revision,0,CERBERUS_UPLOAD_WDGWARS)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision+1,0,CERBERUS_UPLOAD_WDGWARS)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,-1,CERBERUS_UPLOAD_WDGWARS)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,1,CERBERUS_UPLOAD_WDGWARS)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,0,CERBERUS_UPLOAD_WIGLE)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,0,(cerberus_upload_service_t)3)!=ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,0,CERBERUS_UPLOAD_WDGWARS)==ESP_OK);
            assert(cerberus_log_manager_retry_service(s.session,s.catalog_revision,0,CERBERUS_UPLOAD_WDGWARS)!=ESP_OK);
            wdgwars_unknown=!strcmp(retry_mode,"retry_unknown");
            if(!strcmp(retry_mode,"retry_disabled"))saved.destinations=5;
            network_fail=!strcmp(retry_mode,"retry_network_fail");
            request_cancel=!strcmp(retry_mode,"retry_cancel_send");
            if(!strcmp(retry_mode,"retry_cancel_before"))cerberus_log_manager_cancel();
            if(!strcmp(retry_mode,"retry_corrupt"))change_uncertain_receipt(true);
            if(!strcmp(retry_mode,"retry_acknowledged"))change_uncertain_receipt(false);
            phase=7;break;
        }
        issue(CERBERUS_LOG_DELETE,0);break;
    case 4:assert(s.error==ESP_OK&&s.count==1&&s.processed==1);
        {cerberus_ward_log_entry_t entry;assert(cerberus_log_manager_row(0,&entry)&&!strcmp(entry.name,"UNLOC001.CSV"));}
        issue(CERBERUS_LOG_DELETE,-1);break;
    case 5:assert(s.error==ESP_OK&&s.count==0);cerberus_log_manager_close();break;
    case 6:assert(s.error==ESP_OK&&begins==2&&ends==2&&sends==3);cerberus_log_manager_close();break;
    case 7:{
        assert(s.action==CERBERUS_LOG_RETRY_SERVICE&&service_sends[0]==1&&service_sends[2]==0);
        assert(s.destination[0].result==CERBERUS_UPLOAD_QUEUED);
        assert(s.destination_complete[0]);
        bool disabled=!strcmp(retry_mode,"retry_disabled"),before=!strcmp(retry_mode,"retry_cancel_before");
        if(!strcmp(retry_mode,"retry_corrupt")||!strcmp(retry_mode,"retry_acknowledged")){
            bool corrupt=!strcmp(retry_mode,"retry_corrupt");
            assert(begins==1&&ends==1&&sends==2&&!s.transfer.attempted);
            assert((s.error!=ESP_OK)==corrupt);
            assert(s.destination[1].result==(corrupt?CERBERUS_UPLOAD_UNKNOWN:CERBERUS_UPLOAD_ACCEPTED));
            cerberus_log_manager_close();break;
        }
        if(disabled||before){assert(begins==1&&ends==1&&sends==2&&!s.transfer.attempted);
            assert(s.destination[1].result==CERBERUS_UPLOAD_UNKNOWN);cerberus_log_manager_close();break;}
        assert(begins==2&&ends==2);
        if(network_fail){assert(sends==2&&!s.transfer.attempted&&s.error!=ESP_OK);cerberus_log_manager_close();break;}
        assert(sends==3&&service_sends[1]==2&&s.transfer.attempted);
        if(wdgwars_unknown||request_cancel){assert(s.error!=ESP_OK&&s.destination[1].result==CERBERUS_UPLOAD_UNKNOWN);cerberus_log_manager_close();break;}
        assert(s.error==ESP_OK&&s.destination[1].result==(!strcmp(retry_mode,"retry_queued")?CERBERUS_UPLOAD_QUEUED:CERBERUS_UPLOAD_ACCEPTED));
        issue(CERBERUS_LOG_UPLOAD,0);break;}
    case 8:assert(s.error==ESP_OK&&begins==3&&ends==3&&sends==4&&service_sends[0]==1&&service_sends[1]==2&&service_sends[2]==1);
        issue(CERBERUS_LOG_UPLOAD,0);break;
    case 9:assert(s.error==ESP_OK&&begins==3&&ends==3&&sends==4);cerberus_log_manager_close();break;
    default:assert(!"unexpected worker iteration");
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);strcpy(saved.ssid,"fixture");strcpy(saved.password,"test-password");strcpy(saved.wigle_name,"fixture-name");
    strcpy(saved.wigle_token,"fixture-token");memset(saved.wdgwars_token,'a',64);strcpy(saved.wardrift_token,"fixture-token");saved.destinations=7;
    unknown_reply=!strcmp(argv[1],"unknown");request_cancel=!strcmp(argv[1],"cancel");
    wdgwars_unknown=!strcmp(argv[1],"wdgwars_unknown");
    if(!strncmp(argv[1],"retry_",6)){retry_mode=argv[1];wdgwars_unknown=true;}
    retry_multipart=!strcmp(argv[1],"retry_multipart");
    multipart_cancel=!strcmp(argv[1],"multipart_cancel");
    current_names=!strcmp(argv[1],"current_name");
    /* Diagnostic catalog uses actual leased storage, never the upload worker.
     * It lists the existing files, omits TMP/other paths, and releases its lease. */
    print_wardrive_logs();assert(strstr(console_output,"ward_logs_error=0\r\n")&&
        strstr(console_output,"ward_logs_count=2\r\n")&&strstr(console_output,"UNLOC001.CSV")&&
        strstr(console_output,current_names?"CERBWardrive001.CSV":"WIGLE001.CSV"));
    assert(!strstr(console_output,"WIGLE002.TMP")&&!strstr(console_output,"KEEP.TXT"));
    assert(strstr(console_output,"ward_logs_directory_present=1\r\n")&&
        strstr(console_output,"ward_logs_csv_entries=3\r\n")&&
        strstr(console_output,"ward_logs_temporary_entries=1\r\n")&&
        strstr(console_output,"ward_logs_unrecognized_csv=1\r\n")&&
        strstr(console_output,"ward_logs_first_unrecognized_hex=49474e4f5245442e637376\r\n"));
    assert(!sends&&!begins&&!ends&&!sd_ward_log_lease&&!allocations);
    assert(!rename("card/CERBERUS/WARD","card/CERBERUS/WARD.AWAY"));
    console_output[0]=0;print_wardrive_logs();
    assert(strstr(console_output,"ward_logs_error=0\r\n")&&strstr(console_output,"ward_logs_directory_present=0\r\n"));
    assert(!sd_ward_log_lease&&!allocations);
    assert(!rename("card/CERBERUS/WARD.AWAY","card/CERBERUS/WARD"));
    console_output[0]=0;sd_busy=true;print_wardrive_logs();
    assert(strstr(console_output,"ward_logs_count=0\r\n")&&!strstr(console_output,"ward_log="));
    assert(!sd_ward_log_lease&&!allocations);sd_busy=false;
    console_output[0]=0;fail_alloc=true;print_wardrive_logs();assert(!sd_ward_log_lease&&!allocations);
    fail_alloc=false;console_output[0]=0;
    fail_sync=!strcmp(argv[1],"sd_fail");network_fail=!strcmp(argv[1],"network_fail");sd_busy=!strcmp(argv[1],"busy");
    if(!strncmp(argv[1],"import_",7))import_mode=argv[1];
    fail_alloc=true;assert(cerberus_log_manager_open()==ESP_ERR_NO_MEM&&!allocations);fail_alloc=false;
    fail_task=true;assert(cerberus_log_manager_open()==ESP_ERR_NO_MEM&&!allocations);fail_task=false;
    assert(cerberus_log_manager_open()==ESP_OK&&queued&&allocations);
    assert(cerberus_log_manager_open()==ESP_ERR_INVALID_STATE);queued(NULL);
    cerberus_log_manager_status_t s;cerberus_log_manager_status(&s);
    assert(!s.active&&!allocations&&!sd_ward_log_lease&&!ward_log_open_files&&!critical&&!storage_locked&&deleted==1);
    struct stat st;assert(!stat("card/CERBERUS/WARD/KEEP.TXT",&st));assert(!stat("card/CERBERUS/WARD/WIGLE002.TMP",&st));
    /* A full journal still exposes its final durable outcome. Only a further
     * append is rejected, so the last accepted part never becomes resendable. */
    sd_busy=false;uint64_t token=0;assert(cerberus_ward_log_storage_begin(&token)==ESP_OK);
    char key[69];memset(key,'b',64);memcpy(key+64,"-999",5);
    uint8_t record[128],readback[128];cerberus_upload_reply_t accepted={.result=CERBERUS_UPLOAD_ACCEPTED};
    assert(cerberus_upload_receipt_encode(&accepted,record));
    for(unsigned i=0;i<32;++i)assert(cerberus_ward_log_receipt_write(token,key,record,sizeof(record))==ESP_OK);
    assert(cerberus_ward_log_receipt_read(token,key,readback,sizeof(readback))==ESP_OK&&!memcmp(record,readback,sizeof(record)));
    assert(cerberus_ward_log_receipt_write(token,key,record,sizeof(record))==ESP_ERR_INVALID_STATE);
    assert(cerberus_ward_log_storage_end(token)==ESP_OK);
    puts("Actual log worker/storage: ownership, reviewed catalog, cancellation, receipts and cleanup passed");return 0;
}
