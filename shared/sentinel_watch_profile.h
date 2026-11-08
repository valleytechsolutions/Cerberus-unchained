#pragma once

/* Public Remote ID broadcast fields. This is evidence of advertised values,
 * not authentication, physical identity, or the receiver's position/clock.
 * Offsets follow OpenDroneID core 849c03adbe0cdd68997a462fafab397edcfe53f3.
 * No packed casts, compiler bitfields, or authentication payload are used. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    SENTINEL_WATCH_PROFILE_BASIC,
    SENTINEL_WATCH_PROFILE_LOCATION,
    SENTINEL_WATCH_PROFILE_AUTH,
    SENTINEL_WATCH_PROFILE_SELF,
    SENTINEL_WATCH_PROFILE_SYSTEM,
    SENTINEL_WATCH_PROFILE_OPERATOR,
    SENTINEL_WATCH_PROFILE_GROUPS,
    SENTINEL_WATCH_PROFILE_GROUP_COUNT = SENTINEL_WATCH_PROFILE_GROUPS,
};
#define SENTINEL_WATCH_PROFILE_ALL 63U

/* Canonical byte image, independent of host ABI. Each group starts with the
 * original message byte 1. Reserved tails are omitted; AUTH retains bytes1–7
 * only for page0, otherwise just byte1 and six zero bytes. Absent groups and
 * versions are zero. No raw authentication data can enter this representation. */
typedef struct {
    uint8_t present, version[SENTINEL_WATCH_PROFILE_GROUPS], reserved[2];
    uint8_t basic[21], location[23], auth[7], self[24], system[23], operator_id[21];
} sentinel_watch_profile_t;
_Static_assert(sizeof(sentinel_watch_profile_t) == 128U, "Remote ID profile byte image");

typedef struct {
    uint64_t capture_epoch;
    uint32_t age_ms[SENTINEL_WATCH_PROFILE_GROUPS];
    uint8_t valid_mask;
} sentinel_watch_profile_timing_t;

typedef struct { uint8_t id_type, ua_type, id[20], length; bool text; } sentinel_watch_profile_basic_t;
typedef struct {
    int32_t latitude_e7, longitude_e7, pressure_altitude_dm, geo_altitude_dm, height_dm;
    uint16_t speed_qms, direction_deg, timestamp_ds;
    int8_t vertical_speed_half_ms;
    uint8_t status, horizontal_accuracy, vertical_accuracy, baro_accuracy, speed_accuracy, timestamp_accuracy;
    bool position_valid, pressure_valid, geo_valid, height_valid, height_agl;
    bool speed_valid, direction_valid, vertical_speed_valid, timestamp_valid;
} sentinel_watch_profile_location_t;
typedef struct {
    uint32_t timestamp_2019_s;
    uint8_t type, page, last_page, length;
    bool page_zero;
} sentinel_watch_profile_auth_t;
typedef struct { uint8_t type, text[23], length; } sentinel_watch_profile_self_t;
typedef struct { uint8_t type, text[20], length; } sentinel_watch_profile_operator_t;
typedef struct {
    int32_t latitude_e7, longitude_e7, ceiling_dm, floor_dm, altitude_dm;
    uint32_t timestamp_2019_s;
    uint16_t area_count, radius_m;
    uint8_t location_type, classification_type, category_eu, class_eu;
    bool position_valid, ceiling_valid, floor_valid, altitude_valid;
} sentinel_watch_profile_system_t;

static inline uint16_t sentinel_watch_profile_le16(const uint8_t *p)
{ return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U); }
static inline uint32_t sentinel_watch_profile_le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8U | (uint32_t)p[2]<<16U | (uint32_t)p[3]<<24U; }
static inline int32_t sentinel_watch_profile_sle32(const uint8_t *p)
{ uint32_t u=sentinel_watch_profile_le32(p); return u<=INT32_MAX?(int32_t)u:-1-(int32_t)(UINT32_MAX-u); }
static inline bool sentinel_watch_profile_zero(const uint8_t *p,size_t n)
{ for(size_t i=0;i<n;++i)if(p[i])return false;return true; }
static inline uint8_t sentinel_watch_profile_text_length(const uint8_t *p,size_t n)
{ size_t used=0;while(used<n&&p[used])++used;return (uint8_t)used; }
static inline bool sentinel_watch_profile_text_valid(const uint8_t *p,size_t n)
{
    size_t used=sentinel_watch_profile_text_length(p,n);
    if(!used||!sentinel_watch_profile_zero(p+used,n-used))return false;
    for(size_t i=0;i<used;++i)if(p[i]<32U||p[i]>126U)return false;
    return true;
}
static inline const uint8_t *sentinel_watch_profile_group(const sentinel_watch_profile_t *p,
    unsigned group,size_t *length)
{
    if (length) *length = 0U;
    if (!p || !length) return NULL;
    switch(group){
    case SENTINEL_WATCH_PROFILE_BASIC:*length=sizeof(p->basic);return p->basic;
    case SENTINEL_WATCH_PROFILE_LOCATION:*length=sizeof(p->location);return p->location;
    case SENTINEL_WATCH_PROFILE_AUTH:*length=sizeof(p->auth);return p->auth;
    case SENTINEL_WATCH_PROFILE_SELF:*length=sizeof(p->self);return p->self;
    case SENTINEL_WATCH_PROFILE_SYSTEM:*length=sizeof(p->system);return p->system;
    case SENTINEL_WATCH_PROFILE_OPERATOR:*length=sizeof(p->operator_id);return p->operator_id;
    default:return NULL;
    }
}
static inline bool sentinel_watch_profile_group_valid(unsigned group,const uint8_t *p)
{
    switch(group){
    case SENTINEL_WATCH_PROFILE_BASIC:{
        unsigned type=p[0]>>4U;
        return type>=1U&&type<=4U&&(type<=2U?sentinel_watch_profile_text_valid(p+1U,20U):
            !sentinel_watch_profile_zero(p+1U,20U));
    }
    case SENTINEL_WATCH_PROFILE_LOCATION:{
        int32_t lat=sentinel_watch_profile_sle32(p+4U),lon=sentinel_watch_profile_sle32(p+8U);
        uint16_t stamp=sentinel_watch_profile_le16(p+20U);
        return (p[0]>>4U)<=4U&&!(p[0]&8U)&&p[1]<=181U&&lat>=-900000000&&lat<=900000000&&
            lon>=-1800000000&&lon<=1800000000&&(p[18]&15U)<=12U&&(p[18]>>4U)<=6U&&
            (p[19]&15U)<=4U&&(p[19]>>4U)<=6U&&(stamp<=36000U||stamp==UINT16_MAX)&&!(p[22]&0xf0U);
    }
    case SENTINEL_WATCH_PROFILE_AUTH:
        return (p[0]>>4U)<=5U&&((p[0]&15U)?sentinel_watch_profile_zero(p+1U,6U):
            p[1]<16U&&p[2]&&p[2]<=17U+23U*p[1]);
    case SENTINEL_WATCH_PROFILE_SELF:
        return (p[0]<=2U||p[0]>=201U)&&sentinel_watch_profile_text_valid(p+1U,23U);
    case SENTINEL_WATCH_PROFILE_SYSTEM:{
        int32_t lat=sentinel_watch_profile_sle32(p+1U),lon=sentinel_watch_profile_sle32(p+5U);
        return !(p[0]&0xe0U)&&(p[0]&3U)<=2U&&((p[0]>>2U)&7U)<=1U&&
            lat>=-900000000&&lat<=900000000&&lon>=-1800000000&&lon<=1800000000;
    }
    case SENTINEL_WATCH_PROFILE_OPERATOR:
        return (p[0]==0U||p[0]>=201U)&&sentinel_watch_profile_text_valid(p+1U,20U);
    default:return false;
    }
}
static inline bool sentinel_watch_profile_valid(const sentinel_watch_profile_t *p)
{
    if(!p||(p->present&~SENTINEL_WATCH_PROFILE_ALL)||!sentinel_watch_profile_zero(p->reserved,2U))return false;
    for(unsigned group=0;group<SENTINEL_WATCH_PROFILE_GROUPS;++group){
        size_t length;const uint8_t *bytes=sentinel_watch_profile_group(p,group,&length);
        if(p->present&(1U<<group)){
            if(p->version[group]>2U||!sentinel_watch_profile_group_valid(group,bytes))return false;
        }else if(p->version[group]||!sentinel_watch_profile_zero(bytes,length))return false;
    }
    return true;
}
static inline bool sentinel_watch_profile_has(const sentinel_watch_profile_t *p,unsigned group)
{ return group<SENTINEL_WATCH_PROFILE_GROUPS&&sentinel_watch_profile_valid(p)&&(p->present&(1U<<group)); }

static inline bool sentinel_watch_profile_basic(const sentinel_watch_profile_t *p,sentinel_watch_profile_basic_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !sentinel_watch_profile_has(p, SENTINEL_WATCH_PROFILE_BASIC)) return false;
    out->id_type=p->basic[0]>>4U;out->ua_type=p->basic[0]&15U;
    memcpy(out->id,p->basic+1U,20U);out->text=out->id_type<=2U;
    out->length=out->text?sentinel_watch_profile_text_length(out->id,20U):20U;return true;
}
static inline bool sentinel_watch_profile_location(const sentinel_watch_profile_t *p,sentinel_watch_profile_location_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !sentinel_watch_profile_has(p, SENTINEL_WATCH_PROFILE_LOCATION)) return false;
    const uint8_t *b=p->location;
    out->status=b[0]>>4U;out->latitude_e7=sentinel_watch_profile_sle32(b+4U);out->longitude_e7=sentinel_watch_profile_sle32(b+8U);
    out->position_valid=out->latitude_e7||out->longitude_e7;
    out->direction_deg=(uint16_t)(b[1]+((b[0]&2U)?180U:0U));out->direction_valid=out->direction_deg<=360U;
    out->speed_qms=(b[0]&1U)?(uint16_t)(255U+3U*b[2]):b[2];out->speed_valid=out->speed_qms<=1017U;
    out->vertical_speed_half_ms=b[3]<=INT8_MAX?(int8_t)b[3]:(int8_t)(-1-(int)(UINT8_MAX-b[3]));
    out->vertical_speed_valid=out->vertical_speed_half_ms>=-124&&out->vertical_speed_half_ms<=124;
    uint16_t a=sentinel_watch_profile_le16(b+12U);out->pressure_valid=a!=0U;out->pressure_altitude_dm=(int32_t)a*5-10000;
    a=sentinel_watch_profile_le16(b+14U);out->geo_valid=a!=0U;out->geo_altitude_dm=(int32_t)a*5-10000;
    a=sentinel_watch_profile_le16(b+16U);out->height_valid=a!=0U;out->height_dm=(int32_t)a*5-10000;out->height_agl=(b[0]&4U)!=0U;
    out->horizontal_accuracy=b[18]&15U;out->vertical_accuracy=b[18]>>4U;
    out->speed_accuracy=b[19]&15U;out->baro_accuracy=b[19]>>4U;out->timestamp_accuracy=b[22]&15U;
    out->timestamp_ds=sentinel_watch_profile_le16(b+20U);out->timestamp_valid=out->timestamp_ds!=UINT16_MAX;
    return true;
}
static inline bool sentinel_watch_profile_auth(const sentinel_watch_profile_t *p,sentinel_watch_profile_auth_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !sentinel_watch_profile_has(p, SENTINEL_WATCH_PROFILE_AUTH)) return false;
    out->type=p->auth[0]>>4U;out->page=p->auth[0]&15U;out->page_zero=out->page==0U;
    if(out->page_zero){out->last_page=p->auth[1];out->length=p->auth[2];out->timestamp_2019_s=sentinel_watch_profile_le32(p->auth+3U);}return true;
}
static inline bool sentinel_watch_profile_self(const sentinel_watch_profile_t *p,sentinel_watch_profile_self_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !sentinel_watch_profile_has(p, SENTINEL_WATCH_PROFILE_SELF)) return false;
    out->type=p->self[0];memcpy(out->text,p->self+1U,23U);out->length=sentinel_watch_profile_text_length(out->text,23U);return true;
}
static inline bool sentinel_watch_profile_operator(const sentinel_watch_profile_t *p,sentinel_watch_profile_operator_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !sentinel_watch_profile_has(p, SENTINEL_WATCH_PROFILE_OPERATOR)) return false;
    out->type=p->operator_id[0];memcpy(out->text,p->operator_id+1U,20U);out->length=sentinel_watch_profile_text_length(out->text,20U);return true;
}
static inline bool sentinel_watch_profile_system(const sentinel_watch_profile_t *p,sentinel_watch_profile_system_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !sentinel_watch_profile_has(p, SENTINEL_WATCH_PROFILE_SYSTEM)) return false;
    const uint8_t *b=p->system;
    out->location_type=b[0]&3U;out->classification_type=(b[0]>>2U)&7U;
    out->latitude_e7=sentinel_watch_profile_sle32(b+1U);out->longitude_e7=sentinel_watch_profile_sle32(b+5U);out->position_valid=out->latitude_e7||out->longitude_e7;
    out->area_count=sentinel_watch_profile_le16(b+9U);out->radius_m=(uint16_t)b[11]*10U;
    uint16_t a=sentinel_watch_profile_le16(b+12U);out->ceiling_valid=a!=0U;out->ceiling_dm=(int32_t)a*5-10000;
    a=sentinel_watch_profile_le16(b+14U);out->floor_valid=a!=0U;out->floor_dm=(int32_t)a*5-10000;
    out->category_eu=b[16]>>4U;out->class_eu=b[16]&15U;
    a=sentinel_watch_profile_le16(b+17U);out->altitude_valid=a!=0U;out->altitude_dm=(int32_t)a*5-10000;
    out->timestamp_2019_s=sentinel_watch_profile_le32(b+19U);return true;
}
