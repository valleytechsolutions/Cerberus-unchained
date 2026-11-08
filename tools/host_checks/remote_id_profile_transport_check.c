#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "c5_spi_link.h"
#include "cerberus_watch_capture.h"
static unsigned checks;
#define CHECK(c) do { ++checks; assert(c); } while(0)
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
static int64_t clock_us,c5_clock_us;
static bool locked,critical,fail_lock;
static int mutex_token,defense_capture_mux;
static void *link_mutex=&mutex_token;
static uint8_t sequence;
static unsigned exchanges,peer_reads,legacy_reads,damage,damage_chunk,delay_us;
static esp_err_t wire_error;
static cerberus_c5_status_t peer;
static cerberus_watch_capture_t watch_capture;
static int64_t esp_timer_get_time(void){return clock_us;}
static int64_t c5_now(void){return c5_clock_us;}
static void enter(int *p){CHECK(p==&defense_capture_mux&&!critical);critical=true;}
static void leave(int *p){CHECK(p==&defense_capture_mux&&critical);critical=false;}
#define portENTER_CRITICAL(p) enter(p)
#define portEXIT_CRITICAL(p) leave(p)
#define esp_timer_get_time c5_now
#include "profile_producer.h"
#undef esp_timer_get_time
static cerberus_spi_frame_t response;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t token,const void *bytes,size_t size)
{CHECK(cerberus_protocol_prepare_frame(&response,command,token,bytes,size));}
static void dispatch(const cerberus_spi_frame_t *request)
{
    memset(&response,0,sizeof(response));
    switch(request->command){
#include "profile_dispatch.h"
    default:CHECK(false);
    }
}
static int take_link_mutex(unsigned ticks)
{CHECK(!locked&&ticks>0U&&ticks<=20U);if(fail_lock)return 0;locked=true;return 1;}
static void give_link_mutex(void){CHECK(locked);locked=false;}
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,cerberus_c5_status_t *out)
{
    CHECK(locked&&token==sequence&&deadline-(uint32_t)(clock_us/1000U)<=20U);
    ++exchanges;++peer_reads;*out=peer;clock_us+=delay_us;
    if(peer_reads==2U&&damage>=30U&&damage<=35U){
        if(damage==30U)++out->boot_count;
        if(damage==31U)++out->firmware_version;
        if(damage==32U)++out->protocol_version;
        if(damage==33U)++out->reset_reason;
        if(damage==34U)++out->capabilities;
        if(damage==35U)--out->uptime_ms;
    }
    return wire_error;
}
static esp_err_t spi_request_response_passive(const void *input,void *discard,void *output,size_t size,uint32_t deadline)
{
    (void)discard;CHECK(locked&&size==sizeof(cerberus_spi_frame_t));
    CHECK(deadline-(uint32_t)(clock_us/1000U)<=20U);++exchanges;
    const cerberus_spi_frame_t *request=input;CHECK(cerberus_protocol_frame_is_valid(request));
    dispatch(request);clock_us+=delay_us;
    if(request->command==CERBERUS_SPI_COMMAND_WATCH_PROFILE_READ&&
       ((const cerberus_watch_profile_read_request_t *)request->payload)->chunk==damage_chunk){
        if(damage==1U)response.crc16^=1U;
        else if(damage==2U)++response.sequence;
        else if(damage==3U)++response.command;
        else if(damage==4U)--response.payload_length;
        else if(damage==5U)response.payload[0]^=1U;
        else if(damage==6U)response.payload[4]++;
        else if(damage==7U)response.payload[5]--;
        if(damage>=2U&&damage<=7U)response.crc16=cerberus_protocol_crc16(&response,offsetof(cerberus_spi_frame_t,crc16));
    }
    *(cerberus_spi_frame_t *)output=response;
    return wire_error;
}
esp_err_t c5_spi_link_get_watch_detailed_result(uint8_t radio,uint8_t generation,uint8_t index,
    cerberus_watch_observation_t *row,cerberus_watch_receive_timing_t *timing,
    sentinel_watch_motion_t *motion,cerberus_watch_receive_timing_t *motion_timing)
{
    CHECK(!locked&&radio==CERBERUS_WATCH_RADIO_WIFI&&generation==watch_capture.status.generation&&index==0U);
    ++legacy_reads;*row=watch_capture.records[0];memset(timing,0,sizeof(*timing));
    memset(motion,0,sizeof(*motion));memset(motion_timing,0,sizeof(*motion_timing));return wire_error;
}
#include "profile_validation.h"
#include "c5_watch_profile_link.h"
static bool zero(const void *data,size_t size)
{const uint8_t *p=data;for(size_t i=0;i<size;++i)if(p[i])return false;return true;}
static void seed(uint64_t epoch)
{
    cerberus_watch_capture_begin_with_epoch(&watch_capture,CERBERUS_WATCH_RADIO_WIFI,7U,0U,epoch);
    uint8_t address[6]={2,4,6,8,10,12};
    sentinel_watch_protocol_record_t r={.kind=SENTINEL_WATCH_PROTOCOL_DRONE,
        .transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON,.flags=SENTINEL_WATCH_PROTOCOL_HAS_ID,.id="TEST"};
    r.profile.present=1U;r.profile.version[0]=2U;r.profile.basic[0]=0x10;
    memcpy(r.profile.basic+1,"TEST",4U);
    cerberus_watch_capture_observe_timed(&watch_capture,&r,address,0U,-60,149U,1000001U);
    memset(&r,0,sizeof(r));r.kind=SENTINEL_WATCH_PROTOCOL_DRONE;r.transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON;
    r.profile.present=1U<<SENTINEL_WATCH_PROFILE_SELF;r.profile.version[SENTINEL_WATCH_PROFILE_SELF]=2;
    memcpy(r.profile.self+1,"FLIGHT",6U);
    cerberus_watch_capture_observe_timed(&watch_capture,&r,address,0U,-61,149U,2000001U);
    memset(&r,0,sizeof(r));r.kind=SENTINEL_WATCH_PROTOCOL_DRONE;r.transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON;
    r.profile.present=1U<<SENTINEL_WATCH_PROFILE_LOCATION;r.profile.version[SENTINEL_WATCH_PROFILE_LOCATION]=2;
    r.motion.flags=SENTINEL_WATCH_MOTION_LOCATION_PRESENT;
    cerberus_watch_capture_observe_timed(&watch_capture,&r,address,0U,-62,149U,2500001U);
    cerberus_watch_capture_finish(&watch_capture,CERBERUS_WATCH_COMPLETE,3000U);
    CHECK(watch_capture.status.stored_count==1U);
}
static void reset(void)
{
    CHECK(!locked&&!critical);clock_us=10000000;c5_clock_us=3000002;
    exchanges=peer_reads=legacy_reads=damage=damage_chunk=delay_us=0U;
    wire_error=ESP_OK;fail_lock=false;link_mutex=&mutex_token;sequence=0U;
    peer=(cerberus_c5_status_t){.firmware_version=89,.protocol_version=CERBERUS_PROTOCOL_VERSION,
        .capabilities=CERBERUS_C5_CAP_WATCH_OBSERVATIONS,.boot_count=2,.uptime_ms=3000};
    watch_profile_serial=watch_profile_current=0U;memset(&watch_profile_projection,0,sizeof(watch_profile_projection));
    seed(UINT64_C(0x100000007));
}
static void begin(cerberus_c5_watch_profile_reader_t *r,uint64_t epoch)
{CHECK(c5_spi_link_watch_profile_begin(r,&peer,CERBERUS_WATCH_RADIO_WIFI,7U,0U,epoch,20000U)==ESP_OK);}
static esp_err_t finish(cerberus_c5_watch_profile_reader_t *r,cerberus_c5_watch_profile_result_t *out)
{
    bool complete=false;esp_err_t error=ESP_OK;
    for(unsigned i=0;i<30U&&!complete&&error==ESP_OK;++i){
        unsigned before=exchanges;memset(out,0xa5,sizeof(*out));
        error=c5_spi_link_watch_profile_step(r,out,&complete);
        CHECK(exchanges-before<=1U&&!locked&&!critical);
        if(!complete)CHECK(zero(out,sizeof(*out)));
    }
    CHECK(error!=ESP_OK||complete);return error;
}
static void complete_and_retry(void)
{
    reset();cerberus_c5_watch_profile_reader_t r;cerberus_c5_watch_profile_result_t out;
    begin(&r,0U);CHECK(finish(&r,&out)==ESP_OK&&exchanges==17U&&!legacy_reads);
    CHECK(out.profile.available&&out.profile.capture_epoch==UINT64_C(0x100000007));
    CHECK(out.profile.values.present==11U&&out.profile.timing[0].source_age_ms==2001U);
    CHECK(out.profile.timing[3].source_age_ms==1001U&&out.profile.timing[1].source_age_ms==501U);
    CHECK(!out.profile.timing[2].valid&&out.motion_timing.source_age_ms==501U&&out.timing.source_age_ms==501U);
    CHECK(out.profile.timing[0].request_started_us==10000000U&&cerberus_watch_profile_evidence_valid(&out.profile,10000U));
    cerberus_watch_profile_read_request_t q={r.capture_epoch,r.projection,r.radio,r.generation,r.index,0};
    cerberus_watch_profile_read_payload_t a,b;
    CHECK(cerberus_wifi_watch_profile_read(&q,&a)==ESP_OK);c5_clock_us+=1000000;
    CHECK(cerberus_wifi_watch_profile_read(&q,&b)==ESP_OK&&!memcmp(&a,&b,sizeof(a)));
    unsigned count=exchanges;begin(&r,out.profile.capture_epoch);CHECK(finish(&r,&out)==ESP_OK);
    CHECK(exchanges==count+17U&&out.profile.timing[0].source_age_ms==3001U&&r.projection==2U);
    CHECK(cerberus_wifi_watch_profile_read(&q,&a)==ESP_ERR_INVALID_STATE&&zero(&a,sizeof(a)));
    q.projection=r.projection;q.capture_epoch++;
    CHECK(cerberus_wifi_watch_profile_read(&q,&a)==ESP_ERR_INVALID_STATE);
    q.capture_epoch=r.capture_epoch;q.index=1U;
    CHECK(cerberus_wifi_watch_profile_read(&q,&a)==ESP_ERR_INVALID_STATE);
    q.index=0U;q.chunk=14U;CHECK(cerberus_wifi_watch_profile_read(&q,&a)==ESP_ERR_INVALID_ARG);
}
static void faults_and_turnover(void)
{
    cerberus_c5_watch_profile_reader_t r;cerberus_c5_watch_profile_result_t out;bool done;
    for(unsigned defect=1U;defect<=7U;++defect){reset();damage=defect;damage_chunk=6U;begin(&r,0U);
        CHECK(finish(&r,&out)==ESP_ERR_INVALID_RESPONSE&&zero(&out,sizeof(out)));}
    for(unsigned defect=30U;defect<=35U;++defect){reset();damage=defect;begin(&r,0U);
        CHECK(finish(&r,&out)==ESP_ERR_INVALID_RESPONSE);}
    reset();begin(&r,0U);CHECK(c5_spi_link_watch_profile_step(&r,&out,&done)==ESP_OK);
    CHECK(c5_spi_link_watch_profile_step(&r,&out,&done)==ESP_OK);seed(UINT64_C(0x200000007));
    CHECK(finish(&r,&out)==ESP_ERR_INVALID_STATE); /* same gen8, different epoch */
    reset();begin(&r,UINT64_C(0x200000007));CHECK(finish(&r,&out)==ESP_ERR_INVALID_STATE);
    reset();begin(&r,0U);wire_error=ESP_ERR_TIMEOUT;CHECK(finish(&r,&out)==ESP_ERR_TIMEOUT);
    reset();begin(&r,0U);delay_us=20000U;CHECK(finish(&r,&out)==ESP_ERR_TIMEOUT);
    reset();begin(&r,0U);fail_lock=true;CHECK(finish(&r,&out)==ESP_ERR_TIMEOUT&&!exchanges);
    reset();begin(&r,0U);CHECK(c5_spi_link_watch_profile_step(&r,&out,&done)==ESP_OK);
    --clock_us;CHECK(finish(&r,&out)==ESP_ERR_INVALID_STATE);
    reset();begin(&r,0U);clock_us=20000000;CHECK(finish(&r,&out)==ESP_ERR_TIMEOUT&&!exchanges);
    reset();begin(&r,0U);c5_spi_link_watch_profile_cancel(&r);CHECK(finish(&r,&out)==ESP_ERR_NOT_FINISHED&&!exchanges);
    reset();watch_profile_serial=UINT32_MAX;begin(&r,0U);CHECK(finish(&r,&out)==ESP_ERR_INVALID_STATE);
    reset();watch_capture.status.state=CERBERUS_WATCH_RUNNING;begin(&r,0U);CHECK(finish(&r,&out)==ESP_ERR_INVALID_STATE);
    reset();c5_clock_us=-1;begin(&r,0U);CHECK(finish(&r,&out)==ESP_OK&&out.profile.available);
    CHECK(!out.timing.valid&&!out.motion_timing.valid);
    for(unsigned i=0;i<6U;++i)CHECK(!out.profile.timing[i].valid);
    reset();peer.firmware_version=88U;begin(&r,0U);CHECK(finish(&r,&out)==ESP_OK&&legacy_reads==1U&&exchanges==2U);
    CHECK(zero(&out.profile,sizeof(out.profile))&&!strcmp(out.observation.id,"TEST"));
    reset();peer.protocol_version++;begin(&r,0U);CHECK(finish(&r,&out)==ESP_ERR_INVALID_RESPONSE&&!legacy_reads);
}
static void invalid_images(void)
{
    for(unsigned defect=0;defect<9U;++defect){
        reset();cerberus_c5_watch_profile_reader_t r;cerberus_c5_watch_profile_result_t out;bool done;
        begin(&r,0U);CHECK(c5_spi_link_watch_profile_step(&r,&out,&done)==ESP_OK);
        CHECK(c5_spi_link_watch_profile_step(&r,&out,&done)==ESP_OK);
        if(defect==0U)watch_profile_projection.profile.reserved[0]=1U;
        if(defect==1U)watch_profile_projection.group_valid_mask|=4U;
        if(defect==2U)watch_profile_projection.group_age_ms[0]=UINT32_MAX;
        if(defect==3U)watch_profile_projection.group_age_ms[2]=0U;
        if(defect==4U)watch_profile_projection.timing_flags|=4U;
        if(defect==5U)watch_profile_projection.motion.flags=0U;
        if(defect==6U)watch_profile_projection.observation.transport=SENTINEL_WATCH_TRANSPORT_BLE;
        if(defect==7U)watch_profile_projection.timing_flags&=~CERBERUS_WATCH_PROFILE_SOURCE_TIMED;
        if(defect==8U)watch_profile_projection.source_age_ms=UINT64_MAX;
        CHECK(finish(&r,&out)==ESP_ERR_INVALID_RESPONSE&&zero(&out,sizeof(out)));
    }
}
static void all_groups_and_boundaries(void)
{
    reset();cerberus_watch_capture_begin_with_epoch(&watch_capture,CERBERUS_WATCH_RADIO_WIFI,7U,0U,99U);
    uint8_t address[6]={2,4,6,8,10,12};
    for(unsigned group=0;group<6U;++group){
        sentinel_watch_protocol_record_t p={.kind=SENTINEL_WATCH_PROTOCOL_DRONE,
            .transport=SENTINEL_WATCH_TRANSPORT_WIFI_NAN};
        p.profile.present=(uint8_t)(1U<<group);p.profile.version[group]=2U;
        if(group==0U){p.profile.basic[0]=0x10;memcpy(p.profile.basic+1,"AIRCRAFT",8U);
            p.flags=SENTINEL_WATCH_PROTOCOL_HAS_ID;memcpy(p.id,"AIRCRAFT",8U);}
        if(group==1U)p.motion.flags=SENTINEL_WATCH_MOTION_LOCATION_PRESENT;
        if(group==2U)p.profile.auth[2]=1U;
        if(group==3U)memcpy(p.profile.self+1,"SELF",4U);
        if(group==5U)memcpy(p.profile.operator_id+1,"OPERATOR",8U);
        CHECK(sentinel_watch_profile_valid(&p.profile));
        cerberus_watch_capture_observe_timed(&watch_capture,&p,address,0U,-60,6U,
            (uint64_t)(group+1U)*1000000U+1U);
    }
    cerberus_watch_capture_finish(&watch_capture,CERBERUS_WATCH_COMPLETE,7000U);c5_clock_us=7000002U;
    cerberus_c5_watch_profile_reader_t r;cerberus_c5_watch_profile_result_t out;
    begin(&r,99U);CHECK(finish(&r,&out)==ESP_OK&&out.profile.values.present==63U);
    for(unsigned group=0;group<6U;++group)
        CHECK(out.profile.timing[group].valid&&out.profile.timing[group].source_age_ms==(6U-group)*1000U+1U);
    CHECK(out.motion_timing.source_age_ms==5001U&&out.timing.source_age_ms==1001U);
    CHECK(out.profile.values.auth[2]==1U&&sentinel_watch_profile_zero(out.profile.values.auth+3,4U));
    reset();cerberus_watch_capture_begin_with_epoch(&watch_capture,CERBERUS_WATCH_RADIO_WIFI,7U,0U,100U);
    sentinel_watch_protocol_record_t p={.kind=SENTINEL_WATCH_PROTOCOL_PWNAGOTCHI,
        .transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON,.flags=SENTINEL_WATCH_PROTOCOL_HAS_ID,.id="PWN"};
    cerberus_watch_capture_observe_timed(&watch_capture,&p,address,0U,-50,1U,100U);
    cerberus_watch_capture_finish(&watch_capture,CERBERUS_WATCH_COMPLETE,1U);
    begin(&r,100U);CHECK(finish(&r,&out)==ESP_OK&&out.profile.available&&!out.profile.values.present);
    for(unsigned i=0;i<6U;++i)CHECK(!out.profile.timing[i].valid);
    reset();watch_profile_serial=UINT32_MAX-1U;begin(&r,0U);
    CHECK(finish(&r,&out)==ESP_OK&&r.projection==UINT32_MAX);
    cerberus_watch_profile_read_request_t q={r.capture_epoch,r.projection,r.radio,r.generation,r.index,0U};
    cerberus_watch_profile_read_payload_t chunk;
    begin(&r,0U);CHECK(finish(&r,&out)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_wifi_watch_profile_read(&q,&chunk)==ESP_OK&&chunk.projection==UINT32_MAX);
    reset();begin(&r,0U);wire_error=ESP_ERR_NOT_SUPPORTED;
    CHECK(finish(&r,&out)==ESP_ERR_INVALID_RESPONSE&&!legacy_reads);
}
int main(void)
{
    complete_and_retry();faults_and_turnover();invalid_images();all_groups_and_boundaries();
    printf("Remote ID C5 profile -> actual dispatcher -> bounded S3 reader: %u checks passed; 224B copied image, 14 chunks, six independent ages, epoch/peer/projection fencing, retry/clock/failure/old-peer behavior.\n",checks);
    return 0;
}
