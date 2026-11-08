#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cerberus_protocol.h"
#include "esp_err.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef struct {uint32_t firmware_version;} cerberus_c5_status_t;
static unsigned checks, calls, refreshes, widths[65];
#define CHECK(x) do {++checks;assert(x);}while(0)
static int link_mutex=1,locked,mutex_ready=1;
static uint8_t sequence,peer_firmware_version=74,firmware=74;
static int64_t clock_us=100000,delay;
static bool wrong_crc,wrong_token,wrong_session,wrong_part,timeout;
static uint64_t session=0x123456789abcdefULL;
static int64_t esp_timer_get_time(void){return clock_us;}
static int take_link_mutex(unsigned wait){CHECK(wait==20&&!locked);locked=mutex_ready;return locked;}
static void give_link_mutex(void){CHECK(locked);locked=0;}
static esp_err_t get_status_v2_passive_locked(uint8_t seq,uint32_t deadline,cerberus_c5_status_t *out)
{(void)seq;CHECK(locked&&deadline==(uint32_t)(clock_us/1000)+20);++refreshes;out->firmware_version=firmware;return ESP_OK;}
static esp_err_t survey_session_frame_locked(cerberus_spi_command_t command,const void *q,size_t n,void *out,size_t size,uint32_t deadline)
{
    (void)deadline;CHECK(locked&&size==22);++calls;++widths[32];
    cerberus_packet_count_control_t r={.session_id=command==161?session-1:session,.flags=CERBERUS_PACKET_COUNT_BOUND};
    if(n){uint64_t requested;memcpy(&requested,q,8);CHECK(requested==session);}
    memcpy(out,&r,size);clock_us+=delay;return timeout?ESP_ERR_TIMEOUT:ESP_OK;
}
static esp_err_t spi_request_response_passive(const void *data,void *discarded,void *out,size_t size,uint32_t deadline)
{
    (void)discarded;(void)deadline;CHECK(locked&&size==64);++calls;++widths[size];
    const cerberus_spi_frame_t *q=data;
    CHECK(cerberus_protocol_frame_is_valid(q)&&q->command==170&&q->payload_length==14);
    for(unsigned i=32;i<64;++i)CHECK(((const uint8_t*)data)[i]==0);
    cerberus_packet_count_read_t read;memcpy(&read,q->payload,sizeof(read));
    cerberus_packet_count_raw_chunk_t payload={.session_id=read.session_id+(wrong_session?1:0),.pin=read.pin,.part=(uint16_t)(read.part+(wrong_part?1:0))};
    memset(payload.data,0xab,sizeof(payload.data));
    cerberus_packet_count_raw_frame_t *r=out;
    CHECK(cerberus_protocol_prepare_packet_count_raw(r,q->sequence+(wrong_token?1:0),&payload));
    if(wrong_crc)r->crc16^=1;
    clock_us+=delay;return timeout?ESP_ERR_TIMEOUT:ESP_OK;
}
#include "c5_wifi_packet_count_link_impl.h"
int main(void)
{
    cerberus_packet_count_control_t control;
    CHECK(c5_spi_link_wifi_packet_count_exchange(161,NULL,0,&control,22)==ESP_OK);
    CHECK(refreshes==1&&calls==1);
    cerberus_packet_count_begin_t begin={.session_id=session,.channel_mask=1,.target_count=1,.dwell_ms=100};
    CHECK(c5_spi_link_wifi_packet_count_exchange(162,&begin,sizeof(begin),&control,22)==ESP_OK);
    CHECK(refreshes==2&&calls==2);
    cerberus_packet_count_read_t q={session,5,102};
    cerberus_packet_count_raw_chunk_t raw,zero={0};
    CHECK(c5_spi_link_wifi_packet_count_exchange(170,&q,14,&raw,54)==ESP_OK);
    CHECK(widths[64]==1&&raw.pin==5&&raw.part==102&&raw.data[39]==0xab);
    for(unsigned fault=0;fault<7;++fault){
        wrong_crc=fault==0;wrong_token=fault==1;wrong_session=fault==2;wrong_part=fault==3;timeout=fault==4;delay=fault==5?20000:fault==6?-1:0;
        unsigned before=calls;
        CHECK(c5_spi_link_wifi_packet_count_exchange(170,&q,14,&raw,54)!=ESP_OK);
        CHECK(calls==before+1&&!memcmp(&raw,&zero,sizeof(raw))&&!locked);
        clock_us=100000;
    }
    wrong_crc=wrong_token=wrong_session=wrong_part=timeout=false;delay=0;
    unsigned before=calls;firmware=73;
    CHECK(c5_spi_link_wifi_packet_count_exchange(161,NULL,0,&control,22)==ESP_ERR_NOT_SUPPORTED&&calls==before);
    CHECK(c5_spi_link_wifi_packet_count_exchange(170,&q,14,&raw,54)==ESP_ERR_NOT_SUPPORTED&&calls==before);
    peer_firmware_version=74;mutex_ready=0;
    CHECK(c5_spi_link_wifi_packet_count_exchange(170,&q,14,&raw,54)==ESP_ERR_TIMEOUT&&calls==before);
    mutex_ready=1;
    CHECK(c5_spi_link_wifi_packet_count_exchange(170,&q,14,&raw,22)==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_wifi_packet_count_exchange(170,&q,13,&raw,54)==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_wifi_packet_count_exchange(199,NULL,0,&control,22)==ESP_ERR_INVALID_ARG);
    CHECK(calls==before);
    printf("Packet Count SPI adapter PASS %u checks\n",checks);
    return 0;
}
