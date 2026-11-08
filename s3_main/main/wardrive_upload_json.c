#include "wardrive_upload_json.h"
#include "wardrive_json_internal.h"

static bool object_fields(const json_t *j,int object,const char *const *names,unsigned count)
{
    if(object<0||j->t[object].type!='{')return false;
    unsigned actual=0;
    for(unsigned k=(unsigned)object+1;k<j->t[object].next;k=j->t[j->t[k].next].next)++actual;
    if(actual!=count)return false;
    for(unsigned i=0;i<count;++i)if(field(j,object,names[i])<0)return false;
    return true;
}
static unsigned unicode4(const char *text)
{
    unsigned result=0;
    for(unsigned i=0;i<4;++i){unsigned char c=(unsigned char)text[i];
        result=result*16+(c<='9'?c-'0':c<='F'?c-'A'+10:c-'a'+10);}
    return result;
}
static bool decoded(const json_t *j,int token,char *out,size_t capacity)
{
    if(token<0||j->t[token].type!='"')return false;
    unsigned begin=j->t[token].start+1,end=j->t[token].end-1;size_t used=0;
    for(unsigned i=begin;i<end;++i){
        unsigned c=(unsigned char)j->s[i];unsigned char encoded[4];size_t n=1;
        if(c=='\\'){
            c=(unsigned char)j->s[++i];
            if(c=='u'){
                c=unicode4(j->s+i+1);i+=4;
                if(c>=0xd800&&c<=0xdbff){unsigned low=unicode4(j->s+i+3);i+=6;c=0x10000+((c-0xd800)<<10)+(low-0xdc00);}
                if(c>=0x10000){n=4;encoded[0]=(unsigned char)(0xf0|(c>>18));encoded[1]=(unsigned char)(0x80|((c>>12)&63));encoded[2]=(unsigned char)(0x80|((c>>6)&63));encoded[3]=(unsigned char)(0x80|(c&63));}
                else if(c>=0x800){n=3;encoded[0]=(unsigned char)(0xe0|(c>>12));encoded[1]=(unsigned char)(0x80|((c>>6)&63));encoded[2]=(unsigned char)(0x80|(c&63));}
                else if(c>=0x80){n=2;encoded[0]=(unsigned char)(0xc0|(c>>6));encoded[1]=(unsigned char)(0x80|(c&63));}
            }else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';
        }
        if(!c||used+n>=capacity)return false;
        if(n==1)encoded[0]=(unsigned char)c;
        memcpy(out+used,encoded,n);used+=n;
    }
    out[used]=0;return true;
}
static bool enabled(const json_t *j,int object,unsigned index,cerberus_upload_config_t *config)
{
    int token=field(j,object,"enabled");
    if(literal(j,token,"true")){config->destinations|=(uint8_t)(1U<<index);return true;}
    return literal(j,token,"false");
}
bool cerberus_upload_config_json(const void *bytes,size_t length,cerberus_upload_config_t *out)
{
    if(!out)return false;
    if(bytes){uintptr_t a=(uintptr_t)bytes,b=(uintptr_t)out;
        if(a<=b?b-a<length:a-b<sizeof(*out))return false;}
    cerberus_upload_config_clear(out,sizeof(*out));
    if(!bytes||!length||length>CERBERUS_UPLOAD_JSON_MAX_BYTES||memchr(bytes,0,length))return false;
    json_t j={.s=bytes,.n=length};
    if(!value(&j,0))return false;
    space(&j);if(j.pos!=j.n)return false;
    static const char *const root[]={"schema","home_wifi","wigle","wdgwars","wardrift"};
    static const char *const wifi[]={"ssid","password"};
    static const char *const wigle[]={"enabled","api_name","api_token"};
    static const char *const token[]={"enabled","token"};
    int home=field(&j,0,"home_wifi"),w=field(&j,0,"wigle"),d=field(&j,0,"wdgwars"),r=field(&j,0,"wardrift");
    if(!object_fields(&j,0,root,5)||!literal(&j,field(&j,0,"schema"),"\"cerberus-wardrive-upload-v1\"")||
        !object_fields(&j,home,wifi,2)||!object_fields(&j,w,wigle,3)||!object_fields(&j,d,token,2)||!object_fields(&j,r,token,2))return false;
    cerberus_upload_config_t candidate={0};
#define READ(parent,key,member) decoded(&j,field(&j,parent,key),candidate.member,sizeof(candidate.member))
    bool valid=READ(home,"ssid",ssid)&&READ(home,"password",password)&&READ(w,"api_name",wigle_name)&&
        READ(w,"api_token",wigle_token)&&READ(d,"token",wdgwars_token)&&READ(r,"token",wardrift_token)&&
        enabled(&j,w,0,&candidate)&&enabled(&j,d,1,&candidate)&&enabled(&j,r,2,&candidate)&&
        cerberus_upload_config_invalid(&candidate)==CERBERUS_UPLOAD_FIELD_NONE;
#undef READ
    if(valid&&candidate.destinations){
        valid=cerberus_upload_config_wifi_ready(&candidate);
        for(unsigned i=0;i<3;++i)if((candidate.destinations&(1U<<i))&&!cerberus_upload_config_service_ready(&candidate,(cerberus_upload_service_t)i))valid=false;
    }
    if(valid)*out=candidate;
    cerberus_upload_config_clear(&candidate,sizeof(candidate));return valid;
}
