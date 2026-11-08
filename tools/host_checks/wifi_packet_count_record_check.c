#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_packet_count_record.h"
#include "wifi_pcap.h"
#include "cerberus_protocol.h"
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"Count recorder %u: %s\n",__LINE__,#x);exit(1);}}while(0)
#define portMUX_TYPE unsigned
#define portMUX_INITIALIZER_UNLOCKED 0U
#define portENTER_CRITICAL(p) do {CHECK(!*(p));*(p)=1U;} while(0)
#define portEXIT_CRITICAL(p) do {CHECK(*(p)==1U);*(p)=0U;} while(0)
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef struct {uint64_t bytes[2];esp_err_t error[2];char file_name[2][24];} cerberus_storage_packet_count_result_t;
static unsigned allocations, tasks, closes, appends[2], ack_calls[2];
static bool oom, task_fail, sd_fail, ack_lost, corrupt_crc, bad_meta, bad_tail, mismatch;
static unsigned metadata_fault;
static bool draining, finish_automatic, pending[2];
static bool append_fail, open_fail, finish_fail, ack_mismatch, rebind_automatic, rebound;
static bool producer_staging, cancel_staging, terminal_no_data, expired_data;
static unsigned staging_ticks;
static bool monitor_case, bad_statistics, index_fail;
static unsigned index_rows;
static uint64_t sessions[2]={0x100000000000001ULL,0x200000000000001ULL}, storage_job;
static uint8_t frame[4096];
static uint16_t frame_length=91;
static unsigned open_files;
static void (*task_entry)(void *);
static void *task_argument;
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==3U);if(oom)return NULL;void *p=calloc(n,size);if(p)++allocations;return p;}
static void heap_caps_free(void *p){CHECK(p&&allocations);--allocations;free(p);}
static int xTaskCreate(void (*entry)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{CHECK(!strcmp(name,"count_sd")&&stack==6144U&&priority==4U&&!handle);if(task_fail)return 0;task_entry=entry;task_argument=arg;++tasks;return pdPASS;}
static void vTaskDelete(void *p){CHECK(!p&&tasks);--tasks;}
static int64_t esp_timer_get_time(void){static int64_t clock;return ++clock;}
static void vTaskDelay(unsigned ticks);
static esp_err_t cerberus_storage_packet_count_begin(uint64_t job,char name[8])
{CHECK(!storage_job);if(sd_fail)return ESP_FAIL;storage_job=job;strcpy(name,"P00001");return ESP_OK;}
static esp_err_t cerberus_storage_packet_count_open(uint64_t job,unsigned source,const uint8_t *data,size_t length)
{CHECK(job==storage_job&&source<2&&length==68&&data[0]==0x0a);if(sd_fail||open_fail)return ESP_FAIL;++open_files;return ESP_OK;}
static esp_err_t cerberus_storage_packet_count_append(uint64_t job,unsigned source,const uint8_t *data,size_t length)
{
    CHECK(job==storage_job&&source<2&&length>=40&&data[0]==3);
    /* Inspect the actual worker's bytes at the storage boundary. In-place
     * serialization must preserve every raw byte, including chunk/padding
     * boundaries and a full 4096-byte frame, across alternating sources. */
    size_t captured=16U+frame_length,padded=(captured+3U)&~(size_t)3U;
    CHECK(length==16U+padded);
    CHECK(cerberus_packet_count_get(data+4,4)==length);
    CHECK(cerberus_packet_count_get(data+8,4)==captured);
    CHECK(cerberus_packet_count_get(data+length-4U,4)==length);
    CHECK(cerberus_packet_count_get(data+14,2)==16U);
    CHECK(cerberus_packet_count_get(data+22,2)==(source?5180U:2437U));
    CHECK(cerberus_packet_count_get(data+24,2)==(source?0x100U:0x80U));
    CHECK(data[26]==(uint8_t)-49);
    CHECK(!memcmp(data+28,frame,frame_length));
    for(size_t i=12U+captured;i<length-4U;++i)CHECK(data[i]==0);
    ++appends[source];return sd_fail||append_fail?ESP_FAIL:ESP_OK;
}
static esp_err_t cerberus_storage_packet_monitor_begin(uint64_t job,char name[8])
{ CHECK(monitor_case);esp_err_t e=cerberus_storage_packet_count_begin(job,name);if(!e)strcpy(name,"M00001");return e; }
static esp_err_t cerberus_storage_packet_monitor_append(uint64_t job,uint64_t session,uint32_t sequence,
    uint64_t source_us,uint8_t channel,int8_t rssi,bool known,uint16_t bytes,const uint8_t *record,size_t length)
{
    CHECK(monitor_case&&session==sessions[1]&&sequence==1&&source_us==60000&&channel==36);
    CHECK(rssi==-49&&known&&bytes==frame_length);++index_rows;
    esp_err_t e=cerberus_storage_packet_count_append(job,1,record,length);
    return index_fail?ESP_FAIL:e;
}
static esp_err_t cerberus_storage_packet_count_finish(uint64_t job,cerberus_storage_packet_count_result_t *out)
{CHECK(job==storage_job);memset(out,0,sizeof(*out));out->bytes[0]=68+appends[0]*100;out->bytes[1]=68+appends[1]*100;strcpy(out->file_name[0],"S3.PCAPNG");strcpy(out->file_name[1],"C5.PCAPNG");storage_job=0;open_files=0;++closes;return finish_fail?ESP_FAIL:ESP_OK;}
static esp_err_t exchange(void *ctx,unsigned source,uint8_t command,const void *q,size_t n,void *out,size_t size)
{
    CHECK(!ctx&&source<2);
    uint64_t id;memcpy(&id,q,8);CHECK(id==sessions[source]);
    if(command==CERBERUS_PACKET_COUNT_COMMAND_PEEK){
        CHECK(n==sizeof(cerberus_packet_count_peek_t)&&size==22);
        cerberus_packet_count_control_t *r=out;
        r->session_id=mismatch?id+1:id;r->flags=CERBERUS_PACKET_COUNT_BOUND|CERBERUS_PACKET_COUNT_RECORDING;
        if(((const cerberus_packet_count_peek_t *)q)->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY){
            CHECK(monitor_case&&draining&&!pending[source]);r->state=CERBERUS_PACKET_COUNT_COMPLETE;
            r->sequence=CERBERUS_PACKET_COUNT_PIN_BASE+3;return ESP_OK;
        }
        if(producer_staging||terminal_no_data){
            r->state=producer_staging?(source?CERBERUS_PACKET_COUNT_QUEUED:CERBERUS_PACKET_COUNT_STAGING):CERBERUS_PACKET_COUNT_COMPLETE;
            if(producer_staging)r->flags|=CERBERUS_PACKET_COUNT_HELD;
            r->error_code=ESP_ERR_INVALID_STATE;return ESP_OK;
        }
        r->flags|=CERBERUS_PACKET_COUNT_DATA_READY;
        r->state=draining?CERBERUS_PACKET_COUNT_COMPLETE:CERBERUS_PACKET_COUNT_RUNNING;
        r->sequence=pending[source]?CERBERUS_PACKET_COUNT_PIN_BASE+2:0;
        r->error_code=pending[source]?ESP_ERR_TIMEOUT:ESP_ERR_NOT_FOUND; /* retained runtime error doesn't hide data */
    }else if(command==CERBERUS_PACKET_COUNT_COMMAND_STATUS){
        CHECK(n==8&&size==22&&terminal_no_data);
        cerberus_packet_count_control_t *r=out;r->session_id=id;r->sequence=expired_data?55:0;
        r->state=CERBERUS_PACKET_COUNT_COMPLETE;r->flags=CERBERUS_PACKET_COUNT_BOUND|CERBERUS_PACKET_COUNT_RECORDING;
    }else if(command==CERBERUS_PACKET_COUNT_COMMAND_READ){
        CHECK(n==14&&size==22);
        const cerberus_packet_count_read_t *request=q;cerberus_packet_count_chunk_t *r=out;
        r->session_id=id;r->pin=request->pin;r->part=request->part;
        if(monitor_case&&request->pin==CERBERUS_PACKET_COUNT_PIN_BASE+3){
            CHECK(request->part<8);uint8_t stats[64]={1,0,0,2};
            cerberus_packet_count_put(stats+4,1,4);cerberus_packet_count_put(stats+8,1,4);
            cerberus_packet_count_put(stats+12,7,4);cerberus_packet_count_put(stats+16,2,4);
            cerberus_packet_count_put(stats+36,1,4);if(bad_statistics)stats[60]=1;
            memcpy(r->data,stats+request->part*8,8);return ESP_OK;
        }
        CHECK(request->part<4);
        uint8_t bytes[32]={0};
        cerberus_packet_count_put(bytes,1,4);cerberus_packet_count_put(bytes+4,bad_meta?5000:frame_length,2);
        cerberus_packet_count_put(bytes+6,frame_length,2);cerberus_packet_count_put(bytes+8,60000,8);
        cerberus_packet_count_put(bytes+16,cerberus_protocol_crc32(frame,frame_length)+(corrupt_crc?1:0),4);
        bytes[20]=source?36:6;bytes[21]=(uint8_t)-49;bytes[22]=3;
        if(metadata_fault==1)bytes[21]=128;
        if(metadata_fault==2)bytes[22]=1;
        if(metadata_fault==3)bytes[0]=3;
        memcpy(r->data,bytes+request->part*8,8);
    }else if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_READ){
        CHECK(n==14&&size==54);const cerberus_packet_count_read_t *request=q;cerberus_packet_count_raw_chunk_t *r=out;
        r->session_id=id;r->pin=request->pin;r->part=request->part;
        size_t offset=request->part*40U;CHECK(offset<frame_length);size_t length=frame_length-offset;if(length>40)length=40;
        memcpy(r->data,frame+offset,length);if(bad_tail&&length<40)r->data[length]=1;
    }else if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK){
        CHECK(n==12&&size==22);const cerberus_packet_count_ack_t *request=q;CHECK(request->sequence==1);
        cerberus_packet_count_control_t *r=out;r->session_id=id;r->sequence=1;r->flags=CERBERUS_PACKET_COUNT_BOUND;
        r->error_code=ESP_ERR_TIMEOUT;pending[source]=false;++ack_calls[source];
        if(ack_mismatch)r->sequence=2;
        if(ack_lost&&ack_calls[source]==1)return ESP_ERR_TIMEOUT;
    }else CHECK(false);
    return ESP_OK;
}
#include "wifi_packet_count_record_impl.h"
static void vTaskDelay(unsigned ticks)
{
    CHECK(ticks==10);
    if(!finish_automatic)return;
    uint64_t job=packet_count_record_public.job;
    if(!packet_count_record_public.source[1].session){
        if(!monitor_case)CHECK(wifi_packet_count_record_bind(job,0,sessions[0])==ESP_OK);
        CHECK(wifi_packet_count_record_bind(job,1,sessions[1])==ESP_OK);
        return;
    }
    if(producer_staging){
        if(++staging_ticks<12)return;
        producer_staging=false;
        if(cancel_staging){terminal_no_data=true;pending[0]=pending[1]=false;}
    }
    draining=true;
    bool a=monitor_case||wifi_packet_count_record_drained(job,0,sessions[0]);
    bool b=wifi_packet_count_record_drained(job,1,sessions[1]);
    if(a&&b){
        if(rebind_automatic&&!rebound){
            rebound=true;draining=false;
            for(unsigned i=0;i<2;++i){
                CHECK(wifi_packet_count_record_bind(job,i,sessions[i])==ESP_ERR_INVALID_STATE);
                ++sessions[i];pending[i]=true;
                CHECK(wifi_packet_count_record_bind(job,i,sessions[i])==ESP_OK);
            }
        }else CHECK(wifi_packet_count_record_finish(job)==ESP_OK);
    }
}
static void reset(void)
{
    CHECK(!allocations&&!tasks&&!storage_job&&!packet_count_record_public.job);
    oom=task_fail=sd_fail=ack_lost=corrupt_crc=bad_meta=bad_tail=mismatch=draining=false;
    finish_automatic=true;pending[0]=pending[1]=true;
    metadata_fault=0;
    monitor_case=bad_statistics=index_fail=false;index_rows=0;
    append_fail=open_fail=finish_fail=ack_mismatch=rebind_automatic=rebound=false;
    producer_staging=cancel_staging=terminal_no_data=expired_data=false;staging_ticks=0;
    memset(appends,0,sizeof(appends));memset(ack_calls,0,sizeof(ack_calls));closes=0;frame_length=91;
    for(unsigned i=0;i<sizeof(frame);++i)frame[i]=(uint8_t)(i*13U+7U);
}
static void recording_extent(uint16_t length,bool lost_ack)
{
    reset();frame_length=length;ack_lost=lost_ack;
    wifi_packet_count_transport_t transport={.exchange=exchange};uint64_t job;
    CHECK(wifi_packet_count_record_start(&transport,&job)==ESP_OK);
    task_entry(task_argument);
    wifi_packet_count_record_status_t result;
    CHECK(wifi_packet_count_record_status(job,&result)&&!result.busy&&!result.error);
    CHECK(!allocations&&!tasks&&!storage_job);
    for(unsigned source=0;source<2;++source){
        CHECK(appends[source]==1&&result.source[source].written==1);
        CHECK(!result.source[source].discarded&&!result.source[source].loss_unknown);
        CHECK(ack_calls[source]==(lost_ack?2U:1U));
    }
    CHECK(wifi_packet_count_record_release(job)==ESP_OK);
}
int main(void)
{
    wifi_packet_count_transport_t transport={.exchange=exchange};uint64_t job=0;
    reset();oom=true;CHECK(wifi_packet_count_record_start(&transport,&job)==ESP_ERR_NO_MEM&&!job);oom=false;
    task_fail=true;CHECK(wifi_packet_count_record_start(&transport,&job)==ESP_ERR_NO_MEM&&!job);task_fail=false;
    for(unsigned fault=0;fault<19;++fault){
        reset();ack_lost=fault==1;corrupt_crc=fault==2;bad_meta=fault==3;bad_tail=fault==4;mismatch=fault==5;sd_fail=fault==6;
        if(fault==7)frame_length=4096;
        if(fault>=8&&fault<=10)metadata_fault=fault-7;
        append_fail=fault==11;open_fail=fault==12;finish_fail=fault==13;ack_mismatch=fault==14;rebind_automatic=fault==15;
        producer_staging=fault>=16;cancel_staging=fault>=17;expired_data=fault==18;
        CHECK(wifi_packet_count_record_start(&transport,&job)==ESP_OK&&job);
        uint64_t other;CHECK(wifi_packet_count_record_start(&transport,&other)==ESP_ERR_INVALID_STATE);
        CHECK(wifi_packet_count_record_bind(job+1,0,sessions[0])==ESP_ERR_INVALID_STATE);
        CHECK(wifi_packet_count_record_release(job)==ESP_ERR_INVALID_STATE);
        CHECK(!wifi_packet_count_record_drained(job+1,0,sessions[0]));
        task_entry(task_argument);
        wifi_packet_count_record_status_t result;CHECK(wifi_packet_count_record_status(job,&result));
        CHECK(!result.busy&&!allocations&&!tasks&&!storage_job);
        if(fault==0||fault==1||fault==7||fault==13||fault==14||fault==16){
            CHECK(appends[0]==1&&appends[1]==1&&result.source[0].written==1&&result.source[1].written==1);
            CHECK(ack_calls[0]==(fault==1?2U:fault==14?3U:1U)&&ack_calls[1]==ack_calls[0]);
        }else if(fault==11)CHECK(appends[0]==1&&appends[1]==1&&!result.source[0].written&&!result.source[1].written);
        else if(fault==15)CHECK(rebound&&appends[0]==2&&appends[1]==2&&result.source[0].written==2&&result.source[1].written==2);
        else CHECK(!appends[0]&&!appends[1]);
        if(fault==2)CHECK(result.source[0].discarded==1&&result.source[1].discarded==1);
        if(fault>=3&&fault<=5)CHECK(result.source[0].loss_unknown&&result.source[1].loss_unknown);
        if((fault>=8&&fault<=10)||fault==14)CHECK(result.source[0].loss_unknown&&result.source[1].loss_unknown);
        if(fault==11||fault==12)CHECK(result.source[0].discarded==1&&result.source[1].discarded==1&&ack_calls[0]==1&&ack_calls[1]==1);
        if(fault==13)CHECK(result.error==ESP_FAIL&&closes==1);
        if(fault==16||fault==17)CHECK(staging_ticks==12&&!result.source[0].transport_errors&&!result.source[1].transport_errors&&!result.source[0].loss_unknown&&!result.source[1].loss_unknown);
        if(fault==18)CHECK(result.source[0].loss_unknown&&result.source[1].loss_unknown&&!result.source[0].written);
        CHECK(wifi_packet_count_record_release(job+1)==ESP_ERR_INVALID_STATE);
        CHECK(wifi_packet_count_record_release(job)==ESP_OK);
    }
    for(uint16_t length=24;length<=96;++length)recording_extent(length,(length&1U)!=0);
    for(uint16_t boundary=120;boundary<4096;boundary+=40){
        recording_extent(boundary-1U,false);
        recording_extent(boundary,true);
        recording_extent(boundary+1U,false);
    }
    recording_extent(4095,true);recording_extent(4096,true);
    for(unsigned fault=0;fault<5;++fault){
        reset();monitor_case=true;ack_lost=fault==1;bad_statistics=fault==2;
        index_fail=fault==3;sd_fail=fault==4;
        CHECK(wifi_packet_count_record_start_monitor(&transport,&job)==ESP_OK);
        task_entry(task_argument);
        wifi_packet_count_record_status_t result;CHECK(wifi_packet_count_record_status(job,&result));
        CHECK(!result.busy&&!allocations&&!tasks&&!storage_job&&!appends[0]);
        if(fault!=4){
            CHECK(index_rows==1&&appends[1]==1&&!strcmp(result.directory,"M00001"));
            CHECK(result.source[1].statistics_valid==(fault!=2));
            if(fault!=2)CHECK(result.source[1].received==1&&result.source[1].acknowledged==1&&
                result.source[1].queue_full==7&&result.source[1].rejected==2);
            if(fault==2)CHECK(result.source[1].loss_unknown&&result.source[1].error!=ESP_OK);
            if(fault==3)CHECK(result.source[1].written==0&&result.source[1].discarded==1&&result.source[1].error==ESP_FAIL);
        }else CHECK(!index_rows&&result.error==ESP_FAIL);
        CHECK(wifi_packet_count_record_release(job)==ESP_OK);
    }
    printf("Packet Count recorder PASS %u checks; worker context %zu bytes\n",checks,sizeof(packet_count_record_context_t));
    return 0;
}
