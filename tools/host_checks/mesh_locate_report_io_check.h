/* Actual controller/parser/CSV -> production async worker with fake SD/RTOS. */
#include "mesh_locate_fixture.h"
static mesh_locate_t locate_input,locate_expected;
static char locate_expected_bytes[MESH_LOCATE_REPORT_MAX_BYTES+1U];
static size_t locate_expected_length;
static void fill_locate(unsigned count)
{
    CHECK(mesh_locate_test_fill_options(&locate_input,count,3U,ESP_OK));
    locate_expected=locate_input;
    CHECK(mesh_locate_report_encode(&locate_expected,locate_expected_bytes,
        sizeof(locate_expected_bytes),&locate_expected_length));
}
static uint64_t start_locate(void)
{
    uint64_t id=0;
    CHECK(cerberus_ble_archive_io_save_mesh_locate(&locate_input,&id)==ESP_OK && id>last_job);
    last_job=id;
    CHECK(status().operation==CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE &&
        status().format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT);
    CHECK(!strcmp(status().relative_path,"/CERBERUS/LOGS"));
    if(!immediate_task) {
        CHECK(allocation_size==offsetof(ble_archive_io_context_t,payload)+sizeof(archive_io_context->payload.locate));
        CHECK(!memcmp(&archive_io_context->payload.locate.snapshot,&locate_expected,sizeof(locate_expected)));
    }
    return id;
}
static void check_locate_operations(void)
{
    const unsigned counts[]={0,1,32,40};
    for(size_t i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
        reset();fill_locate(counts[i]);uint64_t id=start_locate();
        CHECK(!mesh_saves && !locate_formats && !mesh_formats);check_probe_no_typed_copy(id);
        memset(&locate_input,0xA5,sizeof(locate_input));run_worker();
        CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status().result_ready);
        CHECK(status().count==locate_expected.history_count && status().bytes==locate_expected_length);
        CHECK(mesh_saves==1U && locate_formats==1U && !mesh_formats && mesh_length==locate_expected_length);
        CHECK(!memcmp(mesh_bytes,locate_expected_bytes,mesh_length));
        CHECK(!strcmp(status().file_name,"M00001.CSV"));
        CHECK(!saves && !loads && !json_saves && !json_loads && !wifi_saves && !wifi_loads &&
            !ap_json_saves && !ap_json_loads && !ssid_text_saves && !ssid_text_loads && !probe_saves);
        check_probe_no_typed_copy(id);console_check(CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
        CHECK(strstr(console_text,"ble_archive_format=MESH_LOCATE_REPORT\r\n"));
    }
    for(unsigned phase=0;phase<3;++phase)for(unsigned release=0;release<2;++release) {
        reset();fill_locate(40);uint64_t id=start_locate();
        if(!phase){if(release)cerberus_ble_archive_io_release(id);else cerberus_ble_archive_io_cancel(id);}
        else if(phase==1)locate_format_hook=release?release_hook:cancel_hook;
        else storage_hook=release?release_hook:cancel_hook;
        run_worker();CHECK(status().cancel_requested);
        CHECK(status().state==(phase==2?CERBERUS_BLE_ARCHIVE_IO_COMPLETE:CERBERUS_BLE_ARCHIVE_IO_CANCELED));
        CHECK(mesh_saves==(phase==2?1U:0U) && locate_formats==(phase==0?0U:1U));
        CHECK(status().result_ready==(phase==2 && !release));
        cerberus_ble_archive_io_release(id);CHECK(!outstanding);
    }
    for(unsigned failure=0;failure<5;++failure) {
        reset();fill_locate(1);
        if(failure==0)free_internal=24575;
        if(failure==1)largest_internal=8191;
        if(failure==2)fail_alloc=true;
        if(failure==3)fail_task=true;
        if(failure==4){release_in_alloc=true;immediate_task=true;}
        uint64_t id=0;esp_err_t result=cerberus_ble_archive_io_save_mesh_locate(&locate_input,&id);
        CHECK(id>last_job);last_job=id;
        CHECK(result==(failure==4?ESP_OK:ESP_ERR_NO_MEM));
        CHECK(status().state==(failure==4?CERBERUS_BLE_ARCHIVE_IO_CANCELED:CERBERUS_BLE_ARCHIVE_IO_ERROR));
        CHECK(!pending_task && !outstanding && !mesh_saves);
    }
    reset();fill_locate(32);storage_error=ESP_FAIL;partial_save=true;
    uint64_t id=start_locate();run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR && status().error==ESP_FAIL &&
        !status().bytes && !status().result_ready && !outstanding);
    CHECK(!strcmp(status().file_name,"M00001.TMP"));check_probe_no_typed_copy(id);
    reset();fill_locate(1);id=start_locate();
    archive_io_context->payload.locate.snapshot.frozen=false;run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_ERROR && status().error==ESP_ERR_INVALID_RESPONSE && !mesh_saves);
    reset();fill_locate(1);locate_input.final_valid=false;
    unsigned allocations_before=allocations;uint64_t job_before=archive_io_last_job;id=99;
    CHECK(cerberus_ble_archive_io_save_mesh_locate(&locate_input,&id)==ESP_ERR_INVALID_ARG && !id);
    CHECK(allocations==allocations_before && archive_io_last_job==job_before && !pending_task && !outstanding);
    CHECK(cerberus_ble_archive_io_save_mesh_locate(NULL,&id)==ESP_ERR_INVALID_ARG && !id);
    CHECK(cerberus_ble_archive_io_save_mesh_locate(&locate_input,NULL)==ESP_ERR_INVALID_ARG);
    reset();fill_locate(1);id=start_locate();uint64_t stale=id;run_worker();cerberus_ble_archive_io_release(id);
    id=start_locate();cerberus_ble_archive_io_cancel(stale);cerberus_ble_archive_io_release(stale);
    CHECK(!status().cancel_requested && outstanding==1);run_worker();
    CHECK(status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    reset();fill_locate(0);immediate_task=true;id=start_locate();
    CHECK(!pending_task && status().state==CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    reset();fill_locate(40);
    printf("Mesh Locate worker allocation=%zu; controller=%zu; CSV cap=%u plus NUL.\n",
        archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE),sizeof(locate_input),MESH_LOCATE_REPORT_MAX_BYTES);
}
