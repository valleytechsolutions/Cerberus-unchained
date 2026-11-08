#ifdef NDEBUG
#undef NDEBUG
#endif
#include "s3_wifi_list.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)
#define SIGNAL CERBERUS_S3_WIFI_SORT_SIGNAL_DESC
#define NAME CERBERUS_S3_WIFI_SORT_NAME_ASC
#define CHANNEL CERBERUS_S3_WIFI_SORT_CHANNEL_ASC
#define ALL CERBERUS_S3_WIFI_BAND_ALL
#define BAND24 CERBERUS_S3_WIFI_BAND_24_GHZ
#define BAND5 CERBERUS_S3_WIFI_BAND_5_GHZ

static cerberus_c5_wifi_result_t row(const char *name, int8_t rssi, uint8_t channel,
    uint8_t id)
{
    cerberus_c5_wifi_result_t result = {
        .bssid={2,0,0,0,0,id}, .rssi=rssi, .channel=channel,
        .security=CERBERUS_WIFI_SECURITY_WPA2};
    CHECK(strlen(name) <= CERBERUS_WIFI_SSID_MAX_LENGTH);
    memcpy(result.ssid,name,strlen(name)+1U);
    return result;
}

static void expect(const cerberus_c5_wifi_result_t *rows,
    const cerberus_s3_wifi_observation_t *observations, size_t count,
    cerberus_s3_wifi_sort_t sort, cerberus_s3_wifi_band_t band, bool recent,
    uint64_t now, const uint8_t *wanted, size_t expected)
{
    cerberus_c5_wifi_result_t before[32];
    cerberus_s3_wifi_observation_t ages_before[32];
    CHECK(count<=32U && expected<=count);
    if(count)memcpy(before,rows,count*sizeof(*rows));
    if(observations&&count)memcpy(ages_before,observations,count*sizeof(*observations));
    for(size_t capacity=0U;capacity<=34U;++capacity){
        uint8_t output[36];memset(output,0xa5,sizeof(output));size_t matched=SIZE_MAX;
        CHECK(cerberus_s3_wifi_list_indexes(rows,observations,count,sort,band,recent,
            now,output+1U,capacity,&matched));
        CHECK(matched==expected && output[0]==0xa5);
        size_t copied=capacity<expected?capacity:expected;
        if(copied)CHECK(!memcmp(output+1U,wanted,copied));
        for(size_t i=1U+copied;i<sizeof(output);++i)CHECK(output[i]==0xa5);
        if(count)CHECK(!memcmp(before,rows,count*sizeof(*rows)));
        if(observations&&count)CHECK(!memcmp(ages_before,observations,count*sizeof(*observations)));
    }
    size_t matched=SIZE_MAX;
    CHECK(cerberus_s3_wifi_list_indexes(rows,observations,count,sort,band,recent,
        now,NULL,0U,&matched)&&matched==expected);
}

static void ordering_examples(void)
{
    cerberus_c5_wifi_result_t rows[]={row("Zulu",-30,6,0),row("alpha",-50,36,1),
        row("Bravo",-30,1,2),row("ALPHA",-50,6,3),row("",-50,149,4),
        row("A?",127,0,5)};
    const uint8_t signal[]={2,0,4,1,3,5},name[]={4,5,1,3,2,0},channel[]={2,3,0,1,4,5};
    expect(rows,NULL,6,SIGNAL,ALL,false,0,signal,6);
    expect(rows,NULL,6,NAME,ALL,false,0,name,6);
    expect(rows,NULL,6,CHANNEL,ALL,false,0,channel,6);
    const uint8_t band24[]={2,0,3},band5[]={4,1};
    expect(rows,NULL,6,SIGNAL,BAND24,false,0,band24,3);
    expect(rows,NULL,6,SIGNAL,BAND5,false,0,band5,2);

    /* Unknown channels and RSSI form an unavailable group, sorted by name,
     * rather than numerically outranking a valid channel or measurement. */
    cerberus_c5_wifi_result_t unknown[]={row("z",127,0,0),row("B",1,37,1),
        row("a",-128,255,2),row("valid",-127,177,3),row("near",0,1,4)};
    const uint8_t unavailable[]={4,3,2,1,0};
    expect(unknown,NULL,5,SIGNAL,ALL,false,0,unavailable,5);
    expect(unknown,NULL,5,CHANNEL,ALL,false,0,unavailable,5);
}

static void raw_names_and_hidden_addresses(void)
{
    cerberus_c5_wifi_result_t rows[]={row("A?",-40,6,0),row("A\002",-40,6,1),
        row("a\001",-40,6,2),row("a?",-40,6,3),row("",-40,6,4),
        row("02:00:00:00:00:04",-40,6,5),row("",-40,6,6),row("A",-40,6,7)};
    const uint8_t wanted[]={4,5,6,7,2,1,0,3};
    for(unsigned sort=0;sort<CERBERUS_S3_WIFI_SORT_COUNT;++sort)
        expect(rows,NULL,8,(cerberus_s3_wifi_sort_t)sort,ALL,false,0,wanted,8);
    /* High bytes compare unsigned and are not passed through locale folding. */
    rows[0]=row("\377",-40,6,0);rows[1]=row("\200",-40,6,1);rows[2]=row("z",-40,6,2);
    const uint8_t high[]={2,1,0};expect(rows,NULL,3,NAME,ALL,false,0,high,3);
    char longest[33];memset(longest,'A',32U);longest[32]='\0';
    rows[0]=row(longest,-40,6,0);rows[1]=row(longest,-40,6,1);
    rows[0].ssid[31]='z';rows[1].ssid[31]='y';rows[2]=row("AA",-40,6,2);
    const uint8_t maxnames[]={2,1,0};expect(rows,NULL,3,NAME,ALL,false,0,maxnames,3);
    /* Canonical received strings end at the first NUL; unrelated trailing
     * bytes cannot split equal names or alter row identity/source index. */
    rows[0]=row("same",-40,6,0);rows[1]=row("SAME",-40,6,1);
    memset(rows[0].ssid+5,0x81,28U);memset(rows[1].ssid+5,0x92,28U);
    const uint8_t same[]={0,1};expect(rows,NULL,2,NAME,ALL,false,0,same,2);
}

static void all_channel_and_signal_values(void)
{
    static const uint8_t channels5[]={36,40,44,48,52,56,60,64,100,104,108,112,
        116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177};
    cerberus_c5_wifi_result_t single=row("one",-50,1,1);
    for(unsigned channel=0U;channel<=UINT8_MAX;++channel){
        cerberus_s3_wifi_band_t wanted=channel>=1U&&channel<=14U?BAND24:ALL;
        for(size_t i=0U;i<sizeof(channels5);++i)if(channel==channels5[i])wanted=BAND5;
        CHECK(cerberus_s3_wifi_list_channel_band((uint8_t)channel)==wanted);
        single.channel=(uint8_t)channel;
        for(unsigned band=0;band<CERBERUS_S3_WIFI_BAND_COUNT;++band){
            uint8_t output=0xa5;size_t matched=SIZE_MAX;
            CHECK(cerberus_s3_wifi_list_indexes(&single,NULL,1,CHANNEL,
                (cerberus_s3_wifi_band_t)band,false,0,&output,1,&matched));
            bool included=band==ALL||band==(unsigned)wanted;
            CHECK(matched==(included?1U:0U)&&output==(included?0U:0xa5U));
        }
    }
    CHECK(!cerberus_s3_wifi_list_signal_valid(INT_MIN));
    CHECK(!cerberus_s3_wifi_list_signal_valid(INT_MAX));
    for(int signal=-128;signal<=127;++signal){
        bool valid=signal>=-127&&signal<=0;
        CHECK(cerberus_s3_wifi_list_signal_valid(signal)==valid);
        cerberus_c5_wifi_result_t pair[]={row("a",(int8_t)signal,1,0),row("z",-127,1,1)};
        uint8_t output[2];size_t matched=0;
        CHECK(cerberus_s3_wifi_list_indexes(pair,NULL,2,SIGNAL,ALL,false,0,output,2,&matched));
        CHECK(matched==2U&&output[0]==(valid?0U:1U)&&output[1]==(valid?1U:0U));
    }
}

static void recent_intersection(void)
{
    uint64_t start=UINT64_C(4294967296)*1000U+123U;
    cerberus_c5_wifi_result_t rows[]={row("b",-30,1,0),row("a",-30,36,1),
        row("c",-20,6,2),row("d",-10,149,3),row("e",-40,15,4),row("f",-50,14,5)};
    cerberus_s3_wifi_observation_t ages[]={
        {.valid=true,.request_started_us=start,.source_age_ms=30000},
        {.valid=true,.request_started_us=start,.source_age_ms=29999},
        {.valid=false,.request_started_us=start,.source_age_ms=0},
        {.valid=true,.request_started_us=start+1U,.source_age_ms=0},
        {.valid=true,.request_started_us=start,.source_age_ms=0},
        {.valid=true,.request_started_us=start,.source_age_ms=30001}};
    const uint8_t all[]={1,0,4},b24[]={0},b5[]={1};
    expect(rows,ages,6,SIGNAL,ALL,true,start,all,3);
    expect(rows,ages,6,SIGNAL,BAND24,true,start,b24,1);
    expect(rows,ages,6,SIGNAL,BAND5,true,start,b5,1);
    const uint8_t next[]={3,1,4};expect(rows,ages,6,SIGNAL,ALL,true,start+1000U,next,3);
    const uint8_t expired[]={3,4};expect(rows,ages,6,SIGNAL,ALL,true,start+1001U,expired,2);
    expect(rows,ages,6,SIGNAL,ALL,true,start-1U,NULL,0);
    expect(rows,ages,6,SIGNAL,ALL,true,UINT64_MAX,NULL,0);
    ages[0].request_started_us=UINT64_MAX;ages[0].source_age_ms=30000;
    expect(rows,ages,1,SIGNAL,ALL,true,UINT64_MAX,b24,1);
    /* The older API keeps its original-order/all-band behavior unchanged. */
    uint8_t old[6];
    CHECK(cerberus_s3_wifi_observation_indexes(ages,6,false,start,old,6)==6U);
    for(uint8_t i=0;i<6;++i)CHECK(old[i]==i);
}

static void full_capacity_and_invalid_inputs(void)
{
    cerberus_c5_wifi_result_t rows[32];cerberus_s3_wifi_observation_t ages[32];
    uint8_t wanted[32];
    for(size_t i=0;i<32U;++i){
        char name[8];snprintf(name,sizeof(name),"n%02u",(unsigned)(31U-i));
        rows[i]=row(name,-50,1,(uint8_t)i);wanted[i]=(uint8_t)(31U-i);
        ages[i]=(cerberus_s3_wifi_observation_t){.valid=true,.request_started_us=1U};
    }
    for(unsigned sort=0;sort<CERBERUS_S3_WIFI_SORT_COUNT;++sort)
        expect(rows,ages,32,(cerberus_s3_wifi_sort_t)sort,ALL,true,30000001U,wanted,32);
    expect(NULL,NULL,0,SIGNAL,ALL,true,0,NULL,0);

    uint8_t indexes[34],before[34];memset(indexes,0xa5,sizeof(indexes));memcpy(before,indexes,sizeof(before));
    size_t matched;
#define FAIL(r,o,n,s,b,recent,out,cap) do { \
    matched=SIZE_MAX; \
    CHECK(!cerberus_s3_wifi_list_indexes((r),(o),(n),(s),(b),(recent),1U,(out),(cap),&matched)); \
    CHECK(matched==0U&&!memcmp(indexes,before,sizeof(indexes))); \
} while(0)
    FAIL(rows,ages,33,SIGNAL,ALL,false,indexes,32);
    FAIL(rows,ages,SIZE_MAX,SIGNAL,ALL,false,indexes,32);
    FAIL(NULL,ages,1,SIGNAL,ALL,false,indexes,32);
    FAIL(rows,NULL,1,SIGNAL,ALL,true,indexes,32);
    FAIL(rows,ages,1,(cerberus_s3_wifi_sort_t)-1,ALL,false,indexes,32);
    FAIL(rows,ages,1,CERBERUS_S3_WIFI_SORT_COUNT,ALL,false,indexes,32);
    FAIL(rows,ages,1,SIGNAL,(cerberus_s3_wifi_band_t)-1,false,indexes,32);
    FAIL(rows,ages,1,SIGNAL,CERBERUS_S3_WIFI_BAND_COUNT,false,indexes,32);
    FAIL(rows,ages,1,SIGNAL,ALL,false,NULL,1);
    CHECK(!cerberus_s3_wifi_list_indexes(rows,ages,1,SIGNAL,ALL,false,1U,indexes,32,NULL));
    CHECK(!memcmp(indexes,before,sizeof(indexes)));
    /* Even a hidden final row that filters out must not permit partial output. */
    memset(rows[31].ssid,'x',sizeof(rows[31].ssid));ages[31].valid=false;
    FAIL(rows,ages,32,NAME,BAND5,true,indexes,32);
    rows[31]=row("last",-50,1,31);rows[31].security=CERBERUS_WIFI_SECURITY_OTHER+1U;
    FAIL(rows,ages,32,SIGNAL,BAND5,true,indexes,32);
    rows[31].security=CERBERUS_WIFI_SECURITY_OTHER;
    CHECK(cerberus_s3_wifi_list_indexes(rows,ages,32,SIGNAL,ALL,false,1U,indexes,SIZE_MAX,&matched));
    CHECK(matched==32U&&indexes[32]==0xa5&&indexes[33]==0xa5);
#undef FAIL
}

int main(void)
{
    ordering_examples();raw_names_and_hidden_addresses();all_channel_and_signal_values();
    recent_intersection();full_capacity_and_invalid_inputs();
    printf("AP list: %u checks; stable source indexes, upstream name ties, raw/hidden names, all channel/signal values, recent bounds, atomic faults and immutable inputs passed.\n",checks);
    return 0;
}
