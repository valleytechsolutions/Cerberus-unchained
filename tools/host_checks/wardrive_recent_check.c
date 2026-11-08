#include "wardrive_recent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { \
    fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static cerberus_wardrive_observation_t observation(unsigned identity, bool ble)
{
    cerberus_wardrive_observation_t row = {0};
    row.type = ble ? CERBERUS_WARDRIVE_BLE : CERBERUS_WARDRIVE_WIFI;
    row.radio = ble ? CERBERUS_WARDRIVE_RADIO_S3 : CERBERUS_WARDRIVE_RADIO_C5;
    row.address[0] = 2U;
    row.address[4] = (uint8_t)(identity >> 8U);
    row.address[5] = (uint8_t)identity;
    row.channel = ble ? 0U : 6U;
    row.rssi_dbm = -55;
    row.label_length = 4U;
    memcpy(row.label, "test", 4U);
    row.security = ble ? 0U : 3U;
    row.manufacturer_id = ble ? 0x004cU : 0U;
    return row;
}

static cerberus_wardrive_recent_ticket_t offer(cerberus_wardrive_recent_t *state,
    uint64_t sequence, cerberus_wardrive_observation_t *row, uint64_t stamp, bool valid)
{
    cerberus_wardrive_recent_ticket_t ticket = {0};
    CHECK(cerberus_wardrive_recent_observe(state, state->session_id, sequence, row, stamp, valid, &ticket));
    CHECK(ticket.session_id == state->session_id && ticket.sequence == sequence);
    return ticket;
}

static void rejected(cerberus_wardrive_recent_t *state, uint64_t session,
    uint64_t sequence, const cerberus_wardrive_observation_t *row)
{
    cerberus_wardrive_recent_t before = *state;
    cerberus_wardrive_recent_ticket_t ticket = {44U,55U};
    CHECK(!cerberus_wardrive_recent_observe(state, session, sequence, row, 100U, true, &ticket));
    CHECK(!ticket.session_id && !ticket.sequence);
    CHECK(memcmp(&before, state, sizeof(before)) == 0);
}

static void basic_and_invalid(void)
{
    cerberus_wardrive_recent_t state;
    cerberus_wardrive_recent_init(&state);
    cerberus_wardrive_recent_init(NULL);
    cerberus_wardrive_observation_t row = observation(1U, false);
    cerberus_wardrive_recent_row_t out[12];
    CHECK(!cerberus_wardrive_recent_begin(NULL, 1U));
    CHECK(!cerberus_wardrive_recent_begin(&state, 0U));
    rejected(&state, 1U, 1U, &row);
    CHECK(cerberus_wardrive_recent_copy_recent(&state, out, 12U) == 0U);
    CHECK(cerberus_wardrive_recent_begin(&state, 7U));
    CHECK(state.revision == 1U && state.session_id == 7U && !state.count);
    rejected(&state, 6U, 1U, &row);
    rejected(&state, 8U, 1U, &row);
    rejected(&state, 7U, 0U, &row);
    rejected(&state, 7U, 1U, NULL);
    CHECK(!cerberus_wardrive_recent_observe(&state,7U,1U,&row,0U,true,NULL));
    CHECK(!state.event_count);
    for (unsigned field = 0; field < 10U; ++field) {
        cerberus_wardrive_observation_t bad = row;
        switch (field) {
        case 0: bad.type = CERBERUS_WARDRIVE_WIFI_PROBE; break;
        case 1: bad.type = CERBERUS_WARDRIVE_WIFI_RELATION; break;
        case 2: bad.label_length = 33U; break;
        case 3: memset(bad.address,0,6U); break;
        case 4: bad.type=CERBERUS_WARDRIVE_BLE; bad.radio=CERBERUS_WARDRIVE_RADIO_R1; break;
        case 5: bad.radio = 8U; break;
        case 6: bad.channel = 0U; break;
        case 7: bad.rssi_dbm = 1; break;
        case 8: bad.rssi_dbm = -128; break;
        case 9: bad.address_type = 1U; break;
        }
        rejected(&state,7U,1U,&bad);
    }
    cerberus_wardrive_observation_t ble = observation(2U,true);
    ble.address_type = 4U; rejected(&state,7U,1U,&ble);
    ble.address_type = 0U; ble.ble_evidence.flags = 16U; rejected(&state,7U,1U,&ble);
    (void)offer(&state,2U,&row,10U,true);
    rejected(&state,7U,2U,&row);
    rejected(&state,7U,1U,&row);
    cerberus_wardrive_recent_t before = state;
    CHECK(!cerberus_wardrive_recent_begin(&state,7U));
    CHECK(!cerberus_wardrive_recent_begin(&state,6U));
    CHECK(memcmp(&state,&before,sizeof(state)) == 0);
    CHECK(cerberus_wardrive_recent_begin(&state,8U));
    CHECK(state.count == 0U && state.last_sequence == 0U && state.revision == 1U);
}

static void identity_and_horizon(void)
{
    cerberus_wardrive_recent_t state = {0};
    CHECK(cerberus_wardrive_recent_begin(&state,1U));
    cerberus_wardrive_observation_t wifi = observation(1U,false);
    cerberus_wardrive_recent_ticket_t oldest = offer(&state,1U,&wifi,1U,true);
    wifi.radio = CERBERUS_WARDRIVE_RADIO_R6; wifi.channel = 149U; wifi.rssi_dbm = -72;
    (void)offer(&state,2U,&wifi,2U,true);
    cerberus_wardrive_observation_t ble = observation(1U,true);
    (void)offer(&state,3U,&ble,3U,true);
    ble.address_type = 1U; (void)offer(&state,4U,&ble,4U,true);
    cerberus_wardrive_recent_row_t out[12];
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,12U) == 3U);
    CHECK(out[0].sequence == 4U && out[1].sequence == 3U && out[2].sequence == 2U);
    CHECK(out[2].observation.radio == CERBERUS_WARDRIVE_RADIO_R6 && out[2].observation.channel == 149U);
    CHECK(out[2].observation.rssi_dbm == -72);
    CHECK(cerberus_wardrive_recent_set_outcome(&state,oldest,CERBERUS_WARDRIVE_RECENT_ERROR));
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,12U) == 3U);
    CHECK(out[2].outcome == CERBERUS_WARDRIVE_RECENT_PENDING);
    for (uint64_t i = 5U; i <= 28U; ++i) (void)offer(&state,i,&ble,i,true);
    CHECK(state.count == 24U && state.event_count == 28U && state.overwritten == 4U);
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,12U) == 1U);
    CHECK(out[0].sequence == 28U);
    CHECK(!cerberus_wardrive_recent_set_outcome(&state,oldest,CERBERUS_WARDRIVE_RECENT_ERROR));
    CHECK(cerberus_wardrive_recent_copy_recent(&state,NULL,0U) == 0U);
    CHECK(cerberus_wardrive_recent_copy_recent(&state,NULL,10U) == 0U);
    CHECK(cerberus_wardrive_recent_copy_recent(NULL,out,10U) == 0U);
    CHECK(cerberus_wardrive_recent_visible_count(NULL) == 0U);
}

static void values_and_outcomes(void)
{
    cerberus_wardrive_recent_t state = {0};
    CHECK(cerberus_wardrive_recent_begin(&state,10U));
    cerberus_wardrive_observation_t row = observation(3U,true);
    row.address_type = 3U; row.security = 99U;
    row.label_length = 32U;
    for (unsigned i = 0U; i < 32U; ++i) row.label[i] = (uint8_t)(i * 9U);
    row.ble_evidence.flags = CERBERUS_S3_BLE_WARDRIVE_ADV_VALID | CERBERUS_S3_BLE_WARDRIVE_RSP_VALID |
        CERBERUS_S3_BLE_WARDRIVE_SID_VALID;
    row.ble_evidence.advertisement_sequence = 8U;
    row.ble_evidence.advertisement_observed_ms = UINT32_MAX - 8U;
    row.ble_evidence.scan_response_sequence = 9U;
    row.ble_evidence.scan_response_observed_ms = 3U;
    row.ble_evidence.sid = 4U;
    row.ble_evidence.advertisement.facts = CERBERUS_BLE_FLOCK_MFG_09C8;
    cerberus_wardrive_observation_t original = row;
    cerberus_wardrive_recent_ticket_t ticket = offer(&state,1U,&row,1000U,true);
    memset(&row,0xa5,sizeof(row));
    cerberus_wardrive_recent_row_t out[10];
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,10U) == 1U);
    CHECK(memcmp(out[0].observation.label,original.label,32U) == 0);
    CHECK(memcmp(&out[0].observation.ble_evidence,&original.ble_evidence,sizeof(original.ble_evidence)) == 0);
    CHECK(out[0].observation.security == 0U && out[0].observation.channel == 0U);
    CHECK(out[0].observation.manufacturer_id == original.manufacturer_id && out[0].observation.address_type == 3U);
    cerberus_wardrive_recent_row_t frozen = out[0];
    CHECK(cerberus_wardrive_recent_set_outcome(&state,ticket,CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED));
    uint64_t revision = state.revision;
    CHECK(cerberus_wardrive_recent_set_outcome(&state,ticket,CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED));
    CHECK(state.revision == revision);
    CHECK(!cerberus_wardrive_recent_set_outcome(&state,ticket,CERBERUS_WARDRIVE_RECENT_ERROR));
    CHECK(!cerberus_wardrive_recent_set_outcome(&state,ticket,(cerberus_wardrive_recent_outcome_t)-1));
    CHECK(frozen.outcome == CERBERUS_WARDRIVE_RECENT_PENDING && frozen.receipt_ms == 1000U);
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,10U) == 1U);
    CHECK(out[0].outcome == CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED);
    CHECK(out[0].sequence == frozen.sequence && out[0].receipt_ms == frozen.receipt_ms);
    CHECK(memcmp(&out[0].observation,&frozen.observation,sizeof(frozen.observation)) == 0);
    for (unsigned outcome = 1U; outcome <= CERBERUS_WARDRIVE_RECENT_NOT_LOGGED; ++outcome) {
        row = observation(10U+outcome,false); row.manufacturer_id = 0x1234U;
        memset(row.label+row.label_length,0xa5,sizeof(row.label)-row.label_length);
        ticket = offer(&state,outcome+1U,&row,1000U+outcome,true);
        CHECK(cerberus_wardrive_recent_set_outcome(&state,ticket,(cerberus_wardrive_recent_outcome_t)outcome));
        CHECK(cerberus_wardrive_recent_copy_recent(&state,out,10U) == outcome+1U);
        CHECK(out[0].observation.manufacturer_id == 0U);
        for (size_t i = row.label_length; i < sizeof(row.label); ++i) CHECK(out[0].observation.label[i] == 0U);
    }
    CHECK(!cerberus_wardrive_recent_set_outcome(&state,(cerberus_wardrive_recent_ticket_t){9U,6U},CERBERUS_WARDRIVE_RECENT_ERROR));
    CHECK(cerberus_wardrive_recent_begin(&state,11U));
    CHECK(!cerberus_wardrive_recent_set_outcome(&state,ticket,CERBERUS_WARDRIVE_RECENT_ERROR));
}

static void clocks_and_exhaustion(void)
{
    cerberus_wardrive_recent_t state = {0};
    CHECK(cerberus_wardrive_recent_begin(&state,1U));
    cerberus_wardrive_observation_t row = observation(1U,false);
    cerberus_wardrive_recent_row_t out[10];
    (void)offer(&state,1U,&row,0U,true);
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,10U) == 1U);
    uint64_t age = 99U;
    CHECK(cerberus_wardrive_recent_age(&out[0],0U,true,&age) && age == 0U);
    CHECK(cerberus_wardrive_recent_age(&out[0],UINT64_MAX,true,&age) && age == UINT64_MAX);
    (void)offer(&state,2U,&row,100U,true);
    (void)offer(&state,3U,&row,99U,true);
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,10U) == 1U);
    CHECK(!out[0].receipt_valid && !out[0].receipt_ms && state.last_receipt_ms == 100U);
    CHECK(!cerberus_wardrive_recent_age(&out[0],200U,true,&age) && age == 0U);
    (void)offer(&state,4U,&row,UINT64_MAX,false);
    CHECK(state.last_receipt_ms == 100U && state.invalid_receipts == 2U);
    (void)offer(&state,5U,&row,100U,true);
    CHECK(cerberus_wardrive_recent_copy_recent(&state,out,10U) == 1U);
    CHECK(!cerberus_wardrive_recent_age(&out[0],99U,true,&age) && age == 0U);
    CHECK(!cerberus_wardrive_recent_age(&out[0],101U,false,&age) && age == 0U);
    CHECK(cerberus_wardrive_recent_age(&out[0],101U,true,&age) && age == 1U);
    CHECK(!cerberus_wardrive_recent_age(NULL,1U,true,&age));
    CHECK(!cerberus_wardrive_recent_age(&out[0],101U,true,NULL));
    state.revision = UINT64_MAX;
    rejected(&state,1U,6U,&row);
    CHECK(!cerberus_wardrive_recent_set_outcome(&state,(cerberus_wardrive_recent_ticket_t){1U,5U},CERBERUS_WARDRIVE_RECENT_ERROR));
    state.revision = 1U; state.event_count = UINT64_MAX;
    rejected(&state,1U,6U,&row);
    CHECK(cerberus_wardrive_recent_begin(&state,2U));
    (void)offer(&state,UINT64_MAX,&row,UINT64_MAX,true);
    rejected(&state,2U,UINT64_MAX,&row);
    rejected(&state,2U,0U,&row);
    CHECK(cerberus_wardrive_recent_begin(&state,UINT64_MAX));
    CHECK(!cerberus_wardrive_recent_begin(&state,1U));
    CHECK(!cerberus_wardrive_recent_begin(&state,UINT64_MAX));
    state.invalid_receipts = UINT32_MAX;
    (void)offer(&state,1U,&row,0U,false);
    CHECK(state.invalid_receipts == UINT32_MAX);
    for (uint64_t i=2U;i<=24U;++i) (void)offer(&state,i,&row,i,true);
    state.overwritten = UINT32_MAX;
    (void)offer(&state,25U,&row,25U,true);
    CHECK(state.overwritten == UINT32_MAX);
}

static void reference_stream(void)
{
    cerberus_wardrive_recent_t state = {0};
    CHECK(cerberus_wardrive_recent_begin(&state,1U));
    struct { unsigned identity; bool ble; uint8_t address_type; uint64_t sequence; } history[24] = {0};
    uint32_t random = 0x12345678U;
    for (uint64_t sequence = 1U; sequence <= 6000U; ++sequence) {
        random = random * 1664525U + 1013904223U;
        unsigned identity = (random >> 8U) % 17U;
        bool ble = (random & 1U) != 0U;
        uint8_t type = ble ? (uint8_t)((random >> 4U) & 3U) : 0U;
        cerberus_wardrive_observation_t row = observation(identity,ble);
        row.address_type = type;
        if (!ble) row.radio = (uint8_t)(2U + ((random >> 16U) % 6U));
        (void)offer(&state,sequence,&row,sequence,true);
        memmove(history,history+1U,23U*sizeof(history[0]));
        history[23].identity=identity; history[23].ble=ble; history[23].address_type=type; history[23].sequence=sequence;
        uint64_t expected[10]; size_t wanted=0U;
        for (int i=23; i>=0 && wanted<10U; --i) {
            if (!history[i].sequence) continue;
            bool duplicate=false;
            for (int j=i+1;j<24;++j)
                if (history[j].identity==history[i].identity && history[j].ble==history[i].ble &&
                    history[j].address_type==history[i].address_type) duplicate=true;
            if (!duplicate) expected[wanted++]=history[i].sequence;
        }
        cerberus_wardrive_recent_t before=state;
        CHECK(cerberus_wardrive_recent_visible_count(&state) == wanted);
        for (size_t capacity=0U; capacity<=12U; ++capacity) {
            cerberus_wardrive_recent_row_t out[12]; memset(out,0xa5,sizeof(out));
            size_t count=cerberus_wardrive_recent_copy_recent(&state,out,capacity);
            size_t expected_count=wanted<capacity?wanted:capacity;
            CHECK(count==expected_count);
            for(size_t i=0U;i<count;++i) CHECK(out[i].sequence==expected[i]);
            const unsigned char *tail=(const unsigned char *)(out+count);
            for(size_t i=0U;i<(12U-count)*sizeof(out[0]);++i) CHECK(tail[i]==0xa5U);
        }
        CHECK(memcmp(&before,&state,sizeof(state))==0);
    }
}

int main(void)
{
    basic_and_invalid(); identity_and_horizon(); values_and_outcomes();
    clocks_and_exhaustion(); reference_stream();
    cerberus_wardrive_recent_t repeated={0};CHECK(cerberus_wardrive_recent_begin(&repeated,1));
    repeated.all_observations=true;cerberus_wardrive_observation_t same=observation(1,false);
    for(unsigned i=1;i<=12;++i)(void)offer(&repeated,i,&same,i,true);
    cerberus_wardrive_recent_row_t rows[10];
    CHECK(cerberus_wardrive_recent_copy_recent(&repeated,rows,10)==10);
    for(unsigned i=0;i<10;++i)CHECK(rows[i].sequence==12U-i);
    printf("wardrive_recent PASS %u assertions; model=%zu row=%zu bytes\n",assertions,
        sizeof(cerberus_wardrive_recent_t),sizeof(cerberus_wardrive_recent_row_t));
    return 0;
}
