#include "wardrive_upload_net.h"
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <time.h>

static esp_netif_t *station;
static bool claimed, initialized, started, sntp_owned, time_ready;
static bool cancelled(cerberus_upload_cancel_fn fn,void *context){return fn&&fn(context);}
static void report(cerberus_upload_progress_fn fn,void *context,const char *s,uint32_t n,uint32_t total)
{if(fn)fn(context,s,n,total);}

#include "wardrive_upload_clock.h"

esp_err_t cerberus_upload_net_end(void)
{
    time_ready=false;
    upload_sntp_end();
    if(started){esp_err_t e=esp_wifi_stop();if(e!=ESP_OK&&e!=ESP_ERR_WIFI_NOT_STARTED)return e;started=false;}
    if(initialized){esp_err_t e=esp_wifi_deinit();if(e!=ESP_OK)return e;initialized=false;}
    if(station){esp_netif_destroy_default_wifi(station);station=NULL;}
    if(claimed){esp_err_t e=cerberus_s3_wifi_release(CERBERUS_S3_WIFI_UPLOAD);if(e!=ESP_OK)return e;claimed=false;}
    return ESP_OK;
}
esp_err_t cerberus_upload_net_begin(const cerberus_upload_config_t *config,
    cerberus_upload_cancel_fn cancel,cerberus_upload_progress_fn progress,void *context)
{
    report(progress,context,"CHECKING UPLOAD CONFIGURATION",0,0);
    if(!cerberus_upload_config_wifi_ready(config)||claimed)return ESP_ERR_INVALID_STATE;
    time_ready=false;
    if(cancelled(cancel,context))return ESP_ERR_INVALID_STATE;
    report(progress,context,"CLAIMING S3 FOR HOME WI-FI",0,0);
    esp_err_t err=cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_UPLOAD);if(err!=ESP_OK)return err;claimed=true;
    report(progress,context,"CHECKING BLUETOOTH RELEASED",0,0);
    if(cerberus_s3_ble_host_owner()!=CERBERUS_S3_BLE_OWNER_NONE)return ESP_ERR_INVALID_STATE;
    report(progress,context,"PREPARING HOME WI-FI",0,0);
    err=cerberus_s3_wifi_prepare(CERBERUS_S3_WIFI_UPLOAD);if(err!=ESP_OK)return err;
    station=esp_netif_create_default_wifi_sta();if(!station)return ESP_ERR_NO_MEM;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    init.static_rx_buf_num=4;init.dynamic_rx_buf_num=8;init.dynamic_tx_buf_num=4;
    init.rx_ba_win=4;init.ampdu_rx_enable=0;init.ampdu_tx_enable=0;init.amsdu_tx_enable=0;
    report(progress,context,"INITIALIZING HOME WI-FI",0,0);
    err=esp_wifi_init(&init);if(err!=ESP_OK)return err;initialized=true;
    report(progress,context,"CONFIGURING HOME WI-FI",0,0);
    if((err=esp_wifi_set_storage(WIFI_STORAGE_RAM))!=ESP_OK)return err;
    if((err=esp_wifi_set_mode(WIFI_MODE_STA))!=ESP_OK)return err;
    wifi_config_t wifi={0};
    memcpy(wifi.sta.ssid,config->ssid,strlen(config->ssid));
    memcpy(wifi.sta.password,config->password,strlen(config->password));
    wifi.sta.pmf_cfg.capable=true;wifi.sta.pmf_cfg.required=false;
    err=esp_wifi_set_config(WIFI_IF_STA,&wifi);cerberus_upload_config_clear(&wifi,sizeof(wifi));
    if(err!=ESP_OK)return err;
    report(progress,context,"STARTING HOME WI-FI",0,0);
    if((err=esp_wifi_start())!=ESP_OK)return err;
    started=true;
    if((err=esp_wifi_set_ps(WIFI_PS_NONE))!=ESP_OK)return err;
    report(progress,context,"CONNECTING HOME WI-FI",0,0);
    if((err=esp_wifi_connect())!=ESP_OK)return err;
    int64_t deadline=esp_timer_get_time()+25000000LL;
    for(;;){
        if(cancelled(cancel,context))return ESP_ERR_INVALID_STATE;
        esp_netif_ip_info_t ip={0};
        if(esp_netif_get_ip_info(station,&ip)==ESP_OK&&ip.ip.addr)break;
        if(esp_timer_get_time()>=deadline)return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    err=upload_clock_prepare(cancel,progress,context);
    time_ready=err==ESP_OK;
    return err;
}
static bool write_all(esp_http_client_handle_t client,const char *data,size_t length,
    cerberus_upload_cancel_fn cancel,void *context,int64_t deadline)
{
    while(length){
        if(cancelled(cancel,context)||esp_timer_get_time()>=deadline)return false;
        int n=esp_http_client_write(client,data,(int)length);
        if(n<=0||(size_t)n>length)return false;
        data+=n;length-=(size_t)n;
    }return true;
}
cerberus_upload_reply_t cerberus_upload_net_send(const cerberus_upload_config_t *config,
    cerberus_upload_service_t service,FILE *file,const cerberus_upload_file_t *plan,
    unsigned part,const char *name,cerberus_upload_cancel_fn cancel,
    cerberus_upload_progress_fn progress,void *context,cerberus_upload_transfer_t *transfer)
{
    cerberus_upload_reply_t reply={.result=CERBERUS_UPLOAD_UNKNOWN};
    cerberus_upload_transfer_t detail={.error=ESP_ERR_INVALID_ARG};
    if(transfer)*transfer=detail;
    if(!claimed||!started||!time_ready||!file||!plan||part>=plan->parts||!name||strlen(name)>64||
        !cerberus_upload_config_service_ready(config,service))return reply;
    for(const char *p=name;*p;++p)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='z')||
        (*p>='A'&&*p<='Z')||*p=='-'||*p=='_'||*p=='.'))return reply;
    static const char *const urls[]={"https://api.wigle.net/api/v2/file/upload",
        "https://wdgwars.pl/api/v2/upload-csv","https://wardrift.net/v1/ingest/signals"};
    esp_http_client_config_t cfg={.url=urls[service],.method=HTTP_METHOD_POST,
        .timeout_ms=10000,.disable_auto_redirect=true,.crt_bundle_attach=esp_crt_bundle_attach,
        .buffer_size=1024,.buffer_size_tx=1024,.user_agent="Cerberus/" CERBERUS_APP_VERSION};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    if(!client){if(transfer)transfer->error=ESP_ERR_NO_MEM;return reply;}
    char *buffer=heap_caps_malloc(4097,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    char head[256]={0},tail[48]={0},auth[300]={0},plain[196]={0};
    const char *boundary="CerberusWardrivePartV1";
    size_t head_bytes=0,tail_bytes=0;
    esp_err_t err=buffer?ESP_OK:ESP_ERR_NO_MEM;
    if(err==ESP_OK)err=esp_http_client_set_header(client,"Accept","application/json");
    if(err==ESP_OK&&service==CERBERUS_UPLOAD_WIGLE){
        int n=snprintf(plain,sizeof(plain),"%s:%s",config->wigle_name,config->wigle_token);size_t encoded=0;
        memcpy(auth,"Basic ",6);
        if(n<0||(size_t)n>=sizeof(plain)||mbedtls_base64_encode((unsigned char *)auth+6,sizeof(auth)-7,
            &encoded,(const unsigned char *)plain,(size_t)n))err=ESP_FAIL;
        else{auth[6+encoded]=0;err=esp_http_client_set_header(client,"Authorization",auth);}
    }else if(err==ESP_OK)err=esp_http_client_set_header(client,"X-API-Key",
        service==CERBERUS_UPLOAD_WDGWARS?config->wdgwars_token:config->wardrift_token);
    cerberus_upload_config_clear(auth,sizeof(auth));cerberus_upload_config_clear(plain,sizeof(plain));
    if(service!=CERBERUS_UPLOAD_WARDRIFT){
        head_bytes=(size_t)snprintf(head,sizeof(head),"--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: text/csv\r\n\r\n",boundary,name);
        tail_bytes=(size_t)snprintf(tail,sizeof(tail),"\r\n--%s--\r\n",boundary);
        if(head_bytes>=sizeof(head)||tail_bytes>=sizeof(tail))err=ESP_ERR_INVALID_SIZE;
        if(err==ESP_OK)err=esp_http_client_set_header(client,"Content-Type","multipart/form-data; boundary=CerberusWardrivePartV1");
    }else{
        if(err==ESP_OK)err=esp_http_client_set_header(client,"Content-Type","text/csv");
        if(err==ESP_OK)err=esp_http_client_set_header(client,"X-File-Name",name);
    }
    uint32_t length=plan->offsets[part+1]-plan->offsets[part];
    uint32_t total=(uint32_t)(head_bytes+plan->header_bytes+length+tail_bytes),sent=0;
    int64_t deadline=esp_timer_get_time()+180000000LL;
    if(err==ESP_OK&&cancelled(cancel,context))err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK){report(progress,context,"CONNECTING TO UPLOAD SERVICE",0,0);
        detail.attempted=true;err=esp_http_client_open(client,(int)total);}
    if(err==ESP_OK&&(!write_all(client,head,head_bytes,cancel,context,deadline)||
        !write_all(client,plan->headers,plan->header_bytes,cancel,context,deadline)||
        fseek(file,(long)plan->offsets[part],SEEK_SET)))err=ESP_FAIL;
    while(err==ESP_OK&&sent<length){
        size_t n=length-sent<4096?length-sent:4096;
        if(fread(buffer,1,n,file)!=n||!write_all(client,buffer,n,cancel,context,deadline)){err=ESP_FAIL;break;}
        sent+=(uint32_t)n;report(progress,context,"SENDING CSV",sent,length);
    }
    if(err==ESP_OK&&!write_all(client,tail,tail_bytes,cancel,context,deadline))err=ESP_FAIL;
    if(err==ESP_OK){
        report(progress,context,"WAITING FOR RECEIPT",sent,length);
        int64_t response_length=esp_http_client_fetch_headers(client);
        unsigned status=(unsigned)esp_http_client_get_status_code(client);
        detail.http_status=status;
        size_t used=0;bool complete=false;
        if(response_length>=0&&response_length<=4096)while(used<4096&&!cancelled(cancel,context)&&esp_timer_get_time()<deadline){
            int n=esp_http_client_read(client,buffer+used,(int)(4096-used));
            if(n<0)break;
            used+=(size_t)n;
            if(esp_http_client_is_complete_data_received(client)){complete=true;break;}
            if(n==0)break;
        }
        if(buffer)buffer[used]=0;
        detail.response_bytes=(uint32_t)used;
        detail.response_complete=complete&&!cancelled(cancel,context);
        reply=cerberus_upload_reply(service,status,buffer,used,detail.response_complete);
        if(reply.result==CERBERUS_UPLOAD_UNKNOWN)err=ESP_ERR_INVALID_RESPONSE;
    }
    if(err!=ESP_OK){
        /* Copy numeric diagnostics before close destroys the TLS transport.
         * Never retain headers, credential values or response bodies here. */
        detail.socket_errno=esp_http_client_get_errno(client);
        detail.tls_error=esp_http_client_get_and_clear_last_tls_error(client,
            &detail.tls_code,&detail.tls_flags);
    }
    detail.error=err;if(transfer)*transfer=detail;
    esp_http_client_close(client);esp_http_client_cleanup(client);
    if(buffer){cerberus_upload_config_clear(buffer,4097);heap_caps_free(buffer);}
    return reply;
}
