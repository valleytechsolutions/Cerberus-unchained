#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "mesh_locate_protocol.h"
#define CERBERUS_SPI_FRAME_PAYLOAD_SIZE 22U
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef uint8_t cerberus_spi_command_t;
typedef struct { uint8_t firmware_version; } cerberus_c5_status_t;
static unsigned sequence, peer_firmware_version, firmware;
static int link_mutex=1, locked, mutex_ready=1;
static unsigned calls, refreshes, unlocks;
static int64_t clock_us=100000, delay_us;
static esp_err_t frame_error, status_error;
static uint8_t response[22], sent[22], last_command;
static size_t sent_length;
static uint32_t received_deadline;
static int64_t esp_timer_get_time(void) { return clock_us; }
static int take_link_mutex(unsigned ticks) { assert(ticks==20U&&!locked); locked=mutex_ready; return locked; }
static void give_link_mutex(void) { assert(locked); locked=0; ++unlocks; }
static esp_err_t get_status_v2_passive_locked(unsigned serial,uint32_t deadline,cerberus_c5_status_t *out)
{ assert(serial&&locked); received_deadline=deadline; ++refreshes; out->firmware_version=firmware; return status_error; }
static esp_err_t survey_session_frame_locked(cerberus_spi_command_t command,
    const void *q,size_t length,void *r,size_t reply_length,uint32_t deadline)
{
    assert(locked&&length<=sizeof(sent)&&reply_length==22U);
    ++calls; last_command=command; sent_length=length; received_deadline=deadline;
    memset(sent,0,sizeof(sent)); if(length)memcpy(sent,q,length);
    memcpy(r,response,22U); clock_us+=delay_us; return frame_error;
}
#include "c5_mesh_locate_link_impl.h"
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while(0)
static void setup(void)
{
    calls=refreshes=unlocks=0U; delay_us=0; frame_error=status_error=ESP_OK;
    firmware=peer_firmware_version=73U; link_mutex=mutex_ready=1; clock_us=100000;
    memset(response,0,sizeof(response)); assert(!locked);
}
static void control(uint64_t session)
{
    cerberus_mesh_locate_control_t r={.session_id=session,.state=CERBERUS_MESH_LOCATE_RECEIVING,
        .flags=CERBERUS_MESH_LOCATE_BOUND|CERBERUS_MESH_LOCATE_HELD|CERBERUS_MESH_LOCATE_RX_ACTIVE,
        .version=1U,.channel=15U,.projection_bytes=90U};
    memcpy(response,&r,sizeof(r));
}
static void chunk(const cerberus_mesh_locate_read_t *q)
{
    cerberus_mesh_locate_chunk_t r={.session_id=q->session_id,.pin=q->pin,.part=q->part};
    memcpy(response,&r,sizeof(r));
}
int main(void)
{
    uint8_t out[22], zero[22]={0};
    cerberus_mesh_locate_start_t start={.session_id=UINT64_C(0x123456789abcdef0),
        .address=UINT64_C(0x0011223344556677),.pan=0xabcd,.channel=15U,
        .address_mode=3U,.pan_known=1U,.version=1U};
    setup(); control(start.session_id);
    CHECK(c5_spi_link_mesh_locate_exchange(155,&start,sizeof(start),out,sizeof(out))==ESP_OK);
    CHECK(calls==1U&&refreshes==1U&&unlocks==1U&&received_deadline==120U);
    CHECK(sent_length==22U&&memcmp(sent,&start,sizeof(start))==0&&last_command==155U);
    setup(); control(start.session_id); memcpy(out,&start,sizeof(start));
    CHECK(c5_spi_link_mesh_locate_exchange(155,out,sizeof(start),out,sizeof(out))==ESP_OK);
    CHECK(memcmp(sent,&start,sizeof(start))==0); /* Full22B alias-safe Start. */
    for(unsigned i=0;i<11U;++i) {
        setup(); cerberus_mesh_locate_start_t q=start;
        if(i==0)q.session_id=0U; else if(i==1)q.channel=10U; else if(i==2)q.channel=27U;
        else if(i==3)q.address_mode=0U; else if(i==4)q.address_mode=1U;
        else if(i==5)q.version=0U; else if(i==6)q.pan_known=2U;
        else if(i==7)q.pan_known=0U;
        else { q.address_mode=2U; q.address=i==8?0xfffeU:i==9?0xffffU:0x10000U; }
        memset(out,0xff,sizeof(out));
        CHECK(c5_spi_link_mesh_locate_exchange(155,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_ARG);
        CHECK(!calls&&!refreshes&&!unlocks&&memcmp(out,zero,sizeof(out))==0);
    }
    for(unsigned i=0;i<3U;++i) {
        setup(); control(start.session_id); cerberus_mesh_locate_start_t q=start;
        if(i==0)q.address=UINT64_MAX;
        else if(i==1) { q.address_mode=2U; q.address=0xfffdU; }
        else { q.pan_known=0U; q.pan=0U; }
        CHECK(c5_spi_link_mesh_locate_exchange(155,&q,sizeof(q),out,sizeof(out))==ESP_OK);
    }
    setup(); firmware=72U;
    CHECK(c5_spi_link_mesh_locate_exchange(155,&start,sizeof(start),out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
    CHECK(!calls&&refreshes==1U&&unlocks==1U&&memcmp(out,zero,sizeof(out))==0);
    setup(); status_error=ESP_ERR_TIMEOUT;
    CHECK(c5_spi_link_mesh_locate_exchange(154,NULL,0U,out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(!calls&&refreshes==1U&&peer_firmware_version==0U&&unlocks==1U);
    setup(); control(start.session_id); frame_error=ESP_ERR_TIMEOUT;
    CHECK(c5_spi_link_mesh_locate_exchange(155,&start,sizeof(start),out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(calls==1U&&unlocks==1U); /* Possibly accepted Start is never replayed. */
    for(unsigned i=0;i<2U;++i) {
        setup(); control(start.session_id); delay_us=i?-1:20000;
        CHECK(c5_spi_link_mesh_locate_exchange(157,&start,8U,out,sizeof(out))==ESP_ERR_TIMEOUT);
        CHECK(calls==1U&&!refreshes&&memcmp(out,zero,sizeof(out))==0);
    }
    setup(); mutex_ready=0;
    CHECK(c5_spi_link_mesh_locate_exchange(156,&start,8U,out,sizeof(out))==ESP_ERR_TIMEOUT);
    CHECK(!calls&&!unlocks);
    setup(); link_mutex=0;
    CHECK(c5_spi_link_mesh_locate_exchange(156,&start,8U,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    setup(); clock_us=-1;
    CHECK(c5_spi_link_mesh_locate_exchange(156,&start,8U,out,sizeof(out))==ESP_ERR_INVALID_STATE);
    for(unsigned i=0;i<8U;++i) {
        setup(); control(start.session_id); cerberus_mesh_locate_control_t r; memcpy(&r,response,sizeof(r));
        if(i==0)r.session_id^=UINT64_C(1)<<40U; else if(i==1)r.state=6U;
        else if(i==2)r.flags|=0x80U; else if(i==3)r.reserved=1U;
        else if(i==4)r.channel=10U; else if(i==5)r.channel=27U;
        else r.projection_bytes=i==6?71U:198U;
        memcpy(response,&r,sizeof(r));
        CHECK(c5_spi_link_mesh_locate_exchange(156,&start,8U,out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
        CHECK(memcmp(out,zero,sizeof(out))==0);
    }
    for(unsigned part=0;part<22U;++part) {
        cerberus_mesh_locate_read_t q={.session_id=start.session_id,.pin=0x80000001U,.part=(uint8_t)part};
        setup(); chunk(&q);
        CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_OK);
        CHECK(calls==1U&&!refreshes);
        setup(); chunk(&q); response[12]^=1U;
        CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
        setup(); chunk(&q); response[8]^=1U;
        CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
        setup(); chunk(&q); response[5]^=1U;
        CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    }
    cerberus_mesh_locate_read_t q={.session_id=start.session_id,.pin=UINT32_MAX,.part=21U};
    setup(); chunk(&q); response[21]=1U;
    CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    setup(); q.pin=0x80000000U;
    CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_ARG);
    CHECK(!calls); q.pin=0x80000001U; q.part=22U;
    CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_ARG);
    q.part=0U; setup(); chunk(&q); memset(response+8U,0,4U);
    int32_t remote=ESP_ERR_NOT_FOUND; memcpy(response+13U,&remote,4U);
    CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_NOT_FOUND);
    response[21]=1U;
    CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    setup(); chunk(&q); memset(response+8U,0,4U);
    CHECK(c5_spi_link_mesh_locate_exchange(159,&q,sizeof(q),out,sizeof(out))==ESP_ERR_INVALID_RESPONSE);
    for(unsigned cmd=0;cmd<256U;++cmd) if(cmd<154U||cmd>160U) {
        setup(); CHECK(c5_spi_link_mesh_locate_exchange((uint8_t)cmd,NULL,0U,out,sizeof(out))==ESP_ERR_INVALID_ARG);
        CHECK(!calls&&!refreshes&&!unlocks);
    }
    setup(); CHECK(c5_spi_link_mesh_locate_exchange(154,&start,0U,out,sizeof(out))==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_mesh_locate_exchange(155,&start,21U,out,sizeof(out))==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_mesh_locate_exchange(154,NULL,0U,out,21U)==ESP_ERR_INVALID_ARG);
    puts("Locate SPI adapter: exact target/identity, bounded capability/chunk checks and no mutation replay passed.");
    printf("%u checks\n",checks);
    return 0;
}
