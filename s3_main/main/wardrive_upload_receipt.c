#include "wardrive_upload_receipt.h"
#include <string.h>
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t c=~0U;while(n--){c^=*p++;for(unsigned i=0;i<8;++i)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}return ~c;}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[i]<<(8*i);return v;}
bool cerberus_upload_receipt_encode(const cerberus_upload_reply_t *r,uint8_t b[128])
{
    if(!r||!b||r->result<CERBERUS_UPLOAD_SENDING||r->result>CERBERUS_UPLOAD_UNKNOWN)return false;
    size_t n=0;while(n<sizeof(r->transaction)&&r->transaction[n])++n;
    if(n==sizeof(r->transaction))return false;
    for(size_t i=0;i<n;++i){char c=r->transaction[i];if(!((c>='0'&&c<='9')||
        (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='-'||c=='_'))return false;}
    memset(b,0,128);memcpy(b,"CBUPRCT1",8);b[8]=(uint8_t)r->result;b[9]=r->counts_valid?1:0;
    put(b+12,r->imported);put(b+16,r->rejected_rows);memcpy(b+20,r->transaction,n);put(b+124,crc(b,124));return true;
}
bool cerberus_upload_receipt_decode(const uint8_t b[128],cerberus_upload_reply_t *r)
{
    if(!r)return false;
    memset(r,0,sizeof(*r));r->result=CERBERUS_UPLOAD_UNKNOWN;
    if(!b||memcmp(b,"CBUPRCT1",8)||get(b+124)!=crc(b,124)||b[9]>1)return false;
    cerberus_upload_reply_t parsed={.result=(cerberus_upload_result_t)b[8],.counts_valid=b[9]!=0,
        .imported=get(b+12),.rejected_rows=get(b+16)};
    memcpy(parsed.transaction,b+20,81);uint8_t check[128];
    if(!cerberus_upload_receipt_encode(&parsed,check)||memcmp(b,check,128))return false;
    *r=parsed;return true;
}
