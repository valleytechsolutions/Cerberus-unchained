#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "wardrive_upload_net.h"
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "wardrive_gps_uart.h"
#include <sys/time.h>

static unsigned calls,fail_call,allocs,stops,deinits,releases;
static int64_t clock_us;
static int64_t wall_offset_us=INT64_C(1767225600000000);
static unsigned sync_starts,sync_server,time_sets,cancel_after_sync;
static bool first_server_missing,invalid_ntp_time,set_time_fail,read_time_fail,reverse_sync_clock;
static esp_err_t sync_error,gps_time_error=ESP_ERR_NOT_FOUND;
static cerberus_wardrive_gps_time_t gps_time;
static char last_stage[40];
static bool started,initialized,claimed,station_alive,sync_alive,stop_fail,ble_busy,cancel_now,ip_missing,sync_missing;
static bool response_incomplete,read_fail,write_fail,open_fail,transport_closed;
static unsigned tls_reads;
static esp_netif_t iface;
static unsigned service,status=200,expected_length,used,read_at;
static char sent[8192],auth[300],content_type[100],api_key[150],filename[100];
static const char *response;
struct test_http {bool active;};static struct test_http client;
static esp_err_t call(void){return ++calls==fail_call?ESP_FAIL:ESP_OK;}
void test_enter(void){} void test_exit(void){}
void *heap_caps_malloc(size_t n,unsigned caps){assert(caps==6);void *p=malloc(n);if(p)++allocs;return p;}
void heap_caps_free(void *p){if(p){assert(allocs);--allocs;free(p);}}
int64_t esp_timer_get_time(void){return clock_us;}
void vTaskDelay(unsigned ms){clock_us+=(int64_t)ms*1000;}
int gettimeofday(struct timeval *tv,void *zone)
{
    assert(!zone);if(read_time_fail)return -1;
    int64_t value=wall_offset_us+clock_us;
    tv->tv_sec=(time_t)(value/1000000);tv->tv_usec=(suseconds_t)(value%1000000);return 0;
}
int settimeofday(const struct timeval *tv,const void *zone)
{
    assert(!zone);++time_sets;if(set_time_fail)return -1;
    wall_offset_us=(int64_t)tv->tv_sec*1000000+tv->tv_usec-clock_us;return 0;
}
esp_err_t cerberus_wardrive_gps_uart_recent_time(cerberus_wardrive_gps_time_t *out)
{memset(out,0,sizeof(*out));if(gps_time_error==ESP_OK)*out=gps_time;return gps_time_error;}
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void){return ble_busy?CERBERUS_S3_BLE_OWNER_SCANNER:CERBERUS_S3_BLE_OWNER_NONE;}
esp_err_t cerberus_s3_wifi_claim(cerberus_s3_wifi_owner_t owner){assert(owner==CERBERUS_S3_WIFI_UPLOAD);if(claimed)return ESP_ERR_INVALID_STATE;claimed=true;return ESP_OK;}
esp_err_t cerberus_s3_wifi_release(cerberus_s3_wifi_owner_t owner){assert(owner==CERBERUS_S3_WIFI_UPLOAD&&claimed&&!initialized&&!station_alive);claimed=false;++releases;return ESP_OK;}
esp_err_t cerberus_s3_wifi_prepare(cerberus_s3_wifi_owner_t owner){assert(owner==CERBERUS_S3_WIFI_UPLOAD&&claimed);return call();}
esp_netif_t *esp_netif_create_default_wifi_sta(void){if(call()!=ESP_OK)return NULL;station_alive=true;return &iface;}
void esp_netif_destroy_default_wifi(esp_netif_t *p){assert(p==&iface&&station_alive&&!initialized);station_alive=false;}
esp_err_t esp_wifi_init(const wifi_init_config_t *c){assert(c->static_rx_buf_num==4&&c->dynamic_rx_buf_num==8);esp_err_t e=call();if(e==ESP_OK)initialized=true;return e;}
esp_err_t esp_wifi_deinit(void){assert(initialized&&!started);initialized=false;++deinits;return ESP_OK;}
esp_err_t esp_wifi_start(void){assert(initialized);esp_err_t e=call();if(e==ESP_OK)started=true;return e;}
esp_err_t esp_wifi_stop(void){assert(started);++stops;if(stop_fail){stop_fail=false;return ESP_FAIL;}started=false;return ESP_OK;}
esp_err_t esp_wifi_set_storage(int v){assert(v==WIFI_STORAGE_RAM);return call();}
esp_err_t esp_wifi_set_mode(int v){assert(v==WIFI_MODE_STA);return call();}
esp_err_t esp_wifi_set_ps(int v){assert(v==WIFI_PS_NONE);return call();}
esp_err_t esp_wifi_set_config(int v,const wifi_config_t *c){assert(v==WIFI_IF_STA&&!memcmp(c->sta.ssid,"fixture",7));return call();}
esp_err_t esp_wifi_connect(void){assert(started);return call();}
esp_err_t esp_netif_get_ip_info(esp_netif_t *p,esp_netif_ip_info_t *ip){assert(p==&iface);ip->ip.addr=ip_missing?0:1;return ESP_OK;}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c)
{
    assert(!sync_alive);++sync_starts;
    if(!strcmp(c->server,"pool.ntp.org"))sync_server=0;
    else {assert(!strcmp(c->server,"time.cloudflare.com"));sync_server=1;}
    esp_err_t e=call();if(e==ESP_OK)sync_alive=true;return e;
}
void esp_netif_sntp_deinit(void){assert(sync_alive);sync_alive=false;}
esp_err_t esp_netif_sntp_sync_wait(unsigned ms)
{
    assert(sync_alive);clock_us+=(int64_t)ms*1000;
    if(cancel_after_sync&&!--cancel_after_sync)cancel_now=true;
    if(reverse_sync_clock){clock_us=-1;return ESP_ERR_TIMEOUT;}
    if(sync_error)return sync_error;
    if(sync_missing||(first_server_missing&&!sync_server))return ESP_ERR_TIMEOUT;
    wall_offset_us=(invalid_ntp_time?0:INT64_C(1767225600000000))-clock_us;
    return ESP_OK;
}
esp_err_t esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
int mbedtls_base64_encode(unsigned char *out,size_t cap,size_t *n,const unsigned char *data,size_t len)
{
    static const char b[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if(cap<((len+2)/3)*4+1)return -1;*n=0;
    for(size_t i=0;i<len;i+=3){unsigned v=(unsigned)data[i]<<16;if(i+1<len)v|=(unsigned)data[i+1]<<8;if(i+2<len)v|=data[i+2];
        out[(*n)++]=b[(v>>18)&63];out[(*n)++]=b[(v>>12)&63];out[(*n)++]=i+1<len?b[(v>>6)&63]:'=';out[(*n)++]=i+2<len?b[v&63]:'=';}
    out[*n]=0;return 0;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    assert(!client.active&&started&&!sync_alive);assert(c->disable_auto_redirect&&c->crt_bundle_attach==esp_crt_bundle_attach&&c->method==HTTP_METHOD_POST);
    const char *urls[]={"https://api.wigle.net/api/v2/file/upload","https://wdgwars.pl/api/v2/upload-csv","https://wardrift.net/v1/ingest/signals"};
    assert(!strcmp(c->url,urls[service])&&c->timeout_ms==10000&&c->buffer_size<=1024&&c->buffer_size_tx<=1024);
    client.active=true;transport_closed=false;used=read_at=0;sent[0]=auth[0]=content_type[0]=api_key[0]=filename[0]=0;return &client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value)
{assert(c==&client&&c->active);if(!strcmp(key,"Authorization"))strcpy(auth,value);
    else if(!strcmp(key,"Content-Type"))strcpy(content_type,value);else if(!strcmp(key,"X-API-Key"))strcpy(api_key,value);
    else if(!strcmp(key,"X-File-Name"))strcpy(filename,value);
    else if(!strcmp(key,"Accept"))assert(!strcmp(value,"application/json"));else assert(!"unknown header");return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int length){assert(c==&client&&length>0&&length<8192);expected_length=(unsigned)length;return open_fail?ESP_ERR_HTTP_CONNECT:ESP_OK;}
int esp_http_client_write(esp_http_client_handle_t c,const char *bytes,int length)
{assert(c==&client&&length>0);if(write_fail)return -1;int n=length>17?17:length;assert(used+(unsigned)n<sizeof(sent));memcpy(sent+used,bytes,(size_t)n);used+=(unsigned)n;sent[used]=0;return n;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c){assert(c==&client&&used==expected_length);return (int64_t)strlen(response);}
int esp_http_client_get_status_code(esp_http_client_handle_t c){assert(c==&client);return (int)status;}
int esp_http_client_read(esp_http_client_handle_t c,char *buffer,int length){assert(c==&client);if(read_fail)return -1;
    size_t n=strlen(response)-read_at;if(n>(size_t)length)n=(size_t)length;if(n>7)n=7;memcpy(buffer,response+read_at,n);read_at+=(unsigned)n;return (int)n;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c){assert(c==&client);return !response_incomplete&&read_at==strlen(response);}
int esp_http_client_get_errno(esp_http_client_handle_t c){assert(c==&client&&c->active&&!transport_closed);return open_fail?110:0;}
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,int *code,int *flags)
{assert(c==&client&&c->active&&!transport_closed);++tls_reads;*code=open_fail?-0x2700:0;*flags=open_fail?8:0;return open_fail?0x801a:ESP_OK;}
esp_err_t esp_http_client_close(esp_http_client_handle_t c){assert(c==&client);transport_closed=true;return ESP_OK;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c){assert(c==&client&&c->active);c->active=false;return ESP_OK;}
static bool canceled(void *p){(void)p;return cancel_now;}
static void progress(void *p,const char *stage,uint32_t sent_bytes,uint32_t total)
{(void)p;assert(sent_bytes<=total&&strlen(stage)<sizeof(last_stage));strcpy(last_stage,stage);}

static void end_upload(void)
{assert(cerberus_upload_net_end()==ESP_OK&&!claimed&&!started&&!initialized&&!station_alive&&!sync_alive);}

static void clock_checks(const cerberus_upload_config_t *cfg)
{
    /* A plausible system date without a recorded source still needs sync. */
    sync_missing=true;unsigned before=sync_starts;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT);
    assert(sync_starts==before+2&&!strcmp(last_stage,"UTC TIMEOUT / LOG NOT SENT"));end_upload();
    sync_missing=false;first_server_missing=true;before=sync_starts;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_OK);
    assert(sync_starts==before+2&&!sync_alive&&sync_server==1);end_upload();
    first_server_missing=false;sync_missing=true;before=sync_starts;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_OK);
    assert(sync_starts==before);end_upload();

    --clock_us;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT);
    assert(sync_starts==before+2);end_upload();
    sync_missing=false;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_OK);end_upload();
    sync_missing=true;before=sync_starts;

    /* Moving wall time invalidates the cached proof, even within 15 minutes. */
    wall_offset_us+=3000000;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT);
    assert(sync_starts==before+2);end_upload();
    gps_time_error=ESP_OK;strcpy(gps_time.utc,"2024-02-29T12:34:56.123Z");
    gps_time.received_ms=(uint64_t)clock_us/1000U;
    before=sync_starts;unsigned sets=time_sets;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_OK);
    assert(sync_starts==before&&time_sets==sets+1&&!strcmp(last_stage,"USING GPS UTC FOR TLS"));
    assert(wall_offset_us+clock_us==INT64_C(1709210096123000)+clock_us%1000);end_upload();
    clock_us=(int64_t)gps_time.received_ms*1000+900000000;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_OK&&sync_starts==before);end_upload();
    ++clock_us;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT&&sync_starts==before+2);end_upload();

    /* Reading a held-over GPS sample must not restart its age allowance. */
    gps_time.received_ms=(uint64_t)clock_us/1000U-899000U;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_OK);end_upload();
    before=sync_starts;clock_us+=1001000;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT&&sync_starts==before+2);end_upload();
    const char *invalid[]={"2025-02-29T12:34:56.123Z","2026-13-01T12:34:56.123Z",
        "2026-01-01T24:34:56.123Z","2026-01-01T12:34:60.123Z","2026-01-01T12:34:56.12XZ",
        "2023-12-31T23:59:59.999Z","2100-01-01T00:00:00.000Z"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
        strcpy(gps_time.utc,invalid[i]);gps_time.received_ms=(uint64_t)clock_us/1000U;
        sets=time_sets;assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT);
        assert(time_sets==sets);end_upload();
    }
    strcpy(gps_time.utc,"2026-01-01T00:00:00.000Z");gps_time.received_ms=(uint64_t)clock_us/1000U;
    set_time_fail=true;assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_FAIL);end_upload();set_time_fail=false;
    gps_time_error=ESP_ERR_NOT_FOUND;sync_missing=false;invalid_ntp_time=true;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_INVALID_RESPONSE);end_upload();invalid_ntp_time=false;
    reverse_sync_clock=true;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_INVALID_STATE);end_upload();reverse_sync_clock=false;
    clock_us=1000000;sync_missing=true;cancel_after_sync=2;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_INVALID_STATE&&cancel_now&&!sync_alive);
    end_upload();cancel_now=false;sync_missing=false;
    clock_us=1000000;sync_error=ESP_FAIL;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_FAIL);end_upload();sync_error=ESP_OK;
    read_time_fail=true;
    assert(cerberus_upload_net_begin(cfg,canceled,progress,NULL)==ESP_ERR_INVALID_RESPONSE);end_upload();read_time_fail=false;
}
int main(int argc,char **argv)
{
    assert(argc==2||argc==3);cerberus_upload_config_t cfg={0};strcpy(cfg.ssid,"fixture");strcpy(cfg.password,"password");
    strcpy(cfg.wigle_name,"name");strcpy(cfg.wigle_token,"token");memset(cfg.wdgwars_token,'a',64);strcpy(cfg.wardrift_token,"fixture-token");
    if(argc==3){
        assert(!strcmp(argv[2],"retry-clock"));
        assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)==ESP_OK);end_upload();
        sync_missing=true;unsigned before=sync_starts;
        assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)==ESP_OK);
        assert(sync_starts==before);end_upload();puts("Repeated upload connection reuses recent synchronized UTC: passed");return 0;
    }
    for(unsigned failure=1;failure<=10;++failure){calls=0;fail_call=failure;assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)!=ESP_OK);
        assert(cerberus_upload_net_end()==ESP_OK&&!claimed&&!initialized&&!started&&!station_alive&&!sync_alive);}
    fail_call=0;ble_busy=true;assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)!=ESP_OK);assert(cerberus_upload_net_end()==ESP_OK);ble_busy=false;
    ip_missing=true;assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT);assert(cerberus_upload_net_end()==ESP_OK);ip_missing=false;
    sync_missing=true;assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)==ESP_ERR_TIMEOUT);assert(cerberus_upload_net_end()==ESP_OK);sync_missing=false;
    cancel_now=true;assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)!=ESP_OK&&!claimed);cancel_now=false;
    clock_checks(&cfg);
    assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)==ESP_OK);
    FILE *f=fopen(argv[1],"rb");assert(f);cerberus_upload_file_t plan;assert(cerberus_upload_file_prepare(f,&plan,NULL,NULL));
    const char *responses[]={"{\"success\":true,\"results\":{\"transids\":[{\"transId\":\"test-123\"}]}}","{\"ok\":true,\"imported\":1}","{\"accepted\":true}"};
    for(service=0;service<3;++service){response=responses[service];cerberus_upload_transfer_t transfer;
        cerberus_upload_reply_t r=cerberus_upload_net_send(&cfg,(cerberus_upload_service_t)service,f,&plan,0,"fixture-001.csv",canceled,progress,NULL,&transfer);
        assert(r.result==(service==0?CERBERUS_UPLOAD_QUEUED:CERBERUS_UPLOAD_ACCEPTED)&&transfer.http_status==200&&!client.active&&!allocs);
        assert(transfer.error==ESP_OK&&transfer.attempted&&transfer.response_complete&&transfer.response_bytes==strlen(response));
        assert(!transfer.socket_errno&&!transfer.tls_error&&!transfer.tls_code&&!transfer.tls_flags);
        if(service==0)assert(!strcmp(auth,"Basic bmFtZTp0b2tlbg==")&&!api_key[0]);
        else assert(!auth[0]&&!strcmp(api_key,service==1?cfg.wdgwars_token:cfg.wardrift_token));
        if(service==2){assert(!strcmp(content_type,"text/csv")&&!strcmp(filename,"fixture-001.csv"));assert(used==plan.summary.bytes);}
        else assert(strstr(content_type,"multipart/form-data")&&strstr(sent,"name=\"file\"")&&strstr(sent,"filename=\"fixture-001.csv\""));
        for(unsigned fault=0;fault<5;++fault){response_incomplete=fault==0;read_fail=fault==1;write_fail=fault==2;status=fault==3?302:200;cancel_now=fault==4;
            r=cerberus_upload_net_send(&cfg,(cerberus_upload_service_t)service,f,&plan,0,"fixture.csv",canceled,progress,NULL,&transfer);
            assert(r.result==CERBERUS_UPLOAD_UNKNOWN&&!client.active&&!allocs&&transfer.error!=ESP_OK);
            assert(transfer.attempted==!cancel_now);
            if(fault==0||fault==1)assert(!transfer.response_complete&&transfer.http_status==200);
            if(fault==2||fault==4)assert(!transfer.response_bytes&&!transfer.http_status);
            if(fault==3)assert(transfer.http_status==302&&transfer.response_complete);}
        response_incomplete=read_fail=write_fail=cancel_now=false;status=200;
    }
    service=1;status=202;response="{\"ok\":true,\"job_id\":1234,\"status\":\"queued\"}";
    cerberus_upload_transfer_t queued_transfer;
    cerberus_upload_reply_t queued_reply=cerberus_upload_net_send(&cfg,CERBERUS_UPLOAD_WDGWARS,f,&plan,0,
        "fixture.csv",canceled,progress,NULL,&queued_transfer);
    assert(queued_reply.result==CERBERUS_UPLOAD_QUEUED&&!strcmp(queued_reply.transaction,"1234")&&!queued_reply.counts_valid);
    assert(queued_transfer.error==ESP_OK&&queued_transfer.http_status==202&&queued_transfer.response_complete&&!allocs&&!client.active);
    open_fail=true;unsigned before=tls_reads;
    queued_reply=cerberus_upload_net_send(&cfg,CERBERUS_UPLOAD_WDGWARS,f,&plan,0,
        "fixture.csv",canceled,progress,NULL,&queued_transfer);
    assert(queued_reply.result==CERBERUS_UPLOAD_UNKNOWN&&queued_transfer.error==ESP_ERR_HTTP_CONNECT);
    assert(queued_transfer.attempted&&!used&&!queued_transfer.http_status&&!queued_transfer.response_bytes);
    assert(queued_transfer.socket_errno==110&&queued_transfer.tls_error==0x801a&&queued_transfer.tls_code==-0x2700&&queued_transfer.tls_flags==8);
    assert(tls_reads==before+1&&!client.active&&!allocs);open_fail=false;
    queued_reply=cerberus_upload_net_send(&cfg,CERBERUS_UPLOAD_WDGWARS,f,&plan,0,
        "fixture.csv",canceled,progress,NULL,&queued_transfer);
    assert(queued_reply.result==CERBERUS_UPLOAD_QUEUED&&tls_reads==before+1);
    assert(!queued_transfer.socket_errno&&!queued_transfer.tls_error&&!queued_transfer.tls_code&&!queued_transfer.tls_flags);
    fclose(f);stop_fail=true;assert(cerberus_upload_net_end()!=ESP_OK&&claimed&&started&&initialized);
    assert(cerberus_upload_net_begin(&cfg,canceled,progress,NULL)!=ESP_OK);
    assert(cerberus_upload_net_end()==ESP_OK&&!claimed&&!station_alive&&!initialized&&!sync_alive);
    assert(stops&&deinits&&releases);puts("Actual network adapter: HTTPS/auth/body streaming, partial I/O, timeout/cancel and retained cleanup passed");return 0;
}
