#include "mesh_report.h"
#include "mesh_report_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks;if(!(c)){fprintf(stderr,"mesh report line %d: %s\n",__LINE__,#c);exit(1);} } while(0)
static mesh_scan_t sample,before,bad;
static char encoded[MESH_REPORT_MAX_BYTES+1U],sentinel[MESH_REPORT_MAX_BYTES+1U];

static void one_record(const uint8_t *mac,size_t length)
{
    CHECK(mesh_report_test_fill(&sample,0U));
    mesh_observation_input_t in={0};in.session=sample.model.session;in.sequence=1U;
    in.channel=11U;in.flags=7U;in.callback_us=1000U;in.rssi=-42;in.lqi=222U;
    in.length=(uint8_t)length;memcpy(in.mac,mac,length);
    mesh_observation_result_t result=mesh_observation_ingest(&sample.model,&in);
    CHECK(result==MESH_OBSERVATION_OK || result==MESH_OBSERVATION_MALFORMED || result==MESH_OBSERVATION_UNSUPPORTED);
    sample.acked_sequence=sample.last_enqueued=1U;sample.observed_mask=1U;
    sample.callback_time_seen=true;sample.first_callback_us=sample.last_callback_us=1000U;
    sample.final_summary.callbacks=sample.final_summary.enqueued=sample.final_summary.acked=sample.final_summary.last_sequence=1U;
}
static size_t render(void)
{
    size_t needed=999U,written=0U;
    CHECK(mesh_report_valid(&sample));CHECK(mesh_report_encode(&sample,NULL,0U,&needed));
    CHECK(needed>0U && needed<=MESH_REPORT_MAX_BYTES);
    memset(encoded,0xa5,sizeof(encoded));
    CHECK(mesh_report_encode(&sample,encoded,needed+1U,&written));
    CHECK(written==needed && strlen(encoded)==needed && encoded[needed]=='\0');
    if(needed+1U<sizeof(encoded))CHECK((unsigned char)encoded[needed+1U]==0xa5U);
    return needed;
}
static void capacities_and_content(void)
{
    size_t maximum=0U;
    for(unsigned n=0U;n<=100U;++n){
        CHECK(mesh_report_test_fill(&sample,n));before=sample;size_t length=render();
        if(length>maximum)maximum=length;
        CHECK(memcmp(&sample,&before,sizeof(sample))==0);
        CHECK(strstr(encoded,"# CERBERUS_MESH_OBSERVATIONS_CSV,1\r\n"));
        CHECK(strstr(encoded,"[SESSION]\r\n") && strstr(encoded,"[C5_COUNTERS]\r\n"));
        CHECK(strstr(encoded,"[CHANNEL_DWELL]\r\n") && strstr(encoded,"[S3_COUNTERS]\r\n"));
        CHECK(strstr(encoded,"[PAN_BUCKETS]\r\n") && strstr(encoded,"[ADDRESSES]\r\n"));
        CHECK(strstr(encoded,"[ADDRESS_SOURCE_SIGNALS]\r\n") && strstr(encoded,"[DIRECTED_MAC_OBSERVATIONS]\r\n"));
        CHECK(strstr(encoded,"[DIRECTED_OBSERVATION_TIMES]\r\n") && strstr(encoded,"[RAW_WITNESSES]\r\n"));
        CHECK(strstr(encoded,"1234567887654321"));
        if(n){CHECK(strstr(encoded,"EXT64,FEDCBA9800000001"));CHECK(strstr(encoded,",UNKNOWN,,1,"));}
        if(n>=79U)CHECK(sample.model.pan_count==16U && sample.model.address_count==64U && sample.model.edge_count==64U && sample.model.witness_count==64U);
        memset(encoded,0x5a,sizeof(encoded));memcpy(sentinel,encoded,sizeof(encoded));
        size_t written=999U;
        CHECK(!mesh_report_encode(&sample,encoded,length,&written) && written==0U);
        CHECK(memcmp(encoded,sentinel,sizeof(encoded))==0);
        CHECK(!mesh_report_encode(&sample,encoded,0U,&written));CHECK(memcmp(encoded,sentinel,sizeof(encoded))==0);
    }
    CHECK(mesh_report_test_fill(&sample,1000U));size_t length=render();if(length>maximum)maximum=length;
    CHECK(sample.model.counters.witness_omissions==936U);
    CHECK(strstr(encoded,"255"));
    /* Every capacity below an exact completed encoding fails before any write. */
    CHECK(mesh_report_test_fill(&sample,0U));length=render();
    memset(encoded,0x5a,sizeof(encoded));memcpy(sentinel,encoded,sizeof(encoded));
    for(size_t cap=0U;cap<=length;++cap){size_t written=77U;
        CHECK(!mesh_report_encode(&sample,encoded,cap,&written) && written==0U);
        CHECK(memcmp(encoded,sentinel,sizeof(encoded))==0);
    }
    printf("mesh report observed bounded fixture maximum=%zu bytes (cap=%u)\n",maximum,MESH_REPORT_MAX_BYTES);
}
static void opaque_and_provenance(void)
{
    const uint8_t malformed[]={0x41U,0x88U};one_record(malformed,sizeof(malformed));render();
    CHECK(strstr(encoded,",MALFORMED,2,4188\r\n"));CHECK(!sample.model.address_count);
    const uint8_t unsupported[]={4U,0U,0U};one_record(unsupported,sizeof(unsupported));render();
    CHECK(strstr(encoded,",UNSUPPORTED,3,040000\r\n"));CHECK(!sample.model.edge_count);
    uint8_t named[]={0x41,0x88,0x55,0,0,0x44,0x33,0x22,0x11,'=',',','"','\r','\n',0,0xff};
    one_record(named,sizeof(named));render();
    CHECK(strstr(encoded,"SHORT16,1122,11,KNOWN,0000,4"));
    CHECK(strstr(encoded,"SHORT16,3344,11,KNOWN,0000,2"));
    CHECK(strstr(encoded,"3D2C220D0A00FF"));CHECK(!strstr(encoded,"=,\""));
    sample.final_summary.error_code=sample.error=ESP_FAIL;render();CHECK(strstr(encoded,",-1\r\n"));
    /* Missing clocks remain unknown; no current clock is consulted. */
    CHECK(mesh_report_test_fill(&sample,0U));sample.final_summary.flags=CERBERUS_MESH_OBSERVATION_STOPPED|
        CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID;
    sample.final_summary.started_us=sample.final_summary.stopped_us=sample.final_summary.copied_us=0U;
    sample.final_summary.error_code=sample.error=ESP_ERR_INVALID_STATE;render();
    CHECK(strstr(encoded,"0,0,0,0,0,0\r\n"));
    CHECK(mesh_report_test_fill(&sample,1U));
    sample.final_summary.queue_drops=UINT32_MAX;sample.final_summary.callbacks=UINT32_MAX;
    sample.final_summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;render();
    CHECK(strstr(encoded,"4294967295"));
}
static void address_roles_and_pan_origins(void)
{
    /* One actual received frame may name the same endpoint in both roles.
     * It increments each role once but the endpoint's seen counter only once. */
    const uint8_t self[]={0x41,0x88,0x55,0,0,0x22,0x11,0x22,0x11};
    one_record(self,sizeof(self));render();
    CHECK(sample.model.address_count==1U && sample.model.edge_count==1U);
    const mesh_observation_address_t *a=&sample.model.addresses[0];
    CHECK(a->seen.frames==1U && a->source_frames==1U && a->destination_frames==1U);
    CHECK(a->origin_bits==6U && sample.model.pans[0].origin_bits==6U);
    CHECK(sample.model.edges[0].source==0U && sample.model.edges[0].destination==0U);

    /* Enhanced ACK addresses are retained as observations, never inferred edges. */
    const uint8_t ack[]={0x42,0xa8,0x56,0,0,0x44,0x33,0x22,0x11};
    one_record(ack,sizeof(ack));render();
    CHECK(sample.model.counters.headers==1U && sample.model.address_count==2U);
    CHECK(sample.model.edge_count==0U && sample.model.witnesses[0].parse_result==MESH_MAC_HEADER_OK);

    /* An addressless supported header retains a witness without inventing a PAN. */
    const uint8_t no_addresses[]={0x01,0x00,0x57};
    one_record(no_addresses,sizeof(no_addresses));render();
    CHECK(sample.model.counters.headers==1U && sample.model.pan_count==0U);
    CHECK(sample.model.address_count==0U && sample.model.edge_count==0U);
    CHECK(strstr(encoded,",HEADER_OK,3,010057\r\n"));

    /* Swapping roles in two actual compressed frames merges provenance for the
     * same known PAN and full typed endpoints while preserving separate edges. */
    const uint8_t first[]={0x41,0x88,0x58,0x34,0x12,0x44,0x33,0x22,0x11};
    one_record(first,sizeof(first));
    mesh_observation_input_t second=sample.model.witnesses[0].input;
    second.sequence=2U;second.callback_us=1010U;second.mac[2]=0x59U;
    second.mac[5]=0x22U;second.mac[6]=0x11U;second.mac[7]=0x44U;second.mac[8]=0x33U;
    CHECK(mesh_observation_ingest(&sample.model,&second)==MESH_OBSERVATION_OK);
    sample.acked_sequence=sample.last_enqueued=2U;sample.last_callback_us=1010U;
    sample.final_summary.callbacks=sample.final_summary.enqueued=sample.final_summary.acked=
        sample.final_summary.last_sequence=2U;
    render();
    CHECK(sample.model.pan_count==1U && sample.model.pans[0].key.value==0x1234U);
    CHECK(sample.model.pans[0].origin_bits==6U && sample.model.pans[0].seen.frames==2U);
    CHECK(sample.model.address_count==2U && sample.model.edge_count==2U);
    for(unsigned i=0;i<2U;++i){a=&sample.model.addresses[i];
        CHECK(a->origin_bits==6U && a->seen.frames==2U && a->source_frames==1U && a->destination_frames==1U);
    }
    CHECK(strstr(encoded,"SHORT16,1122,11,KNOWN,1234,6"));
    CHECK(strstr(encoded,"SHORT16,3344,11,KNOWN,1234,6"));
}
static void invalid_sources(void)
{
    CHECK(mesh_report_test_fill(&sample,80U));before=sample;
    for(unsigned field=0;field<55U;++field){
        bad=before;
        switch(field){
        case 0:bad.frozen=false;break;case 1:bad.final_valid=false;break;case 2:bad.faulted=true;break;
        case 3:bad.requested=true;break;case 4:bad.request.session_id=bad.model.session;break;
        case 5:bad.phase=MESH_SCAN_READ_ROW;break;case 6:bad.remote.session_id++;break;
        case 7:bad.remote.flags=CERBERUS_MESH_OBSERVATION_BOUND;break;case 8:bad.remote.error_code=ESP_FAIL;break;
        case 9:bad.accepted=true;break;case 10:bad.model.pan_count=17U;break;
        case 11:bad.model.address_count=65U;break;case 12:bad.model.edge_count=65U;break;
        case 13:bad.model.witness_count=65U;break;case 14:bad.final_summary.acked++;break;
        case 15:bad.final_summary.callbacks++;break;case 16:bad.final_summary.flags|=0x80U;break;
        case 17:bad.final_summary.channel=11U;break;case 18:bad.final_summary.requested_mask^=1U;break;
        case 19:bad.final_summary.dwell_ms++;break;case 20:bad.final_summary.copied_us=1U;break;
        case 21:bad.model.counters.headers++;break;case 22:bad.model.counters.witness_omissions++;break;
        case 23:bad.model.counters.wrong_session++;break;case 24:bad.model.last_sequence++;break;
        case 25:bad.model.witnesses[0].input.session++;break;
        case 26:bad.model.witnesses[0].input.sequence++;break;
        case 27:bad.model.witnesses[0].input.channel=10U;break;
        case 28:bad.model.witnesses[0].input.length=126U;break;
        case 29:bad.model.witnesses[0].input.flags|=8U;break;
        case 30:bad.model.witnesses[0].input.reserved[1]=1U;break;
        case 31:bad.model.witnesses[0].parse_result=MESH_MAC_MALFORMED;break;
        case 32:bad.model.pans[0].origin_bits=0U;break;
        case 33:bad.model.pans[0].key.channel=27U;break;
        case 34:bad.model.pans[0].seen.first_witness=64U;break;
        case 35:bad.model.pans[0].seen.last_sequence=999U;break;
        case 36:bad.model.addresses[0].key.mode=1U;break;
        case 37:bad.model.addresses[0].key.value^=0x100U;break;
        case 38:bad.model.addresses[0].source_flags|=8U;break;
        case 39:bad.model.addresses[0].last_source_witness=64U;break;
        case 40:bad.model.addresses[0].source_frames=UINT32_MAX;break;
        case 41:bad.model.addresses[0].source_rssi++;break;
        case 42:bad.model.edges[0].source=64U;break;case 43:bad.model.edges[0].destination=255U;break;
        case 44:bad.model.edges[0].frame_type_bits|=4U;break;
        case 45:bad.model.edges[0].encrypted_frames=UINT32_MAX;break;
        case 46:bad.model.edges[0].last_source_lqi++;break;
        case 47:bad.model.edges[0].seen.last_witness=255U;break;
        case 48:bad.model.edges[1]=bad.model.edges[0];break;
        case 49:bad.model.addresses[1]=bad.model.addresses[0];break;
        case 50:bad.model.pans[1]=bad.model.pans[0];break;
        case 51:bad.model.time_seen=false;break;
        case 52:bad.model.counters.saturated=true;break;
        case 53:bad.error=ESP_FAIL;break;
        case 54:bad.final_summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;break;
        }
        CHECK(!mesh_report_valid(&bad));
        memset(encoded,0x5a,sizeof(encoded));size_t written=9U;
        CHECK(!mesh_report_encode(&bad,encoded,sizeof(encoded),&written) && written==0U);
        for(size_t i=0;i<sizeof(encoded);++i)CHECK((unsigned char)encoded[i]==0x5aU);
    }
    CHECK(memcmp(&sample,&before,sizeof(sample))==0);
    /* Small witness tail must be canonical, even when the parser ignores it. */
    const uint8_t mac[]={4U,0U};one_record(mac,sizeof(mac));sample.model.witnesses[0].input.mac[124]=1U;
    CHECK(!mesh_report_valid(&sample));
}
static void alias_and_arguments(void)
{
    CHECK(mesh_report_test_fill(&sample,4U));before=sample;
    size_t size=3U;
    CHECK(!mesh_report_encode(NULL,encoded,sizeof(encoded),&size) && size==0U);
    CHECK(!mesh_report_encode(&sample,NULL,1U,&size));
    CHECK(!mesh_report_encode(&sample,encoded,sizeof(encoded),NULL));
    CHECK(!mesh_report_encode(&sample,(char *)&sample,sizeof(sample),&size));
    CHECK(memcmp(&sample,&before,sizeof(sample))==0);
    CHECK(!mesh_report_encode(&sample,encoded,sizeof(encoded),(size_t *)&sample));
    CHECK(memcmp(&sample,&before,sizeof(sample))==0);
    union { size_t aligned;char bytes[128]; } alias;
    memset(&alias,0x55,sizeof(alias));unsigned char saved[sizeof(alias)];memcpy(saved,&alias,sizeof(saved));
    CHECK(!mesh_report_encode(&sample,alias.bytes,sizeof(alias.bytes),&alias.aligned));
    CHECK(memcmp(saved,&alias,sizeof(saved))==0);
}
int main(void)
{
    capacities_and_content();opaque_and_provenance();address_roles_and_pan_origins();
    invalid_sources();alias_and_arguments();
    printf("mesh CSV report: %u checks PASS\n",checks);return 0;
}
