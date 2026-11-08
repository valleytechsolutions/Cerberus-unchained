#include "ble_response.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while(0)
static cerberus_dual_ble_snapshot_family_row_t original;
static bool valid(const cerberus_dual_ble_response_wire_t *r,uint8_t family)
{ return cerberus_dual_ble_response_valid(r,&original,family); }

int main(void)
{
    original.metadata.row.address[0]=2;original.metadata.row.address_type=1;
    original.metadata.row.raw_length=3;original.metadata.row.event_type=3;
    original.metadata.receive=(cerberus_dual_ble_receive_metadata_t){2,2,3,7,1,1,0,1};
    const uint8_t samples[6][31]={
        {26,0xff,0x4c,0,2,0x15,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x12,0x34,0xab,0xcd,0xc5},
        {3,3,0x44,0xfd},{3,3,0x81,0x30},{15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'},
        {3,3,0x5f,0xfd},{6,9,'H','C','-','0','5'}};
    const uint8_t lengths[]={27,4,4,16,4,7};
    cerberus_dual_ble_response_wire_t unknown={0};CHECK(valid(&unknown,0));
    for(size_t i=0;i<sizeof(unknown);++i){((uint8_t*)&unknown)[i]=0xa5;CHECK(!valid(&unknown,0));((uint8_t*)&unknown)[i]=0;}
    cerberus_dual_ble_response_wire_t none={.version=1,.state=1,.original_sequence=UINT64_C(0xf000000000000001)};
    CHECK(valid(&none,0));none.original_sequence=0;CHECK(!valid(&none,0));none.original_sequence=1;
    original.metadata.receive.sid=255;CHECK(!valid(&none,0));original.metadata.receive.sid=7;
    original.metadata.receive.properties=0;CHECK(!valid(&none,0));original.metadata.receive.properties=2;
    original.metadata.receive.ad_validation=2;CHECK(!valid(&none,0));original.metadata.receive.ad_validation=1;
    for(uint8_t family=0;family<=6;++family){
        uint8_t data[255]={39,0x30};uint8_t index=family?family-1:5;
        memcpy(data+40,samples[index],lengths[index]);
        cerberus_dual_ble_family_evidence_t facts;
        CHECK(cerberus_dual_ble_family_parse_ad(data,255,original.metadata.row.address,1,&facts));
        cerberus_dual_ble_response_wire_t r={.version=1,.state=2,.flags=3,.raw_length=31,.event_type=3,.rssi=-71,.raw_flags=1,
            .original_sequence=UINT64_C(0xf123456789abcdef),.response_sequence=UINT64_C(0xf23456789abcdef0),
            .age_ms=0x1020304,.interval_us=2000000,.receive={2,10,255,7,1,1,0,1}};
        memcpy(r.raw,data,31);CHECK(cerberus_dual_ble_response_family_encode(&facts,family,&r.family));CHECK(valid(&r,family));
        CHECK(!valid(&r,family==6?0:family+1));
        uint8_t bytes[152];cerberus_dual_ble_response_encode_bytes(&r,bytes);
        /* Independent fixed scalar offsets/LE bytes, not struct memcpy. */
        const uint8_t original_le[]={0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0xf1};
        const uint8_t response_le[]={0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0xf2};
        CHECK(!memcmp(bytes+8,original_le,8)&&!memcmp(bytes+16,response_le,8));
        CHECK(bytes[24]==4&&bytes[25]==3&&bytes[26]==2&&bytes[27]==1);
        CHECK(bytes[28]==0x80&&bytes[29]==0x84&&bytes[30]==0x1e&&bytes[31]==0);
        CHECK(bytes[32]==2&&bytes[33]==10&&bytes[34]==255&&bytes[35]==7);
        CHECK(!memcmp(bytes+40,data,31)&&bytes[103]==1&&bytes[104]==family&&bytes[151]==0);
        cerberus_dual_ble_response_wire_t round;memset(&round,0xa5,sizeof(round));
        cerberus_dual_ble_response_decode_bytes(bytes,&round);CHECK(!memcmp(&round,&r,sizeof(r))&&valid(&round,family));
        for(unsigned fault=0;fault<29;++fault){cerberus_dual_ble_response_wire_t bad=r;
            switch(fault){case 0:bad.version=2;break;case 1:bad.state=3;break;case 2:bad.flags|=16;break;
            case 3:bad.raw_length=32;break;case 4:bad.event_type=4;break;case 5:bad.name_length=33;break;
            case 6:bad.raw_flags=3;break;case 7:bad.reserved_tail=1;break;case 8:bad.original_sequence=0;break;
            case 9:bad.response_sequence=bad.original_sequence;break;case 10:bad.interval_us++;break;
            case 11:bad.age_ms=UINT32_MAX;break;case 12:bad.flags&=(uint8_t)~1U;break;
            case 13:bad.rssi=127;break;case 14:bad.flags&=(uint8_t)~2U;break;
            case 15:bad.receive.source_kind=0;break;case 16:bad.receive.properties=2;break;
            case 17:bad.receive.sid=255;break;case 18:bad.receive.primary_phy=3;break;
            case 19:bad.receive.secondary_phy=2;break;case 20:bad.receive.data_status=1;break;
            case 21:bad.receive.ad_validation=2;break;case 22:bad.receive.callback_length=31;break;
            case 23:bad.name[31]='x';break;case 24:bad.flags|=4;break;case 25:bad.flags|=8;break;
            case 26:bad.family.reserved[0]=1;break;case 27:bad.family.categories^=1;break;
            case 28:bad.family.flags^=2;break;}
            CHECK(!valid(&bad,family));
        }
        r.flags&=(uint8_t)~3U;r.rssi=127;r.age_ms=UINT32_MAX;CHECK(valid(&r,family));
        r.original_sequence=UINT64_MAX-1;r.response_sequence=UINT64_MAX;CHECK(valid(&r,family));
        r.response_sequence=0;CHECK(!valid(&r,family));
    }
    /* Exact legacy and legacy-PDU EXT domain pairs; never cross-source match. */
    const uint8_t hc[]={6,9,'H','C','-','0','5'};cerberus_dual_ble_family_evidence_t facts;
    CHECK(cerberus_dual_ble_family_parse_ad(hc,sizeof(hc),original.metadata.row.address,1,&facts));
    cerberus_dual_ble_response_wire_t r={.version=1,.state=2,.flags=3,.raw_length=7,.event_type=4,.rssi=-50,
        .original_sequence=1,.response_sequence=2,.receive={1,0,7,255,0,0,255,1}};
    memcpy(r.raw,hc,7);CHECK(cerberus_dual_ble_response_family_encode(&facts,6,&r.family));
    original.metadata.receive=(cerberus_dual_ble_receive_metadata_t){1,0,3,255,0,0,255,1};original.metadata.row.event_type=0;
    CHECK(valid(&r,6));original.metadata.row.event_type=2;CHECK(valid(&r,6));original.metadata.row.event_type=3;CHECK(!valid(&r,6));
    original.metadata.receive=(cerberus_dual_ble_receive_metadata_t){2,0x12,3,255,1,0,0,1};original.metadata.row.event_type=2;
    CHECK(!valid(&r,6));r.receive=(cerberus_dual_ble_receive_metadata_t){2,0x1a,7,255,1,0,0,1};CHECK(valid(&r,6));
    original.metadata.receive.properties=0x13;original.metadata.row.event_type=0;r.receive.properties=0x1b;CHECK(valid(&r,6));
    r.raw[30]=1;CHECK(!valid(&r,6));
    printf("Response source/portable codec: %u checks; canonical UNKNOWN/NONE/PRESENT, all6 full255, explicit LE golden offsets, typed/context/SID/PHY/clock/sequence and separate domains passed\n",checks);
    return 0;
}
