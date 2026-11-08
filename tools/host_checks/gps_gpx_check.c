#undef NDEBUG
#include "gps_gpx.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static cerberus_wardrive_gps_snapshot_t point(void)
{
    cerberus_wardrive_gps_snapshot_t p = {.state=CERBERUS_WARDRIVE_GPS_FRESH,
        .coordinates_valid=true,.utc_valid=true,.utc_from_rmc=true,
        .latitude=12.3456789,.longitude=-98.7654321,.altitude_valid=true,.altitude_m=123.5f,
        .hdop_valid=true,.hdop=0.8f,.satellites_valid=true,.satellites=12};
    strcpy(p.utc,"2026-11-08T01:02:03.456Z");
    return p;
}

static void invalid_points(void)
{
    cerberus_wardrive_gps_snapshot_t p=point();
    assert(cerberus_gpx_point_valid(&p));
    assert(!cerberus_gpx_point_valid(NULL));
#define INVALID(field,value) do {p=point();p.field=value;assert(!cerberus_gpx_point_valid(&p));} while(0)
    INVALID(state,CERBERUS_WARDRIVE_GPS_STALE);
    INVALID(coordinates_valid,false);INVALID(utc_valid,false);INVALID(utc_from_rmc,false);
    INVALID(fix_age_ms,5001);INVALID(utc_age_ms,UINT32_MAX);
    INVALID(latitude,NAN);INVALID(latitude,90.001);INVALID(longitude,-180.001);
    INVALID(altitude_m,INFINITY);INVALID(hdop,NAN);INVALID(hdop,-1);
#undef INVALID
    p=point();p.altitude_valid=p.hdop_valid=false;p.altitude_m=p.hdop=NAN;
    assert(cerberus_gpx_point_valid(&p));
    char row[CERBERUS_GPX_ROW_CAPACITY];size_t n;
    assert(cerberus_gpx_point(row,sizeof(row),&n,CERBERUS_GPX_TRACK,&p,1,NULL));
    assert(!strstr(row,"<ele>")&&!strstr(row,"<hdop>")&&!strstr(row,"nan"));
    p=point();p.latitude=-90;p.longitude=180;p.fix_age_ms=p.utc_age_ms=5000;
    assert(cerberus_gpx_point_valid(&p));
    assert(cerberus_gpx_point(row,sizeof(row),&n,CERBERUS_GPX_TRACK,&p,1,NULL));
    assert(strstr(row,"lon=\"-180.0000000\""));
    p.longitude=179.99999999;
    assert(cerberus_gpx_point(row,sizeof(row),&n,CERBERUS_GPX_TRACK,&p,1,NULL));
    assert(strstr(row,"lon=\"-180.0000000\""));
}

static void timestamps(void)
{
    uint64_t n;
    assert(cerberus_gpx_utc_ms("2000-01-01T00:00:00.000Z",&n)&&n==UINT64_C(946684800000));
    assert(cerberus_gpx_utc_ms("2024-02-29T12:34:56.789Z",&n)&&n==UINT64_C(1709210096789));
    static const char invalid[][25]={"2025-02-29T00:00:00.000Z","2026-04-31T00:00:00.000Z",
        "2026-00-01T00:00:00.000Z","2026-01-00T00:00:00.000Z","2026-01-01T24:00:00.000Z",
        "2026-01-01T00:60:00.000Z","2026-01-01T00:00:60.000Z","2100-01-01T00:00:00.000Z",
        "2026-01-01T00:00:00.00xZ","2026-01-01T00:00:00.000+",""};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)assert(!cerberus_gpx_utc_ms(invalid[i],&n)&&!n);
    assert(!cerberus_gpx_utc_ms(NULL,&n)&&!cerberus_gpx_utc_ms(invalid[0],NULL));
}

static void labels_and_buffers(void)
{
    const char *labels[]={NULL,"","Lake & <bridge> \"north\" 'side'","caf\xc3\xa9 \xf0\x9f\x93\x8d"};
    for(unsigned i=0;i<sizeof(labels)/sizeof(labels[0]);++i)assert(cerberus_gpx_label_valid(labels[i]));
    const char *invalid[]={"\n","\x7f","\x80","\xc0\xaf","\xe0\x80\xaf","\xed\xa0\x80",
        "\xf4\x90\x80\x80","\xef\xbf\xbe","\xe2\x82","\xc2"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)assert(!cerberus_gpx_label_valid(invalid[i]));
    char label[50];memset(label,'&',48);label[48]=0;assert(cerberus_gpx_label_valid(label));
    cerberus_wardrive_gps_snapshot_t p=point();char row[CERBERUS_GPX_ROW_CAPACITY];size_t n;
    assert(cerberus_gpx_point(row,sizeof(row),&n,CERBERUS_GPX_POINTS,&p,UINT32_MAX,label));
    assert(n==strlen(row));size_t required=n+1;
    for(size_t capacity=1;capacity<required;++capacity){
        memset(row,0xa5,sizeof(row));n=999;
        assert(!cerberus_gpx_point(row,capacity,&n,CERBERUS_GPX_POINTS,&p,1,label)&&!n&&!row[0]);
        assert((unsigned char)row[capacity]==0xa5);
    }
    label[48]='x';label[49]=0;assert(!cerberus_gpx_label_valid(label));
    assert(!cerberus_gpx_point(row,sizeof(row),&n,(cerberus_gpx_mode_t)9,&p,1,NULL));
    assert(!cerberus_gpx_header((cerberus_gpx_mode_t)9)&&!cerberus_gpx_footer((cerberus_gpx_mode_t)9));
    assert(!cerberus_gpx_point(row,sizeof(row),&n,CERBERUS_GPX_TRACK,&p,0,NULL));
}

static void motion(void)
{
    cerberus_gps_track_stats_t s={0};cerberus_wardrive_gps_snapshot_t p=point();bool segment;
    p.latitude=p.longitude=0;strcpy(p.utc,"2026-11-08T00:00:00.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&segment&&s.points==1&&s.segments==1);
    assert(!cerberus_gps_track_observe(&s,&p,&segment)&&s.points==1);
    p.latitude=.001;strcpy(p.utc,"2026-11-08T00:00:01.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&!segment);
    assert(fabs(s.distance_m-111.1949266)<.001&&fabs(s.speed_mps-111.1949266)<.001);
    double retained=s.distance_m;
    p.latitude+=.000001;strcpy(p.utc,"2026-11-08T00:00:02.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&!segment&&!s.speed_mps&&s.distance_m==retained);
    p.latitude=40;strcpy(p.utc,"2026-11-08T00:00:03.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&!s.speed_mps&&s.distance_m==retained&&s.filtered_steps==2);
    cerberus_gps_track_gap(&s);p.latitude=50;strcpy(p.utc,"2026-11-08T00:00:04.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&segment&&s.segments==2&&s.distance_m==retained);
    strcpy(p.utc,"2026-11-08T00:00:20.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&segment&&s.segments==3);
    strcpy(p.utc,"2026-11-08T00:00:05.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&segment&&s.segments==4);
    s=(cerberus_gps_track_stats_t){0};p.latitude=0;p.longitude=179.9999;
    assert(cerberus_gps_track_observe(&s,&p,&segment));
    p.longitude=-179.9999;strcpy(p.utc,"2026-11-08T00:00:06.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&!segment&&fabs(s.distance_m-22.2389853)<.001);
    s.points=UINT32_MAX;s.segments=UINT32_MAX;s.filtered_steps=UINT32_MAX;
    cerberus_gps_track_gap(&s);strcpy(p.utc,"2026-11-08T00:00:07.000Z");
    assert(cerberus_gps_track_observe(&s,&p,&segment)&&s.points==UINT32_MAX&&s.segments==UINT32_MAX);
}

static void export(const char *directory,cerberus_gpx_mode_t mode)
{
    char path[1024],row[CERBERUS_GPX_ROW_CAPACITY];size_t n;
    snprintf(path,sizeof(path),"%s/%s.gpx",directory,mode==CERBERUS_GPX_TRACK?"track":"points");
    FILE *f=fopen(path,"wb");assert(f);
    assert(fputs(cerberus_gpx_header(mode),f)>=0);
    cerberus_wardrive_gps_snapshot_t p=point();
    for(unsigned i=1;i<=3;++i){
        p.latitude=12.3456789+(i-1)*.0001;
        if(i==2){p.altitude_valid=p.hdop_valid=p.satellites_valid=false;}
        if(i==3&&mode==CERBERUS_GPX_TRACK)assert(fputs(cerberus_gpx_segment_break(),f)>=0);
        const char *label=i==1?"Lake & <bridge> \"north\" 'side'":i==2?"caf\xc3\xa9 \xf0\x9f\x93\x8d":NULL;
        assert(cerberus_gpx_point(row,sizeof(row),&n,mode,&p,i,label));
        assert(fwrite(row,1,n,f)==n);
    }
    assert(fputs(cerberus_gpx_footer(mode),f)>=0);assert(fclose(f)==0);
}

int main(int argc,char **argv)
{
    assert(argc==2);invalid_points();timestamps();labels_and_buffers();motion();
    export(argv[1],CERBERUS_GPX_TRACK);export(argv[1],CERBERUS_GPX_POINTS);
    printf("GPX core: fresh-fix/UTC admission, XML escaping, bounded output and segmented motion estimates pass; stats %zu bytes.\n",sizeof(cerberus_gps_track_stats_t));
}
