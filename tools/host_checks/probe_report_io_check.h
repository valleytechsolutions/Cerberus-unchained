/* Actual parser/table -> frozen model -> production worker -> storage boundary.
 * The separate filesystem fixture executes the real transactional writer. */
static cerberus_probe_sniff_snapshot_t probe_input,probe_expected;
static cerberus_probe_sniff_table_t probe_table;
static char probe_expected_bytes[CERBERUS_PROBE_SNIFF_REPORT_MAX_BYTES+1U];
static size_t probe_expected_length;
static void fill_probe(unsigned count)
{
    memset(&probe_input,0,sizeof(probe_input));
    cerberus_probe_sniff_table_begin(&probe_table,UINT64_C(0x1234567800000001),1000U);
    for(unsigned i=0;i<count;++i){
        uint8_t frame[58]={0x40,0x08};
        frame[10]=2;frame[15]=(uint8_t)i;frame[22]=0xF0;frame[23]=0xFF;
        frame[24]=0;frame[25]=(uint8_t)(i==0?0:32);
        for(unsigned j=0;j<32;++j)frame[26+j]=(uint8_t)(j*7U); /* includes NUL, LF and binary bytes */
        cerberus_probe_sniff_row_t row;
        CHECK(cerberus_probe_sniff_decode(frame,26U+frame[25],177,-127,1100U+i,&row)==CERBERUS_PROBE_SNIFF_DECODE_VALID);
        CHECK(cerberus_probe_sniff_table_commit(&probe_table,probe_table.session_id,&row,1100U+i));
    }
    cerberus_probe_sniff_table_finish(&probe_table,probe_table.session_id,2000);
    CHECK(cerberus_probe_sniff_table_copy_summary(&probe_table,probe_table.session_id,3000,&probe_input.summary));
    for(unsigned i=0;i<count;++i)
        CHECK(cerberus_probe_sniff_table_copy_row(&probe_table,probe_table.session_id,(uint8_t)i,3000,&probe_input.rows[i]));
    probe_input.control=(cerberus_probe_sniff_control_t){.session_id=probe_table.session_id,
        .revision=probe_input.summary.revision,.state=CERBERUS_PROBE_SNIFF_COMPLETE,
        .flags=CERBERUS_PROBE_SNIFF_BOUND|CERBERUS_PROBE_SNIFF_DATA_READY,
        .count=(uint8_t)count,.mode=CERBERUS_PROBE_SNIFF_HOP_BOTH,.capacity=32};
    probe_input.count=(uint8_t)count;probe_input.frozen=true;
    CHECK(cerberus_probe_sniff_snapshot_valid(&probe_input));probe_expected=probe_input;
    CHECK(cerberus_probe_sniff_report(&probe_expected,probe_expected_bytes,sizeof(probe_expected_bytes),&probe_expected_length));
    CHECK(probe_expected_length>0&&probe_expected_length<=CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES);
}
static uint64_t start_probe(void)
{
    uint64_t id=0;CHECK(cerberus_ble_archive_io_save_probe(&probe_input,&id)==ESP_OK&&id>last_job);last_job=id;
    CHECK(status().operation==CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE&&status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT);
    CHECK(!strcmp(status().relative_path,"/CERBERUS/LOGS"));
    CHECK(allocation_size==offsetof(ble_archive_io_context_t,payload)+sizeof(archive_io_context->payload.probe));
    CHECK(sizeof(archive_io_context->payload.probe.bytes)==8193);
    CHECK(!memcmp(&archive_io_context->payload.probe.snapshot,&probe_expected,sizeof(probe_expected)));
    return id;
}
static void check_probe_no_typed_copy(uint64_t id)
{
    unsigned before=document_copies;
    CHECK(cerberus_ble_archive_io_copy_result(id,&output)==ESP_ERR_INVALID_STATE);
    CHECK(!memcmp(&output,&untouched,sizeof(output)));
    CHECK(cerberus_ble_archive_io_copy_airtag_json(id,&json_output)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ble_archive_io_copy_wifi(id,&wifi_output)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ble_archive_io_copy_ap_json(id,&ap_output)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ble_archive_io_copy_ssid_text(id,&ssid_output)==ESP_ERR_INVALID_STATE);
    CHECK(document_copies==before);
}
static void check_probe_operations(void)
{
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE)==57648);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON)==58760);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI)==11176);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON)==75784);
    CHECK(archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT)==8080);
    for(unsigned count=0;count<=32;++count){
        reset();fill_probe(count);uint64_t id=start_probe();
        CHECK(!probe_saves&&!probe_formats);check_probe_no_typed_copy(id);
        memset(&probe_input,0xA5,sizeof(probe_input));run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&status().result_ready&&status().count==count);
        CHECK(probe_saves==1&&probe_formats==1&&status().bytes==probe_expected_length&&probe_length==probe_expected_length);
        CHECK(!memcmp(probe_bytes,probe_expected_bytes,probe_length)&&!strcmp(status().file_name,"P00001.CSV"));
        CHECK(strstr((char*)probe_bytes,"not unique devices or raw frames")!=NULL);
        CHECK(!saves&&!loads&&!json_saves&&!json_loads&&!wifi_saves&&!wifi_loads&&!ap_json_saves&&!ap_json_loads&&!ssid_text_saves&&!ssid_text_loads);
        check_probe_no_typed_copy(id);console_check(CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
        CHECK(strstr(console_text,"ble_archive_format=PROBE_REPORT\r\n"));
        CHECK(!strstr(console_text,"ble_archive_wifi_codec_result=")&&!strstr(console_text,"ble_archive_json_eligible="));
    }
    /* Widest valid numerical fields plus all 32 binary names exercise the
     * formatter's payload bound, terminal NUL and exact allocation canary. */
    reset();fill_probe(32);
    for(unsigned i=0;i<32;++i){
        cerberus_probe_sniff_row_t *r=&probe_input.rows[i].row;
        r->ssid_length=32;memset(r->ssid,0xFF,sizeof(r->ssid));
        r->frames=r->retry_frames=UINT32_MAX;r->flags|=CERBERUS_PROBE_SNIFF_ROW_SATURATED;
        r->first_callback_us=r->last_callback_us=INT64_MAX;
        probe_input.rows[i].copied_source_us=INT64_MAX;
    }
    probe_input.control.session_id=UINT64_MAX;probe_input.control.error_code=INT32_MIN;
    probe_input.control.state=CERBERUS_PROBE_SNIFF_ERROR;
    probe_input.control.flags|=CERBERUS_PROBE_SNIFF_SATURATED|CERBERUS_PROBE_SNIFF_OVERFLOW;
    cerberus_probe_sniff_summary_t *m=&probe_input.summary;
    m->started_source_us=m->stopped_source_us=m->copied_source_us=INT64_MAX;
    m->valid_frames=m->retained_frames=m->retry_frames=m->capacity_dropped_frames=UINT32_MAX;
    m->malformed_frames=m->unsupported_frames=m->clock_dropped_frames=m->channel_failures=UINT32_MAX;
    m->revision=probe_input.control.revision=UINT32_MAX;
    m->flags|=CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED|CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED;
    m->attempted_channels=m->successful_channels=(UINT64_C(1)<<CERBERUS_PROBE_SNIFF_CHANNEL_COUNT)-1U;
    CHECK(cerberus_probe_sniff_snapshot_valid(&probe_input));probe_expected=probe_input;
    CHECK(cerberus_probe_sniff_report(&probe_expected,probe_expected_bytes,sizeof(probe_expected_bytes),&probe_expected_length));
    CHECK(probe_expected_length<=8192);uint64_t widest_id=start_probe();run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE&&probe_length==probe_expected_length);
    CHECK(!memcmp(probe_bytes,probe_expected_bytes,probe_length));
    CHECK(strstr((char*)probe_bytes,"source_error,-2147483648\r\n"));
    cerberus_ble_archive_io_release(widest_id);CHECK(!outstanding);
    reset();fill_probe(1);probe_input.control.mode=CERBERUS_PROBE_SNIFF_FIXED;
    probe_input.requested_channel=177;probe_expected=probe_input;
    CHECK(probe_input.control.channel==0&&cerberus_probe_sniff_snapshot_valid(&probe_input));
    uint64_t fixed_id=start_probe();probe_input.requested_channel=1;run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    CHECK(strstr((char*)probe_bytes,"requested_channel,177\r\n"));
    cerberus_ble_archive_io_release(fixed_id);
    for(unsigned phase=0;phase<3;++phase)for(unsigned release=0;release<2;++release){
        reset();fill_probe(32);uint64_t id=start_probe();
        if(phase==0){if(release)cerberus_ble_archive_io_release(id);else cerberus_ble_archive_io_cancel(id);}
        else if(phase==1)probe_format_hook=release?release_hook:cancel_hook;
        else storage_hook=release?release_hook:cancel_hook;
        run_worker();CHECK(status().cancel_requested);
        CHECK(status().state==(phase==2?CERBERUS_BLE_ARCHIVE_IO_COMPLETE:CERBERUS_BLE_ARCHIVE_IO_CANCELED));
        CHECK(probe_saves==(phase==2?1U:0U)&&probe_formats==(phase==0?0U:1U));
        CHECK(status().result_ready==(phase==2&&!release));
        cerberus_ble_archive_io_release(id);CHECK(!outstanding);
    }
    for(unsigned failure=0;failure<5;++failure){
        reset();fill_probe(1);
        if(failure==0)free_internal=24575;
        if(failure==1)largest_internal=8191;
        if(failure==2)fail_alloc=true;
        if(failure==3)fail_task=true;
        if(failure==4){release_in_alloc=true;immediate_task=true;}
        uint64_t id=0;esp_err_t e=cerberus_ble_archive_io_save_probe(&probe_input,&id);
        CHECK(id>last_job);last_job=id;
        CHECK(e==(failure==4?ESP_OK:ESP_ERR_NO_MEM));
        CHECK(status().state==(failure==4?CERBERUS_BLE_ARCHIVE_IO_CANCELED:CERBERUS_BLE_ARCHIVE_IO_ERROR));
        CHECK(!pending_task&&!outstanding&&!probe_saves);
    }
    for(unsigned cancel=0;cancel<2;++cancel){
        reset();fill_probe(32);storage_error=ESP_FAIL;partial_save=true;
        if(cancel)storage_hook=cancel_hook;
        uint64_t id=start_probe();run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR&&status().error==ESP_FAIL);
        CHECK(status().bytes==0&&!status().count&&!status().result_ready&&!outstanding);
        CHECK(probe_saves==1&&!strcmp(status().file_name,"P00001.TMP"));check_probe_no_typed_copy(id);
    }
    /* Malformed/unfenced sources reject before allocating or consuming a job. */
    for(unsigned fault=0;fault<7;++fault){
        reset();fill_probe(1);
        if(fault==0)probe_input.frozen=false;
        if(fault==1)probe_input.count=33;
        if(fault==2)++probe_input.control.revision;
        if(fault==3)probe_input.control.flags|=CERBERUS_PROBE_SNIFF_OWNED;
        if(fault==4)probe_input.rows[0].row.ssid_length=33;
        if(fault==5)++probe_input.summary.retained_frames;
        if(fault==6)probe_input.summary.flags&=~CERBERUS_PROBE_SNIFF_STOPPED;
        unsigned before=allocations;uint64_t prior=archive_io_last_job,id=77;
        CHECK(cerberus_ble_archive_io_save_probe(&probe_input,&id)==ESP_ERR_INVALID_ARG&&id==0);
        CHECK(allocations==before&&archive_io_last_job==prior&&!pending_task&&!probe_saves);
    }
    reset();fill_probe(1);uint64_t id=77;
    CHECK(cerberus_ble_archive_io_save_probe(NULL,&id)==ESP_ERR_INVALID_ARG&&id==0);
    CHECK(cerberus_ble_archive_io_save_probe(&probe_input,NULL)==ESP_ERR_INVALID_ARG);
    id=start_probe();uint64_t stale=id;run_worker();cerberus_ble_archive_io_release(id);
    id=start_probe();cerberus_ble_archive_io_cancel(stale);cerberus_ble_archive_io_release(stale);
    CHECK(!status().cancel_requested&&outstanding==1);run_worker();CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    reset();fill_probe(0);immediate_task=true;id=start_probe();CHECK(!pending_task&&status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    reset();fill_probe(32); /* valid input for the following global exhaustion test */
    printf("Probe report worker allocation=%zu; snapshot=%zu; bounded CSV payload=%u plus NUL.\n",
        archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE),sizeof(probe_input),CERBERUS_PROBE_SNIFF_REPORT_MAX_BYTES);
}
