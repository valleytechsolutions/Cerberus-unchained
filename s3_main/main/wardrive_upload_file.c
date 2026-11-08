#include "wardrive_upload_file.h"
#include <string.h>
bool cerberus_upload_file_prepare(FILE *file,cerberus_upload_file_t *out,
    cerberus_upload_cancel_fn cancel,void *context)
{
    if(!file||!out)return false;
    memset(out,0,sizeof(*out));
    if(fseek(file,0,SEEK_SET))return false;
    cerberus_ward_log_parser_t parser;cerberus_ward_log_init(&parser);
    char buffer[4096];size_t n;uint32_t offset=0,last_line=0;unsigned lines=0;
    bool valid=true;
    while((n=fread(buffer,1,sizeof(buffer),file))>0) {
        if((cancel&&cancel(context))||!cerberus_ward_log_feed(&parser,buffer,n)){valid=false;break;}
        for(size_t i=0;i<n;++i) {
            ++offset;
            if(lines<2) {
                if(out->header_bytes==sizeof(out->headers)){valid=false;break;}
                out->headers[out->header_bytes++]=buffer[i];
            }
            if(buffer[i]!='\n')continue;
            if(lines<2){if(++lines==2)out->offsets[0]=offset;last_line=offset;continue;}
            if(offset-out->offsets[out->parts]>CERBERUS_UPLOAD_PART_BYTES) {
                if(last_line==out->offsets[out->parts]||out->parts+1>=CERBERUS_UPLOAD_PARTS){valid=false;break;}
                out->offsets[++out->parts]=last_line;
            }
            last_line=offset;
        }
        if(!valid)break;
    }
    if(ferror(file)||!valid||!cerberus_ward_log_finish(&parser)) {
        out->summary=parser.summary;
        if(out->summary.error==CERBERUS_WARD_LOG_OK)out->summary.error=CERBERUS_WARD_LOG_ROW;
        out->parts=0;return false;
    }
    out->summary=parser.summary;
    if(offset>out->offsets[out->parts])out->offsets[++out->parts]=offset;
    return out->parts>0;
}
