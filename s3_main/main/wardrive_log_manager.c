#include "wardrive_log_manager.h"
#include "wardrive_upload_file.h"
#include "wardrive_upload_settings.h"
#include "wardrive_upload_net.h"
#include "wardrive_upload_receipt.h"
#include "wardrive_upload_json.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <string.h>
#include <limits.h>
#include <time.h>

typedef struct {
    cerberus_ward_log_entry_t entries[CERBERUS_WARD_LOG_CAPACITY];
    cerberus_upload_file_t plan;
    cerberus_upload_config_t config;
    char buffer[4096];
} work_t;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static cerberus_log_manager_status_t state;
static work_t *work;
static uint64_t sequence;
static bool cancel_requested,close_requested,command_pending;
static cerberus_log_action_t command;
static int selection;
static int command_service;

static bool cancel(void *unused)
{(void)unused;portENTER_CRITICAL(&lock);bool c=cancel_requested||close_requested;portEXIT_CRITICAL(&lock);return c;}
static bool closing(void)
{portENTER_CRITICAL(&lock);bool c=close_requested;portEXIT_CRITICAL(&lock);return c;}
static void progress(void *unused,const char *stage,uint32_t sent,uint32_t bytes)
{(void)unused;portENTER_CRITICAL(&lock);snprintf(state.stage,sizeof(state.stage),"%s",stage);state.sent=sent;state.bytes=bytes;portEXIT_CRITICAL(&lock);}
void cerberus_log_manager_status(cerberus_log_manager_status_t *out)
{if(out){portENTER_CRITICAL(&lock);*out=state;portEXIT_CRITICAL(&lock);}}
void cerberus_log_manager_cancel(void){portENTER_CRITICAL(&lock);cancel_requested=true;portEXIT_CRITICAL(&lock);}
void cerberus_log_manager_close(void){portENTER_CRITICAL(&lock);close_requested=cancel_requested=true;state.closing=state.active;portEXIT_CRITICAL(&lock);}
bool cerberus_log_manager_row(unsigned i,cerberus_ward_log_entry_t *out)
{
    if(!out)return false;
    portENTER_CRITICAL(&lock);
    bool valid=state.active&&!state.busy&&work&&i<state.count;
    if(valid)*out=work->entries[i];else memset(out,0,sizeof(*out));portEXIT_CRITICAL(&lock);return valid;
}
static esp_err_t request(uint64_t session,uint32_t revision,cerberus_log_action_t action,int index,int service)
{
    portENTER_CRITICAL(&lock);esp_err_t e=ESP_ERR_INVALID_STATE;
    bool retry=action==CERBERUS_LOG_RETRY_SERVICE;
    bool reviewed=state.active&&!state.busy&&!state.closing&&retry&&service>=0&&service<3&&index>=0&&index<(int)state.count&&work&&
        state.destination[service].result==CERBERUS_UPLOAD_UNKNOWN&&
        state.summary.complete&&!state.summary.error&&!state.summary.legacy_metadata&&
        state.summary.kind==CERBERUS_WARD_LOG_WIGLE&&!strcmp(state.name,work->entries[index].name);
    if(state.active&&!state.busy&&!state.closing&&state.session==session&&state.catalog_revision==revision&&
        ((!retry&&(unsigned)action<=CERBERUS_LOG_IMPORT_PROFILE&&service==-1)||reviewed)&&
        index>=-1&&(index==-1||index<(int)state.count)&&
        !(action==CERBERUS_LOG_INSPECT&&index==-1)&&!(action==CERBERUS_LOG_IMPORT_PROFILE&&index!=-1)){
        command=action;selection=index;command_service=service;command_pending=true;cancel_requested=false;
        state.busy=true;state.error=ESP_OK;state.processed=state.sent=state.bytes=0;
        state.total=action==CERBERUS_LOG_IMPORT_PROFILE?1:index==-1?state.count:1;state.profile_saved=false;
        state.action=action;state.service=0;memset(&state.transfer,0,sizeof(state.transfer));
        memset(state.accepted,0,sizeof(state.accepted));
        memset(state.queued,0,sizeof(state.queued));memset(state.skipped,0,sizeof(state.skipped));
        if(action==CERBERUS_LOG_IMPORT_PROFILE)snprintf(state.name,sizeof(state.name),"UPLOAD.JSON");
        memset(&state.summary,0,sizeof(state.summary));
        if(!retry){memset(state.destination,0,sizeof(state.destination));memset(state.destination_complete,0,sizeof(state.destination_complete));}
        else state.destination_complete[service]=false;
        e=ESP_OK;
    }portEXIT_CRITICAL(&lock);return e;
}
esp_err_t cerberus_log_manager_request(uint64_t session,uint32_t revision,cerberus_log_action_t action,int index)
{return request(session,revision,action,index,-1);}
esp_err_t cerberus_log_manager_retry_service(uint64_t session,uint32_t revision,int index,cerberus_upload_service_t service)
{return request(session,revision,CERBERUS_LOG_RETRY_SERVICE,index,(int)service);}
static esp_err_t hash_file(FILE *f,uint8_t out[32])
{
    if(fseek(f,0,SEEK_SET))return ESP_FAIL;
    psa_hash_operation_t hash=PSA_HASH_OPERATION_INIT;size_t n,length=0;
    psa_status_t rc=psa_hash_setup(&hash,PSA_ALG_SHA_256);
    while(rc==PSA_SUCCESS&&(n=fread(work->buffer,1,sizeof(work->buffer),f))>0){
        if(cancel(NULL)){psa_hash_abort(&hash);return ESP_ERR_INVALID_STATE;}
        rc=psa_hash_update(&hash,(const uint8_t *)work->buffer,n);
    }
    if(rc==PSA_SUCCESS&&!ferror(f))rc=psa_hash_finish(&hash,out,32,&length);
    else rc=PSA_ERROR_GENERIC_ERROR;
    psa_hash_abort(&hash);return rc==PSA_SUCCESS&&length==32?ESP_OK:ESP_FAIL;
}
static esp_err_t receipt_key(const uint8_t file_hash[32],unsigned service,unsigned part,char key[69])
{
    /* Bind receipts to full original contents and exact service credentials.
     * No key, observed identifier or position is stored in a receipt. */
    const cerberus_upload_config_t *c=&work->config;
    const char *name=service==0?c->wigle_name:"";
    const char *token=service==0?c->wigle_token:service==1?c->wdgwars_token:c->wardrift_token;
    psa_hash_operation_t h=PSA_HASH_OPERATION_INIT;uint8_t digest[32],s=(uint8_t)service;size_t length=0;
    psa_status_t rc=psa_hash_setup(&h,PSA_ALG_SHA_256);
    static const uint8_t domain[]="Cerberus-upload-v1-1048576";
    if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,domain,sizeof(domain));
    if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,file_hash,32);
    if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,&s,1);
    if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,(const uint8_t *)name,strlen(name)+1);
    if(rc==PSA_SUCCESS)rc=psa_hash_update(&h,(const uint8_t *)token,strlen(token));
    if(rc==PSA_SUCCESS)rc=psa_hash_finish(&h,digest,sizeof(digest),&length);
    psa_hash_abort(&h);if(rc!=PSA_SUCCESS||length!=32||part>=1000)return ESP_FAIL;
    for(unsigned i=0;i<32;++i)snprintf(key+i*2,3,"%02x",digest[i]);
    snprintf(key+64,5,"-%03u",part);cerberus_upload_config_clear(digest,sizeof(digest));return ESP_OK;
}
static esp_err_t receipt_read(uint64_t lease,const char *key,cerberus_upload_reply_t *reply)
{
    uint8_t bytes[128];memset(reply,0,sizeof(*reply));
    esp_err_t e=cerberus_ward_log_receipt_read(lease,key,bytes,sizeof(bytes));
    if(e==ESP_ERR_NOT_FOUND)return ESP_OK;
    if(e!=ESP_OK||!cerberus_upload_receipt_decode(bytes,reply)){
        reply->result=CERBERUS_UPLOAD_UNKNOWN;return e==ESP_OK?ESP_ERR_INVALID_RESPONSE:e;
    }
    if(reply->result==CERBERUS_UPLOAD_SENDING)reply->result=CERBERUS_UPLOAD_UNKNOWN;
    return ESP_OK;
}
static esp_err_t receipt_write(uint64_t lease,const char *key,const cerberus_upload_reply_t *reply)
{
    uint8_t bytes[128];if(!cerberus_upload_receipt_encode(reply,bytes))return ESP_ERR_INVALID_ARG;
    return cerberus_ward_log_receipt_write(lease,key,bytes,sizeof(bytes));
}
static void destination(unsigned s,const cerberus_upload_reply_t *reply,bool complete)
{portENTER_CRITICAL(&lock);state.destination[s]=*reply;
    state.destination_complete[s]=complete&&(reply->result==CERBERUS_UPLOAD_QUEUED||reply->result==CERBERUS_UPLOAD_ACCEPTED);
    portEXIT_CRITICAL(&lock);}
static esp_err_t process_file(uint64_t lease,unsigned index,bool upload,int retry_service,bool *network)
{
    const cerberus_ward_log_entry_t *entry=&work->entries[index];FILE *file=NULL;
    portENTER_CRITICAL(&lock);memcpy(state.name,entry->name,sizeof(state.name));
    if(retry_service<0){memset(state.destination,0,sizeof(state.destination));memset(state.destination_complete,0,sizeof(state.destination_complete));}
    portEXIT_CRITICAL(&lock);
    progress(NULL,"VALIDATING SAVED CSV",0,0);
    esp_err_t e=cerberus_ward_log_storage_open(lease,entry->name,&file);
    if(e!=ESP_OK)return e;
    bool valid=cerberus_upload_file_prepare(file,&work->plan,cancel,NULL);
    portENTER_CRITICAL(&lock);state.summary=work->plan.summary;portEXIT_CRITICAL(&lock);
    if(!valid||work->plan.summary.bytes!=entry->bytes)e=ESP_ERR_INVALID_RESPONSE;
    bool eligible=e==ESP_OK&&work->plan.summary.kind==CERBERUS_WARD_LOG_WIGLE&&!work->plan.summary.legacy_metadata;
    if(e==ESP_OK&&!eligible){
        progress(NULL,work->plan.summary.kind==CERBERUS_WARD_LOG_UNLOCATED?"LOCAL ONLY / NO GEOLOCATION":"LEGACY HEADER / EXPORT ON PC",0,0);
        if(upload){portENTER_CRITICAL(&lock);for(unsigned s=0;s<3;++s)if(work->config.destinations&(1U<<s))++state.skipped[s];portEXIT_CRITICAL(&lock);}
    }
    uint8_t digest[32]={0};
    if(eligible){progress(NULL,"CHECKING FILE IDENTITY",0,0);e=hash_file(file,digest);}
    for(unsigned s=0;e==ESP_OK&&eligible&&s<3&&!cancel(NULL);++s){
        if(retry_service>=0&&s!=(unsigned)retry_service)continue;
        if(!cerberus_upload_config_service_ready(&work->config,(cerberus_upload_service_t)s))continue;
        if(upload&&!(work->config.destinations&(1U<<s)))continue;
        portENTER_CRITICAL(&lock);state.service=s+1;portEXIT_CRITICAL(&lock);
        cerberus_upload_reply_t aggregate={.result=CERBERUS_UPLOAD_ACCEPTED};
        for(unsigned p=0;e==ESP_OK&&p<work->plan.parts&&!cancel(NULL);++p){
            char key[69];cerberus_upload_reply_t reply={0};
            e=receipt_key(digest,s,p,key);if(e!=ESP_OK)break;
            progress(NULL,"READING SAVED UPLOAD RECEIPT",0,0);
            e=receipt_read(lease,key,&reply);
            if(e!=ESP_OK||reply.result==CERBERUS_UPLOAD_UNKNOWN){
                destination(s,&reply,false);
                if(e!=ESP_OK||(upload&&retry_service!=(int)s)){
                    char stage[40];snprintf(stage,sizeof(stage),"%s / CHECK ACCOUNT FIRST",
                        cerberus_upload_service_name((cerberus_upload_service_t)s));
                    progress(NULL,stage,0,0);
                    if(e==ESP_OK)e=ESP_ERR_INVALID_STATE;
                    break;
                }
            }
            if(upload&&(reply.result==CERBERUS_UPLOAD_NEW||reply.result==CERBERUS_UPLOAD_REJECTED||
                (reply.result==CERBERUS_UPLOAD_UNKNOWN&&retry_service==(int)s))){
                if(!*network){
                    e=cerberus_upload_net_begin(&work->config,cancel,progress,NULL);
                    *network=true; /* End also cleans partially initialized networking. */
                    if(e!=ESP_OK)break;
                }
                if(cancel(NULL))break;
                time_t now=time(NULL)+60;struct tm utc;char latest[20];
                if(!gmtime_r(&now,&utc)||strftime(latest,sizeof(latest),"%Y-%m-%d %H:%M:%S",&utc)!=19||
                    strcmp(work->plan.summary.last_utc,latest)>0){
                    progress(NULL,"CSV UTC IS IN THE FUTURE",0,0);e=ESP_ERR_INVALID_RESPONSE;break;
                }
                reply.result=CERBERUS_UPLOAD_SENDING;e=receipt_write(lease,key,&reply);if(e!=ESP_OK)break;
                char name[64];snprintf(name,sizeof(name),"%.*s-%.16s-%03u.csv",
                    (int)strlen(entry->name)-4,entry->name,key,p+1);
                cerberus_upload_transfer_t transfer={0};
                reply=cerberus_upload_net_send(&work->config,(cerberus_upload_service_t)s,file,&work->plan,p,name,cancel,progress,NULL,&transfer);
                portENTER_CRITICAL(&lock);state.transfer=transfer;portEXIT_CRITICAL(&lock);
                e=receipt_write(lease,key,&reply);
                if(e!=ESP_OK){reply.result=CERBERUS_UPLOAD_UNKNOWN;destination(s,&reply,false);break;}
                if(reply.result==CERBERUS_UPLOAD_UNKNOWN||reply.result==CERBERUS_UPLOAD_REJECTED){
                    progress(NULL,transfer.http_status==429?"RATE LIMITED / TRY LATER":reply.result==CERBERUS_UPLOAD_UNKNOWN?
                        "CHECK SERVICE ACCOUNT BEFORE RETRY":"UPLOAD REJECTED / CHECK SETUP",0,0);
                    destination(s,&reply,false);e=ESP_ERR_INVALID_RESPONSE;break;
                }
                /* Sequential requests, below the documented WDGWars limit. No
                 * automatic retry of 429, authentication or ambiguous errors. */
                for(unsigned wait=0;wait<25&&!cancel(NULL);++wait)vTaskDelay(pdMS_TO_TICKS(100));
            }
            if(!cerberus_upload_reply_merge(&aggregate,&reply))e=ESP_ERR_INVALID_SIZE;
            if(upload){portENTER_CRITICAL(&lock);if(reply.result==CERBERUS_UPLOAD_ACCEPTED)++state.accepted[s];
                else if(reply.result==CERBERUS_UPLOAD_QUEUED)++state.queued[s];else ++state.skipped[s];
                portEXIT_CRITICAL(&lock);}
            destination(s,&aggregate,e==ESP_OK&&p+1==work->plan.parts);
        }
    }
    cerberus_upload_config_clear(digest,sizeof(digest));
    /* A transient mutex timeout must not abandon an open FILE and its lease. */
    esp_err_t close_error;
    do{close_error=cerberus_ward_log_storage_close(lease,&file);if(file)vTaskDelay(pdMS_TO_TICKS(100));}while(file);
    if(e==ESP_OK)e=close_error;
    return e;
}
static esp_err_t catalog(uint64_t lease)
{
    size_t count=0;esp_err_t e=cerberus_ward_log_storage_list(lease,work->entries,CERBERUS_WARD_LOG_CAPACITY,&count);
    portENTER_CRITICAL(&lock);state.count=(unsigned)count;++state.catalog_revision;portEXIT_CRITICAL(&lock);return e;
}
static esp_err_t import_profile(uint64_t lease)
{
    progress(NULL,"READING SD UPLOAD PROFILE",0,0);
    size_t length=0;
    esp_err_t e=cerberus_ward_log_profile_read(lease,work->buffer,sizeof(work->buffer),&length);
    if(e==ESP_OK&&!cerberus_upload_config_json(work->buffer,length,&work->config)){
        progress(NULL,"INVALID JSON / SETUP UNCHANGED",0,0);e=ESP_ERR_INVALID_ARG;
    }
    cerberus_upload_config_clear(work->buffer,sizeof(work->buffer));
    if(e==ESP_OK&&cancel(NULL))e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK){
        progress(NULL,"SAVING UPLOAD PROFILE",0,0);
        e=cerberus_upload_settings_save(&work->config);
        if(e==ESP_OK){
            cerberus_upload_config_t verified={0};e=cerberus_upload_settings_load(&verified);
            if(e==ESP_OK&&memcmp(&verified,&work->config,sizeof(verified)))e=ESP_ERR_INVALID_RESPONSE;
            cerberus_upload_config_clear(&verified,sizeof(verified));
        }
        if(e==ESP_OK){
            portENTER_CRITICAL(&lock);state.profile_saved=true;state.processed=1;portEXIT_CRITICAL(&lock);
            e=cerberus_ward_log_profile_remove(lease,length);
            progress(NULL,e==ESP_OK?"PROFILE SAVED / SD COPY REMOVED":"PROFILE SAVED / SD COPY REMAINS",0,0);
        }
    }
    cerberus_upload_config_clear(&work->config,sizeof(work->config));return e;
}
static void worker(void *unused)
{
    (void)unused;uint64_t lease=0;esp_err_t error=cerberus_ward_log_storage_begin(&lease);
    if(error==ESP_OK)error=catalog(lease);
    portENTER_CRITICAL(&lock);state.error=error;state.busy=false;
    snprintf(state.stage,sizeof(state.stage),"%s",error==ESP_OK?"SELECT A SAVED LOG":"SD BUSY / CLOSE AND RETRY");portEXIT_CRITICAL(&lock);
    while(!closing()){
        portENTER_CRITICAL(&lock);bool pending=command_pending;cerberus_log_action_t action=command;
        int index=selection,retry_service=command_service;command_pending=false;portEXIT_CRITICAL(&lock);
        if(!pending){vTaskDelay(pdMS_TO_TICKS(50));continue;}
        bool network=false;
        bool upload=action==CERBERUS_LOG_UPLOAD||action==CERBERUS_LOG_RETRY_SERVICE;
        error=lease?ESP_OK:ESP_ERR_INVALID_STATE;
        if(error==ESP_OK&&action!=CERBERUS_LOG_DELETE&&action!=CERBERUS_LOG_IMPORT_PROFILE)error=cerberus_upload_settings_load(&work->config);
        if(error==ESP_OK&&action==CERBERUS_LOG_IMPORT_PROFILE)error=import_profile(lease);
        if(error==ESP_OK&&upload){
            if(!cerberus_upload_config_wifi_ready(&work->config)||!work->config.destinations)error=ESP_ERR_INVALID_STATE;
            if(retry_service>=0&&!(work->config.destinations&(1U<<retry_service)))error=ESP_ERR_INVALID_STATE;
            for(unsigned s=0;s<3;++s)if((work->config.destinations&(1U<<s))&&
                (retry_service<0||s==(unsigned)retry_service)&&
                !cerberus_upload_config_service_ready(&work->config,(cerberus_upload_service_t)s))error=ESP_ERR_INVALID_STATE;
            if(error!=ESP_OK)progress(NULL,"SAVE HOME WI-FI AND API KEYS FIRST",0,0);
        }
        unsigned count;portENTER_CRITICAL(&lock);count=state.count;portEXIT_CRITICAL(&lock);
        for(unsigned i=0;action!=CERBERUS_LOG_IMPORT_PROFILE&&error==ESP_OK&&i<count&&!cancel(NULL);++i){
            if(index>=0&&i!=(unsigned)index)continue;
            if(action==CERBERUS_LOG_DELETE){progress(NULL,"DELETING CONFIRMED LOGS",0,0);
                error=cerberus_ward_log_storage_delete(lease,work->entries[i].name,work->entries[i].bytes);}
            else error=process_file(lease,i,upload,retry_service,&network);
            if(error==ESP_OK){portENTER_CRITICAL(&lock);++state.processed;portEXIT_CRITICAL(&lock);}
        }
        if(network){esp_err_t cleanup;do{cleanup=cerberus_upload_net_end();if(cleanup!=ESP_OK){
            progress(NULL,"RELEASING WI-FI / PLEASE WAIT",0,0);vTaskDelay(pdMS_TO_TICKS(500));}}while(cleanup!=ESP_OK);}
        cerberus_upload_config_clear(&work->config,sizeof(work->config));
        if(action==CERBERUS_LOG_DELETE){esp_err_t list_error=catalog(lease);if(error==ESP_OK)error=list_error;}
        bool stopped=cancel(NULL);
        portENTER_CRITICAL(&lock);state.error=error;state.busy=false;
        if(stopped&&action!=CERBERUS_LOG_IMPORT_PROFILE)snprintf(state.stage,sizeof(state.stage),"%s",action==CERBERUS_LOG_DELETE?
            "DELETE CANCELED / CHECK FILE COUNT":"CANCELED / ORIGINAL LOGS RETAINED");
        else if(stopped&&!state.profile_saved)snprintf(state.stage,sizeof(state.stage),"PROFILE CANCELED / CHECK SETUP");
        else if(error==ESP_OK&&upload)snprintf(state.stage,sizeof(state.stage),"UPLOAD PASS FINISHED / SEE RECEIPTS");
        else if(error==ESP_OK&&action==CERBERUS_LOG_DELETE)snprintf(state.stage,sizeof(state.stage),"CONFIRMED LOGS DELETED");
        else if(error==ESP_OK&&state.summary.kind==CERBERUS_WARD_LOG_WIGLE&&!state.summary.legacy_metadata)
            snprintf(state.stage,sizeof(state.stage),"CSV VALIDATED / RECEIPTS BELOW");
        portEXIT_CRITICAL(&lock);
    }
    if(lease){do{error=cerberus_ward_log_storage_end(lease);if(error!=ESP_OK)vTaskDelay(pdMS_TO_TICKS(100));}while(error!=ESP_OK);}
    portENTER_CRITICAL(&lock);work_t *old=work;work=NULL;portEXIT_CRITICAL(&lock);
    cerberus_upload_config_clear(old,sizeof(*old));heap_caps_free(old);
    portENTER_CRITICAL(&lock);memset(&state,0,sizeof(state));portEXIT_CRITICAL(&lock);vTaskDelete(NULL);
}
esp_err_t cerberus_log_manager_open(void)
{
    portENTER_CRITICAL(&lock);
    if(state.active||sequence==UINT64_MAX){portEXIT_CRITICAL(&lock);return ESP_ERR_INVALID_STATE;}
    memset(&state,0,sizeof(state));state.active=state.busy=true;state.session=++sequence;
    close_requested=cancel_requested=command_pending=false;portEXIT_CRITICAL(&lock);
    work=heap_caps_calloc(1,sizeof(*work),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    esp_err_t error=ESP_OK;
    if(!work)error=ESP_ERR_NO_MEM;
    else if(xTaskCreate(worker,"ward_logs",16384,NULL,3,NULL)!=pdPASS)error=ESP_ERR_NO_MEM;
    if(error!=ESP_OK){heap_caps_free(work);work=NULL;portENTER_CRITICAL(&lock);state.active=state.busy=false;state.error=error;portEXIT_CRITICAL(&lock);}
    return error;
}
