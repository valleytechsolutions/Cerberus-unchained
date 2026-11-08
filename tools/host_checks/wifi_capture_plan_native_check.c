#define main capture_legacy_main
#include "wifi_capture_radio_check.c"
#undef main

static uint8_t reply_bytes[22];
static size_t reply_size;
static unsigned replies;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t sequence,
    const void *bytes,size_t size)
{
    assert(sequence==17U&&size<=sizeof(reply_bytes));
    assert(command>=CERBERUS_SPI_COMMAND_WIFI_CAPTURE_START);
    memcpy(reply_bytes,bytes,size);reply_size=size;++replies;
}
static void dispatch(const cerberus_spi_frame_t *request)
{
    switch(request->command){
#include "wifi_capture_plan_dispatch.h"
    default:assert(false);
    }
}
static void send(cerberus_spi_command_t command,const void *request,size_t size)
{
    cerberus_spi_frame_t q;assert(cerberus_protocol_prepare_frame(&q,command,17U,request,size));
    reply_size=0;dispatch(&q);assert(reply_size);
}
static cerberus_wifi_capture_plan_status_payload_t stage(cerberus_wifi_capture_plan_request_t *p)
{
    send(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_PLAN,p,sizeof(*p));
    cerberus_wifi_capture_plan_status_payload_t r;assert(reply_size==sizeof(r));memcpy(&r,reply_bytes,sizeof(r));return r;
}
static cerberus_wifi_capture_plan_status_payload_t target(cerberus_wifi_capture_target_request_t *p)
{
    send(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_TARGET,p,sizeof(*p));
    cerberus_wifi_capture_plan_status_payload_t r;assert(reply_size==sizeof(r));memcpy(&r,reply_bytes,sizeof(r));return r;
}
static cerberus_wifi_capture_hop_status_payload_t hop(uint32_t gen)
{
    cerberus_wifi_capture_stop_request_t q={gen};send(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_HOP_STATUS,&q,sizeof(q));
    cerberus_wifi_capture_hop_status_payload_t r;assert(reply_size==sizeof(r));memcpy(&r,reply_bytes,sizeof(r));return r;
}
static cerberus_wifi_capture_start_request_t planned_request(uint32_t gen)
{
    hop_expected=true;
    return (cerberus_wifi_capture_start_request_t){.generation=gen,
        .mode_mask=CERBERUS_WIFI_CAPTURE_MODE_SAE_COMMIT,
        .flags=CERBERUS_WIFI_CAPTURE_FLAG_HOP|CERBERUS_WIFI_CAPTURE_FLAG_PLAN,.lease_ms=5000U};
}
static void observe_sae(const uint8_t bssid[6],uint8_t ch)
{
    uint8_t sae[32]={0xb0};sae[24]=3;sae[26]=1;sae[30]=19;
    memcpy(sae+16,bssid,6U);
    cerberus_wifi_capture_service_receive(sae,sizeof(sae),ch,-45,fake_us,
        cerberus_wifi_capture_service_admission_token());
}
static void plans_and_lifecycle(void)
{
    reset_driver();fake_us=1000000;last_admission_token=0;
    cerberus_wifi_capture_plan_request_t query={0};
    cerberus_wifi_capture_plan_status_payload_t s=stage(&query);
    assert(!s.generation&&!s.state&&!s.channel_mask&&!s.error_code);
    cerberus_wifi_capture_plan_request_t p={.generation=++generation,
        .channel_mask=cerberus_wifi_capture_plan_channel_bit(1)|cerberus_wifi_capture_plan_channel_bit(36)|
            cerberus_wifi_capture_plan_channel_bit(177),.dwell_ms=250,.target_count=2};
    for(unsigned fault=0;fault<5;++fault){
        cerberus_wifi_capture_plan_request_t bad=p;
        if(fault==0)bad.channel_mask=0;if(fault==1)bad.channel_mask=UINT64_C(1)<<42;
        if(fault==2)bad.dwell_ms=249;if(fault==3)bad.reserved=1;if(fault==4)bad.target_count=33;
        s=stage(&bad);assert(s.error_code==ESP_ERR_INVALID_ARG&&!s.generation);
    }
    s=stage(&p);assert(s.generation==p.generation&&s.state==CERBERUS_WIFI_CAPTURE_PLAN_STAGING&&!s.target_present_mask);
    cerberus_wifi_capture_target_request_t a={.generation=p.generation,.bssid={2,3,4,5,6,7}};
    s=target(&a);assert(!s.error_code&&s.target_present_mask==1U);
    s=stage(&p);assert(!s.error_code&&s.target_present_mask==1U);
    a.index=1;s=target(&a);assert(s.error_code==ESP_ERR_INVALID_ARG&&s.target_present_mask==1U);
    cerberus_wifi_capture_start_request_t r=planned_request(p.generation);
    assert(cerberus_wifi_capture_start(&r)==ESP_ERR_INVALID_STATE&&!atomic_load(&capture_radio_reserved));
    a.bssid[5]=8;s=target(&a);assert(!s.error_code&&s.state==CERBERUS_WIFI_CAPTURE_PLAN_READY&&s.target_present_mask==3U);
    cerberus_wifi_capture_start_request_t bad=r;bad.flags=CERBERUS_WIFI_CAPTURE_FLAG_PLAN;
    assert(cerberus_wifi_capture_start(&bad)==ESP_ERR_INVALID_ARG);
    bad=r;bad.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_BEACON;assert(cerberus_wifi_capture_start(&bad)==ESP_ERR_INVALID_ARG);
    bad=r;bad.channel=1;assert(cerberus_wifi_capture_start(&bad)==ESP_ERR_INVALID_ARG);
    bad=r;bad.flags=0;bad.channel=1;assert(cerberus_wifi_capture_start(&bad)==ESP_ERR_INVALID_STATE);
    reject_channel[36]=true;
    assert(cerberus_wifi_capture_start(&r)==ESP_OK&&radio_channel==1&&promiscuous);
    assert(cerberus_wifi_capture_start(&r)==ESP_OK);
    s=stage(&p);assert(!s.error_code&&s.state==CERBERUS_WIFI_CAPTURE_PLAN_READY);
    s=target(&a);assert(!s.error_code);a.bssid[5]=9;s=target(&a);assert(s.error_code==ESP_ERR_INVALID_STATE);a.bssid[5]=8;
    cerberus_wifi_capture_plan_request_t higher=p;++higher.generation;
    assert(stage(&higher).error_code==ESP_ERR_INVALID_STATE);
    const uint8_t first[6]={2,3,4,5,6,7},unknown[6]={2,3,4,5,6,99};
    observe_sae(unknown,1);assert(!status().accepted);
    observe_sae(first,1);observe_sae(a.bssid,1);assert(status().accepted==2&&status().pending==2);
    cerberus_wifi_capture_peek_payload_t pinned;assert(cerberus_wifi_capture_service_peek(p.generation,&pinned)==ESP_OK);
    uint32_t old_token=cerberus_wifi_capture_service_admission_token();
    advance(249);assert(radio_channel==1&&!hop(p.generation).completed_sweeps);
    advance(1);assert(radio_channel==177&&old_token!=cerberus_wifi_capture_service_admission_token());
    cerberus_wifi_capture_hop_status_payload_t h=hop(p.generation);
    assert(h.attempted_mask==p.channel_mask&&h.applied_mask==(p.channel_mask&~cerberus_wifi_capture_plan_channel_bit(36))&&!h.completed_sweeps);
    cerberus_wifi_capture_peek_payload_t retained;assert(cerberus_wifi_capture_service_peek(p.generation,&retained)==ESP_OK);
    assert(!memcmp(&pinned,&retained,sizeof(pinned)));
    advance(249);assert(!hop(p.generation).completed_sweeps);
    fail_count[DISABLE]=1;assert(cerberus_wifi_capture_stop(p.generation)==ESP_FAIL);
    assert(atomic_load(&capture_radio_reserved)&&stage(&higher).error_code==ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_capture_stop(p.generation)==ESP_OK);
    assert(!hop(p.generation).completed_sweeps&&stage(&higher).error_code==ESP_ERR_INVALID_STATE);
    drain(p.generation);assert(!stage(&higher).error_code);
    /* Staging a replacement does not relabel the prior frozen hop result. */
    assert(hop(p.generation).generation==p.generation);
    cerberus_wifi_capture_stop_request_t missing={higher.generation};
    send(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_HOP_STATUS,&missing,sizeof(missing));
    cerberus_wifi_capture_hop_error_payload_t failure;assert(reply_size==sizeof(failure));memcpy(&failure,reply_bytes,sizeof(failure));
    assert(failure.generation==missing.generation&&failure.error_code==ESP_ERR_NOT_FOUND);
    generation=higher.generation;
}
static void complete_sweeps_and_errors(void)
{
    reset_driver();cerberus_wifi_capture_plan_request_t p={.generation=++generation,
        .channel_mask=cerberus_wifi_capture_plan_channel_bit(6),.dwell_ms=250};
    assert(stage(&p).state==CERBERUS_WIFI_CAPTURE_PLAN_READY);
    cerberus_wifi_capture_start_request_t r=planned_request(p.generation);
    assert(cerberus_wifi_capture_start(&r)==ESP_OK);unsigned tuned=calls[SET_CHANNEL],enabled=calls[ENABLE];
    advance(249);assert(!hop(p.generation).completed_sweeps);
    advance(1);assert(hop(p.generation).completed_sweeps==1);
    advance(1000);assert(hop(p.generation).completed_sweeps==2); /* No catch-up loop. */
    assert(calls[SET_CHANNEL]==tuned&&calls[ENABLE]==enabled&&promiscuous);
    assert(cerberus_wifi_capture_stop(p.generation)==ESP_OK);
    reset_driver();p.generation=++generation;p.channel_mask=cerberus_wifi_capture_plan_channel_bit(1)|cerberus_wifi_capture_plan_channel_bit(36);
    assert(!stage(&p).error_code);r=planned_request(p.generation);assert(cerberus_wifi_capture_start(&r)==ESP_OK);
    advance(250);assert(radio_channel==36&&!hop(p.generation).completed_sweeps);
    advance(250);assert(radio_channel==1&&hop(p.generation).completed_sweeps==1);
    assert(cerberus_wifi_capture_stop(p.generation)==ESP_OK);
    reset_driver();p.generation=++generation;assert(!stage(&p).error_code);r=planned_request(p.generation);reject_all=true;
    assert(cerberus_wifi_capture_start(&r)==ESP_ERR_NOT_SUPPORTED);
    cerberus_wifi_capture_hop_status_payload_t h=hop(p.generation);
    assert(h.attempted_mask==p.channel_mask&&!h.applied_mask&&!h.completed_sweeps);
    assert(!atomic_load(&capture_radio_reserved)&&!promiscuous&&status().error_code==ESP_ERR_NOT_SUPPORTED);
    reset_driver();p.generation=++generation;p.target_count=32;assert(!stage(&p).error_code);
    for(unsigned i=0;i<32;++i){cerberus_wifi_capture_target_request_t t={.generation=p.generation,.index=i,.bssid={2,3,4,5,6,(uint8_t)i}};
        cerberus_wifi_capture_plan_status_payload_t s=target(&t);assert(!s.error_code);
        assert(s.target_present_mask==(i==31?UINT32_MAX:(UINT32_C(1)<<(i+1U))-1U));}
    r=planned_request(p.generation);assert(cerberus_wifi_capture_start(&r)==ESP_OK);
    assert(cerberus_wifi_capture_stop(p.generation)==ESP_OK);
    p.generation=UINT32_MAX;p.target_count=0;assert(!stage(&p).error_code);r=planned_request(p.generation);
    assert(cerberus_wifi_capture_start(&r)==ESP_OK&&cerberus_wifi_capture_stop(p.generation)==ESP_OK);
    p.generation=1;assert(stage(&p).error_code==ESP_ERR_INVALID_STATE);
    /* Wrong request lengths never mutate staging or return a response. */
    cerberus_spi_frame_t malformed;assert(cerberus_protocol_prepare_frame(&malformed,CERBERUS_SPI_COMMAND_WIFI_CAPTURE_PLAN,17,&p,sizeof(p)-1U));
    unsigned before=replies;dispatch(&malformed);assert(replies==before);
}
int main(void)
{
    assert(capture_legacy_main()==0);plans_and_lifecycle();complete_sweeps_and_errors();
    puts("C5 planned SAE: actual dispatcher/service/worker, staged identity/32 targets, frozen scope, cross-band rejects, full dwells, single-channel continuity, pinned drain and generation exhaustion passed.");
    return 0;
}
