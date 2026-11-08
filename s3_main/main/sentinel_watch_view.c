#include "sentinel_watch_view.h"
#include "display.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define BG RGB(6,11,21)
#define PANEL RGB(13,23,37)
#define EDGE RGB(34,53,73)
#define MUTED RGB(127,149,170)
#define WHITE RGB(229,239,249)
#define CYAN RGB(67,227,219)
#define LILAC RGB(183,157,255)
#define PINK RGB(248,133,188)
#define AMBER RGB(255,194,99)
#define BLUE RGB(65,139,255)
#define ORANGE RGB(246,151,68)
#define TRY(call) do { esp_err_t e_=(call); if(e_!=ESP_OK) return e_; } while(0)

#define MINT RGB(104,241,166)

typedef struct { uint16_t x,y,w,h; sentinel_watch_action_t action; } target_t;
static const target_t targets[] = {
    {8,426,68,44,SENTINEL_WATCH_ACTION_BACK},
    {84,426,68,44,SENTINEL_WATCH_ACTION_PAUSE},
    {160,426,68,44,SENTINEL_WATCH_ACTION_HISTORY},
    {236,426,76,44,SENTINEL_WATCH_ACTION_DEMO},
};

typedef struct { sentinel_watch_scene_t scene; bool demo,paused,releasing,details,radar; char title[25]; } snapshot_t;

static uint16_t accent(sentinel_watch_scene_t scene)
{
    if(scene==SENTINEL_WATCH_SCENE_WELLNESS)return PINK;
    if(scene==SENTINEL_WATCH_SCENE_FINDMY)return MINT;
    if(scene==SENTINEL_WATCH_SCENE_TRACKER || scene==SENTINEL_WATCH_SCENE_BEACON ||
       scene==SENTINEL_WATCH_SCENE_FLIPPER)return ORANGE;
    if(scene==SENTINEL_WATCH_SCENE_SAMSUNG)return BLUE;
    if(scene==SENTINEL_WATCH_SCENE_FLOCK ||
       scene==SENTINEL_WATCH_SCENE_DRONE)return AMBER;
    if(scene==SENTINEL_WATCH_SCENE_CAMERA || scene==SENTINEL_WATCH_SCENE_SMART_GLASSES ||
       scene==SENTINEL_WATCH_SCENE_PWNAGOTCHI ||
       scene==SENTINEL_WATCH_SCENE_BLE_ACTIVITY)return LILAC;
    return CYAN;
}

static void copy_text(char *dst,size_t size,const char *src)
{
    size_t i=0;if(src)for(;i+1<size && src[i];++i){unsigned char c=(unsigned char)src[i];dst[i]=c>=32 && c<=126?(char)c:'?';}
    dst[i]='\0';
}

static bool valid(const sentinel_watch_view_t *v)
{
    return v && v->scene>=SENTINEL_WATCH_SCENE_GUARDIAN && v->scene<SENTINEL_WATCH_SCENE_COUNT &&
        v->confidence>=SENTINEL_WATCH_CONFIDENCE_NONE && v->confidence<=SENTINEL_WATCH_CONFIDENCE_HIGH;
}

static esp_err_t text(int x,int y,const char *s,uint16_t color,uint16_t bg,uint8_t scale)
{ return cerberus_display_draw_text((uint16_t)x,(uint16_t)y,s,color,bg,scale); }

static esp_err_t centered(int y,const char *s,uint16_t color,uint16_t bg,uint8_t scale)
{return text((320-(int)strlen(s)*6*scale)/2,y,s,color,bg,scale);}

static esp_err_t draw_controls(const snapshot_t *s)
{
    for(unsigned i=0;i<4;++i){const target_t *t=&targets[i];
        const char *label=i==0?"BACK":i==1?(s->paused?"RESUME":"PAUSE"):i==2?"LOG":s->demo?"NEXT":"OPTIONS";
        bool enabled=!(s->demo && (i==1 || i==2));uint16_t fg=enabled?WHITE:EDGE;
        TRY(cerberus_display_fill_rect(t->x,t->y,t->w,t->h,PANEL));
        TRY(cerberus_display_fill_rect(t->x,t->y,t->w,1,enabled?accent(s->scene):EDGE));
        TRY(text(t->x+(t->w-(int)strlen(label)*6)/2,t->y+18,label,fg,PANEL,1));
    }
    return ESP_OK;
}

static int radar_radius(int8_t rssi)
{ int strength=rssi+100;if(strength<0)strength=0;if(strength>60)strength=60;return 78-strength*68/60; }

static esp_err_t radar_circle(int cx,int cy,int radius,uint16_t color)
{
    int x=radius,y=0,error=1-radius;
    while(x>=y){
        const int points[8][2]={{cx+x,cy+y},{cx+y,cy+x},{cx-y,cy+x},{cx-x,cy+y},
            {cx-x,cy-y},{cx-y,cy-x},{cx+y,cy-x},{cx+x,cy-y}};
        for(unsigned i=0;i<8;++i)TRY(cerberus_display_fill_rect((uint16_t)points[i][0],(uint16_t)points[i][1],1,1,color));
        ++y;if(error<0)error+=2*y+1;else{--x;error+=2*(y-x)+1;}
    }
    return ESP_OK;
}
static esp_err_t draw_signal_radar(const sentinel_watch_dashboard_t *d,unsigned count)
{
    static const int signs[4][2]={{-1,-1},{1,-1},{-1,1},{1,1}};
    /* Angles are fixed list-slot positions. Radius is a labelled, clamped RSSI
     * scale only, not an estimate of physical position or distance. */
    TRY(cerberus_display_fill_rect(12,120,296,168,PANEL));
    for(int radius=10;radius<=78;radius+=34)TRY(radar_circle(160,204,radius,EDGE));
    TRY(cerberus_display_fill_rect(82,204,157,1,EDGE));
    TRY(cerberus_display_fill_rect(160,126,1,157,EDGE));
    for(unsigned i=0;i<count;++i)if(d->rows[i].rssi_valid){
        int r=radar_radius(d->rows[i].rssi)*7/10;
        TRY(cerberus_display_fill_rect((uint16_t)(157+signs[i][0]*r),(uint16_t)(201+signs[i][1]*r),7,7,accent(d->rows[i].scene)));
    }
    TRY(text(246,128,"-100",MUTED,PANEL,1));TRY(text(246,145,"-70",MUTED,PANEL,1));
    TRY(text(246,162,"-40 dBm",MUTED,PANEL,1));
    TRY(text(22,270,"POSITIONS ARE LIST SLOTS",MUTED,PANEL,1));
    for(unsigned i=0;i<count;++i)if(d->rows[i].rssi_valid){
        int r=radar_radius(d->rows[i].rssi)*7/10;char number[2]={(char)('1'+i),0};
        TRY(text(166+signs[i][0]*r,201+signs[i][1]*r,number,WHITE,PANEL,1));
    }
    return ESP_OK;
}
static esp_err_t draw_view_tabs(const snapshot_t *s)
{
    static const unsigned x[]={100,232},width[]={124,80};
    static const char *const labels[]={"DASHBOARD","RADAR"};
    for(unsigned i=0;i<2;++i){
        const bool active=(i!=0)==s->radar;
        TRY(cerberus_display_fill_rect(x[i],4,width[i],38,PANEL));
        TRY(cerberus_display_fill_rect(x[i],40,width[i],2,active?CYAN:EDGE));
        TRY(text(x[i]+(width[i]-strlen(labels[i])*12)/2,16,labels[i],active?WHITE:MUTED,PANEL,2));
    }
    return ESP_OK;
}

static esp_err_t draw_dashboard(const snapshot_t *s,const sentinel_watch_view_t *v)
{
    const sentinel_watch_dashboard_t *d=v->dashboard;
    char buf[53];
    TRY(cerberus_display_fill_rect(0,0,320,480,BG));
    TRY(text(12,10,"RECON",WHITE,BG,2));
    TRY(draw_view_tabs(s));
    const char *badge=s->demo?"PREVIEW":s->releasing?"PAUSING":s->paused?"PAUSED":"MONITOR";
    TRY(text(12,33,badge,s->releasing?AMBER:MUTED,BG,1));
    for(unsigned source=0;source<2;++source){
        unsigned x=8+156*source;
        TRY(cerberus_display_fill_rect(x,48,148,38,PANEL));
        TRY(cerberus_display_fill_rect(x,48,2,38,source?AMBER:CYAN));
        TRY(text(x+8,55,source?"S3 / BLUETOOTH LE":"C5 / WI-FI",WHITE,PANEL,1));
        const char *state=source?v->ble_status:v->wifi_status;
        char bounded[23];copy_text(bounded,sizeof(bounded),state?state:"STATUS UNAVAILABLE");
        TRY(text(x+8,72,bounded,MUTED,PANEL,1));
    }
    copy_text(buf,49,v->sd_status?v->sd_status:"SD: SEE OPTIONS FOR RECORDING");
    TRY(text(12,94,buf,MUTED,BG,1));
    if(!s->radar){
        TRY(text(12,110,"ALL LIST CHANGES / LAST 32 SECONDS",WHITE,BG,1));
        TRY(cerberus_display_fill_rect(12,124,296,29,PANEL));
        uint32_t maximum=1;
        unsigned n=d && d->buckets<=32?d->buckets:0;
        for(unsigned i=0;i<n;++i){if(d->added[i]>maximum)maximum=d->added[i];if(d->expired[i]>maximum)maximum=d->expired[i];}
        for(unsigned i=0;i<n;++i){
            unsigned x=16+(32-n+i)*9;
            unsigned a=(unsigned)((uint64_t)d->added[i]*26/maximum),e=(unsigned)((uint64_t)d->expired[i]*26/maximum);
            if(a)TRY(cerberus_display_fill_rect(x,151-a,3,a,CYAN));
            if(e)TRY(cerberus_display_fill_rect(x+4,151-e,3,e,AMBER));
        }
        if(!n)TRY(text(23,135,"WAITING FOR COMPLETED SWEEPS",MUTED,PANEL,1));
        TRY(text(12,158,"ADDED",CYAN,BG,1));TRY(text(54,158,"/ EXPIRED",AMBER,BG,1));
        snprintf(buf,sizeof(buf),"MAX %lu%s",(unsigned long)maximum,maximum==UINT32_MAX?"+":"");
        TRY(text(176,158,buf,MUTED,BG,1));
    }else TRY(text(12,110,"SIGNAL RADAR / RETAINED RSSI",WHITE,BG,1));
    unsigned count=d && d->count<=4?d->count:0;
    if(s->radar){
        TRY(draw_signal_radar(d,count));
        TRY(text(12,290,"RSSI CLAMPED / NO BEARING OR DISTANCE",AMBER,BG,1));
        for(unsigned i=0;i<count;++i){
            const sentinel_watch_dashboard_row_t *r=&d->rows[i];unsigned y=304+i*18;
            char signal[12];if(r->rssi_valid)snprintf(signal,sizeof(signal),"%d dBm",r->rssi);
            else snprintf(signal,sizeof(signal),"RSSI ?");
            snprintf(buf,sizeof(buf),"%u %.24s",i+1,r->hint);
            TRY(text(12,y,buf,r->scene==SENTINEL_WATCH_SCENE_GUARDIAN?MUTED:accent(r->scene),BG,1));
            TRY(text(302-(int)strlen(signal)*6,y,signal,WHITE,BG,1));
        }
    }
    for(unsigned i=0;!s->radar&&i<count;++i){
        const sentinel_watch_dashboard_row_t *r=&d->rows[i];unsigned y=178+i*49;
        bool recognized=r->scene!=SENTINEL_WATCH_SCENE_GUARDIAN;
        uint16_t color=recognized?accent(r->scene):MUTED;
        TRY(cerberus_display_fill_rect(8,y,304,46,PANEL));
        TRY(cerberus_display_fill_rect(8,y,3,46,color));
        /* Use the larger title where it fits beside the complete age. The
         * bounded fallback preserves both long categories and large ages. */
        unsigned age_width=(unsigned)strlen(r->age)*6;
        uint8_t title_scale=recognized&&strlen(r->hint)*12U+age_width+8U<=286U?2U:1U;
        TRY(text(16,y+3,r->hint,color,PANEL,title_scale));
        TRY(text(302-(int)age_width,y+6,r->age,MUTED,PANEL,1));
        TRY(text(16,y+20,r->name,WHITE,PANEL,1));
        TRY(text(16,y+33,r->identity,MUTED,PANEL,1));
    }
    if(!count){
        TRY(text(26,s->radar?318:228,d?"NO RETAINED OBSERVATIONS":"INVENTORY UNAVAILABLE",WHITE,BG,1));
        TRY(text(20,s->radar?340:250,d?"WAIT FOR A COMPLETED SOURCE SWEEP":"RADIO AND SD CONTROLS STILL AVAILABLE",MUTED,BG,1));
    }
    TRY(cerberus_display_fill_rect(8,378,68,27,PANEL));
    TRY(text(30,388,"PREV",WHITE,PANEL,1));
    TRY(cerberus_display_fill_rect(244,378,68,27,PANEL));
    TRY(text(266,388,"NEXT",WHITE,PANEL,1));
    snprintf(buf,sizeof(buf),"%u OBS / %u OF %u",d?d->total:0,d?d->page+1:1,d&&d->pages?d->pages:1);
    TRY(centered(388,buf,MUTED,BG,1));
    TRY(cerberus_display_fill_rect(8,410,304,10,PANEL));
    TRY(text(12,412,"HINTS FIRST / TAP A ROW FOR DETAILS",MUTED,PANEL,1));
    TRY(draw_controls(s));return ESP_OK;
}

/* The UI owns a two-Hz refresh cadence and successful painted-row lifetime.
 * No renderer framebuffer, animation state or duplicate evidence cache. */
void sentinel_watch_view_invalidate(void) {}
esp_err_t sentinel_watch_view_draw(const sentinel_watch_view_t *v,uint32_t now_ms)
{
    (void)now_ms;
    if(!valid(v))return ESP_ERR_INVALID_ARG;
    snapshot_t s={.scene=v->scene,.demo=v->demo,.paused=v->paused,
        .releasing=v->paused&&!v->demo&&v->releasing,.details=v->has_details&&!v->demo,.radar=v->radar};
    copy_text(s.title,sizeof(s.title),v->title);
    return draw_dashboard(&s,v);
}
esp_err_t sentinel_watch_view_update(const sentinel_watch_view_t *v,uint32_t now_ms)
{ return sentinel_watch_view_draw(v,now_ms); }
sentinel_watch_action_t sentinel_watch_view_hit_test(const sentinel_watch_view_t *v,uint16_t x,uint16_t y)
{
    if(!valid(v))return SENTINEL_WATCH_ACTION_NONE;
    if(!v->demo){
        if(y>=4&&y<42){
            if(x>=100&&x<224)return SENTINEL_WATCH_ACTION_DASHBOARD;
            if(x>=232&&x<312)return SENTINEL_WATCH_ACTION_RADAR;
        }
        if(y>=378&&y<405){
            if(x>=8&&x<76)return SENTINEL_WATCH_ACTION_PREV;
            if(x>=244&&x<312)return SENTINEL_WATCH_ACTION_NEXT;
        }
        if(v->dashboard&&x>=8&&x<312&&y>=(v->radar?304:178)&&y<374){
            unsigned row=(y-(v->radar?304:178))/(v->radar?18:49);
            if((v->radar||(y-178)%49<46)&&row<v->dashboard->count)
                return (sentinel_watch_action_t)(SENTINEL_WATCH_ACTION_ROW_0+row);
        }
    }
    for(unsigned i=0;i<sizeof(targets)/sizeof(targets[0]);++i){const target_t *t=&targets[i];
        if(x<t->x || x>=t->x+t->w || y<t->y || y>=t->y+t->h)continue;
        if(v->demo && (i==1 || i==2))return SENTINEL_WATCH_ACTION_NONE;
        return t->action;
    }
    return SENTINEL_WATCH_ACTION_NONE;
}
