#include "c5_spi_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static void *link_mutex=(void *)1;
static bool held,mutex_error;
static uint8_t sequence;
static uint32_t peer_firmware_version,reported_version=61;
static int64_t now_us=1000000,probe_us,request_us;
static unsigned probes,commands;
static esp_err_t probe_error,request_error;
static bool corrupt;
static uint8_t expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_INFO;
static int64_t esp_timer_get_time(void){return now_us;}
static int take_link_mutex(unsigned ticks){assert(ticks==5&&!held);if(mutex_error)return 0;held=true;return 1;}
static void give_link_mutex(void){assert(held);held=false;}
static esp_err_t get_status_v2_passive_locked(uint8_t seq,uint32_t deadline,cerberus_c5_status_t *out)
{(void)seq;assert(held&&deadline==1020);++probes;memset(out,0,sizeof(*out));out->firmware_version=reported_version;now_us+=probe_us;return probe_error;}
static esp_err_t spi_request_response_passive(const void *input,void *discard,void *output,size_t size,uint32_t deadline)
{
    (void)discard;assert(held&&deadline==1020&&size==sizeof(cerberus_spi_frame_t));++commands;
    const cerberus_spi_frame_t *request=input;assert(request->command==expected_command);
    cerberus_dual_ble_info_t info={.boot_id=123,.next_job_id=1,.version=1,.capacity=32,.raw_bytes=31,.flags=8};
    assert(cerberus_protocol_prepare_frame(output,request->command,request->sequence,&info,sizeof(info)));
    if(corrupt)((cerberus_spi_frame_t *)output)->sequence++;
    now_us+=request_us;return request_error;
}
#include "c5_dual_ble_link_impl.h"
static esp_err_t run(cerberus_dual_ble_info_t *out)
{return c5_spi_link_dual_ble_exchange(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,out,sizeof(*out));}
static void reset(void)
{assert(!held);peer_firmware_version=0;reported_version=61;probes=commands=0;probe_error=request_error=ESP_OK;
 probe_us=request_us=0;now_us=1000000;mutex_error=corrupt=false;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_INFO;}
static bool zero(const void *p,size_t n){const unsigned char *b=p;for(size_t i=0;i<n;++i)if(b[i])return false;return true;}
int main(void)
{
    cerberus_dual_ble_info_t out;
    reset();peer_firmware_version=61;assert(run(&out)==ESP_OK&&commands==1&&!probes&&out.boot_id==123);
    reset();assert(run(&out)==ESP_OK&&probes==1&&commands==1&&peer_firmware_version==61);
    reset();probe_error=ESP_ERR_TIMEOUT;memset(&out,0xa5,sizeof(out));
    assert(run(&out)==ESP_ERR_TIMEOUT&&probes==1&&!commands&&!peer_firmware_version&&zero(&out,sizeof(out)));
    probe_error=ESP_OK;assert(run(&out)==ESP_OK&&probes==2&&commands==1); /* Transient loss recovers without false unsupported. */
    reset();reported_version=60;assert(run(&out)==ESP_ERR_NOT_SUPPORTED&&probes==1&&!commands&&zero(&out,sizeof(out)));
    now_us=1000000;assert(run(&out)==ESP_ERR_NOT_SUPPORTED&&probes==1&&!commands); /* Known old remains blocked. */
    reset();probe_us=20000;assert(run(&out)==ESP_ERR_TIMEOUT&&probes==1&&!commands&&!peer_firmware_version);
    reset();probe_us=-1;assert(run(&out)==ESP_ERR_TIMEOUT&&!commands);
    reset();probe_us=10000;request_us=10001;assert(run(&out)==ESP_ERR_TIMEOUT&&commands==1&&zero(&out,sizeof(out)));
    reset();probe_us=5000;request_us=5000;assert(run(&out)==ESP_OK&&commands==1);
    reset();probe_error=ESP_ERR_INVALID_RESPONSE;assert(run(&out)==ESP_ERR_INVALID_RESPONSE&&!commands);
    reset();corrupt=true;assert(run(&out)==ESP_ERR_INVALID_RESPONSE&&zero(&out,sizeof(out)));
    reset();mutex_error=true;assert(run(&out)==ESP_ERR_TIMEOUT&&!probes&&!commands);
    reset();assert(c5_spi_link_dual_ble_exchange(1,NULL,0,&out,sizeof(out))==ESP_ERR_INVALID_ARG&&!probes&&!commands);
    /* v2 snapshot commands cannot be sent to a known v1 peer. Unknown peers
       still get exactly one bounded firmware probe before the command gate. */
    cerberus_dual_ble_snapshot_begin_t begin={.binding={123,1},.request_id=1};
    cerberus_dual_ble_snapshot_status_t snapshot;
    for(uint8_t c=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN;c<=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS;++c){
        reset();peer_firmware_version=61;expected_command=c;
        assert(c5_spi_link_dual_ble_exchange(c,&begin,sizeof(begin),&snapshot,sizeof(snapshot))==ESP_ERR_NOT_SUPPORTED);
        assert(!probes&&!commands&&zero(&snapshot,sizeof(snapshot)));
        reset();reported_version=62;expected_command=c;
        assert(c5_spi_link_dual_ble_exchange(c,&begin,sizeof(begin),&snapshot,sizeof(snapshot))==ESP_OK);
        assert(probes==1&&commands==1&&peer_firmware_version==62);
    }
    cerberus_dual_ble_snapshot_binding_t inventory_request={.binding={123,1},.handle=456};
    cerberus_dual_ble_snapshot_inventory_t inventory;
    reset();peer_firmware_version=62;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&inventory_request,sizeof(inventory_request),
        &inventory,sizeof(inventory))==ESP_ERR_NOT_SUPPORTED&&!probes&&!commands&&zero(&inventory,sizeof(inventory)));
    reset();reported_version=63;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&inventory_request,sizeof(inventory_request),
        &inventory,sizeof(inventory))==ESP_OK&&probes==1&&commands==1);
    /* The opt-in110-byte row must not be requested from a known63 peer.
     * Unknown cache still uses the same one-probe/shared-deadline contract. */
    cerberus_dual_ble_snapshot_read_t metadata_request={.binding={123,1},.handle=456,.index=2,.chunk=10};
    cerberus_dual_ble_snapshot_chunk_t metadata;
    reset();peer_firmware_version=63;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_NOT_SUPPORTED&&!probes&&!commands&&zero(&metadata,sizeof(metadata)));
    reset();reported_version=64;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_OK&&probes==1&&commands==1&&peer_firmware_version==64);
    reset();peer_firmware_version=64;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_OK&&!probes&&commands==1);
    reset();reported_version=64;probe_error=ESP_ERR_TIMEOUT;
    expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_TIMEOUT&&probes==1&&!commands&&!peer_firmware_version&&zero(&metadata,sizeof(metadata)));
    probe_error=ESP_OK;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_OK&&probes==2&&commands==1);
    reset();reported_version=64;probe_us=10000;request_us=10001;
    expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_TIMEOUT&&probes==1&&commands==1&&zero(&metadata,sizeof(metadata)));
    reset();peer_firmware_version=64;corrupt=true;
    expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_INVALID_RESPONSE&&!probes&&commands==1&&zero(&metadata,sizeof(metadata)));
    /* Analyzer START is gated before RF, not merely its later sample read. */
    cerberus_dual_ble_start_t analyzer_start={.boot_id=123,.job_id=1,.version=2,.mode=CERBERUS_DUAL_BLE_ANALYZER};
    cerberus_dual_ble_binding_t analyzer_binding={123,1};
    for(unsigned start=0;start<2;++start){
        uint8_t command=start?CERBERUS_SPI_COMMAND_DUAL_BLE_START:CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE;
        const void *request=start?(const void*)&analyzer_start:(const void*)&analyzer_binding;
        size_t length=start?sizeof(analyzer_start):sizeof(analyzer_binding);
        reset();peer_firmware_version=64;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
        assert(!probes&&!commands&&zero(&out,sizeof(out)));
        reset();reported_version=64;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
        assert(probes==1&&!commands&&peer_firmware_version==64);
        reset();reported_version=65;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_OK);
        assert(probes==1&&commands==1&&peer_firmware_version==65);
        reset();reported_version=65;probe_error=ESP_ERR_TIMEOUT;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_TIMEOUT);
        assert(probes==1&&!commands&&zero(&out,sizeof(out)));
        reset();reported_version=65;probe_us=10000;request_us=10001;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_TIMEOUT);
        assert(probes==1&&commands==1&&zero(&out,sizeof(out)));
    }
    reset();peer_firmware_version=65;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_START;
    uint8_t unaligned[sizeof(analyzer_start)+1];memcpy(unaligned+1,&analyzer_start,sizeof(analyzer_start));
    assert(c5_spi_link_dual_ble_exchange(expected_command,unaligned+1,sizeof(analyzer_start),&out,sizeof(out))==ESP_OK&&commands==1);
    reset();peer_firmware_version=64;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_START;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&analyzer_start,sizeof(analyzer_start)-1,&out,sizeof(out))==ESP_ERR_INVALID_ARG&&!commands);
    analyzer_start.version=1;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&analyzer_start,sizeof(analyzer_start),&out,sizeof(out))==ESP_ERR_INVALID_ARG&&!commands);
    analyzer_start.mode=CERBERUS_DUAL_BLE_DISCOVERY;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&analyzer_start,sizeof(analyzer_start),&out,sizeof(out))==ESP_OK&&commands==1);
    /* FAMILY is a new admission policy: both START and READ_FAMILY require66,
     * and an invalid selector must never be sent even to a supported peer. */
    cerberus_dual_ble_start_t family_start={.boot_id=123,.job_id=1,.version=2,
        .mode=CERBERUS_DUAL_BLE_FAMILY,.reserved=1};
    for(unsigned start=0;start<2;++start){
        uint8_t command=start?CERBERUS_SPI_COMMAND_DUAL_BLE_START:CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY;
        const void *request=start?(const void*)&family_start:(const void*)&metadata_request;
        size_t length=start?sizeof(family_start):sizeof(metadata_request);
        reset();peer_firmware_version=65;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
        assert(!probes&&!commands&&zero(&out,sizeof(out)));
        reset();reported_version=65;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
        assert(probes==1&&!commands&&peer_firmware_version==65);
        reset();reported_version=66;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_OK);
        assert(probes==1&&commands==1&&peer_firmware_version==66);
        reset();reported_version=66;probe_error=ESP_ERR_TIMEOUT;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_TIMEOUT);
        assert(probes==1&&!commands&&zero(&out,sizeof(out)));
        reset();reported_version=66;probe_us=10000;request_us=10001;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_TIMEOUT);
        assert(probes==1&&commands==1&&zero(&out,sizeof(out)));
    }
    for(unsigned family=0;family<=7;++family){
        reset();peer_firmware_version=66;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_START;
        family_start.reserved=(uint8_t)family;
        esp_err_t result=c5_spi_link_dual_ble_exchange(expected_command,&family_start,sizeof(family_start),&out,sizeof(out));
        assert(result==((family>=1&&family<=6)?ESP_OK:ESP_ERR_INVALID_ARG));
        assert(commands==((family>=1&&family<=6)?1U:0U));
    }
    family_start.reserved=1;family_start.version=1;reset();peer_firmware_version=66;
    assert(c5_spi_link_dual_ble_exchange(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&family_start,
        sizeof(family_start),&out,sizeof(out))==ESP_ERR_INVALID_ARG&&!commands);
    /* Solicited response acquisition is opt-in, and its START must be gated
     * before RF on every older firmware, not merely the response read. */
    for(unsigned variant=0;variant<3;++variant){
        cerberus_dual_ble_start_t response_start={.boot_id=123,.job_id=1,.version=2,
            .mode=variant==2?CERBERUS_DUAL_BLE_RESPONSE_FAMILY:CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY,
            .reserved=variant==2?6:0};
        uint8_t command=variant?CERBERUS_SPI_COMMAND_DUAL_BLE_START:CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE;
        const void *request=variant?(const void*)&response_start:(const void*)&metadata_request;
        size_t length=variant?sizeof(response_start):sizeof(metadata_request);
        reset();peer_firmware_version=66;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
        assert(!probes&&!commands&&zero(&out,sizeof(out)));
        reset();reported_version=66;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_NOT_SUPPORTED);
        assert(probes==1&&!commands&&peer_firmware_version==66);
        reset();reported_version=67;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_OK);
        assert(probes==1&&commands==1&&peer_firmware_version==67);
        reset();reported_version=67;probe_error=ESP_ERR_TIMEOUT;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_TIMEOUT);
        assert(probes==1&&!commands&&!peer_firmware_version&&zero(&out,sizeof(out)));
        probe_error=ESP_OK;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_OK&&probes==2&&commands==1);
        reset();reported_version=67;probe_us=10000;request_us=10001;expected_command=command;
        assert(c5_spi_link_dual_ble_exchange(command,request,length,&out,sizeof(out))==ESP_ERR_TIMEOUT);
        assert(probes==1&&commands==1&&zero(&out,sizeof(out)));
    }
    for(unsigned mode=CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY;mode<=CERBERUS_DUAL_BLE_RESPONSE_FAMILY;++mode){
        for(unsigned code=0;code<=7;++code){
            reset();peer_firmware_version=67;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_START;
            cerberus_dual_ble_start_t request={.boot_id=123,.job_id=1,.version=2,.mode=(uint8_t)mode,.reserved=(uint8_t)code};
            bool valid=mode==CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY?code==0:code>=1&&code<=6;
            esp_err_t result=c5_spi_link_dual_ble_exchange(expected_command,&request,sizeof(request),&out,sizeof(out));
            assert(result==(valid?ESP_OK:ESP_ERR_INVALID_ARG)&&commands==(valid?1U:0U));
            request.version=1;reset();peer_firmware_version=67;
            assert(c5_spi_link_dual_ble_exchange(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&request,sizeof(request),
                &out,sizeof(out))==ESP_ERR_INVALID_ARG&&!commands&&!probes);
        }
    }
    /* Compact transfer is optional at68; firmware67 must be rejected locally
     * so the reader can reuse its frozen handle through the full133 command.
     * Acquisition START/full response reads above remain supported at67. */
    const uint8_t compact=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT;
    reset();peer_firmware_version=67;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_NOT_SUPPORTED&&!probes&&!commands&&zero(&metadata,sizeof(metadata)));
    reset();reported_version=67;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_NOT_SUPPORTED&&probes==1&&!commands&&peer_firmware_version==67);
    reset();reported_version=68;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_OK&&probes==1&&commands==1&&peer_firmware_version==68);
    reset();peer_firmware_version=68;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_OK&&!probes&&commands==1);
    reset();reported_version=68;probe_error=ESP_ERR_TIMEOUT;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_TIMEOUT&&probes==1&&!commands&&!peer_firmware_version&&zero(&metadata,sizeof(metadata)));
    probe_error=ESP_OK;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_OK&&probes==2&&commands==1);
    reset();reported_version=68;probe_us=20000;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_TIMEOUT&&probes==1&&!commands&&!peer_firmware_version&&zero(&metadata,sizeof(metadata)));
    reset();reported_version=68;probe_us=10000;request_us=10001;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_TIMEOUT&&probes==1&&commands==1&&zero(&metadata,sizeof(metadata)));
    reset();peer_firmware_version=68;corrupt=true;expected_command=compact;
    assert(c5_spi_link_dual_ble_exchange(compact,&metadata_request,sizeof(metadata_request),
        &metadata,sizeof(metadata))==ESP_ERR_INVALID_RESPONSE&&!probes&&commands==1&&zero(&metadata,sizeof(metadata)));
    /* Saved-address discovery must never downgrade into an unfiltered scan. */
    cerberus_dual_ble_start_t reacquire={.boot_id=123,.job_id=1,.version=2,
        .mode=CERBERUS_DUAL_BLE_REACQUIRE,.address={1,2,3,4,5,6},
        .address_type=CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE};
    for (unsigned cached=0;cached<2;++cached) for (unsigned version=83;version<=84;++version) {
        reset();reported_version=version;peer_firmware_version=cached?version:0;
        expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_START;
        esp_err_t error=c5_spi_link_dual_ble_exchange(expected_command,&reacquire,sizeof(reacquire),&out,sizeof(out));
        assert(error==(version==84?ESP_OK:ESP_ERR_NOT_SUPPORTED));
        assert(commands==(version==84?1U:0U) && probes==(cached?0U:1U));
        if (version==83) assert(zero(&out,sizeof(out)));
    }
    reset();reported_version=84;probe_error=ESP_ERR_TIMEOUT;expected_command=CERBERUS_SPI_COMMAND_DUAL_BLE_START;
    assert(c5_spi_link_dual_ble_exchange(expected_command,&reacquire,sizeof(reacquire),&out,sizeof(out))==ESP_ERR_TIMEOUT && !commands);
    reset();peer_firmware_version=84;reacquire.address_type=1;
    assert(c5_spi_link_dual_ble_exchange(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&reacquire,sizeof(reacquire),&out,sizeof(out))==ESP_ERR_INVALID_ARG && !commands && !probes);
    puts("Production firmware-cache recovery passed: Analyzer/Family/Response START and read gates, optional compact68/full67 compatibility, transient recovery and shared deadline");
    return 0;
}
