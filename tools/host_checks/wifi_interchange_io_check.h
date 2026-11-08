/* Production worker plus real codecs. Only filesystem/RTOS boundaries are
 * substituted; the allocation canary also checks the selected union arm. */
static cerberus_wifi_marauder_ap_t ap_input,ap_expected,ap_output,ap_untouched,ap_nested;
static cerberus_wifi_marauder_ssid_t ssid_input,ssid_expected,ssid_output,ssid_untouched,ssid_nested;
static bool interchange_ssid;
static void fill_interchange(unsigned count)
{
    memset(&ap_input,0,sizeof(ap_input));memset(&ssid_input,0,sizeof(ssid_input));
    ap_input.count=ssid_input.count=(uint8_t)count;ap_input.station_count=(uint16_t)(count*4);
    ssid_input.final_lf=count!=0;
    for(unsigned i=0;i<count;++i){
        cerberus_wifi_marauder_ap_row_t *r=&ap_input.rows[i];
        r->present=CERBERUS_WIFI_MARAUDER_AP_ALL;r->packets=UINT32_MAX;r->rssi=INT32_MIN;
        r->channel=r->security=255;r->wps=true;r->essid_length=32;r->man_length=64;
        memset(r->essid,1,sizeof(r->essid));memset(r->man,2,sizeof(r->man));
        r->bssid[0]=2;r->bssid[5]=(uint8_t)(i%32);r->station_offset=(uint16_t)(i*4);r->station_count=4;
        for(unsigned j=0;j<4;++j){ap_input.stations[i*4+j][0]=2;ap_input.stations[i*4+j][5]=(uint8_t)j;}
        ssid_input.rows[i].length=32;memset(ssid_input.rows[i].bytes,(int)('A'+i%20),32);
        ssid_input.rows[i].bytes[0]=0;ssid_input.rows[i].bytes[1]='\r';ssid_input.rows[i].bytes[2]=0xff;
    }
    CHECK(cerberus_wifi_marauder_ap_validate(&ap_input)==CERBERUS_WIFI_MARAUDER_OK);
    CHECK(cerberus_wifi_marauder_ssid_validate(&ssid_input)==CERBERUS_WIFI_MARAUDER_OK);
    ap_expected=ap_input;ssid_expected=ssid_input;
    memset(&ap_output,0xA5,sizeof(ap_output));ap_untouched=ap_output;
    memset(&ssid_output,0xA5,sizeof(ssid_output));ssid_untouched=ssid_output;
}
static void seed_interchange(void)
{
    CHECK(cerberus_wifi_marauder_ap_encode(&ap_input,ap_json_bytes,sizeof(ap_json_bytes),&ap_json_length)==CERBERUS_WIFI_MARAUDER_OK);
    CHECK(cerberus_wifi_marauder_ssid_encode(&ssid_input,ssid_text_bytes,sizeof(ssid_text_bytes),&ssid_text_length)==CERBERUS_WIFI_MARAUDER_OK);
}
static uint64_t start_interchange(unsigned operation)
{
    uint64_t id=0;esp_err_t e;
    if(interchange_ssid)e=operation==0?cerberus_ble_archive_io_load_ssid_text("/COPIED","SSIDs_0.log",&id):
        operation==1?cerberus_ble_archive_io_save_ssid_text(&ssid_input,&id):cerberus_ble_archive_io_export_ssid_text(&wifi_input,&id);
    else e=operation==0?cerberus_ble_archive_io_load_ap_json("/COPIED","APs_0.log",&id):
        operation==1?cerberus_ble_archive_io_save_ap_json(&ap_input,&id):cerberus_ble_archive_io_export_ap_json(&wifi_input,&id);
    CHECK(e==ESP_OK&&id>last_job);last_job=id;
    CHECK(status().format==(interchange_ssid?CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT:CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON));
    CHECK(allocation_size==archive_io_allocation_size(status().operation));return id;
}
static esp_err_t copy_interchange(uint64_t id)
{return interchange_ssid?cerberus_ble_archive_io_copy_ssid_text(id,&ssid_output):cerberus_ble_archive_io_copy_ap_json(id,&ap_output);}
static void check_interchange_result(void)
{
    if(interchange_ssid)CHECK(!memcmp(&ssid_output,&ssid_expected,sizeof(ssid_output)));
    else CHECK(!memcmp(&ap_output,&ap_expected,sizeof(ap_output)));
}
static void check_interchange_unpublished(uint64_t id)
{
    CHECK(copy_interchange(id)==ESP_ERR_INVALID_STATE);
    if(interchange_ssid)CHECK(!memcmp(&ssid_output,&ssid_untouched,sizeof(ssid_output)));
    else CHECK(!memcmp(&ap_output,&ap_untouched,sizeof(ap_output)));
}
static void nested_interchange_copy(void)
{
    CHECK(archive_io_readers==1);copy_hook=release_during_copy;
    if(interchange_ssid){CHECK(cerberus_ble_archive_io_copy_ssid_text(status().job_id,&ssid_nested)==ESP_OK);CHECK(!memcmp(&ssid_nested,&ssid_expected,sizeof(ssid_nested)));}
    else {CHECK(cerberus_ble_archive_io_copy_ap_json(status().job_id,&ap_nested)==ESP_OK);CHECK(!memcmp(&ap_nested,&ap_expected,sizeof(ap_nested)));}
    CHECK(archive_io_readers==1&&outstanding==1);
}
static void check_wifi_interchange_operations(void)
{
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE)==57648);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON)==58760);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI)==11176);
    CHECK(CERBERUS_STORAGE_AP_JSON_MAX_BYTES==CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES);
    CHECK(CERBERUS_STORAGE_SSID_TEXT_MAX_BYTES==CERBERUS_WIFI_MARAUDER_SSID_MAX_BYTES);
    for(unsigned format=0;format<2;++format){
        interchange_ssid=format!=0;
        for(unsigned count=0;count<=64;count=count==0?1:count==1?64:65){
            reset();fill_interchange(count);seed_interchange();uint64_t id=start_interchange(1);
            CHECK(!ap_json_saves&&!ssid_text_saves);check_interchange_unpublished(id);
            memset(&ap_input,0,sizeof(ap_input));memset(&ssid_input,0,sizeof(ssid_input));
            run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&status().count==count);
            CHECK(status().wifi_result==CERBERUS_WIFI_MARAUDER_OK&&!strcmp(status().relative_path,"/CERBERUS/WIFI"));
            CHECK(copy_interchange(id)==ESP_OK);check_interchange_result();
            if(format){CHECK(ssid_text_saves==1&&status().bytes==count*33);}
            else CHECK(ap_json_saves==1&&status().bytes>0);
            CHECK(!saves&&!loads&&!json_saves&&!json_loads&&!wifi_saves&&!wifi_loads);
            CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_ERR_INVALID_STATE);
            CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE);
            CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_ERR_INVALID_STATE);
            if(format)CHECK(cerberus_ble_archive_io_copy_ap_json(id,&ap_output)==ESP_ERR_INVALID_STATE);
            else CHECK(cerberus_ble_archive_io_copy_ssid_text(id,&ssid_output)==ESP_ERR_INVALID_STATE);
            cerberus_ble_archive_io_release(id);id=start_interchange(0);run_worker();
            CHECK(status().result_ready&&copy_interchange(id)==ESP_OK);check_interchange_result();
            CHECK(!strcmp(last_path,"/COPIED")&&!strcmp(last_name,format?"SSIDs_0.log":"APs_0.log"));
        }
        for(unsigned nested=0;nested<2;++nested){
            reset();fill_interchange(64);uint64_t id=start_interchange(1);run_worker();
            copy_hook=nested?nested_interchange_copy:release_during_copy;
            CHECK(copy_interchange(id)==ESP_OK);check_interchange_result();
            CHECK(!outstanding&&!archive_io_context&&!archive_io_readers);
        }
        for(unsigned operation=0;operation<3;++operation){
            for(unsigned release=0;release<2;++release){
                reset();fill_interchange(64);fill_wifi(64);seed_interchange();uint64_t id=start_interchange(operation);
                if(release)cerberus_ble_archive_io_release(id);else cerberus_ble_archive_io_cancel(id);
                run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_CANCELED&&!outstanding);
                CHECK(!ap_json_saves&&!ap_json_loads&&!ssid_text_saves&&!ssid_text_loads);check_interchange_unpublished(id);
            }
            for(unsigned release=0;release<2;++release){
                reset();fill_interchange(64);fill_wifi(64);seed_interchange();storage_hook=release?release_hook:cancel_hook;
                uint64_t id=start_interchange(operation);run_worker();
                CHECK(status().cancel_requested&&status().state==(operation?CERBERUS_BLE_ARCHIVE_IO_COMPLETE:CERBERUS_BLE_ARCHIVE_IO_CANCELED));
                CHECK(status().result_ready==(operation!=0&&!release));
                cerberus_ble_archive_io_release(id);CHECK(!outstanding);
            }
            for(unsigned failure=0;failure<5;++failure){
                reset();fill_interchange(1);fill_wifi(1);seed_interchange();
                if(failure==0)free_internal=24575;if(failure==1)largest_internal=8191;
                if(failure==2)fail_alloc=true;if(failure==3)fail_task=true;
                if(failure==4){release_in_alloc=true;immediate_task=true;}
                uint64_t id=0;esp_err_t e;
                if(format)e=operation==0?cerberus_ble_archive_io_load_ssid_text("/","x.TXT",&id):operation==1?
                    cerberus_ble_archive_io_save_ssid_text(&ssid_input,&id):cerberus_ble_archive_io_export_ssid_text(&wifi_input,&id);
                else e=operation==0?cerberus_ble_archive_io_load_ap_json("/","x.LOG",&id):operation==1?
                    cerberus_ble_archive_io_save_ap_json(&ap_input,&id):cerberus_ble_archive_io_export_ap_json(&wifi_input,&id);
                CHECK(e==(failure==4?ESP_OK:ESP_ERR_NO_MEM)&&id>last_job);last_job=id;
                CHECK(!outstanding&&!pending_task&&!ap_json_saves&&!ssid_text_saves&&!ap_json_loads&&!ssid_text_loads);
            }
        }
        reset();fill_wifi(64);fill_interchange(1);cerberus_wifi_marauder_export_stats_t expected_stats={0};
        CHECK((format?cerberus_wifi_marauder_ssid_from_archive(&wifi_input,&ssid_expected,&expected_stats):
            cerberus_wifi_marauder_ap_from_archive(&wifi_input,&ap_expected,&expected_stats))==CERBERUS_WIFI_MARAUDER_OK);
        uint64_t id=start_interchange(2);memset(&wifi_input,0xCC,sizeof(wifi_input));run_worker();
        CHECK(status().result_ready&&status().count==64&&copy_interchange(id)==ESP_OK);check_interchange_result();
        cerberus_ble_archive_io_status_t exported_status=status();
        CHECK(!memcmp(&exported_status.wifi_export_stats,&expected_stats,sizeof(expected_stats)));
        for(unsigned release=0;release<2;++release){
            reset();fill_wifi(64);fill_interchange(1);conversion_hook=release?release_hook:cancel_hook;
            id=start_interchange(2);run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_CANCELED);
            CHECK(!ap_json_saves&&!ssid_text_saves&&!outstanding);check_interchange_unpublished(id);
        }
        reset();fill_interchange(1);storage_error=ESP_FAIL;partial_save=true;storage_hook=cancel_hook;
        id=start_interchange(1);run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&status().error==ESP_FAIL);
        CHECK(!strcmp(status().file_name,format?"S00001.TMP":"P00001.TMP"));check_interchange_unpublished(id);
        reset();fill_interchange(1);seed_interchange();storage_error=ESP_ERR_TIMEOUT;
        id=start_interchange(0);run_worker();CHECK(status().error==ESP_ERR_TIMEOUT);check_interchange_unpublished(id);
        reset();fill_interchange(1);if(format)ssid_input.rows[0].bytes[0]='\n';else ap_input.rows[0].present=0xffff;
        id=start_interchange(1);run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&!ap_json_saves&&!ssid_text_saves);
        check_interchange_unpublished(id);
        reset();fill_interchange(1);seed_interchange();
        if(format){memset(ssid_text_bytes,'X',33);ssid_text_length=33;}else {ap_json_bytes[0]='!';}
        id=start_interchange(0);run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR);check_interchange_unpublished(id);
        reset();fill_wifi(1);fill_interchange(1);
        if(format)wifi_input.rows[0].ssid[0]='\n';else wifi_input.rows[0].ssid[0]=(char)0xff;
        id=start_interchange(2);run_worker();CHECK(status().error==ESP_ERR_NOT_SUPPORTED&&status().wifi_result==CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE);
        CHECK(!ap_json_saves&&!ssid_text_saves);check_interchange_unpublished(id);
        reset();fill_interchange(1);seed_interchange();id=start_interchange(0);run_worker();uint64_t old=id;
        cerberus_ble_archive_io_release(id);id=start_interchange(1);cerberus_ble_archive_io_cancel(old);cerberus_ble_archive_io_release(old);
        CHECK(!status().cancel_requested&&outstanding==1);run_worker();CHECK(status().result_ready&&copy_interchange(old)==ESP_ERR_INVALID_STATE);
        archive_io_readers=UINT16_MAX;CHECK(copy_interchange(id)==ESP_ERR_INVALID_STATE);archive_io_readers=0;
        reset();fill_interchange(0);immediate_task=true;id=start_interchange(1);CHECK(status().result_ready&&!pending_task);reset();
        uint64_t rejected=9;char badpath[512];memset(badpath,'X',sizeof(badpath));
        CHECK((format?cerberus_ble_archive_io_load_ssid_text(badpath,"x.LOG",&rejected):cerberus_ble_archive_io_load_ap_json(badpath,"x.LOG",&rejected))==ESP_ERR_INVALID_ARG&&rejected==0);
        CHECK((format?cerberus_ble_archive_io_save_ssid_text(NULL,&rejected):cerberus_ble_archive_io_save_ap_json(NULL,&rejected))==ESP_ERR_INVALID_ARG&&rejected==0);
        CHECK((format?cerberus_ble_archive_io_export_ssid_text(NULL,&rejected):cerberus_ble_archive_io_export_ap_json(NULL,&rejected))==ESP_ERR_INVALID_ARG&&rejected==0);
        CHECK((format?cerberus_ble_archive_io_copy_ssid_text(0,NULL):cerberus_ble_archive_io_copy_ap_json(0,NULL))==ESP_ERR_INVALID_ARG);
    }
    /* Real import -> typed copy -> Save Copy preserves SSID framing and opaque
     * bytes. No native Wi-Fi model or current source is consulted. */
    reset();interchange_ssid=true;fill_interchange(0);
    static const uint8_t framing[]={'a',0,'\r','\n','\n','s','a','m','e','\n','s','a','m','e'};
    memcpy(ssid_text_bytes,framing,sizeof(framing));ssid_text_length=sizeof(framing);
    uint64_t id=start_interchange(0);run_worker();CHECK(copy_interchange(id)==ESP_OK);
    CHECK(ssid_output.count==4&&!ssid_output.final_lf&&ssid_output.rows[0].length==3);
    CHECK(ssid_output.rows[0].bytes[1]==0&&ssid_output.rows[0].bytes[2]=='\r'&&ssid_output.rows[1].length==0);
    CHECK(!memcmp(&ssid_output.rows[2],&ssid_output.rows[3],sizeof(ssid_output.rows[2])));
    ssid_input=ssid_output;cerberus_ble_archive_io_release(id);id=start_interchange(1);run_worker();
    CHECK(status().result_ready&&ssid_text_length==sizeof(framing)&&!memcmp(ssid_text_bytes,framing,sizeof(framing)));

    /* Missing AP fields do not acquire default channel/security/RSSI. Unknown
     * extension members are counted on import, then deliberately not re-saved. */
    reset();interchange_ssid=false;fill_interchange(0);
    static const char ap_file[]="[{\"essid\":\"\",\"bssid\":\"02:00:00:00:00:01\",\"packets\":7,\"future\":{}},"
        "{\"essid\":\"\",\"bssid\":\"02:00:00:00:00:01\",\"packet\":7}]";
    memcpy(ap_json_bytes,ap_file,sizeof(ap_file)-1);ap_json_length=sizeof(ap_file)-1;
    id=start_interchange(0);run_worker();CHECK(copy_interchange(id)==ESP_OK);
    CHECK(ap_output.count==2&&ap_output.ignored_members==1&&ap_output.rows[0].packets==7);
    CHECK(ap_output.rows[0].present==(CERBERUS_WIFI_MARAUDER_AP_ESSID|CERBERUS_WIFI_MARAUDER_AP_BSSID|CERBERUS_WIFI_MARAUDER_AP_PACKETS));
    CHECK(!memcmp(&ap_output.rows[0],&ap_output.rows[1],sizeof(ap_output.rows[0])));
    ap_input=ap_output;cerberus_ble_archive_io_release(id);id=start_interchange(1);run_worker();
    CHECK(status().result_ready);cerberus_ble_archive_io_release(id);id=start_interchange(0);run_worker();
    CHECK(copy_interchange(id)==ESP_OK&&ap_output.count==2&&!ap_output.ignored_members);
    CHECK(ap_output.rows[0].present==ap_input.rows[0].present&&ap_output.rows[1].packets==7);

    reset();uint64_t prior=archive_io_last_job;archive_io_last_job=UINT64_MAX;
    CHECK(cerberus_ble_archive_io_load_ap_json("/","x.LOG",&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_save_ap_json(&ap_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_export_ap_json(&wifi_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_load_ssid_text("/","x.TXT",&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_save_ssid_text(&ssid_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(cerberus_ble_archive_io_export_ssid_text(&wifi_input,&id)==ESP_ERR_INVALID_STATE&&id==0);
    CHECK(!outstanding&&!pending_task);archive_io_last_job=prior;
    reset();
}
