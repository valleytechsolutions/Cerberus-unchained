/* Both real endpoints and the shared parser are compiled. Only the physical
 * SDK, task schedule and SPI response delivery are modeled. */
#define main retained_mesh_bridge_main
#include "mesh_bridge_check.c"
#undef main
#include "mesh_locate_report.h"
#define LOCATE_COMMAND_REGISTERED(name) \
    _Static_assert(CERBERUS_MESH_LOCATE_COMMAND_##name == CERBERUS_SPI_COMMAND_MESH_LOCATE_##name, \
                   "Locate " #name " must match the central command enum")
LOCATE_COMMAND_REGISTERED(INFO);
LOCATE_COMMAND_REGISTERED(START);
LOCATE_COMMAND_REGISTERED(STATUS);
LOCATE_COMMAND_REGISTERED(STOP);
LOCATE_COMMAND_REGISTERED(PEEK);
LOCATE_COMMAND_REGISTERED(READ);
LOCATE_COMMAND_REGISTERED(RELEASE);
#undef LOCATE_COMMAND_REGISTERED
#define esp_timer_get_time bridge_s3_time
#include "c5_mesh_locate_link_impl.h"
#undef esp_timer_get_time

static mesh_locate_t locate;
static mesh_scan_t saved_survey;
static esp_err_t locate_exchange(void *context,uint8_t command,const void *request,size_t n,void *out,size_t size)
{ CHECK(context==&locate);return c5_spi_link_mesh_locate_exchange(command,request,n,out,size); }
static int64_t locate_clock(void *context){CHECK(context==&locate);return s3_clock;}
static const mesh_scan_transport_t locate_transport={locate_exchange,locate_clock,&locate};
static void locate_pump(void)
{
    if((mesh_context||locate_context)&&!notifications)notifications=1U;
    if(!setjmp(idle))worker(NULL);
    CHECK(!c5_locked&&!mesh_mux.depth);
}
static void locate_step(unsigned ms)
{
    tick_ms+=ms;s3_clock+=(int64_t)ms*1000;source_clock+=(int64_t)ms*1000;
    locate_pump();unsigned before=wire_calls;first_command=0U;
    mesh_locate_tick(&locate,tick_ms,&locate_transport);
    CHECK(wire_calls-before<=MESH_SCAN_TICK_CALLS&&!link_locked);locate_pump();
}
static mesh_observation_address_key_t short_key(void)
{
    for(unsigned i=0U;i<saved_survey.model.address_count;++i)
        if(saved_survey.model.addresses[i].key.mode==MESH_MAC_ADDRESS_SHORT16&&
           saved_survey.model.addresses[i].key.value==0x9abcU)
            return saved_survey.model.addresses[i].key;
    CHECK(false);mesh_observation_address_key_t key={0};return key;
}
static void locate_begin_key(mesh_observation_address_key_t key)
{
    CHECK(!mesh_context&&!locate_context&&allocated==freed&&!rf_enabled);
    mesh_locate_init(&locate);memset(commands,0,sizeof(commands));memset(drop_response,0,sizeof(drop_response));
    firmware=CERBERUS_MESH_LOCATE_MIN_FIRMWARE;peer_firmware_version=0U;
    receive_failure=ESP_OK;foreign_wifi=allocation_failure=false;held_disable=0U;
    CHECK(mesh_locate_begin(&locate,&key,saved_survey.model.session)==ESP_OK);
    locate_step(1U);CHECK(locate.accepted&&locate_context&&rx_active);
    CHECK(commands[CERBERUS_MESH_LOCATE_COMMAND_START]==1U&&locate.session>saved_survey.model.session);
    CHECK(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN);
}
static void locate_begin(void){locate_begin_key(short_key());}
static void wait_sample(uint32_t sequence_value)
{
    for(unsigned i=0U;i<500U&&(!locate.has_snapshot||locate.snapshot.match_sequence<sequence_value)&&!locate.faulted;++i)
        locate_step(25U);
    CHECK(!locate.faulted&&locate.has_snapshot&&locate.snapshot.match_sequence==sequence_value);
}
static void locate_finish(bool frozen)
{
    for(unsigned i=0U;i<1500U&&!mesh_locate_released(&locate)&&(!locate.faulted||locate.close_requested);++i)
        locate_step(25U);
    if(!mesh_locate_released(&locate))fprintf(stderr,"Locate finish phase=%u error=%d fault=%d remote=%u/%u seq=%lu native=%u/%u count=%lu\n",
        locate.phase,locate.error,locate.faulted,locate.remote.state,locate.remote.flags,
        (unsigned long)locate.last_sequence,locate_context?locate_context->phase:0U,
        locate_context?locate_context->state:0U,(unsigned long)(locate_context?locate_context->live.matches:0U));
    CHECK(mesh_locate_released(&locate)&&!locate_context&&allocated==freed&&!rf_enabled);
    CHECK(locate.frozen==frozen&&mesh_locate_frozen_valid(&locate)==frozen);
    CHECK(!(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN));
    CHECK(!memcmp(&controller,&saved_survey,sizeof(controller))); /* No second survey acquisition. */
    if(frozen){
        char csv[MESH_LOCATE_REPORT_MAX_BYTES+1U];size_t length=0U;
        CHECK(mesh_locate_report_encode(&locate,csv,sizeof(csv),&length));
        CHECK(length==strlen(csv)&&strstr(csv,"CERBERUS_MESH_LOCATE_CSV,1"));
        CHECK(strstr(csv,"[FINAL_RAW_WITNESS]")&&strstr(csv,"[SOURCE_AGE_BOUND]"));
    }
}
static void offer_short(uint16_t source,uint16_t destination,uint16_t pan,unsigned length)
{
    uint8_t frame[128]={0};frame[0]=(uint8_t)(length+2U);
    const uint8_t mac[]={0x41,0x88,17U,(uint8_t)pan,(uint8_t)(pan>>8U),
        (uint8_t)destination,(uint8_t)(destination>>8U),(uint8_t)source,(uint8_t)(source>>8U)};
    memcpy(frame+1U,mac,sizeof(mac));for(unsigned i=9U;i<length;++i)frame[i+1U]=(uint8_t)i;
    esp_ieee802154_frame_info_t info={.channel=rf_channel,.rssi=-53,.lqi=199U};
    unsigned before=returned;esp_ieee802154_receive_done(frame,&info);CHECK(returned==before+1U);
}
static void sampled_and_quiet(void)
{
    locate_begin();
    for(unsigned i=0U;i<100U;++i)offer_short(0x5566,0x9abc,0x1234,9U);
    offer_short(0x9abc,0x5678,0x1235,9U);
    for(unsigned i=0U;i<20U;++i){++source_clock;offer_short(0x9abc,0x5678,0x1234,9U);}
    wait_sample(20U);
    CHECK(locate.snapshot.callbacks==121U&&locate.snapshot.filtered_nonmatch==101U);
    CHECK(locate.samples==1U&&locate.coalesced_matches==19U&&locate.history_count==1U);
    CHECK(locate.snapshot.mac_length==9U&&locate.snapshot.rssi==-53&&locate.snapshot.lqi==199U);
    uint64_t age0=0U,age1=0U;CHECK(mesh_locate_age_upper(&locate,s3_clock,&age0));
    unsigned reads=commands[159],peeks=commands[158];
    for(unsigned i=0U;i<40U;++i)locate_step(25U);
    CHECK(commands[159]==reads&&commands[158]==peeks&&locate.samples==1U);
    CHECK(mesh_locate_age_upper(&locate,s3_clock,&age1)&&age1>=age0+1000000U);
    offer_short(0x9abc,0x5678,0x1234,125U);
    for(unsigned i=0U;i<100U&&locate.phase!=MESH_LOCATE_READ;++i)locate_step(25U);
    CHECK(locate.phase==MESH_LOCATE_READ&&locate.wire_size==197U);
    uint32_t pin=locate.pin;uint64_t source_time=locate_context->pinned.last_match_us;
    source_clock+=100;offer_short(0x9abc,0x5678,0x1234,9U);
    while(!locate.faulted&&locate.snapshot.match_sequence==20U)locate_step(25U);
    CHECK(!locate.faulted&&locate.snapshot.match_sequence==21U&&locate.snapshot.mac_length==125U);
    CHECK(locate.snapshot.last_match_us==source_time&&locate_context->pin==pin);
    wait_sample(22U);CHECK(locate.samples==3U&&locate.coalesced_matches==19U);
    mesh_locate_stop(&locate);locate_step(25U);CHECK(first_command==CERBERUS_MESH_LOCATE_COMMAND_STOP);
    locate_finish(true);CHECK(locate.snapshot.matches==22U&&locate.snapshot.callbacks==123U);
}
static void loss_and_history(void)
{
    locate_begin();emit(0U,1U);
    drop_response[CERBERUS_MESH_LOCATE_COMMAND_PEEK]=1U;
    drop_response[CERBERUS_MESH_LOCATE_COMMAND_READ]=1U;
    wait_sample(1U);CHECK(locate.samples==1U&&!drop_response[158]&&!drop_response[159]);
    for(unsigned i=2U;i<=35U;++i){source_clock+=100;emit(0U,i);wait_sample(i);}
    CHECK(locate.history_count==32U&&locate.samples==35U&&!locate.coalesced_matches);
    mesh_locate_sample_t first;CHECK(mesh_locate_history_get(&locate,0U,&first)&&first.sequence==4U);
    drop_response[CERBERUS_MESH_LOCATE_COMMAND_STOP]=1U;
    drop_response[CERBERUS_MESH_LOCATE_COMMAND_RELEASE]=1U;
    mesh_locate_stop(&locate);locate_finish(true);
    CHECK(!drop_response[157]&&!drop_response[160]&&commands[160]>=2U);
    CHECK(locate.samples==35U&&locate.history_count==32U);
    /* Lost accepted START resolves through STATUS; it never issues a second Start. */
    mesh_observation_address_key_t key=short_key();mesh_locate_init(&locate);memset(commands,0,sizeof(commands));
    CHECK(mesh_locate_begin(&locate,&key,saved_survey.model.session)==ESP_OK);
    drop_response[CERBERUS_MESH_LOCATE_COMMAND_START]=1U;locate_step(1U);
    CHECK(locate_context&&!locate.accepted&&locate.start_attempted);
    for(unsigned i=0U;i<100U&&!locate.accepted;++i)locate_step(25U);
    CHECK(locate.accepted&&commands[155]==1U&&commands[156]);
    emit(0U,1U);mesh_locate_stop(&locate);locate_finish(true);
}
static void errors_capability_and_close(void)
{
    locate_begin();emit(0U,1U);wait_sample(1U);
    /* No UI keepalive: worker independently expires, retains evidence and source error. */
    source_clock=(int64_t)locate_context->lease_us;locate_pump();
    CHECK(locate_context->phase==LOCATE_STOPPED&&locate_context->primary_error==ESP_ERR_TIMEOUT);
    locate_finish(true);CHECK(locate.error==ESP_ERR_TIMEOUT&&locate.snapshot.matches==1U);
    locate_begin();emit(0U,1U);wait_sample(1U);
    /* Clock fault arrives with unchanged sequence while cleanup is still held. */
    source_clock--;offer_short(0x9abc,0x5678,0x1234,9U);source_clock+=1000;held_disable=20U;
    for(unsigned i=0U;i<100U&&!(locate.remote.flags&CERBERUS_MESH_LOCATE_CLOCK_INVALID);++i)locate_step(25U);
    uint64_t age=123U;CHECK(!mesh_locate_age_upper(&locate,s3_clock,&age)&&!age&&!locate.anchor_valid);
    held_disable=0U;mesh_locate_close(&locate);locate_finish(false);
    locate_begin();emit(0U,1U);held_disable=3U;mesh_locate_close(&locate);locate_step(25U);
    CHECK(locate_context&&cerberus_ieee802154_scanner_is_running()&&!locate_context->rx_channel);
    locate_finish(false);
    mesh_observation_address_key_t key=short_key();mesh_locate_init(&locate);memset(commands,0,sizeof(commands));
    firmware=72U;unsigned prior=enables,allocs=allocated;
    CHECK(mesh_locate_begin(&locate,&key,saved_survey.model.session)==ESP_OK);locate_step(1U);
    CHECK(mesh_locate_released(&locate)&&locate.error==ESP_ERR_NOT_SUPPORTED);
    CHECK(!commands[154]&&!commands[155]&&enables==prior&&allocated==allocs);
    firmware=73U;mesh_locate_init(&locate);
    CHECK(mesh_locate_begin(&locate,&key,saved_survey.model.session)==ESP_OK);mesh_locate_stop(&locate);
    prior=wire_calls;locate_step(1U);CHECK(wire_calls==prior&&mesh_locate_released(&locate));
    /* A valid modern EXT/EXT source has unknown PAN, never guessed from destination. */
    key.mode=MESH_MAC_ADDRESS_EXT64;key.value=UINT64_C(0xfedcba9876543210);key.pan.known=false;key.pan.value=0U;
    locate_begin_key(key);emit(1U,1U);wait_sample(1U);
    CHECK(locate.snapshot.address==key.value&&!locate.snapshot.pan_known);
    mesh_locate_stop(&locate);locate_finish(true);
}
int main(void)
{
    CHECK(cerberus_ieee802154_scanner_init()==ESP_OK&&created==1U);
    complete_bank();saved_survey=controller; /* Real survey -> immutable selected source. */
    sampled_and_quiet();loss_and_history();errors_capability_and_close();
    CHECK(allocated==freed&&!locate_context&&!mesh_context&&created==1U);
    printf("Actual C5 Locate ISR/worker/dispatch -> S3 framed adapter/controller/report: %lu checks PASS; tracker=%zu context=%zu\n",
        checks,sizeof(locate),sizeof(mesh_locate_context_t));return 0;
}
