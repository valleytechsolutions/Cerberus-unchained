#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wardrive_gps_core.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void sentence(const char *payload, char out[160])
{
    unsigned checksum = 0U;
    for (const unsigned char *cursor = (const unsigned char *)payload;
         *cursor != '\0'; ++cursor) checksum ^= *cursor;
    int length = snprintf(out, 160, "$%s*%02X\r\n", payload, checksum);
    assert(length > 0 && length < 160);
}

static void feed(cerberus_wardrive_gps_core_t *core, const char *payload,
    uint64_t time_ms)
{
    char line[160];
    sentence(payload, line);
    size_t length = strlen(line);
    size_t half = length / 2U;
    cerberus_wardrive_gps_core_feed(core, (const uint8_t *)line, half, time_ms);
    cerberus_wardrive_gps_core_feed(core,
        (const uint8_t *)line + half, length - half, time_ms);
}

static bool near(double a, double b)
{
    double difference = a - b;
    return difference > -0.00001 && difference < 0.00001;
}

static unsigned additional_checks;
#define CHECK(x) do { ++additional_checks; assert(x); } while (0)
static const char good_gga[]="GPGGA,123519.25,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
static const char good_rmc[]="GNRMC,123519.25,A,4807.038,N,01131.000,E,0.0,0.0,230394,,";
static bool qualifies(cerberus_wardrive_gps_core_t *core, uint64_t now)
{
    cerberus_wardrive_gps_snapshot_t out;
    cerberus_wardrive_gps_core_snapshot(core,now,&out);
    return out.state==CERBERUS_WARDRIVE_GPS_FRESH&&out.coordinates_valid&&out.utc_valid&&out.utc_from_rmc;
}
static void satellite_fix_admission(void)
{
    cerberus_wardrive_gps_core_t core;
    char rmc[128],gga[128];
    /* LC29H's final NavStatus V is unsupported, not an invalid position mode. */
    const char allowed[]="ADFR";
    for(unsigned mode=0;mode<sizeof(allowed)-1U;++mode){
        for(unsigned quality=1;quality<=5;++quality){
            snprintf(rmc,sizeof(rmc),"GNRMC,123519.25,A,4807.038,N,01131.000,E,0,0,081126,,,%c,V",allowed[mode]);
            snprintf(gga,sizeof(gga),"GNGGA,123519.25,4807.038,N,01131.000,E,%u,08,0.9,20,M,,,",quality);
            for(unsigned reverse=0;reverse<2;++reverse){
                cerberus_wardrive_gps_core_reset(&core);
                feed(&core,reverse?gga:rmc,1000);feed(&core,reverse?rmc:gga,1020);
                CHECK(qualifies(&core,1020));
            }
        }
    }
    const char *rejected[]={"E","N","M","S","X","AA"," "};
    for(unsigned mode=0;mode<sizeof(rejected)/sizeof(rejected[0]);++mode){
        snprintf(rmc,sizeof(rmc),"GNRMC,123519.25,A,4807.038,N,01131.000,E,0,0,081126,,,%s,V",rejected[mode]);
        for(unsigned reverse=0;reverse<2;++reverse){
            cerberus_wardrive_gps_core_reset(&core);
            feed(&core,reverse?good_gga:rmc,1000);feed(&core,reverse?rmc:good_gga,1020);
            CHECK(!qualifies(&core,1020));
        }
    }
    /* A legacy active RMC cannot override a same-epoch non-satellite GGA. */
    const unsigned rejected_quality[]={0,6,7,8,9};
    for(unsigned q=0;q<sizeof(rejected_quality)/sizeof(rejected_quality[0]);++q){
        snprintf(gga,sizeof(gga),"GPGGA,123519.25,4807.038,N,01131.000,E,%u,08,0.9,20,M,,,",rejected_quality[q]);
        for(unsigned reverse=0;reverse<2;++reverse){
            cerberus_wardrive_gps_core_reset(&core);
            feed(&core,reverse?gga:good_rmc,1000);feed(&core,reverse?good_rmc:gga,1020);
            /* An unparseable quality has no epoch to contradict an independent
             * later RMC. It still invalidates the previous fix immediately. */
            CHECK(qualifies(&core,1020)==(rejected_quality[q]==9&&reverse));
            feed(&core,"GNRMC,123520.25,A,4807.038,N,01131.000,E,0,0,081126,,,A,V",2000);
            CHECK(qualifies(&core,2000));
        }
    }
    const char *void_rmc="GNRMC,123519.25,V,,,,,,,081126,,,N,V";
    for(unsigned reverse=0;reverse<2;++reverse){
        cerberus_wardrive_gps_core_reset(&core);
        feed(&core,reverse?good_gga:void_rmc,1000);feed(&core,reverse?void_rmc:good_gga,1020);
        CHECK(!qualifies(&core,1020));
    }
    const char *malformed[]={
        "GNRMC,123519.25,X,4807.038,N,01131.000,E,0,0,081126,,,A,V",
        "GNRMC,123519.25,A",
        "GNGGA,123519.25,4807.038,N,01131.000,E,bad,08,0.9,20,M,,,"};
    for(unsigned i=0;i<sizeof(malformed)/sizeof(malformed[0]);++i){
        cerberus_wardrive_gps_core_reset(&core);feed(&core,good_rmc,1000);
        CHECK(qualifies(&core,1000));feed(&core,malformed[i],1020);
        CHECK(!qualifies(&core,1020));
    }
}
static void sentence_order(void)
{
    cerberus_wardrive_gps_core_t core;cerberus_wardrive_gps_snapshot_t out;
    cerberus_wardrive_gps_core_reset(&core);
    feed(&core,good_gga,1000);feed(&core,good_rmc,1020);
    cerberus_wardrive_gps_core_snapshot(&core,1020,&out);
    CHECK(out.utc_valid&&out.altitude_valid&&out.altitude_m>545.39f&&out.altitude_m<545.41f);
    feed(&core,"GNRMC,123519.50,A,4807.038,N,01131.000,E,0.0,0.0,230394,,",1050);
    cerberus_wardrive_gps_core_snapshot(&core,1050,&out);
    CHECK(out.utc_valid&&!out.altitude_valid);
    feed(&core,good_gga,1060);cerberus_wardrive_gps_core_snapshot(&core,1060,&out);
    CHECK(!out.utc_valid); /* Different fractional fix cannot borrow RMC time. */
    cerberus_wardrive_gps_core_reset(&core);
    feed(&core,good_rmc,2000);feed(&core,good_gga,2020);
    cerberus_wardrive_gps_core_snapshot(&core,2020,&out);CHECK(out.utc_valid&&out.altitude_valid);
    feed(&core,good_rmc,4000);cerberus_wardrive_gps_core_snapshot(&core,4000,&out);
    CHECK(out.utc_valid&&!out.altitude_valid);
}

static void independent_gga_freshness(void)
{
    cerberus_wardrive_gps_core_t core;
    cerberus_wardrive_gps_snapshot_t out;
    cerberus_wardrive_gps_core_reset(&core);
    cerberus_wardrive_gps_core_snapshot(NULL,0,&out);
    CHECK(out.gga_age_ms==UINT32_MAX&&!out.satellites_valid&&!out.hdop_valid);
    feed(&core,good_gga,0);cerberus_wardrive_gps_core_snapshot(&core,0,&out);
    CHECK(out.gga_age_ms==0&&out.satellites_valid&&out.hdop_valid&&out.satellites==8);
    cerberus_wardrive_gps_core_snapshot(&core,5000,&out);
    CHECK(out.gga_age_ms==5000&&out.satellites_valid&&out.hdop_valid);
    for(uint64_t now=5001;now<15000;now+=1000){
        feed(&core,good_rmc,now);feed(&core,"GPGSV,1,1,00",now);
        cerberus_wardrive_gps_core_snapshot(&core,now,&out);
        CHECK(out.coordinates_valid&&out.fix_age_ms==0&&out.gga_age_ms==now);
        CHECK(!out.satellites_valid&&!out.hdop_valid&&!out.satellites&&out.hdop==0.0f);
    }
    feed(&core,good_gga,16000);feed(&core,"GPGGA,123519.25,,,,,0,00,99.9,,,,,,",16001);
    cerberus_wardrive_gps_core_snapshot(&core,16001,&out);
    CHECK(out.state==CERBERUS_WARDRIVE_GPS_NO_FIX&&!out.coordinates_valid);
    CHECK(out.gga_age_ms==0&&out.satellites_valid&&!out.satellites&&out.hdop_valid&&near(out.hdop,99.9));
    feed(&core,"GPGGA,123519.25,4807.038,N,01131.000,E,1,,,10,M,,,",17000);
    cerberus_wardrive_gps_core_snapshot(&core,17000,&out);
    CHECK(out.coordinates_valid&&!out.satellites_valid&&!out.hdop_valid&&!out.satellites&&!out.hdop);
    feed(&core,"GPGGA,123519.25,,,,,0,00,0.0,,,,,,",18000);
    cerberus_wardrive_gps_core_snapshot(&core,18000,&out);
    CHECK(out.satellites_valid&&out.hdop_valid&&!out.satellites&&!out.hdop);
    feed(&core,"GPGGA,123519.25,,,,,0,bad,0.8,,,,,,",18001);
    cerberus_wardrive_gps_core_snapshot(&core,18001,&out);
    CHECK(!out.satellites_valid&&out.hdop_valid&&near(out.hdop,0.8));
    feed(&core,"GPGGA,123519.25,,,,,0,09,bad,,,,,,",18002);
    cerberus_wardrive_gps_core_snapshot(&core,18002,&out);
    CHECK(out.satellites_valid&&out.satellites==9&&!out.hdop_valid&&out.hdop==0);
    feed(&core,"GPGGA,123519.25,,,,,bad,09,0.8,,,,,,",18003);
    cerberus_wardrive_gps_core_snapshot(&core,18003,&out);
    CHECK(!out.satellites_valid&&!out.hdop_valid&&out.gga_age_ms==0);
    feed(&core,good_gga,19000);cerberus_wardrive_gps_core_snapshot(&core,18999,&out);
    CHECK(out.gga_age_ms==UINT32_MAX&&!out.satellites_valid&&!out.hdop_valid);
    cerberus_wardrive_gps_core_snapshot(&core,UINT64_MAX,&out);
    CHECK(out.gga_age_ms==UINT32_MAX&&!out.satellites_valid&&!out.hdop_valid);
    feed(&core,good_gga,UINT64_MAX);cerberus_wardrive_gps_core_snapshot(&core,UINT64_MAX,&out);
    CHECK(out.gga_age_ms==0&&out.satellites_valid&&out.hdop_valid);
    /* A failed checksum cannot replace or freshen the last accepted GGA. */
    cerberus_wardrive_gps_core_reset(&core);feed(&core,good_gga,10);
    char bad[160];sentence("GPGGA,123519.25,,,,,0,00,0.0,,,,,,",bad);
    bad[strlen(bad)-4]=bad[strlen(bad)-4]=='0'?'1':'0';
    cerberus_wardrive_gps_core_feed(&core,(const uint8_t*)bad,strlen(bad),20);
    cerberus_wardrive_gps_core_snapshot(&core,20,&out);
    CHECK(out.gga_age_ms==10&&out.satellites==8&&out.satellites_valid&&out.hdop_valid);
}

static void strict_decimal_fields(void)
{
    static const char *bad[]={"9e-1","0x1p0"," 0.9","0.9 ","nan","inf","+","-",".9","1.","1..2"};
    static const char *good[]={"-12.5","+12.5","0","545.4","-1000.0","20000.0"};
    cerberus_wardrive_gps_core_t core;cerberus_wardrive_gps_snapshot_t out;char payload[128];
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){
        cerberus_wardrive_gps_core_reset(&core);feed(&core,good_gga,0);
        snprintf(payload,sizeof(payload),"GPGGA,123519.25,4807.038,N,01131.000,E,1,08,%s,10,M,,,",bad[i]);
        feed(&core,payload,1);cerberus_wardrive_gps_core_snapshot(&core,1,&out);
        CHECK(!out.coordinates_valid&&!out.hdop_valid&&out.hdop==0&&out.satellites_valid);
        snprintf(payload,sizeof(payload),"GPGGA,123519.25,4807.038,N,01131.000,E,1,08,0.9,%s,M,,,",bad[i]);
        feed(&core,payload,2);cerberus_wardrive_gps_core_snapshot(&core,2,&out);
        CHECK(!out.coordinates_valid&&!out.altitude_valid&&out.altitude_m==0&&out.hdop_valid);
    }
    for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);++i){
        cerberus_wardrive_gps_core_reset(&core);
        snprintf(payload,sizeof(payload),"GPGGA,123519.25,4807.038,N,01131.000,E,1,08,0.9,%s,M,,,",good[i]);
        feed(&core,payload,0);cerberus_wardrive_gps_core_snapshot(&core,0,&out);
        CHECK(out.coordinates_valid&&out.altitude_valid&&out.hdop_valid);
    }
}

typedef struct {
    uint8_t raw[127],length;
    uint32_t total;
    uint64_t received;
    cerberus_wardrive_gps_line_result_t result;
    cerberus_wardrive_gps_termination_t termination;
    bool complete;
} observed_line_t;
typedef struct {unsigned count;observed_line_t rows[16];} observation_t;
static void observed(void *context,const uint8_t *raw,uint8_t length,uint32_t total,uint64_t now,
    cerberus_wardrive_gps_line_result_t result,cerberus_wardrive_gps_termination_t termination,bool complete)
{
    observation_t *o=context;CHECK(o&&o->count<16&&length<=127&&length<=total);
    observed_line_t *r=&o->rows[o->count++];
    memset(r,0,sizeof(*r));memcpy(r->raw,raw,length);r->length=length;r->total=total;
    r->received=now;r->result=result;r->termination=termination;r->complete=complete;
}
static void observe_bytes(cerberus_wardrive_gps_core_t *core,observation_t *o,const void *bytes,size_t length,uint64_t now)
{cerberus_wardrive_gps_core_feed_observed(core,bytes,length,now,observed,o);}
static void observed_assembly(void)
{
    char line[160];sentence(good_rmc,line);size_t length=strlen(line);
    for(size_t split=0;split<=length;++split){
        cerberus_wardrive_gps_core_t core,plain;observation_t o={0};
        cerberus_wardrive_gps_core_reset(&core);cerberus_wardrive_gps_core_reset(&plain);
        observe_bytes(&core,&o,line,split,100);cerberus_wardrive_gps_core_feed(&plain,(const uint8_t*)line,split,100);
        CHECK(o.count==(split==length?1U:0U));
        observe_bytes(&core,&o,line+split,length-split,200);
        cerberus_wardrive_gps_core_feed(&plain,(const uint8_t*)line+split,length-split,200);
        CHECK(!memcmp(&core,&plain,sizeof(core))&&o.count==1);
        observed_line_t *r=&o.rows[0];
        CHECK(r->length==length-2&&r->total==length-2&&!memcmp(r->raw,line,length-2));
        CHECK(r->complete&&r->result==CERBERUS_WARDRIVE_GPS_LINE_RMC&&r->termination==CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF);
        CHECK(r->received==(split==length?100U:200U));
    }
    cerberus_wardrive_gps_core_t core;cerberus_wardrive_gps_core_reset(&core);observation_t o={0};
    observe_bytes(&core,&o,"\n\r\n",3,0);CHECK(!o.count);
    observe_bytes(&core,&o,line,length-1,10);CHECK(!o.count&&core.pending_cr);
    observe_bytes(&core,&o,"\n",1,20);CHECK(o.count==1&&o.rows[0].received==20);
    observed_line_t first=o.rows[0];
    sentence(good_gga,line);length=strlen(line);line[length-2]='\n';--length;
    observe_bytes(&core,&o,line,length,30);
    CHECK(o.count==2&&o.rows[1].result==CERBERUS_WARDRIVE_GPS_LINE_GGA&&o.rows[1].termination==CERBERUS_WARDRIVE_GPS_TERMINATION_LF);
    CHECK(!memcmp(&first,&o.rows[0],sizeof(first)));
    static const uint8_t controlled[]={'A',0,'B','\r','C',0x80,'\r','\n'};
    for(unsigned i=0;i<sizeof(controlled);++i)observe_bytes(&core,&o,controlled+i,1,40+i);
    CHECK(o.count==3&&o.rows[2].length==6&&o.rows[2].total==6&&!memcmp(o.rows[2].raw,controlled,6));
    CHECK(o.rows[2].complete&&o.rows[2].result==CERBERUS_WARDRIVE_GPS_LINE_CONTROL&&o.rows[2].termination==CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF);
    observe_bytes(&core,&o,"A\r\r\n",4,50);
    CHECK(o.rows[3].length==2&&!memcmp(o.rows[3].raw,"A\r",2)&&o.rows[3].complete&&o.rows[3].result==CERBERUS_WARDRIVE_GPS_LINE_CONTROL);
    /* Exactly127 payload bytes can be retained; byte128 is explicitly lost. */
    char payload[124];memcpy(payload,"GPXYZ,",6);memset(payload+6,'A',117);payload[123]=0;
    sentence(payload,line);CHECK(strlen(line)==129);observe_bytes(&core,&o,line,129,60);
    CHECK(o.rows[4].length==127&&o.rows[4].total==127&&o.rows[4].complete&&o.rows[4].result==CERBERUS_WARDRIVE_GPS_LINE_OTHER);
    uint8_t long_line[181];memset(long_line,'X',180);long_line[180]='\n';observe_bytes(&core,&o,long_line,sizeof(long_line),70);
    CHECK(o.rows[5].length==127&&o.rows[5].total==180&&!o.rows[5].complete&&o.rows[5].result==CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW);
    CHECK(!memcmp(o.rows[5].raw,long_line,127));
    /* Outcomes stay explicit when every diagnostic counter is saturated. */
    core.valid_sentences=core.bad_checksums=core.malformed_sentences=core.overflow_lines=core.fix_updates=UINT32_MAX;
    sentence(good_rmc,line);observe_bytes(&core,&o,line,strlen(line),80);
    CHECK(o.rows[6].result==CERBERUS_WARDRIVE_GPS_LINE_RMC);
    line[strlen(line)-4]=line[strlen(line)-4]=='0'?'1':'0';observe_bytes(&core,&o,line,strlen(line),90);
    CHECK(o.rows[7].result==CERBERUS_WARDRIVE_GPS_LINE_BAD_CHECKSUM&&o.rows[7].complete);
    sentence("GPRMC,123519,A,bad,N,01131.000,E,0,0,230394,,",line);observe_bytes(&core,&o,line,strlen(line),100);
    CHECK(o.rows[8].result==CERBERUS_WARDRIVE_GPS_LINE_FIELDS_INVALID&&o.rows[8].complete);
    CHECK(cerberus_wardrive_gps_core_ingest_line(&core,line,strlen(line)-2,101));
    observe_bytes(&core,&o,"not a sentence\n",15,110);CHECK(o.rows[9].result==CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID);
    observe_bytes(&core,&o,controlled,sizeof(controlled),120);CHECK(o.rows[10].result==CERBERUS_WARDRIVE_GPS_LINE_CONTROL);
    observe_bytes(&core,&o,long_line,sizeof(long_line),130);CHECK(o.rows[11].result==CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW);
    CHECK(core.valid_sentences==UINT32_MAX&&core.bad_checksums==UINT32_MAX&&core.malformed_sentences==UINT32_MAX&&core.overflow_lines==UINT32_MAX);
    core.line_total_length=UINT32_MAX-1;core.line_length=127;memset(core.line,'X',127);
    observe_bytes(&core,&o,"XX\n",3,140);
    CHECK(o.rows[12].total==UINT32_MAX&&!o.rows[12].complete&&o.rows[12].result==CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW);
    observe_bytes(&core,&o,"PARTIAL\r",8,150);CHECK(o.count==13&&core.pending_cr);
    cerberus_wardrive_gps_core_reset(&core);CHECK(!core.line_total_length&&!core.line_length&&!core.pending_cr&&o.count==13);
}

int main(void)
{
    cerberus_wardrive_gps_core_t core;
    cerberus_wardrive_gps_snapshot_t status;
    cerberus_wardrive_gps_core_reset(&core);
    cerberus_wardrive_gps_core_snapshot(&core, 0U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_ABSENT);
    assert(!status.coordinates_valid && !status.utc_valid && status.latitude == 0.0);
    assert(status.utc_age_ms == UINT32_MAX);

    feed(&core, "GNRMC,123519.25,A,4807.038,N,01131.000,E,0.0,0.0,230394,,", 1000U);
    cerberus_wardrive_gps_core_snapshot(&core, 1000U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_FRESH);
    assert(status.coordinates_valid && near(status.latitude, 48.1173));
    assert(near(status.longitude, 11.5166667));
    assert(status.utc_valid && status.utc_from_rmc);
    assert(status.utc_age_ms == 0U);
    assert(strcmp(status.utc, "1994-03-23T12:35:19.250Z") == 0);
    assert(status.fix_updates == 1U && status.valid_sentences == 1U);

    feed(&core, "GPGGA,123519.25,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", 1200U);
    cerberus_wardrive_gps_core_snapshot(&core, 1200U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_FRESH);
    assert(status.altitude_valid && status.altitude_m > 545.39f && status.altitude_m < 545.41f);
    assert(status.satellites == 8U && status.hdop > 0.89f && status.hdop < 0.91f);
    assert(status.utc_valid && status.fix_updates == 2U && status.utc_age_ms == 200U);

    char bad[160];
    sentence("GPRMC,123520.00,V,,,,,,,230394,,", bad);
    size_t bad_length = strlen(bad);
    bad[bad_length - 4U] = bad[bad_length - 4U] == '0' ? '1' : '0';
    cerberus_wardrive_gps_core_feed(&core, (const uint8_t *)bad, bad_length, 1500U);
    cerberus_wardrive_gps_core_snapshot(&core, 1500U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_FRESH);
    assert(status.bad_checksums == 1U && status.valid_sentences == 2U);

    feed(&core, "GPRMC,123520.00,V,,,,,,,230394,,", 1700U);
    cerberus_wardrive_gps_core_snapshot(&core, 1700U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_NO_FIX);
    assert(!status.coordinates_valid && !status.utc_valid && status.latitude == 0.0);

    feed(&core, "GPRMC,123521.00,A,9100.000,N,01131.000,E,0.0,0.0,230394,,", 2000U);
    cerberus_wardrive_gps_core_snapshot(&core, 2000U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_NO_FIX);
    assert(status.malformed_sentences >= 1U);

    feed(&core, "GPRMC,123522.00,A,4807.038,N,01131.000,E,0.0,0.0,310299,,", 2500U);
    cerberus_wardrive_gps_core_snapshot(&core, 2500U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_FRESH);
    assert(status.coordinates_valid && !status.utc_valid);
    assert(status.utc_age_ms == UINT32_MAX);

    feed(&core, "GPRMC,123523.00,A,4807.038,N,01131.000,E,0.0,0.0,290224,,", 3000U);
    cerberus_wardrive_gps_core_snapshot(&core, 3000U, &status);
    assert(status.utc_valid && strcmp(status.utc, "2024-02-29T12:35:23.000Z") == 0);

    feed(&core, "GPGGA,123524.00,4807.038,N,01131.000,E,1,09,1.2,20.0,M,,,", 3200U);
    cerberus_wardrive_gps_core_snapshot(&core, 3200U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_FRESH && !status.utc_valid);
    assert(status.utc_age_ms == UINT32_MAX);

    feed(&core, "GPGSV,1,1,00", 8201U);
    cerberus_wardrive_gps_core_snapshot(&core, 8201U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_STALE);
    assert(!status.coordinates_valid && !status.utc_valid && status.longitude == 0.0);
    cerberus_wardrive_gps_core_snapshot(&core, 18202U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_ABSENT);
    assert(!status.coordinates_valid && !status.utc_valid);

    feed(&core, "GPGGA,123525.00,,,,,0,00,99.9,,,,,,", 19000U);
    cerberus_wardrive_gps_core_snapshot(&core, 19000U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_NO_FIX);

    feed(&core, "GPRMC,123526.00,A,4807.038,N,01131.000,E,0.0,0.0,290224,,", 19100U);
    feed(&core, "GPGGA,123526.00,4807.038,N,01131.000,E,1,08,0.9,999999.0,M,,,", 19200U);
    cerberus_wardrive_gps_core_snapshot(&core, 19200U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_NO_FIX);

    char interrupted[160];
    sentence("GPRMC,123527.00,A,4807.038,N,01131.000,E,0.0,0.0,290224,,", interrupted);
    size_t before_cr = strlen(interrupted) - 4U;
    interrupted[before_cr] = '\r';
    cerberus_wardrive_gps_core_feed(&core, (const uint8_t *)interrupted,
        strlen(interrupted), 19300U);
    cerberus_wardrive_gps_core_snapshot(&core, 19300U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_NO_FIX);

    char overlong[180];
    memset(overlong, 'A', sizeof(overlong));
    overlong[sizeof(overlong) - 1U] = '\n';
    cerberus_wardrive_gps_core_feed(&core, (const uint8_t *)overlong,
        sizeof(overlong), 20000U);
    assert(core.overflow_lines == 1U && core.line_length == 0U);
    feed(&core, "GPGSV,1,1,00", 20100U);
    cerberus_wardrive_gps_core_snapshot(&core, 20100U, &status);
    assert(status.state == CERBERUS_WARDRIVE_GPS_NO_FIX);

    char without_checksum[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,0,0,230394";
    assert(!cerberus_wardrive_gps_core_ingest_line(&core, without_checksum,
        strlen(without_checksum), 21000U));
    cerberus_wardrive_gps_core_snapshot(&core, 21000U, &status);
    assert(status.valid_sentences == 12U);

    sentence_order();satellite_fix_admission();independent_gga_freshness();strict_decimal_fields();observed_assembly();
    printf("Wardriving GPS core checks passed: existing strict NMEA/UTC tests + %u independent-GGA/decimal/observed-assembly assertions; core=%zu snapshot=%zu bytes.\n",
        additional_checks,sizeof(core),sizeof(status));
    return 0;
}
