#include "probe_sniff_scan.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static unsigned checks;
static cerberus_probe_sniff_table_t table;
static cerberus_probe_sniff_scan_t scanner;
static cerberus_probe_sniff_snapshot_t snapshot, unchanged;
static uint64_t epoch, session;
static int64_t clock_us;
static uint8_t mode, channel, pin_kind, pin_index;
static uint32_t pin, pin_counter;
static uint8_t pinned[CERBERUS_PROBE_SNIFF_SUMMARY_BYTES];
static bool owned, exists, hold_stop, lost_start, reset_before_start, lost_release;
static unsigned wire_calls, starts, stops, releases, status_reads, stop_before_reads;
static unsigned corrupt_control, corrupt_read, fail_part, wire_cost;
static esp_err_t source_error;

static void source_reset(void)
{
    memset(&table,0,sizeof(table)); epoch=100U;session=0U;clock_us=1000000;
    mode=channel=pin_kind=pin_index=0U;pin=pin_counter=0U;owned=exists=hold_stop=false;
    lost_start=reset_before_start=lost_release=false;wire_calls=starts=stops=releases=status_reads=0U;
    stop_before_reads=corrupt_control=corrupt_read=wire_cost=0U;fail_part=UINT32_MAX;source_error=ESP_OK;
    cerberus_probe_sniff_scan_init(&scanner);
}
static void fill_control(uint64_t requested, cerberus_probe_sniff_control_t *r)
{
    memset(r,0,sizeof(*r));r->session_id=requested;
    if(!exists||requested!=session){r->error_code=ESP_ERR_NOT_FOUND;return;}
    r->error_code=source_error;r->revision=table.summary.revision;r->count=table.summary.count;
    r->capacity=CERBERUS_PROBE_SNIFF_CAPACITY;r->mode=mode;r->channel=owned?channel:0U;
    r->flags=CERBERUS_PROBE_SNIFF_BOUND;
    if(owned){r->flags|=CERBERUS_PROBE_SNIFF_OWNED|CERBERUS_PROBE_SNIFF_RECEIVING;r->state=CERBERUS_PROBE_SNIFF_RUNNING;}
    else {r->flags|=CERBERUS_PROBE_SNIFF_DATA_READY;r->state=source_error?CERBERUS_PROBE_SNIFF_ERROR:CERBERUS_PROBE_SNIFF_COMPLETE;}
    if(table.summary.capacity_dropped_frames)r->flags|=CERBERUS_PROBE_SNIFF_OVERFLOW;
    if(table.summary.flags&CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED)r->flags|=CERBERUS_PROBE_SNIFF_SATURATED;
}
static esp_err_t fake_control(void *context,uint8_t command,const cerberus_probe_sniff_start_t *q,
    cerberus_probe_sniff_control_t *r)
{
    (void)context;unsigned cost=command==CERBERUS_PROBE_SNIFF_COMMAND_INFO||command==CERBERUS_PROBE_SNIFF_COMMAND_START?2U:1U;
    wire_calls+=cost;clock_us+=(int64_t)wire_cost*cost;memset(r,0,sizeof(*r));
    if(command==CERBERUS_PROBE_SNIFF_COMMAND_INFO){r->session_id=epoch;r->capacity=32U;return ESP_OK;}
    CHECK(q&&q->session_id);
    if(command==CERBERUS_PROBE_SNIFF_COMMAND_START){
        ++starts;if(reset_before_start){reset_before_start=false;epoch+=10000U;}
        if(q->session_id!=epoch+1U||exists){r->session_id=q->session_id;r->error_code=ESP_ERR_INVALID_STATE;return ESP_OK;}
        session=epoch=q->session_id;exists=owned=true;mode=q->mode;channel=q->channel?q->channel:1U;
        cerberus_probe_sniff_table_begin(&table,session,(uint64_t)clock_us);
        table.summary.attempted_channels=table.summary.successful_channels=1U;
        fill_control(session,r);if(lost_start){lost_start=false;return ESP_ERR_TIMEOUT;}
    }else if(command==CERBERUS_PROBE_SNIFF_COMMAND_STOP){
        ++stops;stop_before_reads=wire_calls;
        if(exists&&q->session_id==session&&!hold_stop){owned=false;cerberus_probe_sniff_table_finish(&table,session,clock_us);}
        fill_control(q->session_id,r);
    }else if(command==CERBERUS_PROBE_SNIFF_COMMAND_RELEASE){
        ++releases;
        if(exists&&q->session_id==session&&!owned){exists=false;r->session_id=q->session_id;
            if(lost_release){lost_release=false;return ESP_ERR_TIMEOUT;}return ESP_OK;}
        r->session_id=q->session_id;r->error_code=ESP_ERR_NOT_FOUND;
    }else {CHECK(command==CERBERUS_PROBE_SNIFF_COMMAND_STATUS);++status_reads;fill_control(q->session_id,r);}
    if(corrupt_control){unsigned fault=corrupt_control;corrupt_control=0U;
        if(fault==1)r->count=33U;else if(fault==2)r->count=255U;
        else {memset(r,0,sizeof(*r));r->session_id=q->session_id;r->error_code=ESP_ERR_NOT_FOUND;r->count=1U;}}
    return ESP_OK;
}
static esp_err_t fake_read(void *context,const cerberus_probe_sniff_read_t *q,cerberus_probe_sniff_chunk_t *r)
{
    (void)context;++wire_calls;clock_us+=wire_cost;memset(r,0,sizeof(*r));
    if(fail_part==q->part){fail_part=UINT32_MAX;return ESP_ERR_TIMEOUT;}
    if(!exists||q->session_id!=session)return ESP_ERR_NOT_FOUND;
    if(!q->part){
        CHECK(!q->pin);pin=++pin_counter;pin_kind=q->kind;pin_index=q->index;memset(pinned,0,sizeof(pinned));
        bool good=q->kind==CERBERUS_PROBE_SNIFF_READ_ROW?
            cerberus_probe_sniff_table_copy_row(&table,session,q->index,clock_us,(void*)pinned):
            cerberus_probe_sniff_table_copy_summary(&table,session,clock_us,(void*)pinned);
        if(!good)return ESP_ERR_INVALID_STATE;
    }else if(q->pin!=pin||q->kind!=pin_kind||q->index!=pin_index)return ESP_ERR_NOT_FOUND;
    size_t size=q->kind==CERBERUS_PROBE_SNIFF_READ_ROW?76U:84U,offset=q->part*9U;
    if(offset>=size)return ESP_ERR_INVALID_ARG;
    size_t amount=size-offset;if(amount>9U)amount=9U;
    r->session_id=session;r->pin=pin;r->part=q->part;memcpy(r->data,pinned+offset,amount);
    if(corrupt_read){unsigned f=corrupt_read;corrupt_read=0U;
        if(f==1)++r->session_id;else if(f==2)++r->part;else if(f==3)r->pin=0;else r->data[8]=0xffU;}
    return ESP_OK;
}
static int64_t fake_clock(void *context){(void)context;return clock_us;}
static const cerberus_probe_sniff_transport_t transport={fake_control,fake_read,fake_clock,NULL};
static void tick(unsigned advance_ms)
{clock_us+=(int64_t)advance_ms*1000;unsigned before=wire_calls;
 cerberus_probe_sniff_scan_tick(&scanner,(uint64_t)clock_us/1000U,&transport);CHECK(wire_calls-before<=4U);}
static void packet(uint8_t mac_tail,const uint8_t *name,uint8_t length,bool retry)
{
    uint8_t frame[58]={0x40};frame[1]=retry?8U:0U;frame[10]=2U;frame[15]=mac_tail;
    frame[24]=0U;frame[25]=length;if(length)memcpy(frame+26U,name,length);
    cerberus_probe_sniff_row_t row;
    CHECK(cerberus_probe_sniff_decode(frame,26U+length,channel?channel:1U,-42,(uint64_t)clock_us,&row)==CERBERUS_PROBE_SNIFF_DECODE_VALID);
    bool stored=cerberus_probe_sniff_table_commit(&table,session,&row,(uint64_t)clock_us);
    CHECK(stored||table.summary.count==32U);clock_us+=1000;
}
static void start_and_seed(unsigned count)
{
    CHECK(cerberus_probe_sniff_scan_begin(&scanner,CERBERUS_PROBE_SNIFF_FIXED,1)==ESP_OK);tick(0);
    CHECK(owned&&starts==1U&&scanner.request.session_id==101U);
    for(unsigned i=0;i<count;++i){uint8_t name[3]={(uint8_t)i,0U,'X'};packet((uint8_t)(i+1U),name,3U,i&1U);}
}
static void publish(bool frozen)
{uint64_t before=scanner.publication;for(unsigned i=0;i<1000U&&
    (scanner.publication==before||scanner.published.frozen!=frozen);++i)tick(20U);
 CHECK(scanner.publication>before&&scanner.published.frozen==frozen);}
static void close_scan(void)
{cerberus_probe_sniff_scan_close(&scanner);for(unsigned i=0;i<1000U&&!cerberus_probe_sniff_scan_released(&scanner);++i)tick(20U);
 CHECK(cerberus_probe_sniff_scan_released(&scanner)&&!exists&&!owned);}

static void model_checks(void)
{
    source_reset();start_and_seed(32U);publish(false);CHECK(status_reads>1U);
    cerberus_probe_sniff_scan_stop(&scanner);publish(true);snapshot=scanner.published;
    CHECK(cerberus_probe_sniff_snapshot_valid(&snapshot));
    uint8_t indexes[32];memset(indexes,0xa5,sizeof(indexes));
    for(unsigned group=0;group<3U;++group){CHECK(cerberus_probe_sniff_order(&snapshot,(cerberus_probe_sniff_group_t)group,indexes)==32U);
        for(unsigned i=0;i<32U;++i)CHECK(indexes[i]==i);}
    unchanged=snapshot;CHECK(!cerberus_probe_sniff_order(&snapshot,CERBERUS_PROBE_SNIFF_PAIRS,(void*)&snapshot));
    CHECK(!memcmp(&snapshot,&unchanged,sizeof(snapshot)));
    CHECK(!cerberus_probe_sniff_group_view(&snapshot,CERBERUS_PROBE_SNIFF_PAIRS,0,(void*)&snapshot));
    CHECK(!memcmp(&snapshot,&unchanged,sizeof(snapshot)));
    snapshot.rows[0].row.ssid_length=255U;CHECK(!cerberus_probe_sniff_order(&snapshot,CERBERUS_PROBE_SNIFF_NETWORKS,indexes));
    snapshot=unchanged;char text[33];cerberus_probe_sniff_name(&snapshot.rows[0].row,text,sizeof(text));CHECK(!strcmp(text,"..X"));
    snapshot.rows[0].row.ssid_length=0U;memset(snapshot.rows[0].row.ssid,0,32U);
    cerberus_probe_sniff_name(&snapshot.rows[0].row,text,sizeof(text));CHECK(!strcmp(text,"[WILDCARD]"));
    snapshot=unchanged;memcpy(snapshot.rows[1].row.transmitter,snapshot.rows[0].row.transmitter,6U);
    CHECK(cerberus_probe_sniff_order(&snapshot,CERBERUS_PROBE_SNIFF_SENDERS,indexes)==31U);
    cerberus_probe_sniff_group_view_t group;
    CHECK(cerberus_probe_sniff_group_view(&snapshot,CERBERUS_PROBE_SNIFF_SENDERS,0U,&group)&&group.pairs==2U&&group.frames==2U);
    snapshot=unchanged;snapshot.summary.retained_frames++;snapshot.summary.valid_frames++;
    CHECK(!cerberus_probe_sniff_snapshot_valid(&snapshot));snapshot.frozen=false;
    CHECK(cerberus_probe_sniff_snapshot_valid(&snapshot));snapshot=unchanged;
    static char report[8193],prior[8193];size_t length=0U;
    CHECK(cerberus_probe_sniff_report(&snapshot,NULL,0U,&length)&&length<8192U);
    memset(report,0x5a,sizeof(report));memcpy(prior,report,sizeof(report));
    CHECK(!cerberus_probe_sniff_report(&snapshot,report,length,&length)&&!length&&!memcmp(report,prior,sizeof(report)));
    CHECK(cerberus_probe_sniff_report(&snapshot,report,sizeof(report),&length));
    CHECK(strstr(report,"000058")&&strstr(report,"callback")&&!strstr(report,"name\n"));
    CHECK(!cerberus_probe_sniff_report(&snapshot,report,sizeof(report),(void*)&snapshot));
    CHECK(!memcmp(&snapshot,&unchanged,sizeof(snapshot)));
    CHECK(!cerberus_probe_sniff_report(&snapshot,report,sizeof(report),(void*)report));
    snapshot=unchanged;
    for(unsigned i=0;i<32U;++i){cerberus_probe_sniff_row_t *r=&snapshot.rows[i].row;
        r->ssid_length=32U;memset(r->ssid,0xff,32U);r->ssid[0]=(uint8_t)i;
        r->first_callback_us=INT64_MAX-1;r->last_callback_us=INT64_MAX;
        snapshot.rows[i].copied_source_us=INT64_MAX;r->frames=r->retry_frames=UINT32_MAX;
        r->flags|=CERBERUS_PROBE_SNIFF_ROW_SATURATED;r->last_channel=177U;r->last_rssi=-127;r->last_sequence_control=65520U;}
    snapshot.summary.started_source_us=0U;snapshot.summary.stopped_source_us=snapshot.summary.copied_source_us=INT64_MAX;
    snapshot.summary.valid_frames=snapshot.summary.retained_frames=snapshot.summary.retry_frames=UINT32_MAX;
    snapshot.summary.capacity_dropped_frames=UINT32_MAX;snapshot.summary.malformed_frames=UINT32_MAX;
    snapshot.summary.unsupported_frames=snapshot.summary.clock_dropped_frames=snapshot.summary.channel_failures=UINT32_MAX;
    snapshot.summary.flags|=CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED;snapshot.control.flags|=CERBERUS_PROBE_SNIFF_SATURATED|CERBERUS_PROBE_SNIFF_OVERFLOW;
    CHECK(cerberus_probe_sniff_snapshot_valid(&snapshot));
    CHECK(cerberus_probe_sniff_report(&snapshot,report,sizeof(report),&length)&&length<=8192U);
    printf("Probe report worst fixture: %zu bytes; snapshot %zu; scan %zu\n",length,sizeof(snapshot),sizeof(scanner));
    close_scan();
}
static void controller_checks(void)
{
    source_reset();CHECK(cerberus_probe_sniff_scan_begin(&scanner,0U,1U)==ESP_OK);
    cerberus_probe_sniff_scan_stop(&scanner);tick(0);CHECK(!starts&&!wire_calls&&cerberus_probe_sniff_scan_released(&scanner));
    source_reset();lost_start=true;CHECK(cerberus_probe_sniff_scan_begin(&scanner,0U,1U)==ESP_OK);tick(0);
    CHECK(owned&&starts==1U&&scanner.request.session_id&&scanner.error==ESP_ERR_TIMEOUT);
    tick(700);CHECK(scanner.have_control&&starts==1U&&!stops);close_scan();
    source_reset();reset_before_start=true;CHECK(cerberus_probe_sniff_scan_begin(&scanner,0U,1U)==ESP_OK);tick(0);
    CHECK(!owned&&!exists&&!scanner.request.session_id&&starts==1U);tick(1000);CHECK(starts==1U);
    source_reset();start_and_seed(4U);publish(false);uint64_t published=scanner.publication;
    for(unsigned fault=1U;fault<=3U;++fault){corrupt_control=fault;tick(1000);
        CHECK(scanner.request.session_id==session&&scanner.error==ESP_ERR_INVALID_RESPONSE&&scanner.publication==published);}
    tick(700);for(unsigned i=0;i<3U;++i){fail_part=i;tick(500);for(unsigned j=0;j<10U&&fail_part!=UINT32_MAX;++j)tick(20);
        CHECK(scanner.publication==published&&scanner.request.session_id==session);}
    hold_stop=true;cerberus_probe_sniff_scan_stop(&scanner);unsigned before=wire_calls;tick(0);
    CHECK(stops&&wire_calls-before==1U&&scanner.request.session_id&&owned);
    before=wire_calls;tick(699);CHECK(wire_calls==before);tick(1);CHECK(wire_calls==before+1U);
    hold_stop=false;source_error=ESP_FAIL;publish(true);
    CHECK(scanner.error==ESP_FAIL&&scanner.published.control.error_code==ESP_FAIL);
    lost_release=true;cerberus_probe_sniff_scan_close(&scanner);tick(0);
    CHECK(scanner.request.session_id&&!exists);tick(700);CHECK(cerberus_probe_sniff_scan_released(&scanner));
    source_reset();wire_cost=4000U;CHECK(cerberus_probe_sniff_scan_begin(&scanner,0U,1U)==ESP_OK);tick(0);
    CHECK(wire_calls==2U&&!starts);tick(20);CHECK(starts==1U);wire_cost=0;close_scan();
}
int main(void)
{model_checks();controller_checks();printf("Probe S3 model/controller: %u checks passed.\n",checks);return 0;}
