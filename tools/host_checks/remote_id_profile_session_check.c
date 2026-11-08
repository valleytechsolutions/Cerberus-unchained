/* Reuse the existing independent radio/scheduler mocks. Only the row transport
 * becomes incremental; session, inventory and scheduler are actual production. */
#define main remote_id_legacy_session_suite
#include "remote_id_session_check.c"
#undef main

static struct {
    unsigned steps,completed[2],cancels,chunk,slot,index;
    uint64_t capture_epoch[2],requested_epoch[2][12];
    bool active,legacy[2];
    int fail_slot,fail_chunk,mixed_epoch_slot,mixed_availability_slot,invalid_slot;
} transfer;

static void cancel_profile(void *context)
{
    CHECK(context==&world);++transfer.cancels;transfer.active=false;transfer.chunk=0U;
    for(unsigned i=0;i<2U;++i)world.radios[i].status_round=0U;
}
static int32_t profile_step(void *context,uint8_t radio,uint8_t generation,uint8_t index,
    uint64_t expected_epoch,cerberus_watch_observation_t *out,cerberus_watch_receive_timing_t *timing,
    sentinel_watch_motion_t *motion,cerberus_watch_receive_timing_t *motion_timing,
    cerberus_watch_profile_evidence_t *profile,bool *done)
{
    CHECK(context==&world);unsigned which=slot(radio);fake_radio_t *r=&world.radios[which];
    CHECK(world.session.capture.owner==which+1U&&r->status_round==1U&&
        generation==r->status.generation&&index<r->status.stored_count);
    *done=false;++transfer.steps;memset(out,0,sizeof(*out));memset(timing,0,sizeof(*timing));
    memset(motion,0,sizeof(*motion));memset(motion_timing,0,sizeof(*motion_timing));memset(profile,0,sizeof(*profile));
    if(!transfer.active){transfer.active=true;transfer.slot=which;transfer.index=index;transfer.chunk=0U;}
    CHECK(transfer.slot==which&&transfer.index==index);
    transfer.requested_epoch[which][index]=expected_epoch;
    CHECK(expected_epoch==(index&&!transfer.legacy[which]?transfer.capture_epoch[which]:0U));
    if((int)which==transfer.fail_slot&&(int)transfer.chunk==transfer.fail_chunk)return -77;
    if(++transfer.chunk<14U)return 0;
    transfer.active=false;transfer.chunk=0U;++transfer.completed[which];
    *out=r->rows[index];
    *timing=(cerberus_watch_receive_timing_t){world.now*1000U,100U+index,true};
    if(!transfer.legacy[which]){
        profile->available=true;profile->capture_epoch=transfer.capture_epoch[which];
        profile->values.present=1U;profile->values.version[0]=2U;profile->values.basic[0]=0x10U;
        memcpy(profile->values.basic+1U,out->id,strlen(out->id));
        profile->timing[0]=(cerberus_watch_receive_timing_t){world.now*1000U,1000U+index,true};
    }
    if(index==1U&&(int)which==transfer.mixed_epoch_slot)profile->capture_epoch+=UINT64_C(0x100000000);
    if(index==1U&&(int)which==transfer.mixed_availability_slot)memset(profile,0,sizeof(*profile));
    if(index==1U&&(int)which==transfer.invalid_slot)profile->timing[0].request_started_us=(world.now+1U)*1000U;
    *done=true;return 0;
}
static cerberus_remote_id_ops_t async_ops(void)
{
    cerberus_remote_id_ops_t ops=operations();ops.row=NULL;ops.row_observed=NULL;ops.row_detailed=NULL;
    ops.row_profiled_step=profile_step;ops.row_profiled_cancel=cancel_profile;return ops;
}
static void async_tick(unsigned advance)
{
    world.now+=advance;unsigned previous=transfer.steps;
    cerberus_remote_id_ops_t ops=async_ops();
    cerberus_remote_id_session_tick(&world.session,&world.inventory,&ops);
    CHECK(transfer.steps-previous<=1U);
    CHECK(!transfer.active||world.session.capture.owner==transfer.slot+1U);
}
static void async_reset(bool both)
{
    reset(both);memset(&transfer,0,sizeof(transfer));
    transfer.capture_epoch[0]=UINT64_C(0x100000007);transfer.capture_epoch[1]=UINT64_C(0x200000007);
    transfer.fail_slot=transfer.fail_chunk=transfer.mixed_epoch_slot=transfer.mixed_availability_slot=transfer.invalid_slot=-1;
}
static void async_start(void)
{CHECK(cerberus_remote_id_session_start(&world.session));async_tick(0U);}
static void wait_published(unsigned which,uint64_t token)
{
    for(unsigned n=0;n<1000U&&world.inventory.banks[which].sweep_token<token;++n)async_tick(20U);
    CHECK(world.inventory.banks[which].published&&world.inventory.banks[which].sweep_token==token);
}
static void wait_reading(unsigned which,unsigned index,unsigned chunk)
{
    for(unsigned n=0;n<1000U;++n){
        if(transfer.active&&transfer.slot==which&&transfer.index==index&&transfer.chunk==chunk)return;
        async_tick(20U);
    }
    CHECK(false);
}
static void dual_progress_and_frozen_selection(void)
{
    async_reset(true);async_start();CHECK(world.radios[0].starts==1U&&world.radios[1].starts==1U);
    complete(0,2U);async_tick(20U);async_tick(20U);wait_reading(0,0,5U);
    unsigned ble_polls=world.radios[1].polls;complete(1,2U);
    cerberus_remote_id_bank_t empty=world.inventory.banks[1];
    wait_published(0,1U);CHECK(transfer.completed[0]==2U&&!transfer.completed[1]);
    CHECK(!memcmp(&world.inventory.banks[1],&empty,sizeof(empty))&&world.radios[1].polls>ble_polls);
    CHECK(transfer.requested_epoch[0][0]==0U&&transfer.requested_epoch[0][1]==transfer.capture_epoch[0]);
    cerberus_remote_id_page_t page;CHECK(cerberus_remote_id_inventory_page(&world.inventory,0,&page));
    CHECK(cerberus_remote_id_inventory_select(&world.inventory,&page.rows[0]));
    cerberus_remote_id_record_t frozen=world.inventory.selected.frozen;
    wait_reading(1,0,4U);CHECK(world.radios[0].starts==2U&&world.session.radios[0].owned);
    /* A cached pre-START COMPLETE generation must not invalidate the new job. */
    async_tick(20U);CHECK(world.session.radios[0].owned&&!world.session.radios[0].faulted);
    wait_published(1,1U);CHECK(transfer.completed[1]==2U);
    CHECK(world.inventory.banks[0].profiles[0].capture_epoch!=world.inventory.banks[1].profiles[0].capture_epoch);
    CHECK(!memcmp(&frozen,&world.inventory.selected.frozen,sizeof(frozen))&&world.inventory.selected.matching_sweeps==1U);
    strcpy(world.radios[0].rows[0].id,"NEW-CLAIM");transfer.capture_epoch[0]++;
    complete(0,2U);wait_published(0,2U);
    CHECK(!strcmp(world.inventory.selected.latest.observation.id,"NEW-CLAIM")&&world.inventory.selected.matching_sweeps==2U);
    CHECK(!memcmp(&frozen,&world.inventory.selected.frozen,sizeof(frozen)));
    CHECK(transfer.steps==14U*6U);
}
static void preserve_previous_on_failures(void)
{
    for(unsigned defect=0;defect<8U;++defect){
        async_reset(false);async_start();complete(0,2U);wait_published(0,1U);
        cerberus_remote_id_page_t page;CHECK(cerberus_remote_id_inventory_page(&world.inventory,0,&page));
        CHECK(cerberus_remote_id_inventory_select(&world.inventory,&page.rows[0]));
        const cerberus_remote_id_bank_t retained=world.inventory.banks[0];
        const cerberus_remote_id_selection_t selected=world.inventory.selected;
        async_tick(20U);CHECK(world.radios[0].starts==2U);complete(0,2U);
        if(defect==0U){transfer.fail_slot=0;transfer.fail_chunk=6;}
        if(defect==1U)transfer.mixed_epoch_slot=0;
        if(defect==2U)transfer.mixed_availability_slot=0;
        if(defect==3U)world.radios[0].status_corrupt=2U;
        if(defect==4U)world.radios[0].end_epoch_change=true;
        if(defect==5U)world.radios[0].end_poll_fail=true;
        if(defect==6U)transfer.invalid_slot=0;
        if(defect==7U)world.radios[0].status_fail=1U;
        for(unsigned n=0;n<120U;++n){
            async_tick(20U);
            CHECK(!memcmp(&retained,&world.inventory.banks[0],sizeof(retained)));
            CHECK(!memcmp(&selected,&world.inventory.selected,sizeof(selected)));
            if(world.session.radios[0].error)break;
        }
        CHECK(world.session.radios[0].capture_pending&&world.session.radios[0].error&&transfer.cancels>=2U);
        CHECK(world.radios[0].starts==2U&&!world.radios[0].stops);
        transfer.fail_slot=transfer.mixed_epoch_slot=transfer.mixed_availability_slot=transfer.invalid_slot=-1;
        world.radios[0].status_corrupt=world.radios[0].status_fail=0U;
        world.radios[0].end_epoch_change=world.radios[0].end_poll_fail=false;
        wait_published(0,2U);CHECK(world.inventory.selected.matching_sweeps==2U);
        CHECK(!memcmp(&selected.frozen,&world.inventory.selected.frozen,sizeof(selected.frozen)));
    }
}
static void stop_during_copy(void)
{
    for(unsigned phase=0;phase<3U;++phase){
        async_reset(true);async_start();complete(0,2U);wait_reading(0,phase==2U?1U:0U,phase==0U?1U:13U);
        unsigned calls=transfer.steps,starts=world.radios[0].starts+world.radios[1].starts,cancels=transfer.cancels;
        world.radios[1].stop_mode=LOST_DELIVERED;
        cerberus_remote_id_session_stop(&world.session);
        CHECK(!cerberus_remote_id_session_released(&world.session)&&!cerberus_remote_id_session_start(&world.session));
        async_tick(20U);CHECK(transfer.cancels==cancels+1U&&!transfer.active&&!world.session.capture.owner);
        for(unsigned n=0;n<20U;++n)async_tick(100U);
        CHECK(cerberus_remote_id_session_released(&world.session)&&transfer.steps==calls);
        CHECK(world.radios[0].starts+world.radios[1].starts==starts&&!world.inventory.banks[0].published&&!world.inventory.banks[1].published);
    }
}
static void expiry_and_legacy(void)
{
    for(unsigned fault=0;fault<4U;++fault){
        async_reset(true);async_start();complete(0,2U);wait_reading(0,0,5U);
        uint64_t started=world.session.radios[0].capture_since_ms;
        if(fault==0U)world.radios[0].poll_fail=true;
        if(fault==1U){transfer.fail_slot=0;transfer.fail_chunk=5;}
        if(fault==2U)world.radios[0].end_poll_fail=true;
        if(fault==3U){world.now=started-1U;async_tick(0U);}
        else {
            if(fault==2U){ /* Reach and hold the final peer proof. */
                for(unsigned i=0;i<100U&&!world.session.radios[0].error;++i)async_tick(20U);
                CHECK(world.session.radios[0].error);
            }
            world.now=started+14999U;async_tick(0U);CHECK(world.session.radios[0].capture_pending);
            async_tick(1U);
        }
        CHECK(!world.session.radios[0].capture_pending&&world.session.radios[0].faulted&&!world.inventory.banks[0].published);
        CHECK(!world.session.capture.owner&&!transfer.active);
        complete(1,1U);wait_published(1,1U);CHECK(world.radios[0].starts==1U&&world.session.radios[0].faulted);
    }
    async_reset(false);transfer.legacy[0]=true;async_start();complete(0,2U);wait_published(0,1U);
    CHECK(world.inventory.banks[0].count==2U&&!world.inventory.banks[0].profiles[0].available&&
        !world.inventory.banks[0].profiles[1].available&&transfer.requested_epoch[0][1]==0U);
    CHECK(!memcmp(&world.inventory.banks[0].profiles[0],&(cerberus_watch_profile_evidence_t){0},sizeof(cerberus_watch_profile_evidence_t)));
    async_reset(false);async_start();complete(0,0U);wait_published(0,1U);CHECK(!transfer.steps&&world.inventory.banks[0].count==0U);
}
int main(void)
{
    dual_progress_and_frozen_selection();preserve_previous_on_failures();stop_during_copy();expiry_and_legacy();
    printf("Remote ID incremental profile session: %u checks; 14-step rows, serialized staging, independent radios, atomic epoch/peer/status failures, Stop/deadline/clock cleanup and frozen selections passed.\n",checks);
    return 0;
}
