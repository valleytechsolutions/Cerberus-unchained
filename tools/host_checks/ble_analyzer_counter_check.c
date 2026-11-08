#undef NDEBUG
#include "ble_analyzer_counter.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static cerberus_ble_analyzer_counter_t counter;
static cerberus_dual_ble_analyzer_sample_t sample(int64_t now)
{ cerberus_dual_ble_analyzer_sample_t out;cerberus_ble_analyzer_counter_sample(&counter,now,&out);return out; }
static void begin(uint32_t epoch,int64_t now)
{cerberus_ble_analyzer_counter_arm(&counter,epoch,now);cerberus_ble_analyzer_counter_accept(&counter,true);}
int main(void)
{
    cerberus_ble_analyzer_counter_reset(&counter);
    cerberus_dual_ble_analyzer_sample_t out=sample(0);
    CHECK(!out.flags&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
    cerberus_ble_analyzer_counter_arm(&counter,7,0);
    CHECK(!sample(1000).flags); /* An enqueued or attempted SDK start is not accepted RF. */
    cerberus_ble_analyzer_counter_observe(&counter,7,0,-40,0);
    cerberus_ble_analyzer_counter_accept(&counter,true);
    out=sample(0);CHECK(out.flags==17&&out.reports==1&&!out.elapsed_ms&&out.rssi==-40);
    for(unsigned type=0;type<4;++type)cerberus_ble_analyzer_counter_observe(&counter,7,(uint8_t)type,-50,1000);
    cerberus_ble_analyzer_counter_observe(&counter,6,0,-1,-1);
    cerberus_ble_analyzer_counter_observe(&counter,7,4,-1,-1);
    CHECK(sample(2000).reports==5&&!counter.clock_invalid);
    /* Timestamp captured before a newer query acquires the mutex is legal;
     * callback ordering and query ordering have separate high-water clocks. */
    cerberus_ble_analyzer_counter_observe(&counter,7,0,-60,1500);
    out=sample(3000);CHECK(out.reports==6&&out.elapsed_ms==3&&out.rssi==-60);
    cerberus_ble_analyzer_counter_observe(&counter,7,0,127,3000);
    out=sample(4000);CHECK(out.flags==1&&out.rssi==127&&out.reports==7);
    cerberus_ble_analyzer_counter_observe(&counter,7,0,-70,2999);
    out=sample(5000);CHECK(out.flags==32&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
    CHECK(sample(6000).flags==32); /* Invalidity cannot heal within an epoch. */
    begin(8,100000);CHECK(sample(99999).flags==32);
    begin(9,0);CHECK(sample(-1).flags==32);
    begin(10,-1);CHECK(sample(0).flags==32);
    begin(0,0);CHECK(!sample(0).flags);
    begin(11,INT64_MAX-2000);out=sample(INT64_MAX);CHECK(out.flags==1&&out.elapsed_ms==2);
    begin(12,0);out=sample((int64_t)UINT32_MAX*1000LL+999LL);
    CHECK(out.flags==1&&out.elapsed_ms==UINT32_MAX);
    out=sample(((int64_t)UINT32_MAX+1LL)*1000LL);CHECK(out.flags==32&&!out.elapsed_ms);
    begin(13,1000);counter.reports=UINT32_MAX-1;
    cerberus_ble_analyzer_counter_observe(&counter,13,1,-127,2000);
    out=sample(3000);CHECK(out.flags==21&&out.reports==UINT32_MAX&&out.rssi==-127);
    cerberus_ble_analyzer_counter_observe(&counter,13,1,20,4000);
    out=sample(5000);CHECK(out.flags==21&&out.reports==UINT32_MAX&&out.rssi==20);
    cerberus_ble_analyzer_counter_observe(&counter,13,1,21,6000);
    out=sample(7000);CHECK(out.flags==5&&out.rssi==127);
    begin(14,1000000);cerberus_ble_analyzer_counter_observe(&counter,14,0,-30,2000000);
    cerberus_ble_analyzer_counter_finish(&counter,9000000);
    out=sample(10000000);CHECK(out.flags==25&&out.elapsed_ms==8000&&out.reports==1);
    cerberus_ble_analyzer_counter_observe(&counter,14,0,-20,11000000);
    cerberus_ble_analyzer_counter_finish(&counter,11000000);
    out=sample(12000000);CHECK(out.flags==25&&out.elapsed_ms==8000&&out.reports==1&&out.rssi==-30);
    CHECK(sample(11999999).flags==32);
    begin(15,0);cerberus_ble_analyzer_counter_observe(&counter,15,0,-40,1);
    cerberus_ble_analyzer_counter_accept(&counter,false);
    out=sample(100);CHECK(!out.flags&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
    printf("C5 Analyzer pure counter: %u checks, size=%zu bytes; clocks, pending/failed start, typed epochs, saturation and frozen final interval passed\n",checks,sizeof(counter));
    return 0;
}
