#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "mesh_observation_protocol.h"
#define CERBERUS_SPI_FRAME_PAYLOAD_SIZE 22U
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef uint8_t cerberus_spi_command_t;
typedef struct {uint8_t firmware_version;} cerberus_c5_status_t;
static unsigned sequence, peer_firmware_version, firmware=72U;
static int link_mutex=1, locked, mutex_ready=1;
static unsigned calls, refreshes, unlocks, delay_us;
static int64_t clock_us=100000;
static esp_err_t frame_error, status_error;
static uint8_t response[22], sent[22], last_command;
static size_t sent_length;
static uint32_t received_deadline;
static int64_t esp_timer_get_time(void){return clock_us;}
static int take_link_mutex(unsigned ticks){assert(ticks==20U&&!locked);locked=mutex_ready;return locked;}
static void give_link_mutex(void){assert(locked);locked=0;++unlocks;}
static esp_err_t get_status_v2_passive_locked(unsigned serial,uint32_t deadline,cerberus_c5_status_t *out)
{assert(serial&&locked);received_deadline=deadline;++refreshes;out->firmware_version=firmware;return status_error;}
static esp_err_t survey_session_frame_locked(cerberus_spi_command_t command,
    const void *q,size_t length,void *r,size_t reply_length,uint32_t deadline)
{
    assert(locked&&length<=sizeof(sent)&&reply_length==22U);
    ++calls;last_command=command;sent_length=length;received_deadline=deadline;
    memset(sent,0,sizeof(sent));if(length)memcpy(sent,q,length);
    memcpy(r,response,22U);clock_us+=delay_us;return frame_error;
}
#include "c5_mesh_link_impl.h"
static unsigned checks;
#define CHECK(x) do{assert(x);++checks;}while(0)
static void setup(void)
{
    calls=refreshes=unlocks=delay_us=0U;frame_error=status_error=ESP_OK;
    firmware=peer_firmware_version=72U;link_mutex=mutex_ready=1;clock_us=100000;
    memset(response,0,sizeof(response));assert(!locked);
}
static void control(uint64_t session)
{
    cerberus_mesh_observation_control_t r={.session_id=session,
        .state=CERBERUS_MESH_OBSERVATION_RECEIVING,
        .flags=CERBERUS_MESH_OBSERVATION_BOUND|CERBERUS_MESH_OBSERVATION_HELD|
            CERBERUS_MESH_OBSERVATION_RX_ACTIVE,.capacity=16U,.version=1U,.channel=15U};
    memcpy(response,&r,sizeof(r));
}
static void chunk(const cerberus_mesh_observation_read_t *q)
{
    cerberus_mesh_observation_chunk_t r={.session_id=q->session_id,.sequence=q->sequence,.part=q->part};
    memcpy(response,&r,sizeof(r));
}
int main(void)
{
    uint8_t out[22], zero[22]={0};
    cerberus_mesh_observation_start_t start={.session_id=50U,.channel_mask=1U<<4U,.dwell_ms=250U};
    setup();control(50U);
    CHECK(c5_spi_link_mesh_exchange(CERBERUS_MESH_OBSERVATION_COMMAND_START,&start,sizeof(start),out,sizeof(out))==ESP_OK);
    CHECK(calls==1U&&refreshes==1U&&unlocks==1U&&received_deadline==120U);
    CHECK(sent_length==sizeof(start)&&memcmp(sent,&start,sizeof(start))==0);
    CHECK(last_command==CERBERUS_MESH_OBSERVATION_COMMAND_START);
    setup();firmware=71U;memset(out,0xff,sizeof(out));
    CHECK(c5_spi_link_mesh_exchange(CERBERUS_MESH_OBSERVATION_COMMAND_START,&start,sizeof(start),out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
    CHECK(!calls&&refreshes==1U&&unlocks==1U&&memcmp(out,zero,sizeof(out))==0);
    setup();status_error=ESP_ERR_TIMEOUT;
    CHECK(c5_spi_link_mesh_exchange(CERBERUS_MESH_OBSERVATION_COMMAND_INFO,NULL,0U,out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(!calls&&refreshes==1U&&peer_firmware_version==0U&&unlocks==1U);
    for(unsigned i=0;i<5U;++i){
        setup();cerberus_mesh_observation_start_t q=start;
        if(i==0)q.channel_mask=0;else if(i==1)q.channel_mask=3;
        else if(i==2)q.dwell_ms=99;else if(i==3)q.dwell_ms=2001;else q.mode=2;
        CHECK(c5_spi_link_mesh_exchange(147,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_ARG);
        CHECK(!calls&&!refreshes&&!unlocks);
    }
    setup();control(50U);frame_error=ESP_ERR_TIMEOUT;
    CHECK(c5_spi_link_mesh_exchange(147,&start,sizeof(start),out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(calls==1U&&unlocks==1U); /* Never retry a possibly delivered Start. */
    setup();control(50U);delay_us=20000U;
    CHECK(c5_spi_link_mesh_exchange(149,&start,sizeof(uint64_t),out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(calls==1U&&!refreshes&&memcmp(out,zero,sizeof(out))==0);
    setup();mutex_ready=0;
    CHECK(c5_spi_link_mesh_exchange(148,&start,sizeof(uint64_t),out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(!calls&&!unlocks);
    setup();link_mutex=0;
    CHECK(c5_spi_link_mesh_exchange(148,&start,sizeof(uint64_t),out,sizeof(out))==ESP_ERR_INVALID_STATE);
    setup();clock_us=-1;
    CHECK(c5_spi_link_mesh_exchange(148,&start,sizeof(uint64_t),out,sizeof(out))==ESP_ERR_INVALID_STATE);
    setup();control(51U);
    CHECK(c5_spi_link_mesh_exchange(148,&start,sizeof(uint64_t),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    CHECK(memcmp(out,zero,sizeof(out))==0);
    setup();control(50U);memcpy(out,&start,sizeof(start));
    CHECK(c5_spi_link_mesh_exchange(147,out,sizeof(start),out,sizeof(out))==ESP_OK);
    CHECK(memcmp(sent,&start,sizeof(start))==0); /* Aliasing cannot erase request. */
    for(unsigned kind=0;kind<2U;++kind){
        unsigned parts=kind?15U:16U;
        for(unsigned part=0;part<parts;++part){
            cerberus_mesh_observation_read_t q={.session_id=50U,
                .sequence=kind?0x80000001U:1U,.kind=(uint8_t)kind,.part=(uint8_t)part};
            setup();chunk(&q);
            CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_OK);
            CHECK(calls==1U&&!refreshes);
            setup();chunk(&q);response[12]^=1U;
            CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
            setup();chunk(&q);response[8]^=1U;
            CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
        }
    }
    cerberus_mesh_observation_read_t q={.session_id=50U,.sequence=1U,.kind=0U,.part=15U};
    setup();chunk(&q);response[21]=1U;
    CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    setup();q.sequence=0x80000001U;
    CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_ARG);
    CHECK(!calls);q.sequence=1U;q.kind=1U;
    CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_ARG);
    q.kind=0U;q.part=0U;setup();chunk(&q);
    memset(response+8U,0,4U);int32_t remote=ESP_ERR_NOT_FOUND;memcpy(response+13U,&remote,4U);
    CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_NOT_FOUND);
    response[21]=1U;
    CHECK(c5_spi_link_mesh_exchange(151,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    setup();cerberus_mesh_observation_ack_t ack={.session_id=50U,.sequence=1U};control(50U);
    CHECK(c5_spi_link_mesh_exchange(152,&ack,sizeof(ack),out,sizeof(out))==ESP_OK);
    ack.sequence=0x80000001U;
    CHECK(c5_spi_link_mesh_exchange(152,&ack,sizeof(ack),out,sizeof(out))==ESP_ERR_INVALID_ARG);
    for(unsigned cmd=0;cmd<256U;++cmd)if(cmd<146U||cmd>153U){
        setup();CHECK(c5_spi_link_mesh_exchange((uint8_t)cmd,NULL,0,out,sizeof(out))==ESP_ERR_INVALID_ARG);
        CHECK(!calls&&!refreshes&&!unlocks);
    }
    CHECK(c5_spi_link_mesh_exchange(146,NULL,0,out,21U)==ESP_ERR_INVALID_ARG);
    puts("Mesh SPI adapter: bounded capability/identity/chunk checks and no mutation replay passed.");
    printf("%u checks\n",checks);
    return 0;
}
