#include "c5_spi_link.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"Incremental AP line %u: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef unsigned TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static void *link_mutex = (void *)1;
static uint8_t sequence;
static uint32_t peer_firmware_version;
static bool held, fail_lock;
static int64_t now_us, lock_delay_us, transfer_delay_us;
static unsigned attempts, takes, gives, corrupt_at, corruption;
static TickType_t last_wait;
static cerberus_wifi_scan_result_payload_t metadata;
static cerberus_wifi_scan_status_payload_t scan_status;
static char ssid[33];
static cerberus_status_payload_t peer;
static cerberus_wifi_relation_status_t relation_status;
static uint64_t source_now;

static int64_t esp_timer_get_time(void) { return now_us; }
static BaseType_t take_link_mutex(TickType_t ticks)
{
    CHECK(!held && ticks <= 20U); ++takes; last_wait = ticks;
    if (fail_lock) return 0;
    held = true; now_us += lock_delay_us; return pdTRUE;
}
static void give_link_mutex(void) { CHECK(held); held = false; ++gives; }

static void spi_request_response(const cerberus_spi_frame_t *request, void *discarded,
    cerberus_spi_frame_t *response, size_t size)
{
    (void)discarded;
    CHECK(held && size == sizeof(*request)); CHECK(cerberus_protocol_frame_is_valid(request)); ++attempts;
    uint8_t bytes[22] = {0}; size_t length = 0;
    cerberus_spi_command_t command = (cerberus_spi_command_t)request->command;
    if (command == CERBERUS_SPI_COMMAND_WIFI_SCAN_RESULT) {
        cerberus_wifi_scan_result_request_t q; memcpy(&q, request->payload, sizeof(q));
        CHECK(request->payload_length == sizeof(q) && q.generation == metadata.generation && q.index == metadata.index);
        memcpy(bytes, &metadata, sizeof(metadata)); length = sizeof(metadata);
        if (attempts == corrupt_at) {
            switch (corruption) {
            case 10: bytes[0]++; break;
            case 11: bytes[1]++; break;
            case 12: bytes[3] = 0; break;
            case 13: bytes[4] = CERBERUS_WIFI_SECURITY_OTHER + 1; break;
            case 14: bytes[5] = 33; break;
            case 15: bytes[20] = 0x80; break;
            case 16: bytes[21]++; break;
            default: break;
            }
        }
    } else if (command == CERBERUS_SPI_COMMAND_WIFI_SCAN_SSID) {
        cerberus_wifi_scan_ssid_request_t q; memcpy(&q, request->payload, sizeof(q));
        CHECK(request->payload_length == sizeof(q) && q.generation == metadata.generation && q.index == metadata.index);
        size_t offset = q.chunk * CERBERUS_WIFI_SSID_CHUNK_SIZE;
        CHECK(offset < metadata.ssid_length);
        uint8_t n = metadata.ssid_length - offset;
        if (n > CERBERUS_WIFI_SSID_CHUNK_SIZE) n = CERBERUS_WIFI_SSID_CHUNK_SIZE;
        cerberus_wifi_scan_ssid_payload_t p = {q.generation, q.index, q.chunk, n, {0}};
        memcpy(p.bytes, ssid + offset, n); memcpy(bytes, &p, sizeof(p)); length = sizeof(p);
        if (attempts == corrupt_at && corruption >= 10 && corruption <= 13) bytes[corruption - 10]++;
    } else if (command == CERBERUS_SPI_COMMAND_WIFI_SCAN_STATUS) {
        CHECK(!request->payload_length); memcpy(bytes, &scan_status, sizeof(scan_status)); length = sizeof(scan_status);
    } else if (command == CERBERUS_SPI_COMMAND_STATUS) {
        CHECK(!request->payload_length); memcpy(bytes, &peer, sizeof(peer)); length = sizeof(peer);
    } else if (command == CERBERUS_SPI_COMMAND_WIFI_RELATION_STATUS) {
        CHECK(request->payload_length == 1 && request->payload[0] == relation_status.generation);
        memcpy(bytes, &relation_status, sizeof(relation_status)); length = sizeof(relation_status);
    } else if (command == CERBERUS_SPI_COMMAND_WIFI_RELATION_CLOCK) {
        cerberus_wifi_relation_clock_query_t q; memcpy(&q, request->payload, sizeof(q));
        CHECK(q.epoch == relation_status.epoch && q.generation == relation_status.generation);
        cerberus_wifi_relation_clock_t p = {.epoch=q.epoch, .source_now_us=source_now,
            .generation=q.generation, .count=relation_status.count};
        memcpy(bytes, &p, sizeof(p)); length = sizeof(p);
    } else CHECK(false); /* Never a start, stop, connection or row-bank fetch. */
    uint8_t token = request->sequence;
    if (attempts == corrupt_at) {
        if (corruption == 1) token++;
        if (corruption == 2) command++;
        if (corruption == 3) length--;
    }
    CHECK(cerberus_protocol_prepare_frame(response, command, token, bytes, length));
    if (attempts == corrupt_at && corruption == 0) response->payload[0] ^= 1;
    now_us += transfer_delay_us;
}

/* Protocol-only adapter. Real READY budget, GPIO edges and SPI recovery run in
 * check_c5_ready_handshake.py; this suite retains its injected frame faults. */
static esp_err_t spi_request_response_passive(const void *request, void *discarded,
    void *response, size_t size, uint32_t deadline_ms)
{
    (void)deadline_ms;
    spi_request_response(request, discarded, response, size);
    return ESP_OK;
}

#include "scan_peer_impl.h"
#include "c5_wifi_relation_link_impl.h"
#include "c5_wifi_scan_link_impl.h"

static bool zero(const void *value, size_t bytes)
{
    const uint8_t *p = value;
    for (size_t i=0; i<bytes; ++i) if(p[i]) return false;
    return true;
}

static void reset(uint8_t length)
{
    CHECK(!held);
    attempts=takes=gives=corrupt_at=corruption=0; sequence=0; fail_lock=false;
    lock_delay_us=0; transfer_delay_us=1000; now_us=1000000; link_mutex=(void *)1;
    peer_firmware_version=60;
    metadata=(cerberus_wifi_scan_result_payload_t){.generation=255, .index=31, .rssi=-47,
        .channel=36, .security=CERBERUS_WIFI_SECURITY_WPA3, .ssid_length=length,
        .bssid={2,3,4,5,6,7}, .observation_age_ms=20, .observation_source_ms=88000,
        .observation_flags=CERBERUS_WIFI_OBSERVATION_VALID|CERBERUS_WIFI_OBSERVATION_BEACON,
        .observation_channel=36};
    for(size_t i=0;i<32;++i) ssid[i]=(char)('A'+i%26);
    ssid[32]=0;
    scan_status=(cerberus_wifi_scan_status_payload_t){.state=CERBERUS_WIFI_SCAN_COMPLETE,
        .generation=255, .total_count=37, .stored_count=32, .hidden_count=2};
    peer=(cerberus_status_payload_t){.ready=1,.firmware_version=60,.uptime_ms=9000,
        .capabilities=CERBERUS_C5_CAP_WIFI_2_4_GHZ|CERBERUS_C5_CAP_WIFI_5_GHZ,.boot_count=1,.reset_reason=1};
    relation_status=(cerberus_wifi_relation_status_t){.epoch=0x100000003ULL,
        .generation=255, .state=CERBERUS_WIFI_RELATION_COMPLETE,
        .flags=CERBERUS_WIFI_RELATION_AVAILABLE,.count=2,.matched_frames=3};
    source_now=500000;
}

static esp_err_t step(cerberus_c5_wifi_scan_reader_t *reader,
    cerberus_c5_wifi_result_t *row, cerberus_s3_wifi_observation_t *obs, bool *done)
{
    unsigned before=attempts;
    memset(row,0xA5,sizeof(*row)); memset(obs,0xA5,sizeof(*obs)); *done=true;
    esp_err_t error=c5_spi_link_wifi_scan_result_step(reader,row,obs,done);
    CHECK(attempts-before<=1 && !held && takes==gives+(fail_lock?1U:0U));
    if(error!=ESP_OK || !*done) CHECK(zero(row,sizeof(*row))&&zero(obs,sizeof(*obs))&&!*done);
    if(error!=ESP_OK) CHECK(zero(&reader->pending,sizeof(reader->pending))&&zero(&reader->observation,sizeof(reader->observation)));
    return error;
}

static void happy_paths(void)
{
    for(unsigned length=0;length<=32;++length) for(unsigned firmware=50;firmware<=60;firmware+=5) {
        reset((uint8_t)length); peer_firmware_version=firmware;
        cerberus_c5_wifi_scan_reader_t reader={0};
        CHECK(c5_spi_link_wifi_scan_result_begin(&reader,255,31,5000)==ESP_OK&&attempts==0);
        cerberus_c5_wifi_result_t row; cerberus_s3_wifi_observation_t obs; bool done=false;
        while(!done) CHECK(step(&reader,&row,&obs,&done)==ESP_OK);
        CHECK(attempts==1+(length+17)/18&&row.ssid[length]==0&&!memcmp(row.ssid,ssid,length));
        CHECK(row.rssi==-47&&row.channel==36&&row.security==CERBERUS_WIFI_SECURITY_WPA3&&!memcmp(row.bssid,metadata.bssid,6));
        CHECK(obs.valid==(firmware>=51));
        if(obs.valid) CHECK(obs.request_started_us==1000000&&obs.source_age_ms==20);
        unsigned before=attempts; CHECK(step(&reader,&row,&obs,&done)==ESP_OK&&done&&attempts==before);
    }
    reset(32); cerberus_c5_wifi_scan_reader_t reader={0};
    cerberus_c5_wifi_result_t row; cerberus_s3_wifi_observation_t obs; bool done=false;
    CHECK(c5_spi_link_wifi_scan_result_begin(&reader,255,31,10000)==ESP_OK);
    CHECK(step(&reader,&row,&obs,&done)==ESP_OK&&!done);
    now_us+=3000000;
    CHECK(step(&reader,&row,&obs,&done)==ESP_OK&&!done);
    CHECK(step(&reader,&row,&obs,&done)==ESP_OK&&done&&obs.request_started_us==1000000);
    CHECK(!cerberus_s3_wifi_observation_recent(&obs,31000001));
}

static void faults(void)
{
    for(unsigned position=1;position<=3;++position) for(unsigned fault=0;fault<=16;++fault) {
        if(fault>=4&&fault<=9)continue;
        if(position>1&&fault>13)continue;
        reset(32); corrupt_at=position; corruption=fault;
        cerberus_c5_wifi_scan_reader_t reader={0};
        CHECK(c5_spi_link_wifi_scan_result_begin(&reader,255,31,5000)==ESP_OK);
        cerberus_c5_wifi_result_t row; cerberus_s3_wifi_observation_t obs; bool done=false;
        esp_err_t error=ESP_OK;
        while(error==ESP_OK&&!done) error=step(&reader,&row,&obs,&done);
        CHECK(error==ESP_ERR_INVALID_RESPONSE&&attempts==position);
        CHECK(step(&reader,&row,&obs,&done)==error&&attempts==position);
    }
    for(unsigned position=0;position<=3;++position) {
        reset(32); cerberus_c5_wifi_scan_reader_t reader={0};
        CHECK(c5_spi_link_wifi_scan_result_begin(&reader,255,31,5000)==ESP_OK);
        cerberus_c5_wifi_result_t row; cerberus_s3_wifi_observation_t obs; bool done=false;
        for(unsigned i=0;i<position;++i)CHECK(step(&reader,&row,&obs,&done)==ESP_OK);
        c5_spi_link_wifi_scan_result_cancel(&reader);
        CHECK(step(&reader,&row,&obs,&done)==ESP_ERR_NOT_FINISHED&&attempts==position);
    }
    for(unsigned kind=0;kind<8;++kind) {
        reset(32); cerberus_c5_wifi_scan_reader_t reader={0};
        CHECK(c5_spi_link_wifi_scan_result_begin(&reader,255,31,1020)==ESP_OK);
        cerberus_c5_wifi_result_t row; cerberus_s3_wifi_observation_t obs; bool done=false;
        esp_err_t expected=ESP_ERR_TIMEOUT;
        if(kind==0)now_us=1020000;
        if(kind==1)fail_lock=true;
        if(kind==2)lock_delay_us=20000;
        if(kind==3)transfer_delay_us=20000;
        if(kind==4){link_mutex=NULL;expected=ESP_ERR_INVALID_STATE;}
        if(kind==5){now_us=-1;expected=ESP_ERR_INVALID_STATE;}
        if(kind>=6) {
            CHECK(step(&reader,&row,&obs,&done)==ESP_OK);
            if(kind==6)now_us=999999;
            else peer_firmware_version=50;
            expected=ESP_ERR_INVALID_STATE;
        }
        unsigned before=attempts;
        CHECK(step(&reader,&row,&obs,&done)==expected);
        CHECK(attempts==before+(kind==3?1U:0U));
    }
    reset(0); now_us=((int64_t)UINT32_MAX-5)*1000;
    cerberus_c5_wifi_scan_reader_t reader={0};
    CHECK(c5_spi_link_wifi_scan_result_begin(&reader,255,31,15)==ESP_OK);
    cerberus_c5_wifi_result_t row; cerberus_s3_wifi_observation_t obs; bool done=false;
    CHECK(step(&reader,&row,&obs,&done)==ESP_OK&&done&&last_wait==20);
    CHECK(c5_spi_link_wifi_scan_result_begin(&reader,0,32,5000)==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_wifi_scan_result_begin(NULL,0,0,5000)==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_wifi_scan_result_step(&reader,NULL,&obs,&done)==ESP_ERR_INVALID_ARG);
    c5_spi_link_wifi_scan_result_cancel(NULL);
}

static void status_and_fences(void)
{
    for(unsigned kind=0;kind<9;++kind) {
        reset(0);
        if(kind==1)scan_status.state=4;
        if(kind==2)scan_status.stored_count=33;
        if(kind==3)scan_status.total_count=0;
        if(kind==4)scan_status.hidden_count=38;
        if(kind==5)scan_status.reserved[0]=1;
        if(kind==6)scan_status.reserved[1]=1;
        if(kind==7){corrupt_at=1;corruption=0;}
        if(kind==8)transfer_delay_us=5000000;
        cerberus_wifi_scan_status_payload_t output;
        esp_err_t error=c5_spi_link_get_wifi_scan_status_once(5000,&output);
        CHECK(attempts==1&&!held&&takes==gives);
        if(kind)CHECK(error!=ESP_OK&&zero(&output,sizeof(output)));
        else CHECK(error==ESP_OK&&!memcmp(&output,&scan_status,sizeof(output)));
    }
    CHECK(c5_spi_link_get_wifi_scan_status_once(5000,NULL)==ESP_ERR_INVALID_ARG);
    reset(0); cerberus_c5_wifi_relation_reader_t reader={0};
    cerberus_c5_wifi_relation_bank_t proof={0},original={0}; bool done=false;
    CHECK(c5_spi_link_wifi_relation_begin_check(&reader,255,5000)==ESP_OK&&attempts==0);
    while(!done){unsigned before=attempts;CHECK(c5_spi_link_wifi_relation_step(&reader,&proof,&done,NULL,NULL)==ESP_OK&&attempts==before+1);}
    CHECK(attempts==6&&proof.status.count==2&&proof.status.epoch==0x100000003ULL);
    original=proof; now_us+=100000; source_now+=50000;
    CHECK(c5_spi_link_wifi_relation_begin_check_expected(&reader,&original,5000)==ESP_OK);
    done=false;
    while(!done){unsigned before=attempts;CHECK(c5_spi_link_wifi_relation_step(&reader,&proof,&done,NULL,NULL)==ESP_OK&&attempts==before+1);}
    CHECK(attempts==12&&proof.request_started_us==original.request_started_us&&proof.source_now_us==original.source_now_us);
    CHECK(c5_spi_link_wifi_relation_begin_check_expected(&reader,&original,5000)==ESP_OK);
    relation_status.epoch++; done=false; esp_err_t error=ESP_OK;
    while(error==ESP_OK&&!done)error=c5_spi_link_wifi_relation_step(&reader,&proof,&done,NULL,NULL);
    CHECK(error==ESP_ERR_INVALID_RESPONSE&&!done&&zero(&proof,sizeof(proof)));
    CHECK(c5_spi_link_wifi_relation_begin_check(NULL,0,5000)==ESP_ERR_INVALID_ARG);
    CHECK(c5_spi_link_wifi_relation_begin_check_expected(NULL,&original,5000)==ESP_ERR_INVALID_ARG);
}

int main(void)
{
    happy_paths(); faults(); status_and_fences();
    printf("Incremental AP transport: %u checks passed; one physical attempt per step, no partial publication, terminal faults/cancel, original age and exact relation fences.\n",checks);
    return 0;
}
