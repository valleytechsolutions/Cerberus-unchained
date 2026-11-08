#include "wifi_frame_session.h"
#include "c5_spi_link.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks,live,allocated,freed,starts,stops,queries;
static size_t store_size;
static int mutex_token,locked;
static bool fail_mutex=true,fail_lock,fail_alloc,finish_start,replace_on_stop,early_callback;
static uint32_t firmware=87U,next_generation=50U;
static int64_t now_us=1000000;
static esp_err_t peer_error,start_error;
static cerberus_s3_capture_status_t capture;
static cerberus_s3_capture_options_t request;
static const uint8_t ap[6]={0x20,1,2,3,4,5};
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1); } } while(0)

static void send(uint32_t generation,uint32_t sequence,uint8_t sta,uint64_t source,uint8_t channel);
int64_t esp_timer_get_time(void){return now_us;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return fail_mutex?NULL:&mutex_token;}
int xSemaphoreTake(SemaphoreHandle_t mutex,uint32_t ticks)
{
    (void)ticks;CHECK(mutex==&mutex_token&&!locked);
    if(fail_lock)return 0;locked=1;return 1;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    CHECK(mutex==&mutex_token&&locked);locked=0;
    /* The real worker cannot enter its observer until Start releases the
     * mutex. Deliver the waiting callback at that exact scheduling point. */
    if(early_callback){early_callback=false;send(capture.generation,1U,1U,100U,request.channel?request.channel:6U);}
    return 1;
}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{
    CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(fail_alloc)return NULL;
    void *p=calloc(count,size);CHECK(p);++live;++allocated;CHECK(live<=2U);store_size=count*size;return p;
}
void heap_caps_free(void *p){if(p){CHECK(live>0U);--live;++freed;free(p);}}
esp_err_t c5_spi_link_get_status(cerberus_c5_status_t *out)
{++queries;memset(out,0,sizeof(*out));out->firmware_version=firmware;return peer_error;}
void cerberus_s3_capture_status(cerberus_s3_capture_status_t *out)
{*out=capture;}
esp_err_t cerberus_s3_capture_start_options_generation(const cerberus_s3_capture_options_t *options,uint32_t *generation)
{
    CHECK(locked&&!capture.busy);++starts;*generation=0U;
    if(start_error)return start_error;
    request=*options;CHECK(request.observer&&request.observer_context);
    capture=(cerberus_s3_capture_status_t){.generation=++next_generation,.busy=true,
        .state=CERBERUS_S3_CAPTURE_RUNNING,.channel=options->channel,.mode_mask=options->mode_mask,
        .filter_bssid=options->filter_bssid,.hopping=options->hopping,
        .planned=options->plan!=NULL,.save_pcap=options->save_pcap};
    *generation=capture.generation;
    if(finish_start){finish_start=false;capture.busy=false;capture.state=CERBERUS_S3_CAPTURE_ERROR;capture.error_code=ESP_ERR_TIMEOUT;}
    return ESP_OK;
}
bool cerberus_s3_capture_stop_generation(uint32_t generation)
{
    if(replace_on_stop){replace_on_stop=false;capture.generation=++next_generation;}
    if(!capture.busy||generation!=capture.generation)return false;
    ++stops;capture.state=CERBERUS_S3_CAPTURE_STOPPING;return true;
}
static void send(uint32_t generation,uint32_t sequence,uint8_t sta,uint64_t source,uint8_t channel)
{
    uint8_t frame[26]={0xc0U};memcpy(frame+10U,ap,6U);memcpy(frame+16U,ap,6U);
    frame[4]=0x30U;frame[9]=sta;frame[24]=3U;
    cerberus_s3_capture_observation_t metadata={.generation=generation,.sequence=sequence,
        .channel=channel,.observed_us=source,.rssi_dbm=-47};
    request.observer(&metadata,frame,sizeof(frame),request.observer_context);
}
static cerberus_s3_frame_status_t status(void)
{cerberus_s3_frame_status_t s;CHECK(cerberus_s3_frame_status(&s)==ESP_OK);return s;}
static void finish(void)
{capture.busy=false;capture.state=capture.save_pcap?CERBERUS_S3_CAPTURE_SAVED:CERBERUS_S3_CAPTURE_COMPLETE;}

int main(void)
{
    cerberus_s3_frame_status_t state;
    CHECK(cerberus_s3_frame_status(&state)==ESP_ERR_INVALID_STATE&&state.generation==0U);
    CHECK(cerberus_s3_frame_init()==ESP_ERR_NO_MEM);fail_mutex=false;CHECK(cerberus_s3_frame_init()==ESP_OK);
    CHECK(cerberus_s3_frame_init()==ESP_OK&&live==0U);
    cerberus_s3_frame_options_t options={.channel=6U,.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_RAW};
    CHECK(cerberus_s3_frame_start(NULL)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_NOT_SUPPORTED&&live==0U&&starts==0U);
    firmware=88U;peer_error=ESP_ERR_TIMEOUT;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_TIMEOUT);
    peer_error=ESP_OK;fail_alloc=true;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_NO_MEM);
    fail_alloc=false;start_error=ESP_ERR_INVALID_STATE;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_INVALID_STATE&&live==0U);
    start_error=ESP_OK;early_callback=true;CHECK(cerberus_s3_frame_start(&options)==ESP_OK);
    state=status();CHECK(state.capture.busy&&state.summary.received==1U&&live==1U);
    CHECK(!request.save_pcap&&!request.plan&&request.mode_mask==CERBERUS_WIFI_CAPTURE_MODE_RAW);
    uint32_t generation=state.generation;CHECK(state.delivered&&state.delivered_age_ms==0U);
    unsigned before=queries;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_INVALID_STATE&&queries==before);
    now_us+=500000;send(generation,1U,1U,100U,6U);state=status();
    CHECK(state.summary.received==1U&&state.summary.sequence_rejected==1U&&state.delivered_age_ms==500U);
    send(generation+1U,2U,1U,200U,6U);send(generation,2U,1U,200U,36U);
    send(generation,2U,1U,UINT64_C(0x1000000000000),6U);
    CHECK(status().rejected_provenance==3U);
    send(generation,2U,1U,99U,6U);CHECK(status().summary.clock_rejected==1U);
    send(generation,2U,1U,100U,6U);CHECK(status().summary.received==2U);
    cerberus_s3_frame_row_t rows[4];cerberus_s3_frame_page_t page;
    CHECK(cerberus_s3_frame_page(0,rows,4,&page)==ESP_OK&&page.copied==1U);
    CHECK(rows[0].first_received_age_ms==500U&&rows[0].last_received_age_ms==0U);
    now_us=999000;CHECK(status().delivered_age_ms==UINT32_MAX);
    CHECK(cerberus_s3_frame_page(0,rows,4,&page)==ESP_OK&&rows[0].last_received_age_ms==UINT32_MAX);
    now_us=2000000;
    for(unsigned i=2U;i<20U;++i){send(generation,i+1U,(uint8_t)i,100U,6U);now_us+=1000;}
    state=status();CHECK(state.summary.count==16U&&state.summary.event_replaced==3U);
    CHECK(cerberus_s3_frame_page(0,rows,4,&page)==ESP_OK&&page.total==16U&&page.copied==4U);
    CHECK(rows[0].evidence.first_sequence>2U&&rows[0].first_received_age_ms<20U);
    cerberus_s3_frame_row_t frozen=rows[0];
    send(generation,30U,55U,100U,6U);CHECK(!memcmp(&frozen,&rows[0],sizeof(frozen)));
    fail_lock=true;memset(rows,0xff,sizeof(rows));memset(&page,0xff,sizeof(page));
    CHECK(cerberus_s3_frame_page(0,rows,4,&page)==ESP_ERR_TIMEOUT&&page.copied==0U&&rows[0].generation==0U);
    CHECK(cerberus_s3_frame_status(&state)==ESP_ERR_TIMEOUT&&state.generation==0U);
    fail_lock=false;
    CHECK(cerberus_s3_frame_page(0,rows,5,&page)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_s3_frame_page(UINT16_MAX,rows,4,&page)==ESP_OK&&page.copied==0U);
    CHECK(cerberus_s3_frame_release()==ESP_ERR_INVALID_STATE&&live==1U);
    fail_lock=true;cerberus_s3_frame_stop();CHECK(stops==0U&&capture.busy);fail_lock=false;
    cerberus_s3_frame_stop();CHECK(stops==1U&&capture.state==CERBERUS_S3_CAPTURE_STOPPING);
    capture.error_code=ESP_ERR_INVALID_STATE;
    send(generation,UINT32_MAX,66U,100U,6U);CHECK(status().summary.sequence==UINT32_MAX);
    CHECK(cerberus_s3_frame_release()==ESP_ERR_INVALID_STATE);
    finish();capture.state=CERBERUS_S3_CAPTURE_ERROR;
    state=status();CHECK(!state.capture.busy&&state.capture.error_code==ESP_ERR_INVALID_STATE);
    uint32_t retained=state.summary.received;
    firmware=87U;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_NOT_SUPPORTED);
    CHECK(status().generation==generation&&status().summary.received==retained);firmware=88U;
    fail_alloc=true;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_NO_MEM);
    CHECK(status().generation==generation&&status().summary.received==retained);fail_alloc=false;
    start_error=ESP_ERR_TIMEOUT;CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_TIMEOUT);
    CHECK(status().generation==generation&&live==1U);start_error=ESP_OK;

    cerberus_wifi_capture_plan_t plan;cerberus_wifi_capture_plan_clear(&plan);
    plan.dwell_ms=CERBERUS_WIFI_CAPTURE_PLAN_RAW_DWELL_MS;
    plan.channel_mask=cerberus_wifi_capture_plan_channel_bit(6U)|cerberus_wifi_capture_plan_channel_bit(36U);
    options.channel=0U;options.plan=&plan;options.save_pcap=true;
    CHECK(cerberus_s3_frame_start(&options)==ESP_OK&&live==1U);
    generation=status().generation;CHECK(status().summary.received==0U&&!status().delivered);
    CHECK(request.plan!=&plan&&request.save_pcap&&request.hopping);
    plan.channel_mask=0U;CHECK(request.plan->channel_mask!=0U&&status().plan.channel_mask!=0U);
    send(generation,1U,1U,0U,36U);CHECK(status().summary.received==1U);
    now_us=-1;send(generation,2U,2U,0U,36U);CHECK(status().delivered_age_ms==UINT32_MAX);
    now_us=3000000;CHECK(status().delivered_age_ms==UINT32_MAX);
    CHECK(cerberus_s3_frame_page(0,rows,4,&page)==ESP_OK&&rows[1].last_received_age_ms==UINT32_MAX);
    finish();CHECK(cerberus_s3_frame_release()==ESP_OK&&live==0U);
    cerberus_wifi_capture_plan_clear(&plan);plan.channel_mask=cerberus_wifi_capture_plan_channel_bit(6U);
    options.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_PROBE;
    CHECK(cerberus_wifi_capture_plan_add_target(&plan,ap,6U));
    CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_INVALID_ARG);
    plan.target_count=0U;CHECK(cerberus_s3_frame_start(&options)==ESP_OK);finish();
    CHECK(cerberus_s3_frame_release()==ESP_OK);
    options.plan=NULL;options.channel=6U;options.filter_bssid=true;memcpy(options.bssid,ap,6U);
    CHECK(cerberus_s3_frame_start(&options)==ESP_ERR_INVALID_ARG);
    options.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_DEAUTH;finish_start=true;
    CHECK(cerberus_s3_frame_start(&options)==ESP_OK);
    CHECK(status().capture.error_code==ESP_ERR_TIMEOUT&&!status().capture.busy);
    CHECK(cerberus_s3_frame_release()==ESP_OK);
    CHECK(cerberus_s3_frame_start(&options)==ESP_OK);
    unsigned previous_stops=stops;replace_on_stop=true;cerberus_s3_frame_stop();
    CHECK(stops==previous_stops&&capture.busy);CHECK(!status().capture.busy);
    CHECK(cerberus_s3_frame_release()==ESP_OK&&capture.busy&&live==0U);
    CHECK(allocated==freed);CHECK(status().generation==0U&&!status().delivered);
    printf("Wi-Fi frame session: %u checks; optional PSRAM store %zu bytes\n",checks,store_size);
    return 0;
}
