#include "wifi_marauder.h"
#include <limits.h>
#include <string.h>

/* Independently implemented portable adapter for the pinned Marauder schema.
 * JSON lexical rules follow the same strict bounded policy as airtag_json.c.
 * No upstream radio or target-list implementation is incorporated. */
typedef struct {
    const uint8_t *bytes;
    size_t length, pos;
    cerberus_wifi_marauder_result_t error;
} reader_t;
typedef bool (*character_fn)(void *, uint32_t);

static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    if (!an || !bn) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y ||
        (x < y + bn && y < x + an);
}
static bool fail(reader_t *r, cerberus_wifi_marauder_result_t error)
{ if (r->error == CERBERUS_WIFI_MARAUDER_OK) r->error = error; return false; }
static void space(reader_t *r)
{
    while (r->pos < r->length && (r->bytes[r->pos] == ' ' || r->bytes[r->pos] == '\t' ||
        r->bytes[r->pos] == '\r' || r->bytes[r->pos] == '\n')) ++r->pos;
}
static bool take(reader_t *r, uint8_t c)
{ space(r); if (r->pos < r->length && r->bytes[r->pos] == c) { ++r->pos; return true; } return false; }
static int hex(uint32_t c)
{
    if (c >= '0' && c <= '9') return (int)c - '0';
    if (c >= 'a' && c <= 'f') return (int)c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return (int)c - 'A' + 10;
    return -1;
}
static bool hex4(reader_t *r, uint32_t *value)
{
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (r->pos == r->length) return false;
        int n = hex(r->bytes[r->pos++]);
        if (n < 0) return false;
        *value = (*value << 4) | (unsigned)n;
    }
    return true;
}
/* Validate scalar Unicode, including ignored strings. Semantic sinks retain
 * bounded decoded UTF-8 bytes. No temporary full string is constructed. */
static bool string(reader_t *r, character_fn emit, void *context)
{
    if (!take(r, '"')) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    while (r->pos < r->length) {
        uint32_t c = r->bytes[r->pos++];
        if (c == '"') return true;
        if (c < 0x20U) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
        if (c == '\\') {
            if (r->pos == r->length) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
            c = r->bytes[r->pos++];
            if (c == 'u') {
                if (!hex4(r, &c)) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
                if (c >= 0xd800U && c <= 0xdbffU) {
                    uint32_t low;
                    if (r->length - r->pos < 2 || r->bytes[r->pos++] != '\\' ||
                        r->bytes[r->pos++] != 'u' || !hex4(r, &low) || low < 0xdc00U || low > 0xdfffU)
                        return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
                    c = 0x10000U + ((c - 0xd800U) << 10) + low - 0xdc00U;
                } else if (c >= 0xdc00U && c <= 0xdfffU) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
            } else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c != '"' && c != '\\' && c != '/') return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
        } else if (c >= 0x80U) {
            unsigned remaining; uint32_t minimum;
            if (c >= 0xc2U && c <= 0xdfU) { remaining = 1; minimum = 0x80U; c &= 0x1fU; }
            else if (c >= 0xe0U && c <= 0xefU) { remaining = 2; minimum = 0x800U; c &= 0xfU; }
            else if (c >= 0xf0U && c <= 0xf4U) { remaining = 3; minimum = 0x10000U; c &= 7U; }
            else return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
            while (remaining--) {
                if (r->pos == r->length || (r->bytes[r->pos] & 0xc0U) != 0x80U)
                    return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
                c = (c << 6) | (r->bytes[r->pos++] & 0x3fU);
            }
            if (c < minimum || c > 0x10ffffU || (c >= 0xd800U && c <= 0xdfffU))
                return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
        }
        if (emit && !emit(context, c)) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    }
    return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
}
static bool number(reader_t *r)
{
    space(r); size_t p = r->pos;
    if (p < r->length && r->bytes[p] == '-') ++p;
    if (p == r->length) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    if (r->bytes[p] == '0') ++p;
    else {
        if (r->bytes[p] < '1' || r->bytes[p] > '9') return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
        do { ++p; } while (p < r->length && r->bytes[p] >= '0' && r->bytes[p] <= '9');
    }
    if (p < r->length && r->bytes[p] == '.') {
        ++p; size_t start = p;
        while (p < r->length && r->bytes[p] >= '0' && r->bytes[p] <= '9') ++p;
        if (p == start) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    }
    if (p < r->length && (r->bytes[p] == 'e' || r->bytes[p] == 'E')) {
        ++p; if (p < r->length && (r->bytes[p] == '+' || r->bytes[p] == '-')) ++p;
        size_t start = p;
        while (p < r->length && r->bytes[p] >= '0' && r->bytes[p] <= '9') ++p;
        if (p == start) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    }
    r->pos = p; return true;
}
static bool skip(reader_t *r, unsigned depth)
{
    space(r);
    if (r->pos == r->length) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    uint8_t c = r->bytes[r->pos];
    if (c == '"') return string(r, NULL, NULL);
    if (c == '[' || c == '{') {
        if (depth >= CERBERUS_WIFI_MARAUDER_MAX_DEPTH) return fail(r, CERBERUS_WIFI_MARAUDER_LIMIT);
        ++r->pos; uint8_t end = c == '[' ? ']' : '}';
        if (take(r, end)) return true;
        do {
            if (c == '{' && (!string(r, NULL, NULL) || !take(r, ':'))) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
            if (!skip(r, depth + 1)) return false;
            if (take(r, end)) return true;
        } while (take(r, ','));
        return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal) {
        size_t n = strlen(literal);
        if (n > r->length - r->pos || memcmp(r->bytes + r->pos, literal, n)) return fail(r, CERBERUS_WIFI_MARAUDER_INVALID);
        r->pos += n; return true;
    }
    return number(r);
}

typedef struct { uint8_t *bytes; size_t length, capacity; bool limit; } text_t;
static bool utf8_character(void *context, uint32_t c)
{
    text_t *t=context; uint8_t bytes[4]; unsigned n;
    if(c<0x80){bytes[0]=(uint8_t)c;n=1;}
    else if(c<0x800){bytes[0]=(uint8_t)(0xc0|(c>>6));bytes[1]=(uint8_t)(0x80|(c&63));n=2;}
    else if(c<0x10000){bytes[0]=(uint8_t)(0xe0|(c>>12));bytes[1]=(uint8_t)(0x80|((c>>6)&63));bytes[2]=(uint8_t)(0x80|(c&63));n=3;}
    else{bytes[0]=(uint8_t)(0xf0|(c>>18));bytes[1]=(uint8_t)(0x80|((c>>12)&63));bytes[2]=(uint8_t)(0x80|((c>>6)&63));bytes[3]=(uint8_t)(0x80|(c&63));n=4;}
    if(n>t->capacity-t->length){t->limit=true;return false;}
    if(t->bytes)memcpy(t->bytes+t->length,bytes,n);
    t->length+=n;return true;
}
static bool string_bytes(reader_t *r,uint8_t *bytes,size_t capacity,uint8_t *length)
{
    text_t t={.bytes=bytes,.capacity=capacity};
    if(!string(r,utf8_character,&t)){if(t.limit)r->error=CERBERUS_WIFI_MARAUDER_LIMIT;return false;}
    *length=(uint8_t)t.length;return true;
}
typedef struct { uint8_t bytes[16]; size_t length; } key_t;
static bool key_character(void *context,uint32_t c)
{
    key_t *k=context;
    if(k->length<sizeof(k->bytes))k->bytes[k->length]=c<128?(uint8_t)c:255;
    ++k->length;return true;
}
static bool key_is(const key_t *k,const char *s)
{size_t n=strlen(s);return k->length==n&&!memcmp(k->bytes,s,n);}
static bool uint_field(reader_t *r,uint32_t maximum,uint32_t *value,bool signed_value)
{
    space(r);bool negative=false;
    if(signed_value&&r->pos<r->length&&r->bytes[r->pos]=='-'){negative=true;++r->pos;}
    size_t digits=r->pos;uint32_t number_value=0;
    uint32_t bound=signed_value?(negative?UINT32_C(2147483648):INT32_MAX):maximum;
    while(r->pos<r->length&&r->bytes[r->pos]>='0'&&r->bytes[r->pos]<='9'){
        unsigned digit=r->bytes[r->pos++]-'0';
        if(digit>bound || number_value>(bound-digit)/10U)return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
        number_value=number_value*10U+digit;
    }
    if(r->pos==digits||(r->pos>digits+1&&r->bytes[digits]=='0'))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
    *value=negative?0U-number_value:number_value;return true;
}
static bool boolean_field(reader_t *r,bool *value)
{
    space(r);const char *literal=r->pos<r->length&&r->bytes[r->pos]=='t'?"true":"false";
    size_t n=strlen(literal);
    if(n>r->length-r->pos||memcmp(r->bytes+r->pos,literal,n))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
    r->pos+=n;*value=literal[0]=='t';return true;
}
static bool mac_field(reader_t *r,uint8_t mac[6])
{
    uint8_t bytes[17],length=0;
    if(!string_bytes(r,bytes,sizeof(bytes),&length)||length!=17)return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
    for(unsigned i=0;i<6;++i){int high=hex(bytes[i*3]),low=hex(bytes[i*3+1]);
        if(high<0||low<0||(i<5&&bytes[i*3+2]!=':'))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
        mac[i]=(uint8_t)(high*16+low);
    }return true;
}
static bool ap_record(reader_t *r,cerberus_wifi_marauder_ap_t *out,unsigned index,
    uint16_t *used,uint16_t *ignored)
{
    if(!take(r,'{'))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
    cerberus_wifi_marauder_ap_row_t row={.station_offset=*used};unsigned seen=0;
    if(!take(r,'}'))for(;;){
        key_t key={0};if(!string(r,key_character,&key)||!take(r,':'))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
        unsigned field=key_is(&key,"essid")?1:key_is(&key,"channel")?2:key_is(&key,"bssid")?4:
            key_is(&key,"rssi")?8:key_is(&key,"packets")?16:key_is(&key,"sec")?32:
            key_is(&key,"wps")?64:key_is(&key,"man")?128:key_is(&key,"stations")?256:key_is(&key,"packet")?512:0;
        if(field&&(seen&field))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
        seen|=field;uint32_t number_value=0;
        if(field==1){if(!string_bytes(r,row.essid,sizeof(row.essid),&row.essid_length))return false;}
        else if(field==128){if(!string_bytes(r,row.man,sizeof(row.man),&row.man_length))return false;}
        else if(field==4){if(!mac_field(r,row.bssid))return false;}
        else if(field==64){if(!boolean_field(r,&row.wps))return false;}
        else if(field==256){
            if(!take(r,'['))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
            if(!take(r,']'))for(;;){
                uint8_t mac[6];if(*used>=CERBERUS_WIFI_MARAUDER_MAX_STATIONS)return fail(r,CERBERUS_WIFI_MARAUDER_LIMIT);
                if(!mac_field(r,mac))return false;
                if(out)memcpy(out->stations[*used],mac,6);
                ++*used;++row.station_count;
                if(take(r,']'))break;
                if(!take(r,','))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
            }
        }else if(field){
            if(!uint_field(r,field==2||field==32?UINT8_MAX:UINT32_MAX,&number_value,field==8))return false;
            if(field==2)row.channel=(uint8_t)number_value;
            else if(field==32)row.security=(uint8_t)number_value;
            else if(field==8)memcpy(&row.rssi,&number_value,sizeof(number_value));
            else {if((row.present&CERBERUS_WIFI_MARAUDER_AP_PACKETS)&&row.packets!=number_value)return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);row.packets=number_value;}
        }else {if(!skip(r,2))return false;++*ignored;}
        row.present|=(uint16_t)(field==512?CERBERUS_WIFI_MARAUDER_AP_PACKETS:field);
        if(take(r,'}'))break;
        if(!take(r,','))return fail(r,CERBERUS_WIFI_MARAUDER_INVALID);
    }
    if(out)out->rows[index]=row;
    return true;
}
static cerberus_wifi_marauder_result_t ap_parse(const uint8_t *input,size_t length,cerberus_wifi_marauder_ap_t *out)
{
    reader_t r={.bytes=input,.length=length};unsigned count=0;uint16_t used=0,ignored=0;
    if(!take(&r,'['))return CERBERUS_WIFI_MARAUDER_INVALID;
    if(!take(&r,']'))for(;;){
        if(count>=64)return CERBERUS_WIFI_MARAUDER_LIMIT;
        if(!ap_record(&r,out,count++,&used,&ignored))return r.error;
        if(take(&r,']'))break;
        if(!take(&r,','))return CERBERUS_WIFI_MARAUDER_INVALID;
    }
    space(&r);if(r.pos!=length)return CERBERUS_WIFI_MARAUDER_INVALID;
    if(out){out->count=(uint8_t)count;out->station_count=used;out->ignored_members=ignored;}
    return CERBERUS_WIFI_MARAUDER_OK;
}
static bool zero(const void *data,size_t n)
{const uint8_t *p=data;for(size_t i=0;i<n;++i)if(p[i])return false;return true;}
static bool bool_valid(const bool *value)
{const bool no=false,yes=true;return !memcmp(value,&no,sizeof(no))||!memcmp(value,&yes,sizeof(yes));}
static bool utf8_valid(const uint8_t *bytes,size_t length)
{
    for(size_t i=0;i<length;){uint32_t c=bytes[i++],minimum;unsigned n;
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){n=1;minimum=0x80;c&=31;}
        else if(c>=0xe0&&c<=0xef){n=2;minimum=0x800;c&=15;}
        else if(c>=0xf0&&c<=0xf4){n=3;minimum=0x10000;c&=7;}
        else return false;
        while(n--){if(i==length||(bytes[i]&0xc0)!=0x80)return false;c=(c<<6)|(bytes[i++]&63);}
        if(c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
    }return true;
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_validate(const cerberus_wifi_marauder_ap_t *doc)
{
    if(!doc)return CERBERUS_WIFI_MARAUDER_INVALID;
    if(doc->count>64||doc->station_count>256)return CERBERUS_WIFI_MARAUDER_LIMIT;
    unsigned used=0;
    for(unsigned i=0;i<doc->count;++i){const cerberus_wifi_marauder_ap_row_t *r=&doc->rows[i];
        if(r->present&~CERBERUS_WIFI_MARAUDER_AP_ALL||r->station_offset!=used||!bool_valid(&r->wps))return CERBERUS_WIFI_MARAUDER_INVALID;
        if(r->essid_length>32||r->man_length>64||r->station_count>256-used)return CERBERUS_WIFI_MARAUDER_LIMIT;
        if(!zero(r->essid+r->essid_length,32-r->essid_length)||!zero(r->man+r->man_length,64-r->man_length)||
           !utf8_valid(r->essid,r->essid_length)||!utf8_valid(r->man,r->man_length))return CERBERUS_WIFI_MARAUDER_INVALID;
        if((!(r->present&1)&&r->essid_length)||(!(r->present&2)&&r->channel)||(!(r->present&4)&&!zero(r->bssid,6))||
           (!(r->present&8)&&r->rssi)||(!(r->present&16)&&r->packets)||(!(r->present&32)&&r->security)||
           (!(r->present&64)&&r->wps)||(!(r->present&128)&&r->man_length)||(!(r->present&256)&&r->station_count))return CERBERUS_WIFI_MARAUDER_INVALID;
        used+=r->station_count;
    }return used==doc->station_count?CERBERUS_WIFI_MARAUDER_OK:CERBERUS_WIFI_MARAUDER_INVALID;
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_decode(const uint8_t *input,size_t length,cerberus_wifi_marauder_ap_t *out)
{
    if(!input||!out||overlap(input,length,out,sizeof(*out)))return CERBERUS_WIFI_MARAUDER_INVALID;
    if(length>CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES)return CERBERUS_WIFI_MARAUDER_LIMIT;
    cerberus_wifi_marauder_result_t result=ap_parse(input,length,NULL);
    if(result!=CERBERUS_WIFI_MARAUDER_OK)return result;
    memset(out,0,sizeof(*out));return ap_parse(input,length,out);
}

typedef struct {uint8_t *bytes;size_t length;} writer_t;
static void put(writer_t *w,uint8_t byte){if(w->bytes)w->bytes[w->length]=byte;++w->length;}
static void literal(writer_t *w,const char *s){while(*s)put(w,(uint8_t)*s++);}
static void integer(writer_t *w,uint32_t n)
{uint32_t place=1;while(n/place>=10)place*=10;do{put(w,(uint8_t)('0'+n/place));n%=place;place/=10;}while(place);}
static void quoted(writer_t *w,const uint8_t *bytes,size_t n)
{
    static const char hexchars[]="0123456789abcdef";put(w,'"');
    for(size_t i=0;i<n;++i){uint8_t c=bytes[i];
        if(c<32){literal(w,"\\u00");put(w,hexchars[c>>4]);put(w,hexchars[c&15]);}
        else{if(c=='"'||c=='\\')put(w,'\\');put(w,c);}
    }put(w,'"');
}
static void mac_write(writer_t *w,const uint8_t mac[6])
{static const char h[]="0123456789ABCDEF";put(w,'"');for(unsigned i=0;i<6;++i){if(i)put(w,':');put(w,h[mac[i]>>4]);put(w,h[mac[i]&15]);}put(w,'"');}
static void key_write(writer_t *w,bool *first,const char *key)
{if(!*first)put(w,',');*first=false;quoted(w,(const uint8_t*)key,strlen(key));put(w,':');}
static size_t ap_write(const cerberus_wifi_marauder_ap_t *doc,uint8_t *out)
{
    writer_t w={.bytes=out};put(&w,'[');
    for(unsigned i=0;i<doc->count;++i){const cerberus_wifi_marauder_ap_row_t *r=&doc->rows[i];bool first=true;
        if(i)put(&w,',');
        put(&w,'{');
        if(r->present&1){key_write(&w,&first,"essid");quoted(&w,r->essid,r->essid_length);}
        if(r->present&2){key_write(&w,&first,"channel");integer(&w,r->channel);}
        if(r->present&4){key_write(&w,&first,"bssid");mac_write(&w,r->bssid);}
        if(r->present&8){key_write(&w,&first,"rssi");uint32_t n=(uint32_t)r->rssi;if(r->rssi<0){put(&w,'-');n=0U-n;}integer(&w,n);}
        if(r->present&16){key_write(&w,&first,"packets");integer(&w,r->packets);key_write(&w,&first,"packet");integer(&w,r->packets);}
        if(r->present&32){key_write(&w,&first,"sec");integer(&w,r->security);}
        if(r->present&64){key_write(&w,&first,"wps");literal(&w,r->wps?"true":"false");}
        if(r->present&128){key_write(&w,&first,"man");quoted(&w,r->man,r->man_length);}
        if(r->present&256){key_write(&w,&first,"stations");put(&w,'[');
            for(unsigned j=0;j<r->station_count;++j){if(j)put(&w,',');mac_write(&w,doc->stations[r->station_offset+j]);}put(&w,']');}
        put(&w,'}');
    }put(&w,']');return w.length;
}
static bool encode_args(const void *doc,size_t size,uint8_t *out,size_t capacity,size_t *written)
{
    if(written&&((doc&&overlap(written,sizeof(*written),doc,size))||(out&&overlap(written,sizeof(*written),out,capacity))))return false;
    if(written)*written=0;
    return doc&&out&&!overlap(doc,size,out,capacity);
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_encode(const cerberus_wifi_marauder_ap_t *doc,uint8_t *out,size_t capacity,size_t *written)
{
    if(!encode_args(doc,doc?sizeof(*doc):0,out,capacity,written))return CERBERUS_WIFI_MARAUDER_INVALID;
    cerberus_wifi_marauder_result_t result=cerberus_wifi_marauder_ap_validate(doc);if(result)return result;
    size_t length=ap_write(doc,NULL);if(length>capacity||length>CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES)return CERBERUS_WIFI_MARAUDER_LIMIT;
    ap_write(doc,out);
    if(written)*written=length;
    return CERBERUS_WIFI_MARAUDER_OK;
}

cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_validate(const cerberus_wifi_marauder_ssid_t *doc)
{
    if(!doc||!bool_valid(&doc->final_lf))return CERBERUS_WIFI_MARAUDER_INVALID;
    if(doc->count>64)return CERBERUS_WIFI_MARAUDER_LIMIT;
    if((!doc->count&&doc->final_lf)||(doc->count&&!doc->rows[doc->count-1].length&&!doc->final_lf))return CERBERUS_WIFI_MARAUDER_INVALID;
    for(unsigned i=0;i<doc->count;++i){const cerberus_wifi_marauder_ssid_row_t *r=&doc->rows[i];
        if(r->length>32)return CERBERUS_WIFI_MARAUDER_LIMIT;
        if(!zero(r->bytes+r->length,32-r->length))return CERBERUS_WIFI_MARAUDER_INVALID;
        if(memchr(r->bytes,'\n',r->length))return CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE;
    }return CERBERUS_WIFI_MARAUDER_OK;
}
static cerberus_wifi_marauder_result_t ssid_parse(const uint8_t *input,size_t length,cerberus_wifi_marauder_ssid_t *out)
{
    unsigned count=0;size_t start=0;
    for(size_t i=0;i<=length;++i){if(i<length&&input[i]!='\n')continue;
        if(i==length&&i==start)break;
        if(count==64||i-start>32)return CERBERUS_WIFI_MARAUDER_LIMIT;
        if(out){out->rows[count].length=(uint8_t)(i-start);memcpy(out->rows[count].bytes,input+start,i-start);}
        ++count;start=i+1;
    }
    if(out){out->count=(uint8_t)count;out->final_lf=length&&input[length-1]=='\n';}
    return CERBERUS_WIFI_MARAUDER_OK;
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_decode(const uint8_t *input,size_t length,cerberus_wifi_marauder_ssid_t *out)
{
    if((!input&&length)||!out||overlap(input,length,out,sizeof(*out)))return CERBERUS_WIFI_MARAUDER_INVALID;
    if(length>CERBERUS_WIFI_MARAUDER_SSID_MAX_BYTES)return CERBERUS_WIFI_MARAUDER_LIMIT;
    cerberus_wifi_marauder_result_t result=ssid_parse(input,length,NULL);if(result)return result;
    memset(out,0,sizeof(*out));return ssid_parse(input,length,out);
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_encode(const cerberus_wifi_marauder_ssid_t *doc,uint8_t *out,size_t capacity,size_t *written)
{
    if(!encode_args(doc,doc?sizeof(*doc):0,out,capacity,written))return CERBERUS_WIFI_MARAUDER_INVALID;
    cerberus_wifi_marauder_result_t result=cerberus_wifi_marauder_ssid_validate(doc);if(result)return result;
    size_t length=doc->count?(size_t)doc->count-1+(doc->final_lf?1:0):0;
    for(unsigned i=0;i<doc->count;++i)length+=doc->rows[i].length;
    if(length>capacity)return CERBERUS_WIFI_MARAUDER_LIMIT;
    size_t offset=0;for(unsigned i=0;i<doc->count;++i){memcpy(out+offset,doc->rows[i].bytes,doc->rows[i].length);offset+=doc->rows[i].length;
        if(i+1<doc->count||doc->final_lf)out[offset++]='\n';}
    if(written)*written=length;
    return CERBERUS_WIFI_MARAUDER_OK;
}

bool cerberus_wifi_marauder_security_from_archive(uint8_t native_security,uint8_t *upstream_security)
{
    static const uint8_t map[10]={0,1,2,3,5,4,255,255,255,255};
    if(!upstream_security)return false;
    *upstream_security=native_security<10?map[native_security]:255;
    return native_security<6;
}
static bool convert_args(const void *source,size_t source_size,void *out,size_t out_size,cerberus_wifi_marauder_export_stats_t *stats)
{return source&&out&&!overlap(source,source_size,out,out_size)&&(!stats||(!overlap(stats,sizeof(*stats),source,source_size)&&!overlap(stats,sizeof(*stats),out,out_size)));}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_from_archive(const cerberus_wifi_archive_t *archive,cerberus_wifi_marauder_ap_t *out,cerberus_wifi_marauder_export_stats_t *stats)
{
    if(!convert_args(archive,sizeof(*archive),out,sizeof(*out),stats)||!cerberus_wifi_archive_validate(archive))return CERBERUS_WIFI_MARAUDER_INVALID;
    cerberus_wifi_marauder_export_stats_t report={.records=archive->count,.loss_flags=7};
    for(unsigned i=0;i<archive->count;++i){const cerberus_wifi_archive_row_t *r=&archive->rows[i];
        if(!utf8_valid((const uint8_t*)r->ssid,strlen(r->ssid)))return CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE;
        uint8_t security;if(!cerberus_wifi_marauder_security_from_archive(r->security,&security))++report.security_unmapped;
        if(!r->rssi_valid)++report.rssi_omitted;
    }
    memset(out,0,sizeof(*out));out->count=archive->count;
    for(unsigned i=0;i<archive->count;++i){const cerberus_wifi_archive_row_t *r=&archive->rows[i];cerberus_wifi_marauder_ap_row_t *d=&out->rows[i];
        d->present=1|2|4|32;d->essid_length=(uint8_t)strlen(r->ssid);memcpy(d->essid,r->ssid,d->essid_length);memcpy(d->bssid,r->bssid,6);d->channel=r->channel;
        cerberus_wifi_marauder_security_from_archive(r->security,&d->security);
        if(r->rssi_valid){d->present|=8;d->rssi=r->rssi;}
    }
    if(stats)*stats=report;
    return CERBERUS_WIFI_MARAUDER_OK;
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_from_archive(const cerberus_wifi_archive_t *archive,cerberus_wifi_marauder_ssid_t *out,cerberus_wifi_marauder_export_stats_t *stats)
{
    if(!convert_args(archive,sizeof(*archive),out,sizeof(*out),stats)||!cerberus_wifi_archive_validate(archive))return CERBERUS_WIFI_MARAUDER_INVALID;
    for(unsigned i=0;i<archive->count;++i)if(strchr(archive->rows[i].ssid,'\n'))return CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE;
    memset(out,0,sizeof(*out));out->count=archive->count;out->final_lf=out->count!=0;
    for(unsigned i=0;i<archive->count;++i){out->rows[i].length=(uint8_t)strlen(archive->rows[i].ssid);memcpy(out->rows[i].bytes,archive->rows[i].ssid,out->rows[i].length);}
    if(stats)*stats=(cerberus_wifi_marauder_export_stats_t){.records=archive->count,.loss_flags=15};
    return CERBERUS_WIFI_MARAUDER_OK;
}
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_from_ap(const cerberus_wifi_marauder_ap_t *ap,cerberus_wifi_marauder_ssid_t *out,cerberus_wifi_marauder_export_stats_t *stats)
{
    if(!convert_args(ap,sizeof(*ap),out,sizeof(*out),stats))return CERBERUS_WIFI_MARAUDER_INVALID;
    cerberus_wifi_marauder_result_t result=cerberus_wifi_marauder_ap_validate(ap);if(result)return result;
    cerberus_wifi_marauder_export_stats_t report={.loss_flags=CERBERUS_WIFI_MARAUDER_LOSS_AP_DETAILS|(ap->ignored_members?CERBERUS_WIFI_MARAUDER_LOSS_EXTENSIONS:0)};
    for(unsigned i=0;i<ap->count;++i){if(!(ap->rows[i].present&1)){++report.names_absent;continue;}
        if(memchr(ap->rows[i].essid,'\n',ap->rows[i].essid_length))return CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE;
        ++report.records;
    }
    memset(out,0,sizeof(*out));
    for(unsigned i=0;i<ap->count;++i)if(ap->rows[i].present&1){cerberus_wifi_marauder_ssid_row_t *r=&out->rows[out->count++];r->length=ap->rows[i].essid_length;memcpy(r->bytes,ap->rows[i].essid,r->length);}
    out->final_lf=out->count!=0;
    if(stats)*stats=report;
    return CERBERUS_WIFI_MARAUDER_OK;
}
