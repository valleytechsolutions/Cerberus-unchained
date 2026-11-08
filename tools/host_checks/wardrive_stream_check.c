#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wardrive_stream.h"
#include "wardrive_decode.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    cerberus_wd_record_t rows[3],event={.kind=1,.age_ms=100,.address={2,3,4,5,6,7}},out;
    cerberus_wd_ring_t q={.rows=rows,.capacity=3};
    /* Same identity and signal remain three different observations. */
    assert(cerberus_wd_ring_push(&q,&event));assert(cerberus_wd_ring_push(&q,&event));
    assert(cerberus_wd_ring_push(&q,&event));assert(!cerberus_wd_ring_push(&q,&event));
    assert(q.dropped==1&&q.high_water==3);
    assert(cerberus_wd_ring_read(&q,150,&out)&&out.sequence==1&&out.age_ms==50);
    assert(!cerberus_wd_ring_ack(&q,2));
    assert(cerberus_wd_ring_read(&q,160,&out)&&out.sequence==1);
    assert(cerberus_wd_ring_ack(&q,1));assert(cerberus_wd_ring_ack(&q,1));
    assert(q.count==2&&cerberus_wd_ring_read(&q,170,&out)&&out.sequence==2);
    for(unsigned i=2;i<=3;++i)assert(cerberus_wd_ring_ack(&q,i));
    for(unsigned i=0;i<100000;++i){
        assert(cerberus_wd_ring_push(&q,&event));
        assert(cerberus_wd_ring_read(&q,200,&out));
        assert(cerberus_wd_ring_ack(&q,out.sequence));
    }
    q.next_sequence=UINT32_MAX;assert(!cerberus_wd_ring_push(&q,&event));
    assert(!cerberus_wd_ring_read(&q,200,&out)&&!out.sequence);
    uint8_t address[]={1,2,3,4,5,6};uint8_t ad[]={4,9,'A',',','B',3,255,0x4c,0};
    assert(cerberus_wd_ble_decode(ad,sizeof(ad),address,1,-42,1234,&out));
    assert(out.kind==2&&out.manufacturer==0x4c&&out.address[0]==6&&out.label_length==3);
    assert(out.label[1]==','&&out.age_ms==1234);
    assert(!cerberus_wd_ble_decode(ad,sizeof(ad)-1,address,1,-42,1234,&out));
    assert(cerberus_wd_ble_decode(NULL,0,address,0,-30,1234,&out)&&out.manufacturer==65535);
    assert(!cerberus_wd_ble_decode(ad,sizeof(ad),address,4,-42,1234,&out));
    uint8_t rsn[]={1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2};
    assert(cerberus_wd_rsn_security(rsn,sizeof(rsn))==CERBERUS_WIFI_SECURITY_WPA2);
    rsn[17]=8;assert(cerberus_wd_rsn_security(rsn,sizeof(rsn))==CERBERUS_WIFI_SECURITY_WPA3);
    rsn[17]=18;assert(cerberus_wd_rsn_security(rsn,sizeof(rsn))==CERBERUS_WIFI_SECURITY_OWE);
    rsn[17]=1;assert(cerberus_wd_rsn_security(rsn,sizeof(rsn))==CERBERUS_WIFI_SECURITY_ENTERPRISE);
    for(size_t n=0;n<sizeof(rsn);++n)assert(cerberus_wd_rsn_security(rsn,n)==CERBERUS_WIFI_SECURITY_OTHER);
    uint8_t beacon[80]={0x80};memset(beacon+4,255,6);beacon[10]=2;beacon[16]=4;
    beacon[34]=0x10;beacon[37]=1;beacon[38]='X';beacon[39]=48;beacon[40]=sizeof(rsn);
    memcpy(beacon+41,rsn,sizeof(rsn));
    assert(cerberus_wd_wifi_decode(beacon,41+sizeof(rsn),149,-48,100,&out));
    assert(out.channel==149&&out.security==CERBERUS_WIFI_SECURITY_ENTERPRISE&&out.label[0]=='X');
    assert(!cerberus_wd_wifi_decode(beacon,40+sizeof(rsn),149,-48,100,&out));
    puts("Wardrive stream: 100000 repeated observations, ACK replay, overflow and BLE field checks passed");
    return 0;
}
