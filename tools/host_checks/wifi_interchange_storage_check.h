/* Actual new storage functions, injected filesystem failures, no parser mock. */
static uint8_t interchange_bytes[CERBERUS_STORAGE_AP_JSON_MAX_BYTES];
static uint8_t interchange_read[CERBERUS_STORAGE_AP_JSON_MAX_BYTES];
static esp_err_t (*interchange_save)(const uint8_t *,size_t,char *,size_t);
static esp_err_t (*interchange_load)(const char *,const char *,uint8_t *,size_t,size_t *);
static size_t interchange_capacity;
static const char *interchange_file;

static esp_err_t interchange_get(size_t *length)
{
    memset(interchange_read,0xA5,sizeof(interchange_read)); *length=999;
    return interchange_load("/CERBERUS/WIFI",interchange_file,interchange_read,interchange_capacity,length);
}
static void interchange_failed(size_t length)
{
    CHECK(length==0);
    for(size_t i=0;i<interchange_capacity;++i)CHECK(interchange_read[i]==0);
    for(size_t i=interchange_capacity;i<sizeof(interchange_read);++i)CHECK(interchange_read[i]==0xA5);
    clean();
}
static void interchange_verify(const char *file,const void *bytes,size_t length)
{
    char path[128];snprintf(path,sizeof(path),"sdcard/CERBERUS/WIFI/%s",file);
    FILE *f=fopen(path,"rb");CHECK(f);
    CHECK(fread(interchange_read,1,sizeof(interchange_read),f)==length);
    CHECK(!memcmp(interchange_read,bytes,length)&&fgetc(f)==EOF&&fclose(f)==0);
}
static void check_wifi_interchange_storage(void)
{
    for(unsigned ssid=0;ssid<2;++ssid){
        interchange_save=ssid?cerberus_storage_ssid_text_save:cerberus_storage_ap_json_save;
        interchange_load=ssid?cerberus_storage_ssid_text_load:cerberus_storage_ap_json_load;
        interchange_capacity=ssid?CERBERUS_STORAGE_SSID_TEXT_MAX_BYTES:CERBERUS_STORAGE_AP_JSON_MAX_BYTES;
        interchange_file=ssid?"S00001.TXT":"P00001.LOG";
        const char *temporary=ssid?"S00001.TMP":"P00001.TMP";
        const char *second=ssid?"S00002.TXT":"P00002.LOG";
        const char *second_temp=ssid?"S00002.TMP":"P00002.TMP";
        const char *third=ssid?"S00003.TXT":"P00003.LOG";
        char full[128],temp_path[128],second_path[128];
        snprintf(full,sizeof(full),"sdcard/CERBERUS/WIFI/%s",interchange_file);
        snprintf(temp_path,sizeof(temp_path),"sdcard/CERBERUS/WIFI/%s",temporary);
        snprintf(second_path,sizeof(second_path),"sdcard/CERBERUS/WIFI/%s",second_temp);
        for(size_t i=0;i<interchange_capacity;++i)interchange_bytes[i]=(uint8_t)i;
        size_t got=0;
        setup();probe_handoff=true;
        CHECK(interchange_save(interchange_bytes,interchange_capacity,name,sizeof(name))==ESP_OK);
        CHECK(!strcmp(name,interchange_file));short_read=true;
        CHECK(interchange_get(&got)==ESP_OK&&got==interchange_capacity);
        CHECK(!memcmp(interchange_read,interchange_bytes,got)&&handoff_probes>100);clean();
        probe_handoff=false;CHECK(cerberus_storage_reserve_for_usb()==ESP_OK&&!sd_mounted);clean();

        setup();put(full,"KEEP",4);put(second_path,"KEEP",4);put("sdcard/APs_0.log","ORIGINAL",8);
        CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))==ESP_OK&&!strcmp(name,third));
        interchange_verify(interchange_file,"KEEP",4);interchange_verify(second_temp,"KEEP",4);
        CHECK(interchange_load("/","APs_0.log",interchange_read,interchange_capacity,&got)==ESP_OK&&got==8);
        setup();race_temp=true;CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))==ESP_OK&&!strcmp(name,second));
        interchange_verify(temporary,"KEEP",4);
        setup();race_final_path=full;race_final=true;
        CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))!=ESP_OK&&!strcmp(name,temporary));
        interchange_verify(interchange_file,"KEEP",4);interchange_verify(temporary,interchange_bytes,33);
        setup();collide_all=true;
        CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))==ESP_ERR_NOT_FOUND&&!calls[OPEN]);clean();
        setup();if(ssid)sd_ssid_text_next_index=99999;else sd_ap_json_next_index=99999;
        CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))==ESP_OK&&!strcmp(name,ssid?"S99999.TXT":"P99999.LOG"));
        CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))==ESP_OK&&!strcmp(name,interchange_file));
        CHECK(sd_wifi_archive_next_index==1&&sd_ble_archive_next_index==1&&sd_airtag_json_next_index==1);

        const enum event faults[]={STAT,OPEN,FDOPEN,WRITE,FLUSH,SYNC,CLOSE,RENAME};
        for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i){
            setup();fault[faults[i]]=1;
            CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))!=ESP_OK&&!wifi_exists(interchange_file));
            if(faults[i]>=FDOPEN)CHECK(wifi_exists(temporary)&&!strcmp(name,temporary));
            clean();memset(fault,0,sizeof(fault));
            CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))==ESP_OK);clean();
        }
        for(unsigned i=3;i<=5;++i){setup();fault[STAT]=i;CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))!=ESP_OK);clean();}
        setup();CHECK(native_rmdir("sdcard/CERBERUS/WIFI")==0);fault[MKDIR]=1;
        CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))!=ESP_OK&&!name[0]);clean();
        setup();extra_error=true;CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))!=ESP_OK&&!calls[RENAME]);clean();
        for(unsigned owner=0;owner<7;++owner){
            setup();if(owner==0)sd_mounted=false;if(owner==1)storage_owner=CERBERUS_STORAGE_OWNER_USB;
            if(owner==2)sd_recon.active=true;if(owner==3)wardrive_busy=true;if(owner==4)sd_wifi_capture_file=(FILE*)1;
            if(owner==5)fail_lock=true;if(owner==6){storage_mutex=NULL;fail_mutex=true;}
            CHECK(interchange_save(interchange_bytes,33,name,sizeof(name))!=ESP_OK&&!calls[OPEN]);
            CHECK(interchange_get(&got)!=ESP_OK);interchange_failed(got);
        }
        const enum event read_faults[]={STAT,FOPEN,READ,GETC,CLOSE};
        for(unsigned i=0;i<sizeof(read_faults)/sizeof(read_faults[0]);++i){
            setup();put(full,interchange_bytes,128);fault[read_faults[i]]=1;
            CHECK(interchange_get(&got)!=ESP_OK);interchange_failed(got);interchange_verify(interchange_file,interchange_bytes,128);
        }
        for(unsigned kind=0;kind<6;++kind){
            setup();put(full,interchange_bytes,128);
            if(kind==0)stat_size_delta=1;if(kind==1)stat_size_delta=-1;if(kind==2)extra_error=true;
            if(kind==3)no_eof=true;if(kind==4){short_read=true;zero_read_at=3;}if(kind==5)stat_size_delta=(int)interchange_capacity;
            CHECK(interchange_get(&got)!=ESP_OK);if(kind==5)CHECK(!calls[FOPEN]);interchange_failed(got);
        }
        setup();CHECK(interchange_get(&got)==ESP_ERR_NOT_FOUND);interchange_failed(got);
        CHECK(native_mkdir(full)==0);CHECK(interchange_get(&got)==ESP_ERR_INVALID_SIZE&&!calls[FOPEN]);interchange_failed(got);
        setup();put(full,interchange_bytes,0);
        CHECK(interchange_get(&got)==(ssid?ESP_OK:ESP_ERR_INVALID_SIZE)&&got==0);
        if(!ssid)interchange_failed(got);else CHECK(calls[GETC]==1&&!calls[READ]);
        setup();CHECK(interchange_save(interchange_bytes,0,name,sizeof(name))==(ssid?ESP_OK:ESP_ERR_INVALID_ARG));
        if(ssid){interchange_verify(interchange_file,interchange_bytes,0);CHECK(calls[FLUSH]==1&&calls[SYNC]==1&&calls[RENAME]==1);}
        if(ssid){
            const enum event empty_faults[]={FLUSH,SYNC,CLOSE,RENAME};
            for(unsigned i=0;i<sizeof(empty_faults)/sizeof(empty_faults[0]);++i){
                setup();fault[empty_faults[i]]=1;
                CHECK(interchange_save(interchange_bytes,0,name,sizeof(name))!=ESP_OK);
                CHECK(!wifi_exists(interchange_file)&&wifi_exists(temporary));clean();
            }
            setup();put(full,interchange_bytes,0);fault[GETC]=1;
            CHECK(interchange_get(&got)!=ESP_OK);interchange_failed(got);
            setup();put(full,interchange_bytes,1);stat_size_delta=-1;
            CHECK(interchange_get(&got)!=ESP_OK);interchange_failed(got);
        }
        CHECK(interchange_save(interchange_bytes,interchange_capacity+1,name,sizeof(name))==ESP_ERR_INVALID_ARG);
        CHECK(interchange_save(interchange_bytes,1,name,10)==ESP_ERR_INVALID_ARG);
        const char *bad[]={"../x.LOG","x.TMP","x.WFI","x.LOG/",".LOG",".TXT",".JSON","","sub/x.LOG","C:x.LOG"};
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
            CHECK(interchange_load("/",bad[i],interchange_read,interchange_capacity,&got)==ESP_ERR_INVALID_ARG);
        CHECK(interchange_load("/../x","x.LOG",interchange_read,interchange_capacity,&got)==ESP_ERR_INVALID_ARG);
        CHECK(interchange_load("/","x.LOG",interchange_read,interchange_capacity+1,&got)==ESP_ERR_INVALID_ARG);
        setup();const char *allowed=ssid?"SSIDs_0.TxT":"APs_0.JsOn";char input_path[80];
        snprintf(input_path,sizeof(input_path),"sdcard/%s",allowed);put(input_path,interchange_bytes,33);
        CHECK(interchange_load("/",allowed,interchange_read,interchange_capacity,&got)==ESP_OK&&got==33);
        CHECK(interchange_load("/",ssid?"wrong.JSON":"wrong.TXT",interchange_read,interchange_capacity,&got)==ESP_ERR_INVALID_ARG);
        clean();
    }
}
