#include "wardrive_upload_reply.h"
#include "wardrive_upload_receipt.h"
#include "wardrive_upload_file.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static bool stop(void *ctx){return *(bool *)ctx;}
static void merge_checks(void)
{
    const cerberus_upload_result_t ordered[]={CERBERUS_UPLOAD_ACCEPTED,CERBERUS_UPLOAD_QUEUED,
        CERBERUS_UPLOAD_NEW,CERBERUS_UPLOAD_REJECTED,CERBERUS_UPLOAD_UNKNOWN};
    for(unsigned a=0;a<5;++a)for(unsigned b=0;b<5;++b){
        cerberus_upload_reply_t result={.result=CERBERUS_UPLOAD_ACCEPTED};
        cerberus_upload_reply_t first={.result=ordered[a]},second={.result=ordered[b]};
        assert(cerberus_upload_reply_merge(&result,&first));assert(cerberus_upload_reply_merge(&result,&second));
        assert(result.result==ordered[a>b?a:b]);
    }
    cerberus_upload_reply_t sum={.result=CERBERUS_UPLOAD_ACCEPTED};
    cerberus_upload_reply_t part={.result=CERBERUS_UPLOAD_ACCEPTED,.counts_valid=true,.imported=10,.rejected_rows=2};
    assert(cerberus_upload_reply_merge(&sum,&part)&&cerberus_upload_reply_merge(&sum,&part));
    assert(sum.counts_valid&&sum.imported==20&&sum.rejected_rows==4);
    part.imported=UINT32_MAX;assert(!cerberus_upload_reply_merge(&sum,&part)&&sum.result==CERBERUS_UPLOAD_UNKNOWN);
}
int main(int argc,char **argv)
{
    merge_checks();
    assert(argc>=3);
    if(!strcmp(argv[1],"reply")){
        assert(argc==6);FILE *f=fopen(argv[2],"rb");assert(f);char bytes[5000];size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
        cerberus_upload_reply_t r=cerberus_upload_reply((cerberus_upload_service_t)atoi(argv[3]),(unsigned)atoi(argv[4]),bytes,n,atoi(argv[5])!=0);
        printf("%u %u %u %u %s\n",r.result,r.imported,r.rejected_rows,r.counts_valid,r.transaction);
        if(r.result!=CERBERUS_UPLOAD_NEW){uint8_t wire[128],bad[128];cerberus_upload_reply_t other;
            assert(cerberus_upload_receipt_encode(&r,wire));assert(cerberus_upload_receipt_decode(wire,&other));
            assert(!memcmp(&r,&other,sizeof(r)));
            for(unsigned i=0;i<128;++i)for(unsigned bit=0;bit<8;++bit){memcpy(bad,wire,128);bad[i]^=(uint8_t)(1U<<bit);
                assert(!cerberus_upload_receipt_decode(bad,&other));assert(other.result==CERBERUS_UPLOAD_UNKNOWN);}}
    }else{
        assert(argc==4);FILE *f=fopen(argv[2],"rb");assert(f);cerberus_upload_file_t plan;bool cancel=atoi(argv[3])!=0;
        bool ok=cerberus_upload_file_prepare(f,&plan,stop,&cancel);
        printf("%u %u %lu %zu",ok,plan.parts,(unsigned long)plan.summary.rows,plan.header_bytes);
        for(unsigned i=0;i<=plan.parts;++i)printf(" %lu",(unsigned long)plan.offsets[i]);puts("");fclose(f);
    }
    return 0;
}
