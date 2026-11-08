#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wardrive_upload_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool zero(const void *bytes,size_t n){const unsigned char *p=bytes;while(n--)if(*p++)return false;return true;}
int main(int argc,char **argv)
{
    assert(argc==3);FILE *file=fopen(argv[1],"rb");assert(file);char bytes[8192];
    size_t length=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));assert(!fclose(file));
    cerberus_upload_config_t config;memset(&config,0xa5,sizeof(config));
    bool valid=cerberus_upload_config_json(bytes,length,&config);
    if(valid){
        uint8_t wire[512];cerberus_upload_config_t decoded;
        assert(cerberus_upload_config_encode(&config,wire)&&cerberus_upload_config_decode(wire,sizeof(wire),&decoded));
        assert(!memcmp(&config,&decoded,sizeof(config)));
        if(strcmp(argv[2],"-")){file=fopen(argv[2],"wb");assert(file);assert(fwrite(wire,1,sizeof(wire),file)==sizeof(wire));assert(!fclose(file));}
        cerberus_upload_config_clear(wire,sizeof(wire));cerberus_upload_config_clear(&decoded,sizeof(decoded));
    }else assert(zero(&config,sizeof(config)));
    printf("{\"valid\":%s,\"destinations\":%u}\n",valid?"true":"false",config.destinations);
    cerberus_upload_config_clear(&config,sizeof(config));cerberus_upload_config_clear(bytes,sizeof(bytes));
    return 0;
}
