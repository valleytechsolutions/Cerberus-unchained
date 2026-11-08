#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "wifi_marauder.h"

static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);assert(x);}}while(0)
#define OK CERBERUS_WIFI_MARAUDER_OK
#define INVALID CERBERUS_WIFI_MARAUDER_INVALID
#define LIMIT CERBERUS_WIFI_MARAUDER_LIMIT
#define UNREP CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE
static cerberus_wifi_marauder_ap_t ap,prior,decoded;
static cerberus_wifi_marauder_ssid_t ssid,ssid_prior,ssid_decoded;
static cerberus_wifi_archive_t archive;
static uint8_t wire[65537],other[65537],guard[65537];
static cerberus_wifi_marauder_export_stats_t stats,stats_prior;

static void reject(const uint8_t *bytes,size_t n,cerberus_wifi_marauder_result_t expected)
{
    memset(&decoded,0x5a,sizeof(decoded));prior=decoded;
    CHECK(cerberus_wifi_marauder_ap_decode(bytes,n,&decoded)==expected);
    CHECK(!memcmp(&decoded,&prior,sizeof(decoded)));
}
static void reject_json(const char *text,cerberus_wifi_marauder_result_t expected)
{reject((const uint8_t*)text,strlen(text),expected);}
static void ap_roundtrip(void)
{
    size_t n=0,m=0;CHECK(cerberus_wifi_marauder_ap_validate(&ap)==OK);
    CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),&n)==OK);
    CHECK(cerberus_wifi_marauder_ap_decode(wire,n,&decoded)==OK);
    CHECK(!memcmp(&ap,&decoded,sizeof(ap)));
    CHECK(cerberus_wifi_marauder_ap_encode(&decoded,other,sizeof(other),&m)==OK&&n==m&&!memcmp(wire,other,n));
}
static void schema(void)
{
    const char *input="[{\"essid\":\"A\\u0000\\n\\\"\\\\\\u00e9\",\"channel\":255,\"bssid\":\"02:12:34:56:78:Ab\",\"rssi\":-2147483648,\"packets\":4294967295,\"sec\":255,\"wps\":false,\"man\":\"Vendor\",\"stations\":[\"00:00:00:00:00:00\",\"FF:FF:FF:FF:FF:FF\"]}]";
    const char *canonical="[{\"essid\":\"A\\u0000\\u000a\\\"\\\\\xc3\xa9\",\"channel\":255,\"bssid\":\"02:12:34:56:78:AB\",\"rssi\":-2147483648,\"packets\":4294967295,\"packet\":4294967295,\"sec\":255,\"wps\":false,\"man\":\"Vendor\",\"stations\":[\"00:00:00:00:00:00\",\"FF:FF:FF:FF:FF:FF\"]}]";
    CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t*)input,strlen(input),&ap)==OK);
    CHECK(ap.count==1&&ap.rows[0].present==511&&ap.rows[0].essid_length==7&&ap.rows[0].essid[1]==0);
    CHECK(ap.rows[0].bssid[0]==2&&ap.rows[0].bssid[5]==0xab&&ap.rows[0].rssi==INT32_MIN&&ap.rows[0].packets==UINT32_MAX);
    CHECK(ap.station_count==2&&ap.rows[0].station_count==2);
    size_t n=0;CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),&n)==OK);
    CHECK(n==strlen(canonical)&&!memcmp(wire,canonical,n));ap_roundtrip();
    for(size_t i=0;i<strlen(input);++i)reject((const uint8_t*)input,i,INVALID);
    const char *good[]={"[]","[{}]","[{\"packet\":9}]","[{\"packets\":9,\"packet\":9}]","[{\"packet\":9,\"packets\":9}]",
        "[{\"essid\":\"\",\"man\":\"\",\"stations\":[]},{\"essid\":\"\"}]","[{\"rssi\":2147483647}]","[{\"rssi\":-0}]",
        "[{\"essid\":\"\\uD83D\\uDE00\"}]","[{\"essid\":\"\xf0\x9f\x98\x80\"}]"};
    for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);++i){CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t*)good[i],strlen(good[i]),&ap)==OK);ap_roundtrip();}
    const char *unknown="[{\"future\":{\"unicode\":\"\\uD834\\uDD1E\",\"nested\":[true,false,null,-2.3e+4]},\"future\":[],\"essid\":\"ok\"}]";
    CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t*)unknown,strlen(unknown),&ap)==OK&&ap.ignored_members==2);
    CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),&n)==OK&&!memcmp(wire,"[{\"essid\":\"ok\"}]",n));
    const char *bad[]={"","{}","null","[null]","[[],{}]","[1]","[{}]x","[{},]","[{,}]","[{\"essid\":null}]",
        "[{\"packet\":1,\"packets\":2}]","[{\"packets\":2,\"packet\":1}]","[{\"packet\":1,\"packet\":1}]",
        "[{\"essid\":\"x\",\"essid\":\"x\"}]","[{\"channel\":-1}]","[{\"channel\":256}]","[{\"sec\":256}]",
        "[{\"channel\":1.0}]","[{\"channel\":1e0}]","[{\"channel\":01}]","[{\"channel\":+1}]","[{\"packets\":4294967296}]",
        "[{\"rssi\":2147483648}]","[{\"rssi\":-2147483649}]","[{\"rssi\":-}]","[{\"wps\":0}]","[{\"wps\":truefalse}]",
        "[{\"bssid\":\"02-00-00-00-00-01\"}]","[{\"bssid\":\"GG:00:00:00:00:01\"}]","[{\"stations\":[null]}]",
        "[{\"stations\":[\"01:02:03:04:05:06\",]}]","[{\"unknown\":NaN}]","[{\"unknown\":01}]",
        "[{\"unknown\":\"\\uD800\"}]","[{\"unknown\":\"\\uDC00\"}]","[{\"unknown\":\"\\uD800\\u0000\"}]",
        "[{\"unknown\":\"\xc0\x80\"}]","[{\"unknown\":\"\xed\xa0\x80\"}]","[{\"unknown\":\"\xf4\x90\x80\x80\"}]",
        "[{\"essid\":\"raw\nnewline\"}]","[{\"essid\":\"\\x00\"}]","[{\"unknown\":[[[[[[[[[[[[]]]]]]]]]]]]}]"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)reject_json(bad[i],i+1==sizeof(bad)/sizeof(bad[0])?LIMIT:INVALID);
    reject_json("[{\"essid\":\"12345678901234567890123456789012345\"}]",LIMIT);
    CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t*)"[{\"essid\":\"same\"},{\"essid\":\"same\"}]",35,&ap)==OK);
    CHECK(ap.count==2&&!memcmp(ap.rows[0].essid,ap.rows[1].essid,32));
}
static void bounds(void)
{
    memset(&ap,0,sizeof(ap));ap.count=64;ap.station_count=256;
    for(unsigned i=0;i<64;++i){cerberus_wifi_marauder_ap_row_t *r=&ap.rows[i];
        r->present=511;r->essid_length=32;r->man_length=64;r->rssi=INT32_MIN;r->packets=UINT32_MAX;r->channel=255;r->security=255;
        for(unsigned j=0;j<32;++j)r->essid[j]=(uint8_t)j;
        for(unsigned j=0;j<64;++j)r->man[j]=(uint8_t)(j%32);
        memset(r->bssid,0xff,6);r->station_offset=(uint16_t)(i*4);r->station_count=4;
    }memset(ap.stations,0xff,sizeof(ap.stations));ap_roundtrip();
    size_t n=0;CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),&n)==OK);
    /* Independent schema byte accounting: all fields, 96 escaped control
     * bytes, both aliases, widest numbers, 4 quoted17-byte station strings. */
    const char *row_template="{\"essid\":\"\",\"channel\":255,\"bssid\":\"FF:FF:FF:FF:FF:FF\",\"rssi\":-2147483648,\"packets\":4294967295,\"packet\":4294967295,\"sec\":255,\"wps\":false,\"man\":\"\",\"stations\":[\"FF:FF:FF:FF:FF:FF\",\"FF:FF:FF:FF:FF:FF\",\"FF:FF:FF:FF:FF:FF\",\"FF:FF:FF:FF:FF:FF\"]}";
    size_t expected=2+63+64*(strlen(row_template)+96*6);CHECK(n==expected&&n>49152&&n<=65536);
    /* Moving256 stations to one row maximizes comma separators: 63more than
     * distributing four per row. Empty station arrays are still present. */
    for(unsigned i=0;i<64;++i){ap.rows[i].station_offset=i?256:0;ap.rows[i].station_count=i?0:256;}
    ap_roundtrip();CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),&n)==OK&&n==expected+63&&n==52224);
    printf("maximum canonical AP JSON %zu bytes\n",n);
    memcpy(other,wire,n);memset(guard,0x55,sizeof(guard));
    for(size_t cap=0;cap<n;cap+=503){memcpy(wire,guard,sizeof(wire));size_t written=3;
        CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,cap,&written)==LIMIT&&written==0);CHECK(!memcmp(wire,guard,sizeof(wire)));}
    memcpy(wire,other,n);CHECK(cerberus_wifi_marauder_ap_decode(wire,n,&decoded)==OK);
    /* More than64 records and more than256 station addresses are explicit
     * limits, never silently truncated. Every active field remains bounded. */
    size_t p=0;wire[p++]='[';for(unsigned i=0;i<65;++i){if(i)wire[p++]=',';wire[p++]='{';wire[p++]='}';}wire[p++]=']';reject(wire,p,LIMIT);
    p=(size_t)sprintf((char*)wire,"[{\"stations\":[");for(unsigned i=0;i<257;++i)p+=(size_t)sprintf((char*)wire+p,"%s\"00:00:00:00:00:00\"",i?",":"");p+=(size_t)sprintf((char*)wire+p,"]}]");reject(wire,p,LIMIT);
    reject(wire,65537,LIMIT);
    for(unsigned field=0;field<10;++field){decoded=ap;
        switch(field){case 0:decoded.count=65;break;case 1:decoded.station_count=257;break;case 2:decoded.rows[0].present|=512;break;
            case 3:decoded.rows[0].station_offset=1;break;case 4:decoded.rows[0].essid_length=33;break;case 5:decoded.rows[0].man_length=65;break;
            case 6:decoded.rows[0].essid[0]=0xff;break;case 7:((uint8_t*)&decoded.rows[0].wps)[0]=2;break;
            case 8:decoded.rows[0].present&=~CERBERUS_WIFI_MARAUDER_AP_CHANNEL;break;case 9:decoded.rows[0].station_count=257;break;}
        size_t written=1;memcpy(wire,guard,sizeof(wire));CHECK(cerberus_wifi_marauder_ap_encode(&decoded,wire,sizeof(wire),&written)!=OK&&written==0);CHECK(!memcmp(wire,guard,sizeof(wire)));}
    size_t written=1;CHECK(cerberus_wifi_marauder_ap_encode(&ap,(uint8_t*)&ap,sizeof(ap),&written)==INVALID);
    CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),(size_t*)&ap)==INVALID);
    CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),(size_t*)wire)==INVALID);
    CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t*)&ap,2,&ap)==INVALID);
    CHECK(cerberus_wifi_marauder_ap_encode(NULL,wire,sizeof(wire),&written)==INVALID&&written==0);
    written=2;CHECK(cerberus_wifi_marauder_ap_encode(&ap,NULL,0,&written)==INVALID&&written==0);
}
static void text_cases(void)
{
    static const uint8_t example[]={0,'A','\r','\n','\n',0xff,'X','\n'};
    CHECK(cerberus_wifi_marauder_ssid_decode(example,sizeof(example),&ssid)==OK);
    CHECK(ssid.count==3&&ssid.final_lf&&ssid.rows[0].length==3&&ssid.rows[0].bytes[0]==0&&ssid.rows[0].bytes[2]=='\r'&&!ssid.rows[1].length&&ssid.rows[2].bytes[0]==0xff);
    size_t n=0;CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,wire,sizeof(wire),&n)==OK&&n==sizeof(example)&&!memcmp(wire,example,n));
    for(unsigned value=0;value<256;++value)if(value!='\n'){
        memset(wire,(int)value,32);wire[32]='\n';wire[33]='\n';memset(wire+34,(int)value,32);
        for(unsigned tail=0;tail<2;++tail){wire[66]='\n';CHECK(cerberus_wifi_marauder_ssid_decode(wire,66+tail,&ssid)==OK);
            CHECK(ssid.count==3&&ssid.final_lf==(tail!=0));CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,other,sizeof(other),&n)==OK&&n==66+tail&&!memcmp(wire,other,n));}
    }
    memset(&ssid,0,sizeof(ssid));ssid.count=64;ssid.final_lf=true;
    for(unsigned i=0;i<64;++i){ssid.rows[i].length=32;memset(ssid.rows[i].bytes,(int)(i+32),32);}
    CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,wire,sizeof(wire),&n)==OK&&n==2112);
    CHECK(cerberus_wifi_marauder_ssid_decode(wire,n,&ssid_decoded)==OK&&!memcmp(&ssid,&ssid_decoded,sizeof(ssid)));
    ssid_prior=ssid;for(size_t cap=0;cap<n;++cap){memset(other,0x55,sizeof(other));size_t written=1;
        CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,other,cap,&written)==LIMIT&&written==0&&other[0]==0x55&&other[2112]==0x55);}
    memset(wire,'\n',65);CHECK(cerberus_wifi_marauder_ssid_decode(wire,65,&ssid)==LIMIT&&!memcmp(&ssid,&ssid_prior,sizeof(ssid)));
    memset(wire,'x',33);CHECK(cerberus_wifi_marauder_ssid_decode(wire,33,&ssid)==LIMIT&&!memcmp(&ssid,&ssid_prior,sizeof(ssid)));
    CHECK(cerberus_wifi_marauder_ssid_decode(wire,2113,&ssid)==LIMIT);
    CHECK(cerberus_wifi_marauder_ssid_decode(NULL,0,&ssid)==OK&&ssid.count==0&&!ssid.final_lf);
    CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,wire,0,&n)==OK&&n==0);
    CHECK(cerberus_wifi_marauder_ssid_decode((const uint8_t*)"\n",1,&ssid)==OK&&ssid.count==1&&ssid.final_lf&&!ssid.rows[0].length);
    ssid.final_lf=false;CHECK(cerberus_wifi_marauder_ssid_validate(&ssid)==INVALID);
    ssid.final_lf=true;ssid.rows[0].length=1;ssid.rows[0].bytes[0]='\n';CHECK(cerberus_wifi_marauder_ssid_validate(&ssid)==UNREP);
    ssid.rows[0].bytes[0]='x';((uint8_t*)&ssid.final_lf)[0]=2;CHECK(cerberus_wifi_marauder_ssid_validate(&ssid)==INVALID);
    memset(&ssid,0,sizeof(ssid));ssid.count=1;ssid.final_lf=true;ssid.rows[0].bytes[31]=1;CHECK(cerberus_wifi_marauder_ssid_validate(&ssid)==INVALID);
    CHECK(cerberus_wifi_marauder_ssid_decode((const uint8_t*)&ssid,1,&ssid)==INVALID);
    CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,(uint8_t*)&ssid,sizeof(ssid),&n)==INVALID);
}
static void seed_archive(void)
{
    memset(&archive,0,sizeof(archive));archive.count=64;archive.saved_time_valid=true;archive.saved_us=10000;
    archive.publication=(cerberus_wifi_archive_publication_t){.received_us=9000,.c5_session_id=3,.c5_relation_epoch=4,.generation=9,
        .s3_total=32,.c5_total=32,.s3_count=32,.c5_count=32,.s3_coverage=1,.c5_coverage=2,.split=true};
    for(unsigned i=0;i<64;++i){cerberus_wifi_archive_row_t *r=&archive.rows[i];r->source=i<32?1:2;r->source_index=(uint8_t)(i%32);
        r->source_generation=i<32?123:0;r->bssid[0]=2;r->bssid[5]=(uint8_t)(i%32);r->channel=i<32?6:149;r->security=(uint8_t)(i%10);
        memcpy(r->ssid,"same",5);r->rssi=-54;r->rssi_valid=true;}
    CHECK(cerberus_wifi_archive_validate(&archive));
}
static void conversion(void)
{
    seed_archive();CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,&stats)==OK&&stats.records==64&&stats.security_unmapped==24&&stats.loss_flags==7);
    CHECK(ap.count==64&&ap.rows[0].present==(1|2|4|8|32)&&!ap.station_count&&!ap.ignored_members);
    CHECK(!memcmp(ap.rows[0].bssid,ap.rows[32].bssid,6)&&!memcmp(ap.rows[0].essid,ap.rows[32].essid,32));
    const uint8_t mapped[]={0,1,2,3,5,4,255,255,255,255};for(unsigned i=0;i<10;++i)CHECK(ap.rows[i].security==mapped[i]);
    ap_roundtrip();
    archive.rows[0].rssi=0;archive.rows[0].rssi_valid=false;
    CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,&stats)==OK&&stats.rssi_omitted==1&&!(ap.rows[0].present&8)&&!ap.rows[0].rssi);
    memset(&ap,0x55,sizeof(ap));prior=ap;stats_prior=stats;archive.rows[1].ssid[0]=(char)0xff;
    CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,&stats)==UNREP&&!memcmp(&ap,&prior,sizeof(ap))&&!memcmp(&stats,&stats_prior,sizeof(stats)));
    CHECK(cerberus_wifi_marauder_ssid_from_archive(&archive,&ssid,&stats)==OK&&ssid.rows[1].bytes[0]==0xff&&ssid.count==64&&ssid.final_lf&&stats.loss_flags==15);
    ssid_prior=ssid;stats_prior=stats;archive.rows[2].ssid[0]='\n';
    CHECK(cerberus_wifi_marauder_ssid_from_archive(&archive,&ssid,&stats)==UNREP&&!memcmp(&ssid,&ssid_prior,sizeof(ssid))&&!memcmp(&stats,&stats_prior,sizeof(stats)));
    seed_archive();memset(archive.rows[0].ssid,0,33);
    CHECK(cerberus_wifi_marauder_ssid_from_archive(&archive,&ssid,&stats)==OK&&!ssid.rows[0].length&&ssid.count==64);
    CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,&stats)==OK&&(ap.rows[0].present&1)&&!ap.rows[0].essid_length);
    const char *json="[{},{\"essid\":\"\"},{\"essid\":\"x\\r\\u0000\"},{\"essid\":\"x\\r\\u0000\",\"extension\":1}]";
    CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t*)json,strlen(json),&ap)==OK);
    CHECK(cerberus_wifi_marauder_ssid_from_ap(&ap,&ssid,&stats)==OK&&ssid.count==3&&!ssid.rows[0].length&&stats.names_absent==1&&stats.records==3&&stats.loss_flags==(8|16));
    CHECK(ssid.rows[1].length==3&&ssid.rows[1].bytes[1]=='\r'&&!ssid.rows[1].bytes[2]&&!memcmp(&ssid.rows[1],&ssid.rows[2],sizeof(ssid.rows[1])));
    ssid_prior=ssid;ap.rows[1].essid[0]='\n';ap.rows[1].essid_length=1;stats_prior=stats;
    CHECK(cerberus_wifi_marauder_ssid_from_ap(&ap,&ssid,&stats)==UNREP&&!memcmp(&ssid,&ssid_prior,sizeof(ssid))&&!memcmp(&stats,&stats_prior,sizeof(stats)));
    CHECK(cerberus_wifi_marauder_ssid_from_ap(&ap,(cerberus_wifi_marauder_ssid_t*)&ap,&stats)==INVALID);
    CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,(cerberus_wifi_marauder_export_stats_t*)&archive)==INVALID);
    archive.publication.generation=0;prior=ap;CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,&stats)==INVALID&&!memcmp(&ap,&prior,sizeof(ap)));
}
static void adversarial(void)
{
    /* Every presence combination retains absent versus explicitly zero facts.
     * No manufacturer/station/count defaults are invented by re-encoding. */
    for(unsigned mask=0;mask<=511;++mask){memset(&ap,0,sizeof(ap));ap.count=1;ap.rows[0].present=(uint16_t)mask;
        if(mask&1){ap.rows[0].essid_length=1;ap.rows[0].essid[0]='x';}
        if(mask&2)ap.rows[0].channel=255;
        if(mask&4)memset(ap.rows[0].bssid,0xff,6);
        if(mask&8)ap.rows[0].rssi=INT32_MAX;
        if(mask&16)ap.rows[0].packets=UINT32_MAX;
        if(mask&32)ap.rows[0].security=255;
        if(mask&64)ap.rows[0].wps=true;
        if(mask&128){ap.rows[0].man_length=1;ap.rows[0].man[0]='v';}
        if(mask&256){ap.rows[0].station_count=2;ap.station_count=2;memset(ap.stations,0xff,12);}
        ap_roundtrip();
    }
    for(unsigned byte=0;byte<256;++byte){int n=snprintf((char*)wire,sizeof(wire),"[{\"channel\":%u,\"sec\":%u,\"essid\":\"\\u%04x\"}]",byte,byte,byte);
        CHECK(cerberus_wifi_marauder_ap_decode(wire,(size_t)n,&ap)==OK);
        CHECK(ap.rows[0].channel==byte&&ap.rows[0].security==byte&&ap.rows[0].essid_length==(byte<128?1:2));ap_roundtrip();
    }
    const char *utf8[]={"\xc2\x80","\xdf\xbf","\xe0\xa0\x80","\xed\x9f\xbf","\xee\x80\x80","\xef\xbf\xbf","\xf0\x90\x80\x80","\xf4\x8f\xbf\xbf"};
    for(unsigned i=0;i<sizeof(utf8)/sizeof(utf8[0]);++i){memset(&ap,0,sizeof(ap));ap.count=1;ap.rows[0].present=1;
        size_t bytes=strlen(utf8[i]);ap.rows[0].essid_length=(uint8_t)bytes;memcpy(ap.rows[0].essid,utf8[i],bytes);ap_roundtrip();}
    for(unsigned length=0;length<=64;++length){size_t p=(size_t)sprintf((char*)wire,"[{\"man\":\"");memset(wire+p,'v',length);p+=length;memcpy(wire+p,"\"}]",3);p+=3;
        CHECK(cerberus_wifi_marauder_ap_decode(wire,p,&ap)==OK&&ap.rows[0].man_length==length);ap_roundtrip();}
    reject_json("[{\"man\":\"12345678901234567890123456789012345678901234567890123456789012345\"}]",LIMIT);
    /* Scalar truncation and byte corruption cannot leave partial output.
     * Accepted mutations must still pass model validation and canonical IO. */
    const char *seed="[{\"essid\":\"ABC\",\"channel\":6,\"bssid\":\"02:03:04:05:06:07\",\"packet\":22,\"stations\":[\"02:03:04:05:06:07\"]}]";
    size_t length=strlen(seed);uint32_t random=UINT32_C(0x51eefa11);
    for(unsigned iteration=0;iteration<4096;++iteration){memcpy(wire,seed,length);random=random*1664525U+1013904223U;size_t at=random%length;random=random*1664525U+1013904223U;wire[at]=(uint8_t)(random>>24);
        memset(&decoded,0x5a,sizeof(decoded));prior=decoded;cerberus_wifi_marauder_result_t result=cerberus_wifi_marauder_ap_decode(wire,length,&decoded);
        if(result==OK){CHECK(cerberus_wifi_marauder_ap_validate(&decoded)==OK);size_t n;CHECK(cerberus_wifi_marauder_ap_encode(&decoded,other,sizeof(other),&n)==OK);}
        else CHECK(!memcmp(&decoded,&prior,sizeof(decoded)));
    }
    /* Names bounded by decoded BYTES, not scalar count. Eight supplementary
     * characters fit32 bytes; a ninth is a limit, never truncation. */
    for(unsigned count=8;count<=9;++count){size_t p=(size_t)sprintf((char*)wire,"[{\"essid\":\"");for(unsigned j=0;j<count;++j){memcpy(wire+p,"\\uD83D\\uDE00",12);p+=12;}memcpy(wire+p,"\"}]",3);p+=3;
        if(count==8)CHECK(cerberus_wifi_marauder_ap_decode(wire,p,&ap)==OK&&ap.rows[0].essid_length==32);
        else reject(wire,p,LIMIT);
    }
    for(unsigned depth=0;depth<=13;++depth){size_t p=(size_t)sprintf((char*)wire,"[{\"extension\":");for(unsigned i=0;i<depth;++i)wire[p++]='[';wire[p++]='0';for(unsigned i=0;i<depth;++i)wire[p++]=']';memcpy(wire+p,"}]",2);p+=2;
        if(depth<=10)CHECK(cerberus_wifi_marauder_ap_decode(wire,p,&ap)==OK&&ap.ignored_members==1);
        else reject(wire,p,LIMIT);
    }
    /* Every prefix of valid LF text is itself a possible final unterminated
     * line; decode/encode must preserve it, including internal empty names. */
    for(unsigned i=0;i<64;++i){memset(guard+i*33,'a'+i%26,32);guard[i*33+32]='\n';}
    for(size_t length2=0;length2<=2112;++length2){CHECK(cerberus_wifi_marauder_ssid_decode(guard,length2,&ssid)==OK);
        size_t n;CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,other,sizeof(other),&n)==OK&&n==length2&&!memcmp(other,guard,n));}
    size_t n=99;CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,SIZE_MAX,&n)==INVALID&&n==99); /* written aliases claimed range */
    seed_archive();memset(&ap,0x77,sizeof(ap));prior=ap;
    CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,(cerberus_wifi_marauder_export_stats_t*)&ap)==INVALID&&!memcmp(&ap,&prior,sizeof(ap)));
    CHECK(cerberus_wifi_marauder_ssid_from_archive(&archive,(cerberus_wifi_marauder_ssid_t*)&archive,&stats)==INVALID);
    /* A validated empty native bank is a useful empty interchange document,
     * not a fake unavailable-row error. Historical counts/times are not added. */
    archive.count=archive.publication.s3_count=archive.publication.c5_count=0;
    archive.publication.s3_total=archive.publication.c5_total=0;
    CHECK(cerberus_wifi_archive_validate(&archive));
    CHECK(cerberus_wifi_marauder_ap_from_archive(&archive,&ap,&stats)==OK&&!ap.count&&!stats.records);
    CHECK(cerberus_wifi_marauder_ap_encode(&ap,wire,sizeof(wire),&n)==OK&&n==2&&!memcmp(wire,"[]",2));
    CHECK(cerberus_wifi_marauder_ssid_from_archive(&archive,&ssid,&stats)==OK&&!ssid.count&&!ssid.final_lf);
    CHECK(cerberus_wifi_marauder_ssid_encode(&ssid,wire,sizeof(wire),&n)==OK&&n==0);
    /* Input budget includes ignored whitespace too; do not use an otherwise
     * valid prefix to silently disregard an oversized file. */
    memset(wire,' ',65536);wire[0]='[';wire[1]=']';
    CHECK(cerberus_wifi_marauder_ap_decode(wire,65536,&ap)==OK&&!ap.count);
    reject(wire,65537,LIMIT);
    memset(&ap,0,sizeof(ap));ap.count=1;ap.rows[0].essid[31]=1;
    CHECK(cerberus_wifi_marauder_ap_validate(&ap)==INVALID);
    memset(&ap,0,sizeof(ap));ap.count=1;ap.rows[0].wps=true;
    CHECK(cerberus_wifi_marauder_ap_validate(&ap)==INVALID);
}
int main(void)
{
    schema();bounds();text_cases();conversion();adversarial();
    printf("PASS %u Wi-Fi Marauder codec assertions; AP=%zu row=%zu SSID=%zu stats=%zu\n",checks,sizeof(ap),sizeof(ap.rows[0]),sizeof(ssid),sizeof(stats));return 0;
}
