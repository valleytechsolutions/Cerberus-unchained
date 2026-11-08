#include "wardrive_upload_reply.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "wardrive_json_internal.h"

static bool count(const json_t *j,int i,uint32_t *out)
{
    if(i<0||j->t[i].type!='0')return false;
    uint32_t v=0;
    for(unsigned k=j->t[i].start;k<j->t[i].end;++k){unsigned d=(unsigned char)j->s[k]-'0';
        if(d>9||v>(UINT32_MAX-d)/10)return false;
        v=v*10+d;}*out=v;return true;
}
static bool receipt_id(const json_t *j,int i,char out[81])
{
    if(i<0||j->t[i].type!='"')return false;
    unsigned n=j->t[i].end-j->t[i].start-2;if(!n||n>80)return false;
    const char *s=j->s+j->t[i].start+1;
    for(unsigned k=0;k<n;++k)if(!((s[k]>='0'&&s[k]<='9')||(s[k]>='A'&&s[k]<='Z')||
        (s[k]>='a'&&s[k]<='z')||s[k]=='-'||s[k]=='_'))return false;
    memcpy(out,s,n);out[n]=0;return true;
}
static bool queued_job_id(const json_t *j,int i,char out[81])
{
    if(i<0||(j->t[i].type!='0'&&j->t[i].type!='"'))return false;
    unsigned quoted=j->t[i].type=='"',start=j->t[i].start+quoted,end=j->t[i].end-quoted;
    unsigned n=end-start;if(!n||n>20)return false;
    unsigned long long value=0;
    for(unsigned k=start;k<end;++k){
        unsigned digit=(unsigned char)j->s[k]-'0';
        if(digit>9||value>(ULLONG_MAX-digit)/10)return false;
        value=value*10+digit;
    }
    if(!value)return false;
    memcpy(out,j->s+start,n);out[n]=0;return true;
}
cerberus_upload_reply_t cerberus_upload_reply(cerberus_upload_service_t service,
    unsigned status,const char *body,size_t length,bool complete)
{
    cerberus_upload_reply_t out={.result=CERBERUS_UPLOAD_UNKNOWN};
    if(!complete||!body||!length||length>4096||memchr(body,0,length)||(unsigned)service>=CERBERUS_UPLOAD_SERVICE_COUNT)return out;
    json_t j={.s=body,.n=length};
    if(!value(&j,0))return out;
    space(&j);if(j.pos!=j.n||j.t[0].type!='{')return out;
    const char *key=service==CERBERUS_UPLOAD_WIGLE?"success":service==CERBERUS_UPLOAD_WDGWARS?"ok":"accepted";
    int success=field(&j,0,key);
    if(literal(&j,success,"false")&&(status==400||status==401||status==403||status==413||status==422||status==429||status==200)) {
        out.result=CERBERUS_UPLOAD_REJECTED;return out;
    }
    if(!literal(&j,success,"true")||field(&j,0,"error")>=0)return out;
    /* WDGWars v2 acknowledges queue admission with a positive job_id.
     * Preserve that receipt as QUEUED; it does not establish import/credit. */
    if(service==CERBERUS_UPLOAD_WDGWARS&&status==202){
        if(queued_job_id(&j,field(&j,0,"job_id"),out.transaction))out.result=CERBERUS_UPLOAD_QUEUED;
        return out;
    }
    if(status!=200)return out;
    if(service==CERBERUS_UPLOAD_WIGLE) {
        int results=field(&j,0,"results"),trans=field(&j,results,"transids");
        if(trans<0||j.t[trans].type!='['||trans+1>=j.t[trans].next)return out;
        int first=trans+1;
        if(j.t[first].next!=j.t[trans].next||!receipt_id(&j,field(&j,first,"transId"),out.transaction))return out;
        out.result=CERBERUS_UPLOAD_QUEUED;
    } else {
        if(service==CERBERUS_UPLOAD_WDGWARS) {
            if(!count(&j,field(&j,0,"imported"),&out.imported))return out;
            out.counts_valid=true;uint32_t a=0,b=0;
            int ai=field(&j,0,"no_gps"),bi=field(&j,0,"bad_rows");
            if((ai>=0&&!count(&j,ai,&a))||(bi>=0&&!count(&j,bi,&b))||a>UINT32_MAX-b)return out;
            out.rejected_rows=a+b;
        }
        out.result=CERBERUS_UPLOAD_ACCEPTED;
    } return out;
}
const char *cerberus_upload_result_name(cerberus_upload_result_t result)
{
    static const char *const names[]={"NOT SENT","SENDING","UPLOADED","UPLOADED","REJECTED","CHECK ACCOUNT"};
    return (unsigned)result<sizeof(names)/sizeof(*names)?names[result]:"CHECK ACCOUNT";
}
bool cerberus_upload_reply_merge(cerberus_upload_reply_t *aggregate,const cerberus_upload_reply_t *part)
{
    if(!aggregate||!part)return false;
    static const unsigned priority[]={2,5,1,0,3,5};
    unsigned a=(unsigned)aggregate->result,p=(unsigned)part->result;
    if(a>=6||p>=6){aggregate->result=CERBERUS_UPLOAD_UNKNOWN;return false;}
    if(priority[p]>priority[a]){
        aggregate->result=part->result==CERBERUS_UPLOAD_SENDING?CERBERUS_UPLOAD_UNKNOWN:part->result;
        memcpy(aggregate->transaction,part->transaction,sizeof(aggregate->transaction));
    }
    if(part->counts_valid&&part->result==CERBERUS_UPLOAD_ACCEPTED){
        if(aggregate->imported>UINT32_MAX-part->imported||aggregate->rejected_rows>UINT32_MAX-part->rejected_rows){
            aggregate->result=CERBERUS_UPLOAD_UNKNOWN;return false;
        }
        aggregate->counts_valid=true;aggregate->imported+=part->imported;aggregate->rejected_rows+=part->rejected_rows;
    }
    return true;
}
