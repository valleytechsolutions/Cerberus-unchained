#include "wifi_frame_analysis.h"
#include "cerberus_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static cerberus_wifi_frame_model_t model;
static uint8_t frame[4097];
static const uint8_t ap[6] = {0x20,1,2,3,4,5};
static cerberus_wifi_frame_observation_t observation;

static void init(uint8_t mode)
{
    CHECK(cerberus_wifi_frame_init(&model, 6U, mode, false, NULL, NULL));
    observation = (cerberus_wifi_frame_observation_t){.channel=6U,.rssi=-50};
    memset(frame, 0, sizeof(frame));
    memcpy(frame+16U, ap, 6U);
}
static cerberus_wifi_frame_result_t receive(size_t length)
{
    ++observation.sequence; observation.observed_us += 100U;
    return cerberus_wifi_frame_observe(&model, &observation, frame, length);
}
static void disconnect(uint8_t station, uint8_t flags)
{
    memset(frame, 0, sizeof(frame)); frame[0]=0xc0U; frame[1]=flags;
    memcpy(frame+16U,ap,6U); memcpy(frame+10U,ap,6U);
    frame[4]=0x30U;frame[9]=station;frame[24]=7U;
}
static void eapol(uint8_t flags, bool qos, bool wds, bool ht)
{
    memset(frame,0,sizeof(frame));frame[0]=qos?0x88U:8U;
    frame[1]=flags|(wds?3U:1U)|(ht?0x80U:0U);memcpy(frame+4U,ap,6U);
    size_t offset=24U+(wds?6U:0U)+(qos?2U:0U)+(qos&&ht?4U:0U);
    const uint8_t llc[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
    memcpy(frame+offset,llc,sizeof(llc));frame[offset+8U]=2U;frame[offset+9U]=3U;
}

int main(void)
{
    init(CERBERUS_WIFI_CAPTURE_MODE_RAW);
    cerberus_wifi_frame_model_t saved=model;
    CHECK(!cerberus_wifi_frame_init(&model,15U,CERBERUS_WIFI_CAPTURE_MODE_RAW,false,NULL,NULL));
    CHECK(!memcmp(&model,&saved,sizeof(model)));
    CHECK(!cerberus_wifi_frame_init(&model,6U,CERBERUS_WIFI_CAPTURE_MODE_RAW|CERBERUS_WIFI_CAPTURE_MODE_PROBE,false,NULL,NULL));
    CHECK(!cerberus_wifi_frame_init(&model,6U,CERBERUS_WIFI_CAPTURE_MODE_PROBE,true,ap,NULL));
    CHECK(!cerberus_wifi_frame_init(&model,6U,CERBERUS_WIFI_CAPTURE_MODE_RAW,true,NULL,NULL));
    frame[0]=0xd4U;CHECK(receive(10U).delivered);CHECK(model.summary.control==1U);
    frame[0]=0xc4U;CHECK(receive(10U).delivered);CHECK(model.summary.control==2U);
    frame[0]=0xb4U;receive(15U);CHECK(model.summary.malformed==1U);
    receive(16U);CHECK(model.summary.control==3U);
    frame[0]=0x0cU;receive(2U);CHECK(model.summary.other==1U);
    frame[0]=0x81U;receive(36U);CHECK(model.summary.malformed==2U);
    frame[0]=0x80U;frame[1]=1U;receive(36U);CHECK(model.summary.malformed==3U);
    frame[1]=0U;receive(35U);CHECK(model.summary.beacons==0U);
    receive(36U);CHECK(model.summary.beacons==1U);
    frame[0]=0x40U;receive(24U);CHECK(model.summary.probe_requests==1U);
    frame[0]=0x50U;receive(36U);CHECK(model.summary.probe_responses==1U);
    frame[0]=8U;receive(23U);CHECK(model.summary.data==0U);
    receive(24U);CHECK(model.summary.data==1U);
    eapol(0U,false,false,false);receive(36U);CHECK(model.summary.eapol_headers==1U);
    eapol(0U,true,true,true);receive(48U);CHECK(model.summary.eapol_headers==2U);
    frame[30]|=0x80U;receive(48U);CHECK(model.summary.eapol_headers==2U);
    eapol(0x40U,false,false,false);receive(36U);CHECK(model.summary.eapol_headers==2U);
    eapol(4U,false,false,false);receive(36U);CHECK(model.summary.eapol_headers==2U);
    eapol(0U,false,false,false);frame[22]=1U;receive(36U);CHECK(model.summary.eapol_headers==2U);
    eapol(0U,false,false,false);frame[35]=2U;receive(36U);CHECK(model.summary.eapol_headers==2U);
    receive(38U);CHECK(model.summary.eapol_headers==3U);
    frame[32]=0U;receive(38U);CHECK(model.summary.eapol_headers==3U);

    init(CERBERUS_WIFI_CAPTURE_MODE_RAW);disconnect(1U,8U);
    cerberus_wifi_frame_result_t result=receive(26U);CHECK(result.event_index==0);
    CHECK(model.records[0].reason_known&&model.records[0].reason==7U);
    CHECK(model.summary.deauth==1U&&model.summary.retries==1U);
    CHECK(model.records[0].frames==1U&&model.records[0].retries==1U);
    uint64_t first=model.records[0].first_us;uint32_t firstseq=model.records[0].first_sequence;
    receive(26U);CHECK(model.summary.count==1U&&model.records[0].frames==2U);
    CHECK(model.records[0].first_us==first&&model.records[0].first_sequence==firstseq);
    uint32_t received=model.summary.received;
    result=cerberus_wifi_frame_observe(&model,&observation,frame,26U);
    CHECK(!result.delivered&&result.event_index==-1&&model.summary.received==received);
    CHECK(model.summary.sequence_rejected==1U);
    ++observation.sequence;--observation.observed_us;
    CHECK(!cerberus_wifi_frame_observe(&model,&observation,frame,26U).delivered);
    CHECK(model.summary.clock_rejected==1U);
    observation.observed_us=first+100U; /* Equal source time is valid. */
    CHECK(cerberus_wifi_frame_observe(&model,&observation,frame,26U).delivered);
    disconnect(1U,0x40U);result=receive(26U);
    CHECK(result.event_index==1&&!model.records[1].reason_known&&model.records[1].protected_frame);
    disconnect(1U,4U);result=receive(26U);
    CHECK(result.event_index==2&&!model.records[2].reason_known&&model.records[2].fragmented);
    disconnect(1U,0U);frame[22]=1U;result=receive(26U);CHECK(result.event_index==2);
    disconnect(1U,0U);result=receive(24U);CHECK(!model.records[result.event_index].reason_known);
    frame[0]=0xa0U;result=receive(26U);CHECK(model.summary.disassoc==1U&&model.records[result.event_index].subtype==10U);
    for(unsigned i=2U;i<26U;++i){disconnect((uint8_t)i,0U);receive(26U);}
    CHECK(model.summary.count==16U&&model.summary.event_replaced==13U);
    CHECK(model.summary.event_dropped==0U);
    bool latest=false;for(unsigned i=0;i<16U;++i)latest|=model.records[i].receiver[5]==25U;
    CHECK(latest);
    disconnect(25U,0U);result=receive(26U);
    model.records[result.event_index].frames=UINT32_MAX;
    receive(26U);CHECK(model.records[result.event_index].frames==UINT32_MAX&&model.summary.saturated);
    cerberus_wifi_frame_record_t row;CHECK(cerberus_wifi_frame_copy(&model,0,&row));
    memset(&row,0xff,sizeof(row));CHECK(!cerberus_wifi_frame_copy(&model,16,&row));
    cerberus_wifi_frame_record_t zero={0};CHECK(!memcmp(&row,&zero,sizeof(row)));

    init(CERBERUS_WIFI_CAPTURE_MODE_RAW);observation.rssi=-128;disconnect(1U,0U);receive(26U);
    CHECK(!model.summary.rssi_valid&&!model.summary.extrema_valid);
    observation.rssi=0;receive(26U);CHECK(model.summary.extrema_valid&&model.summary.max_rssi==0);
    observation.rssi=-127;receive(26U);CHECK(model.summary.min_rssi==-127);
    observation.rssi=127;receive(26U);CHECK(!model.summary.rssi_valid&&model.summary.max_rssi==0);
    model.summary.received=UINT32_MAX-1U;model.summary.bytes=UINT64_MAX-1U;
    receive(26U);CHECK(model.summary.received==UINT32_MAX&&model.summary.bytes==UINT64_MAX&&model.summary.saturated);
    observation.sequence=UINT32_MAX;observation.observed_us+=100U;
    CHECK(cerberus_wifi_frame_observe(&model,&observation,frame,26U).delivered);
    observation.sequence=1U;CHECK(!cerberus_wifi_frame_observe(&model,&observation,frame,26U).delivered);
    init(CERBERUS_WIFI_CAPTURE_MODE_RAW);disconnect(1U,0x80U);result=receive(30U);
    CHECK(result.event_index==0&&!model.records[0].reason_known); /* Unsupported ordered body. */

    cerberus_wifi_capture_plan_t plan;cerberus_wifi_capture_plan_clear(&plan);
    plan.dwell_ms=CERBERUS_WIFI_CAPTURE_PLAN_RAW_DWELL_MS;
    plan.channel_mask=cerberus_wifi_capture_plan_channel_bit(6U)|cerberus_wifi_capture_plan_channel_bit(36U);
    CHECK(cerberus_wifi_frame_init(&model,0,CERBERUS_WIFI_CAPTURE_MODE_RAW,false,NULL,&plan));
    observation=(cerberus_wifi_frame_observation_t){.channel=36U,.rssi=-40};
    memset(frame,0,sizeof(frame));frame[0]=0xd4U;CHECK(receive(10U).delivered);
    plan.channel_mask=0U;CHECK(model.plan.channel_mask!=0U);
    observation.channel=40U;CHECK(!receive(10U).delivered);
    plan=model.plan;CHECK(cerberus_wifi_capture_plan_add_target(&plan,ap,6U));
    CHECK(cerberus_wifi_frame_init(&model,0,CERBERUS_WIFI_CAPTURE_MODE_RAW,false,NULL,&plan));
    observation.channel=6U;CHECK(!receive(10U).delivered); /* No BSSID in ACK. */
    eapol(0U,false,false,false);CHECK(receive(36U).delivered); /* To DS BSSID=addr1. */
    frame[1]=2U;memcpy(frame+10U,ap,6U);CHECK(receive(36U).delivered);
    frame[1]=3U;CHECK(!receive(36U).delivered); /* WDS lacks infrastructure BSSID. */
    plan.dwell_ms=250U;CHECK(!cerberus_wifi_frame_init(&model,0,CERBERUS_WIFI_CAPTURE_MODE_PROBE,false,NULL,&plan));
    plan.target_count=0U;CHECK(cerberus_wifi_frame_init(&model,0,CERBERUS_WIFI_CAPTURE_MODE_PROBE,false,NULL,&plan));
    frame[0]=0x40U;frame[1]=0U;CHECK(receive(24U).delivered);
    frame[0]=0xc0U;CHECK(!receive(26U).delivered);

    /* Every prefix and FC type/subtype boundary is safe and leaves the input
     * unchanged, including protected, fragmented and optional-header forms. */
    for(unsigned fc=0;fc<256U;++fc)for(size_t length=0;length<50U;++length){
        init(CERBERUS_WIFI_CAPTURE_MODE_RAW);memset(frame,0x5a,50U);frame[0]=(uint8_t)fc;
        uint8_t before[50];memcpy(before,frame,50U);receive(length);
        CHECK(!memcmp(before,frame,50U));CHECK(model.summary.received==1U);
    }
    init(CERBERUS_WIFI_CAPTURE_MODE_RAW);frame[0]=8U;receive(4096U);CHECK(model.summary.data==1U);
    receive(4097U);CHECK(model.summary.malformed==1U);
    printf("Wi-Fi frame model: %u checks; model %zu bytes; row %zu bytes\n",checks,sizeof(model),sizeof(row));
    return 0;
}
