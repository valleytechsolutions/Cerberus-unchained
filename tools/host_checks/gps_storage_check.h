static void gps_storage_checks(void)
{
    const uint64_t job=UINT64_C(0x100000007);
    const char row[]="<wpt lat=\"12\" lon=\"34\"><time>2026-11-08T00:00:00.000Z</time></wpt>\n";
    char name[16],temp[128],final[128],content[1024]; uint64_t bytes; struct stat item;
    assert(!sd_gps&&!wardrive_storage_busy());
    assert(cerberus_storage_gps_begin(0,CERBERUS_GPX_POINTS)==ESP_ERR_INVALID_ARG);
    assert(cerberus_storage_gps_begin(job,(cerberus_gpx_mode_t)99)==ESP_ERR_INVALID_ARG);
    fail_allocation=true;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_ERR_NO_MEM&&!sd_gps&&!locked);
    fail_allocation=false;
    mutex_timeout=true;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_ERR_TIMEOUT&&!sd_gps);
    mutex_timeout=false;
    sd_recon.active=true;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_ERR_INVALID_STATE);
    sd_recon.active=false;sd_ward_log_lease=3;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_ERR_INVALID_STATE);
    sd_ward_log_lease=0;sd_packet_count=(void *)1;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_ERR_INVALID_STATE);
    sd_packet_count=NULL;
    assert(cerberus_storage_reserve_for_usb()==ESP_OK);
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_ERR_INVALID_STATE);
    storage_owner=CERBERUS_STORAGE_OWNER_FIRMWARE;sd_mounted=true;

    /* Both directions of shared admission work. USB stays excluded until the
     * last writer closes, irrespective of which recording started first. */
    for(unsigned order=0;order<2;++order){
        if(order)assert(cerberus_storage_wardrive_session_begin()==ESP_OK);
        assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_OK);
        if(!order)assert(cerberus_storage_wardrive_session_begin()==ESP_OK);
        assert(cerberus_storage_reserve_for_usb()==ESP_ERR_INVALID_STATE);
        assert(cerberus_storage_gps_begin(job+1,CERBERUS_GPX_POINTS)==ESP_ERR_INVALID_STATE);
        assert(cerberus_storage_gps_append(job+1,row,sizeof(row)-1)==ESP_ERR_INVALID_STATE);
        assert(cerberus_storage_gps_finish(job+1,true,name,&bytes)==ESP_ERR_INVALID_STATE&&sd_gps);
        unsigned before=write_calls;
        mutex_timeout=true;
        assert(cerberus_storage_gps_append(job,row,sizeof(row)-1)==ESP_ERR_TIMEOUT&&write_calls==before);
        assert(cerberus_storage_gps_finish(job,true,name,&bytes)==ESP_ERR_TIMEOUT&&sd_gps);
        mutex_timeout=false;
        assert(cerberus_storage_gps_append(job,row,sizeof(row)-1)==ESP_OK);
        strcpy(temp,sd_gps->temp);strcpy(final,sd_gps->final);
        assert(cerberus_storage_gps_finish(job,true,name,&bytes)==ESP_OK&&name[0]&&!sd_gps);
        assert(stat(temp,&item)!=0&&stat(final,&item)==0&&(uint64_t)item.st_size==bytes);
        FILE *saved=fopen(final,"rb");assert(saved&&bytes<sizeof(content));
        assert(fread(content,1,(size_t)bytes,saved)==bytes&&!fclose(saved));content[bytes]=0;
        assert(strstr(content,row)&&strstr(content,"</gpx>\n"));
        assert(cerberus_storage_reserve_for_usb()==ESP_ERR_INVALID_STATE);
        assert(cerberus_storage_wardrive_session_finish()==ESP_OK);
        assert(cerberus_storage_reserve_for_usb()==ESP_OK);
        storage_owner=CERBERUS_STORAGE_OWNER_FIRMWARE;sd_mounted=true;
    }
    /* Existing output and interrupted TMPs are never truncated. */
    sd_gps_next_index=1;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_OK);
    assert(strcmp(sd_gps->name,"GPS-P00003.GPX")==0);
    strcpy(temp,sd_gps->temp);strcpy(final,sd_gps->final);
    assert(cerberus_storage_gps_finish(job,false,name,&bytes)==ESP_OK&&!name[0]);
    assert(stat(temp,&item)!=0&&stat(final,&item)!=0);
    sd_gps_next_index=99999;
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_TRACK)==ESP_OK);
    assert(strcmp(sd_gps->name,"GPS-T99999.GPX")==0&&sd_gps_next_index==1);
    assert(cerberus_storage_gps_finish(job,false,name,&bytes)==ESP_OK);

    /* Every failed finalization leaves recoverable TMP data, no final GPX,
     * no leaked lease and no successful name. Later recordings still work. */
    for(unsigned fault=0;fault<8;++fault){
        assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_OK);
        strcpy(temp,sd_gps->temp);strcpy(final,sd_gps->final);
        if(fault==0)partial_next_write=true;
        if(fault==1){test_now_us+=3000000;fail_next_sync=true;}
        if(fault==6)sd_gps->bytes=CERBERUS_STORAGE_WARDRIVE_MAX_BYTES;
        esp_err_t appended=cerberus_storage_gps_append(job,row,sizeof(row)-1);
        if(fault==0||fault==1||fault==6){
            assert(appended!=ESP_OK);unsigned before=write_calls;
            assert(cerberus_storage_gps_append(job,row,sizeof(row)-1)!=ESP_OK&&write_calls==before);
        }else assert(appended==ESP_OK);
        if(fault==2)fail_flush_at=flush_calls+1;
        if(fault==3)fail_close_at=close_calls+1;
        if(fault==4)fail_rename=true;
        if(fault==5)fail_write_at=write_calls+1;
        if(fault==7){FILE *occupied=fopen(final,"wb");assert(occupied);assert(fwrite("keep",1,4,occupied)==4&&!fclose(occupied));}
        assert(cerberus_storage_gps_finish(job,true,name,&bytes)!=ESP_OK&&!name[0]&&!sd_gps&&!locked);
        assert(stat(temp,&item)==0);
        if(fault!=7)assert(stat(final,&item)!=0);
        else {assert(stat(final,&item)==0&&item.st_size==4);}
        fail_flush_at=fail_close_at=fail_write_at=0;fail_rename=false;
    }
    assert(cerberus_storage_gps_begin(job,CERBERUS_GPX_POINTS)==ESP_OK);
    assert(cerberus_storage_gps_append(job,row,sizeof(row)-1)==ESP_OK);
    assert(cerberus_storage_gps_finish(job,true,name,&bytes)==ESP_OK&&name[0]);
    assert(!wardrive_storage_busy()&&!locked);
}
