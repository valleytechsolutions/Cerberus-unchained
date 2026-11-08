/* Actual shared parser, C5 receive callback/worker and SPI dispatcher. Platform
 * fakes model SDK boundaries; they do not prove RF rate, latency or ISR headroom. */
#include "mesh_mac.h"
static mesh_mac_result_t locate_test_parse(const uint8_t *,size_t,mesh_mac_observation_t *);
#define mesh_mac_parse locate_test_parse
#define main retained_survey_fixture_main
#include "mesh_observation_worker_check.c"
#undef main
#undef mesh_mac_parse

static unsigned parse_interleave;
static cerberus_mesh_locate_start_t request_for(uint64_t address,uint16_t pan,uint8_t known,uint8_t mode)
{
    cerberus_mesh_locate_start_t q={.session_id=mesh_epoch+1U,.address=address,.pan=pan,
        .channel=11U,.address_mode=mode,.pan_known=known,.version=1U};
    return q;
}
static cerberus_mesh_locate_start_t locate_start_test(uint64_t address,uint16_t pan,uint8_t known,uint8_t mode)
{
    cerberus_mesh_locate_start_t q=request_for(address,pan,known,mode);
    cerberus_mesh_locate_control_t c;
    CHECK(cerberus_mesh_locate_start(&q,&c)==ESP_OK);
    CHECK(c.session_id==q.session_id&&c.state==CERBERUS_MESH_LOCATE_QUEUED&&c.flags==11U);
    CHECK(c.version==1U&&c.projection_bytes==72U&&!c.sequence&&!c.channel&&!c.reserved);
    CHECK(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN);
    return q;
}
static void locate_offer(const uint8_t *mac,unsigned length,unsigned rx_channel)
{
    uint8_t frame[128]={0};frame[0]=(uint8_t)(length+2U);
    if(length<=125U)memcpy(frame+1U,mac,length);
    esp_ieee802154_frame_info_t info={.channel=(uint8_t)rx_channel,.rssi=-48,.lqi=159U};
    unsigned done=returned;esp_ieee802154_receive_done(frame,&info);CHECK(returned==done+1U);
}
static void short_offer(uint16_t source,uint16_t destination,uint16_t pan,unsigned length)
{
    uint8_t mac[125]={0x41,0x88,17U,(uint8_t)pan,(uint8_t)(pan>>8U),
        (uint8_t)destination,(uint8_t)(destination>>8U),(uint8_t)source,(uint8_t)(source>>8U)};
    for(unsigned i=9U;i<length&&i<125U;++i)mac[i]=(uint8_t)i;
    locate_offer(mac,length,11U);
}
static void clear_locate(void)
{
    uint64_t token=locate_session;cerberus_mesh_locate_control_t c;
    CHECK(cerberus_mesh_locate_release(token,&c)==ESP_OK);
    CHECK(c.flags&CERBERUS_MESH_LOCATE_HELD);
    pump(4U);CHECK(!locate_context&&!cerberus_ieee802154_scanner_is_running());
    CHECK(cerberus_mesh_locate_release(token,&c)==ESP_OK);
    CHECK(c.session_id==token&&!c.sequence&&!c.flags&&!c.version&&!c.projection_bytes&&!c.error_code);
    CHECK(cerberus_mesh_locate_start(&locate_request,&c)==ESP_ERR_NOT_FOUND);
}
static mesh_mac_result_t locate_test_parse(const uint8_t *raw,size_t length,mesh_mac_observation_t *out)
{
    CHECK(!locked&&!mesh_mux.depth); /* Actual parser runs outside critical. */
    mesh_mac_result_t result=mesh_mac_parse(raw,length,out);
    unsigned action=parse_interleave;parse_interleave=0U;
    if(action==1U){
        source_us+=1000;cerberus_mesh_locate_control_t c;
        CHECK(cerberus_mesh_locate_status(locate_session,&c)==ESP_OK);
    }else if(action==2U){
        clear_locate();(void)locate_start_test(0x9988,0x1234,1U,2U);pump(1U);
    }else if(action==3U){
        cerberus_mesh_locate_control_t c;CHECK(cerberus_mesh_locate_stop(locate_session,&c)==ESP_OK);
    }
    return result;
}
static cerberus_mesh_locate_projection_t read_pin(uint32_t pin,unsigned bytes,bool mutate)
{
    cerberus_mesh_locate_projection_t out;memset(&out,0,sizeof(out));
    for(unsigned part=0U;part<(bytes+8U)/9U;++part){
        cerberus_mesh_locate_read_t q={locate_session,pin,(uint8_t)part};
        cerberus_mesh_locate_chunk_t c;
        CHECK(cerberus_mesh_locate_read(&q,&c)==ESP_OK);
        CHECK(c.session_id==locate_session&&c.pin==pin&&c.part==part);
        unsigned offset=part*9U,n=bytes-offset;if(n>9U)n=9U;
        memcpy((uint8_t*)&out+offset,c.data,n);
        for(unsigned j=n;j<9U;++j)CHECK(c.data[j]==0U);
        if(mutate&&part==0U){source_us+=1000;short_offer(0x3344,0x7788,0x1234,9U);}
    }
    return out;
}
static cerberus_mesh_locate_projection_t locate_snapshot(bool mutate)
{
    cerberus_mesh_locate_control_t c;CHECK(cerberus_mesh_locate_peek(locate_session,&c)==ESP_OK);
    CHECK(c.sequence>CERBERUS_MESH_LOCATE_PIN_BASE&&c.projection_bytes>=72U&&c.projection_bytes<=197U);
    cerberus_mesh_locate_projection_t out=read_pin(c.sequence,c.projection_bytes,mutate);
    CHECK(out.version==1U&&out.state==c.state&&out.channel==c.channel&&out.error_code==c.error_code);
    CHECK(c.projection_bytes==72U+out.mac_length&&!out.reserved0&&!out.reserved1);
    uint64_t total=(uint64_t)out.invalid_metadata+out.malformed+out.unsupported+out.filtered_nonmatch+out.matches;
    CHECK(out.callbacks==(total>UINT32_MAX?UINT32_MAX:total));
    CHECK(out.matches==out.match_sequence);
    return out;
}
static void early_receive(void)
{
    short_offer(0x3344,0x7788,0x1234,9U);
}
static void stop_inside_receive(void)
{
    cerberus_mesh_locate_control_t c;
    CHECK(cerberus_mesh_locate_stop(locate_session,&c)==ESP_OK);
    short_offer(0x3344,0x7788,0x1234,9U);
}
static void dispatcher(void)
{
    cerberus_spi_frame_t q={0};q.command=CERBERUS_MESH_LOCATE_COMMAND_INFO;q.sequence=77U;
    unsigned before=replies;mesh_dispatch(&q);CHECK(replies==before+1U&&reply.command==q.command&&reply.sequence==77U);
    cerberus_mesh_locate_control_t control;memcpy(&control,reply.payload,sizeof(control));
    CHECK(!control.flags&&control.version==1U&&control.projection_bytes==72U&&control.session_id==mesh_epoch);
    cerberus_mesh_locate_start_t startq=request_for(0x3344,0x1234,1U,2U);
    q.command=CERBERUS_MESH_LOCATE_COMMAND_START;q.payload_length=22U;memcpy(q.payload,&startq,22U);
    mesh_dispatch(&q);CHECK(locate_context&&reply.payload_length==22U);pump(1U);
    short_offer(0x3344,0x7788,0x1234,9U);
    q.command=CERBERUS_MESH_LOCATE_COMMAND_PEEK;q.payload_length=8U;memcpy(q.payload,&locate_session,8U);
    mesh_dispatch(&q);memcpy(&control,reply.payload,sizeof(control));CHECK(control.sequence>CERBERUS_MESH_LOCATE_PIN_BASE);
    cerberus_mesh_locate_read_t read={locate_session,control.sequence,0U};
    q.command=CERBERUS_MESH_LOCATE_COMMAND_READ;q.payload_length=13U;memcpy(q.payload,&read,13U);
    mesh_dispatch(&q);cerberus_mesh_locate_chunk_t chunk;memcpy(&chunk,reply.payload,sizeof(chunk));CHECK(chunk.pin==read.pin);
    for(unsigned command=154U;command<=160U;++command){q.command=(uint8_t)command;q.payload_length=1U;before=replies;mesh_dispatch(&q);CHECK(replies==before);}
    clear_locate();
}
static void counter_and_retention_boundaries(void)
{
    cerberus_mesh_locate_control_t c;
    (void)locate_start_test(0x3344,0U,1U,2U);pump(1U);
    /* A known zero PAN remains known, not the unknown-PAN predicate. */
    short_offer(0x3344,0x7788,0U,9U);
    CHECK(locate_context->live.matches==1U&&locate_context->live.pan_known&&locate_context->live.pan==0U);
    locate_context->live.callbacks=UINT32_MAX-1U;
    locate_context->live.filtered_nonmatch=UINT32_MAX-2U;
    short_offer(0x5566,0x3344,0U,9U);
    short_offer(0x5566,0x3344,0U,9U);
    cerberus_mesh_locate_projection_t snapshot=locate_snapshot(false);
    CHECK(snapshot.callbacks==UINT32_MAX&&snapshot.filtered_nonmatch==UINT32_MAX&&snapshot.matches==1U);
    CHECK(snapshot.flags&CERBERUS_MESH_LOCATE_COUNTERS_SATURATED);
    uint64_t lease=locate_context->lease_us;source_us+=1000;
    cerberus_mesh_locate_info(&c);CHECK(locate_context->lease_us==lease);
    CHECK(cerberus_mesh_locate_peek(locate_session,&c)==ESP_OK&&locate_context->lease_us==lease);
    cerberus_mesh_locate_read_t read={locate_session,c.sequence,0U};cerberus_mesh_locate_chunk_t chunk;
    CHECK(cerberus_mesh_locate_read(&read,&chunk)==ESP_OK&&locate_context->lease_us==lease);
    CHECK(cerberus_mesh_locate_status(locate_session,&c)==ESP_OK&&locate_context->lease_us>lease);
    CHECK(cerberus_mesh_locate_stop(locate_session,&c)==ESP_OK);pump(1U);
    CHECK(locate_context->phase==LOCATE_STOPPED);
    CHECK(cerberus_mesh_locate_peek(locate_session,&c)==ESP_OK);
    read.pin=c.sequence;uint64_t retained=locate_context->retention_us;source_us+=1000;
    cerberus_mesh_locate_read_t wrong=read;wrong.session_id++;
    CHECK(cerberus_mesh_locate_read(&wrong,&chunk)==ESP_ERR_NOT_FOUND&&locate_context->retention_us==retained);
    wrong=read;wrong.pin++;
    CHECK(cerberus_mesh_locate_read(&wrong,&chunk)==ESP_ERR_NOT_FOUND&&locate_context->retention_us==retained);
    CHECK(cerberus_mesh_locate_read(&read,&chunk)==ESP_OK&&locate_context->retention_us>retained);
    source_us=(int64_t)locate_context->retention_us;
    CHECK(cerberus_mesh_locate_read(&read,&chunk)==ESP_ERR_NOT_FOUND&&!chunk.pin&&locate_context->retiring);
    pump(1U);CHECK(!locate_context&&!cerberus_ieee802154_scanner_is_running());
}
int main(void)
{
    CHECK(cerberus_ieee802154_scanner_init()==ESP_OK&&created==1U);
    cerberus_mesh_locate_control_t c;
    cerberus_mesh_locate_start_t q=request_for(0x3344,0x1234,1U,2U);
    for(unsigned fault=0U;fault<8U;++fault){
        cerberus_mesh_locate_start_t bad=q;
        switch(fault){case 0:bad.session_id=0U;break;case 1:bad.channel=10U;break;case 2:bad.channel=27U;break;
        case 3:bad.address_mode=1U;break;case 4:bad.pan_known=2U;break;case 5:bad.pan_known=0U;break;
        case 6:bad.address=0xffffU;break;default:bad.version=2U;break;}
        unsigned alloc=allocation_calls;CHECK(cerberus_mesh_locate_start(&bad,&c)==ESP_ERR_INVALID_ARG);
        CHECK(!locate_context&&allocation_calls==alloc&&!c.flags&&c.session_id==bad.session_id);
    }
    allocation_fail=true;CHECK(cerberus_mesh_locate_start(&q,&c)==ESP_ERR_NO_MEM&&!locate_context&&!enables);allocation_fail=false;
    foreign_wifi=true;CHECK(cerberus_mesh_locate_start(&q,&c)==ESP_ERR_INVALID_STATE&&!locate_context);foreign_wifi=false;
    q=locate_start_test(0x3344,0x1234,1U,2U);
    uint64_t deadline=locate_context->lease_us;CHECK(cerberus_mesh_locate_start(&q,&c)==ESP_OK&&locate_context->lease_us==deadline);
    cerberus_mesh_locate_start_t conflict=q;conflict.address++;CHECK(cerberus_mesh_locate_start(&conflict,&c)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ieee802154_scanner_start()==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ieee802154_beacon_scanner_start()==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ieee802154_activity_scanner_start()==ESP_ERR_INVALID_STATE);
    cerberus_mesh_observation_control_t old;cerberus_mesh_observation_info(&old);
    CHECK(old.flags==CERBERUS_MESH_OBSERVATION_HELD&&old.state==CERBERUS_MESH_OBSERVATION_QUEUED&&!old.count);
    cerberus_mesh_locate_projection_t blank=locate_snapshot(false);
    CHECK(!blank.match_sequence&&!blank.matches&&!blank.mac_length&&!blank.rssi&&!blank.lqi&&!blank.last_match_us);
    CHECK(cerberus_mesh_locate_stop(q.session_id+1U,&c)==ESP_ERR_NOT_FOUND&&!locate_context->cancelled);
    locate_receive_hook=early_receive;pump(1U);locate_receive_hook=NULL;
    CHECK(receiving&&continuous&&!auto_ack_rx&&!auto_ack_tx&&locate_context->live.matches==1U);
    for(unsigned i=0U;i<100U;++i)short_offer(0x5566,0x3344,0x1234,9U); /* Destination is not transmitter. */
    short_offer(0x3344,0x7788,0x1235,9U);
    const uint8_t bad[]={0x41,0x88};locate_offer(bad,2U,11U);
    const uint8_t unsupported[]={7U,0U};locate_offer(unsupported,2U,11U);
    locate_offer(bad,2U,12U);
    CHECK(locate_context->live.matches==1U&&locate_context->live.filtered_nonmatch==101U);
    CHECK(locate_context->live.invalid_metadata==1U&&locate_context->live.malformed==1U&&locate_context->live.unsupported==1U);
    source_us+=1000;short_offer(0x3344,0x7788,0x1234,125U);
    cerberus_mesh_locate_projection_t frozen=locate_snapshot(true);
    CHECK(frozen.matches==2U&&frozen.mac_length==125U&&locate_context->live.matches==3U);
    CHECK(frozen.rssi==-48&&frozen.lqi==159U&&frozen.copied_us>=frozen.last_match_us);
    uint32_t pin=locate_context->pin;unsigned size=72U+locate_context->pinned.mac_length;
    CHECK(cerberus_mesh_locate_status(locate_session,&c)==ESP_OK&&locate_context->pin==pin);
    cerberus_mesh_locate_projection_t again=read_pin(pin,size,false);CHECK(memcmp(&again,&frozen,sizeof(frozen))==0);
    for(unsigned part=(size+8U)/9U;part<256U;++part){
        cerberus_mesh_locate_read_t r={locate_session,pin,(uint8_t)part};cerberus_mesh_locate_chunk_t chunk;
        CHECK(cerberus_mesh_locate_read(&r,&chunk)!=ESP_OK&&!chunk.pin&&chunk.session_id==locate_session);
        for(unsigned i=4U;i<9U;++i)CHECK(chunk.data[i]==0U);
    }
    parse_interleave=1U;short_offer(0x3344,0x7788,0x1234,9U);CHECK(!locate_context->cancelled);
    parse_interleave=2U;short_offer(0x3344,0x7788,0x1234,9U);
    CHECK(locate_request.address==0x9988&&!locate_context->live.callbacks&&!locate_context->live.matches);
    clear_locate();
    /* Full extended source with unknown PAN is distinct from any known PAN. */
    (void)locate_start_test(UINT64_C(0x8877665544332211),0U,0U,3U);pump(1U);
    const uint8_t ext[]={0x41,0xec,7U,1,2,3,4,5,6,7,8,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    locate_offer(ext,sizeof(ext),11U);CHECK(locate_context->live.matches==1U);
    frozen=locate_snapshot(false);CHECK(frozen.address==UINT64_C(0x8877665544332211)&&!frozen.pan_known);
    clear_locate();
    (void)locate_start_test(0x3344,0x1234,1U,2U);locate_receive_hook=stop_inside_receive;pump(1U);locate_receive_hook=NULL;
    CHECK(locate_context->phase==LOCATE_STOPPED&&!locate_context->live.matches&&!receiving);clear_locate();
    (void)locate_start_test(0x3344,0x1234,1U,2U);pump(1U);short_offer(0x3344,0x7788,0x1234,9U);
    parse_interleave=3U;short_offer(0x3344,0x7788,0x1234,9U);CHECK(locate_context->live.matches==1U);
    disable_failures=2U;pump(1U);CHECK(locate_context&&cerberus_ieee802154_scanner_is_running()&&locate_context->state==CERBERUS_MESH_LOCATE_STOPPING);
    pump(5U);CHECK(locate_context->phase==LOCATE_STOPPED&&!receiving);
    frozen=locate_snapshot(false);CHECK(frozen.flags&CERBERUS_MESH_LOCATE_STOPPED);CHECK(!frozen.channel&&frozen.matches==1U&&!frozen.error_code);
    clear_locate();
    (void)locate_start_test(0x3344,0x1234,1U,2U);receive_error=ESP_FAIL;pump(1U);receive_error=ESP_OK;
    frozen=locate_snapshot(false);CHECK(frozen.state==CERBERUS_MESH_LOCATE_ERROR&&frozen.error_code==ESP_FAIL&&!frozen.channel);clear_locate();
    (void)locate_start_test(0x3344,0x1234,1U,2U);pump(1U);source_us=(int64_t)locate_context->lease_us;pump(1U);
    frozen=locate_snapshot(false);CHECK(frozen.error_code==ESP_ERR_TIMEOUT&&(frozen.flags&CERBERUS_MESH_LOCATE_STOPPED));
    CHECK(cerberus_mesh_locate_status(locate_session,&c)==ESP_OK&&!receiving&&c.state==CERBERUS_MESH_LOCATE_ERROR);
    source_us=(int64_t)locate_context->retention_us;pump(1U);CHECK(!locate_context);
    CHECK(cerberus_mesh_locate_start(&locate_request,&c)==ESP_ERR_NOT_FOUND);
    (void)locate_start_test(0x3344,0x1234,1U,2U);pump(1U);
    locate_context->live.matches=locate_context->live.match_sequence=locate_context->live.callbacks=CERBERUS_MESH_LOCATE_SEQUENCE_MAX-1U;
    short_offer(0x3344,0x7788,0x1234,9U);CHECK(locate_context->cancelled);pump(1U);
    frozen=locate_snapshot(false);CHECK(frozen.match_sequence==CERBERUS_MESH_LOCATE_SEQUENCE_MAX&&(frozen.flags&CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED));clear_locate();
    (void)locate_start_test(0x3344,0x1234,1U,2U);pump(1U);locate_context->pin_counter=UINT32_MAX;
    CHECK(cerberus_mesh_locate_peek(locate_session,&c)==ESP_ERR_INVALID_STATE&&locate_context->cancelled);pump(1U);clear_locate();
    (void)locate_start_test(0x3344,0x1234,1U,2U);pump(1U);source_us--;
    short_offer(0x3344,0x7788,0x1234,9U);CHECK(locate_context->cancelled&&locate_context->live.invalid_metadata==1U);
    source_us+=1000;pump(1U);clear_locate();
    counter_and_retention_boundaries();dispatcher();
    mesh_epoch=UINT64_MAX;unsigned prior_allocations=allocation_calls;
    q=request_for(0x3344,0x1234,1U,2U);q.session_id=1U;
    CHECK(cerberus_mesh_locate_start(&q,&c)==ESP_ERR_INVALID_STATE&&!locate_context);
    CHECK(allocation_calls==prior_allocations);
    CHECK(allocations==frees&&created==1U);
    printf("mesh Locate worker: %lu checks PASS; context=%zu projection=%zu no extra task\n",checks,sizeof(mesh_locate_context_t),sizeof(cerberus_mesh_locate_projection_t));
    return 0;
}
