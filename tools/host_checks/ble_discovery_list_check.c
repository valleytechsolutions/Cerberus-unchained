#undef NDEBUG
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ble_discovery_list.h"

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr, "BLE discovery list %u: %s\n", __LINE__, #value); exit(1); } } while (0)
_Static_assert(sizeof(cerberus_ble_discovery_list_ref_t) == 2U, "reference-only workspace");
_Static_assert(CERBERUS_BLE_DISCOVERY_LIST_CAPACITY == CERBERUS_BLE_SCAN_MAX_RESULTS + CERBERUS_DUAL_BLE_CAPACITY, "two bounded banks");

static cerberus_s3_ble_discovery_inventory_t inventory, original_inventory;
static cerberus_dual_ble_row_t companion[32], original_companion[32];
static struct {
    uint32_t before;
    cerberus_ble_discovery_list_ref_t refs[64];
    uint32_t after;
} output;

static cerberus_ble_discovery_list_ref_t ref(uint8_t source, uint8_t index)
{ return (cerberus_ble_discovery_list_ref_t){source, index}; }

static void initialize(uint8_t s3_count, uint8_t c5_count)
{
    CHECK(cerberus_s3_ble_discovery_inventory_init(&inventory, CERBERUS_S3_BLE_FILTER_ALL));
    memset(companion, 0, sizeof(companion));
    inventory.count = s3_count;
    inventory.current_mask = 0x55555555U;
    for (uint8_t i = 0; i < s3_count; ++i) {
        cerberus_s3_ble_discovery_entry_t *entry = &inventory.entries[i];
        entry->epoch = UINT64_C(0x100000001);
        entry->source_now_ms = 1000;
        entry->original_source_ms = 500 + i;
        entry->record.index = i;
        entry->record.generation = 255;
        entry->record.peer.address[0] = (uint8_t)(i + 1U);
        entry->record.peer.address_type = i % 4U;
        entry->record.peer.rssi = (int8_t)(-90 + i);
        snprintf(entry->record.peer.name, sizeof(entry->record.peer.name), "S3-%02u", i);
    }
    for (uint8_t i = 0; i < c5_count; ++i) {
        companion[i].address[0] = (uint8_t)(i + 1U);
        companion[i].address_type = i % 4U;
        companion[i].rssi = (int8_t)(-80 + i);
        snprintf(companion[i].name, sizeof(companion[i].name), "C5-%02u", i);
        companion[i].timing_flags = CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID;
        companion[i].request_started_ms = 900;
        companion[i].callback_age_ms = i;
    }
    memset(&output, 0xA5, sizeof(output));
}

static void freeze_inputs(void)
{ original_inventory = inventory; memcpy(original_companion, companion, sizeof(companion)); }
static void unchanged(void)
{
    CHECK(!memcmp(&inventory, &original_inventory, sizeof(inventory)));
    CHECK(!memcmp(companion, original_companion, sizeof(companion)));
    CHECK(output.before == UINT32_C(0xA5A5A5A5) && output.after == UINT32_C(0xA5A5A5A5));
}
static void expect(unsigned at, uint8_t source, uint8_t index)
{ CHECK(output.refs[at].source == source && output.refs[at].index == index); }
static uint8_t order(uint8_t count, cerberus_ble_discovery_list_source_t source,
    cerberus_ble_discovery_list_sort_t sort, bool recent, uint64_t now)
{
    return cerberus_ble_discovery_list_order(&inventory, companion, count, source, sort, recent, now, output.refs);
}

static void capacity_and_sources(void)
{
    initialize(32, 32); freeze_inputs();
    CHECK(order(32, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, false, 1000) == 64);
    for (unsigned i = 0; i < 32; ++i) { expect(i, CERBERUS_BLE_DISCOVERY_LIST_S3, i); expect(i + 32U, CERBERUS_BLE_DISCOVERY_LIST_C5, i); }
    unchanged();
    for (unsigned source = CERBERUS_BLE_DISCOVERY_LIST_S3; source <= CERBERUS_BLE_DISCOVERY_LIST_C5; ++source) {
        memset(output.refs, 0xA5, sizeof(output.refs));
        CHECK(order(32, source, CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, false, 1000) == 32);
        for (unsigned i = 0; i < 32; ++i) expect(i, source, i);
        for (unsigned i = 32; i < 64; ++i) expect(i, 0xA5, 0xA5);
        unchanged();
    }
    CHECK(cerberus_ble_discovery_list_order(NULL, companion, 32, CERBERUS_BLE_DISCOVERY_LIST_ALL,
        CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, false, 0, output.refs) == 32);
    for (unsigned i = 0; i < 32; ++i) expect(i, CERBERUS_BLE_DISCOVERY_LIST_C5, i);
    CHECK(cerberus_ble_discovery_list_order(&inventory, NULL, 0, CERBERUS_BLE_DISCOVERY_LIST_ALL,
        CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, false, 0, output.refs) == 32);
    for (unsigned i = 0; i < 32; ++i) expect(i, CERBERUS_BLE_DISCOVERY_LIST_S3, i);
    memset(output.refs, 0xA5, sizeof(output.refs));
    CHECK(cerberus_ble_discovery_list_order(NULL, companion, 32, CERBERUS_BLE_DISCOVERY_LIST_S3,
        CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0, output.refs) == 0);
    CHECK(cerberus_ble_discovery_list_order(&inventory, NULL, 0, CERBERUS_BLE_DISCOVERY_LIST_C5,
        CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0, output.refs) == 0);
    CHECK(cerberus_ble_discovery_list_order(NULL, NULL, 0, CERBERUS_BLE_DISCOVERY_LIST_ALL,
        CERBERUS_BLE_DISCOVERY_LIST_NAME, false, 0, output.refs) == 0);
    for (unsigned i = 0; i < 64; ++i) expect(i, 0xA5, 0xA5);
    unchanged();
}

static void typed_identity_and_ties(void)
{
    initialize(2, 2);
    for (unsigned i = 0; i < 2; ++i) {
        memset(inventory.entries[i].record.peer.address, 0x42, 6);
        memset(companion[i].address, 0x42, 6);
        inventory.entries[i].record.peer.address_type = companion[i].address_type = (uint8_t)i;
        strcpy(inventory.entries[i].record.peer.name, "same");
        strcpy(companion[i].name, "SAME");
        inventory.entries[i].record.peer.rssi = companion[i].rssi = -42;
    }
    freeze_inputs();
    for (unsigned sort = 0; sort < CERBERUS_BLE_DISCOVERY_LIST_SORT_COUNT; ++sort) {
        CHECK(order(2, CERBERUS_BLE_DISCOVERY_LIST_ALL, sort, false, 1000) == 4);
        expect(0, CERBERUS_BLE_DISCOVERY_LIST_S3, 0); expect(1, CERBERUS_BLE_DISCOVERY_LIST_S3, 1);
        expect(2, CERBERUS_BLE_DISCOVERY_LIST_C5, 0); expect(3, CERBERUS_BLE_DISCOVERY_LIST_C5, 1);
        unchanged();
    }
    /* Name is the secondary key for equal signal, then stable source/index. */
    strcpy(inventory.entries[0].record.peer.name, "Zulu");
    strcpy(inventory.entries[1].record.peer.name, "alpha");
    strcpy(companion[0].name, "ALPHA"); strcpy(companion[1].name, "beta");
    freeze_inputs();
    for (unsigned sort = 0; sort < 2; ++sort) {
        CHECK(order(2, CERBERUS_BLE_DISCOVERY_LIST_ALL, sort, false, 1000) == 4);
        expect(0, CERBERUS_BLE_DISCOVERY_LIST_S3, 1); expect(1, CERBERUS_BLE_DISCOVERY_LIST_C5, 0);
        expect(2, CERBERUS_BLE_DISCOVERY_LIST_C5, 1); expect(3, CERBERUS_BLE_DISCOVERY_LIST_S3, 0);
        unchanged();
    }
}

static void signal_semantics(void)
{
    initialize(4, 4);
    inventory.entries[0].record.peer.rssi = -30;
    inventory.entries[1].record.peer.rssi = 127;
    inventory.entries[2].record.peer.rssi = -128;
    inventory.entries[3].record.peer.rssi = 21;
    companion[0].rssi = -20;
    companion[1].rssi = 20; companion[1].timing_flags = CERBERUS_DUAL_BLE_SEEN;
    companion[2].rssi = -127;
    companion[3].rssi = 20; companion[3].timing_flags = CERBERUS_DUAL_BLE_RSSI_VALID;
    inventory.current_mask = 0; /* EARLIER does not rewrite a retained RSSI. */
    freeze_inputs();
    CHECK(order(4, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 1000) == 8);
    expect(0, CERBERUS_BLE_DISCOVERY_LIST_C5, 0); expect(1, CERBERUS_BLE_DISCOVERY_LIST_S3, 0);
    expect(2, CERBERUS_BLE_DISCOVERY_LIST_C5, 2);
    expect(3, CERBERUS_BLE_DISCOVERY_LIST_C5, 1); expect(4, CERBERUS_BLE_DISCOVERY_LIST_C5, 3);
    expect(5, CERBERUS_BLE_DISCOVERY_LIST_S3, 1); expect(6, CERBERUS_BLE_DISCOVERY_LIST_S3, 2);
    expect(7, CERBERUS_BLE_DISCOVERY_LIST_S3, 3);
    for (unsigned i = 0; i < 8; ++i) {
        int8_t value = 77;
        bool valid = cerberus_ble_discovery_list_rssi(&inventory, companion, 4, output.refs[i], &value);
        CHECK(valid == (i < 3));
        if (!valid) CHECK(value == 77);
    }
    unchanged();
}

static void labels(void)
{
    initialize(1, 1);
    const uint8_t address[6] = {0x00, 0x12, 0x34, 0xab, 0xcd, 0xef};
    memcpy(inventory.entries[0].record.peer.address, address, 6);
    memcpy(companion[0].address, address, 6);
    for (unsigned receiver = CERBERUS_BLE_DISCOVERY_LIST_S3; receiver <= CERBERUS_BLE_DISCOVERY_LIST_C5; ++receiver) {
        char *name = receiver == CERBERUS_BLE_DISCOVERY_LIST_S3 ? inventory.entries[0].record.peer.name : companion[0].name;
        name[0] = 0;
        char label[40]; memset(label, '!', sizeof(label));
        CHECK(cerberus_ble_discovery_list_label(&inventory, companion, 1, ref(receiver, 0), label, sizeof(label)) == 17);
        CHECK(!strcmp(label, "EF:CD:AB:34:12:00"));
        CHECK(cerberus_ble_discovery_list_label(&inventory, companion, 1, ref(receiver, 0), NULL, 0) == 17);
        const unsigned char raw[] = {'A', 0x1f, 0x80, 0x7f, '~', 0};
        memcpy(name, raw, sizeof(raw));
        CHECK(cerberus_ble_discovery_list_label(&inventory, companion, 1, ref(receiver, 0), label, sizeof(label)) == 5);
        CHECK(!strcmp(label, "A???~"));
        memset(name, 'Q', CERBERUS_BLE_NAME_MAX_LENGTH + 1U); /* Damaged missing terminator stays bounded. */
        for (size_t capacity = 0; capacity <= sizeof(label) - 2U; ++capacity) {
            memset(label, '!', sizeof(label));
            CHECK(cerberus_ble_discovery_list_label(&inventory, companion, 1, ref(receiver, 0), label + 1, capacity) == 32);
            CHECK(label[0] == '!' && label[1U + capacity] == '!');
            if (capacity) {
                size_t length = capacity - 1U < 32U ? capacity - 1U : 32U;
                CHECK(strlen(label + 1) == length);
                for (size_t i = 0; i < length; ++i) CHECK(label[1U + i] == 'Q');
            }
        }
        CHECK(name[CERBERUS_BLE_NAME_MAX_LENGTH] == 'Q');
    }
}

static void ages_and_recent(void)
{
    initialize(2, 2);
    inventory.entries[0].source_now_ms = inventory.entries[0].original_source_ms = 0;
    cerberus_ble_discovery_list_age_t age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), 0);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_EXACT && age.ms == 0);
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), UINT64_MAX);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_EXACT && age.ms == UINT64_MAX);
    inventory.entries[0].epoch = 0;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), 1000);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
    inventory.entries[0].epoch = 1; inventory.entries[0].source_now_ms = 1000;
    inventory.entries[0].original_source_ms = 1001;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), 2000);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
    inventory.entries[0].original_source_ms = 900;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), 999);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);

    companion[0].timing_flags = CERBERUS_DUAL_BLE_SEEN; /* Age does not require RSSI. */
    companion[0].request_started_ms = UINT64_C(0x100000000); companion[0].callback_age_ms = 7;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0), UINT64_C(0x100000005));
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UPPER && age.ms == 12);
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0), UINT32_MAX);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
    companion[0].request_started_ms = 0; companion[0].callback_age_ms = UINT32_MAX;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0), 0);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
    companion[0].callback_age_ms = 1;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0), UINT32_MAX);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0), UINT64_MAX);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
    companion[0].timing_flags = 0;
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0), 1);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);

    initialize(2, 2); inventory.current_mask = 0;
    for (unsigned i = 0; i < 2; ++i) {
        inventory.entries[i].source_now_ms = 10000;
        inventory.entries[i].original_source_ms = 10000U - i;
        companion[i].request_started_ms = 39999;
        companion[i].callback_age_ms = 29999U + i;
    }
    freeze_inputs();
    CHECK(order(2, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, true, 40000) == 2);
    expect(0, CERBERUS_BLE_DISCOVERY_LIST_S3, 0); expect(1, CERBERUS_BLE_DISCOVERY_LIST_C5, 0);
    age = cerberus_ble_discovery_list_age(&inventory, companion, 2, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 1), 40000);
    CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UPPER && age.ms == 30001); /* Not a STALE conclusion. */
    CHECK(order(2, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, false, 40000) == 4);
    CHECK(order(2, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, true, 40001) == 0);
    unchanged();
}

static void invalid_arguments(void)
{
    initialize(1, 1); freeze_inputs();
    CHECK(cerberus_ble_discovery_list_order(&inventory, companion, 1, CERBERUS_BLE_DISCOVERY_LIST_ALL,
        CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0, NULL) == 0);
    CHECK(cerberus_ble_discovery_list_order(&inventory, NULL, 1, CERBERUS_BLE_DISCOVERY_LIST_ALL,
        CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0, output.refs) == 0);
    for (unsigned count = 33; count <= UINT8_MAX; ++count)
        CHECK(order((uint8_t)count, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0) == 0);
    for (int value = -1; value <= 4; ++value) {
        if (value >= 0 && value < 3) continue;
        CHECK(order(1, (cerberus_ble_discovery_list_source_t)value, CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0) == 0);
        CHECK(order(1, CERBERUS_BLE_DISCOVERY_LIST_ALL, (cerberus_ble_discovery_list_sort_t)value, false, 0) == 0);
    }
    inventory.initialized = false;
    CHECK(order(1, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0) == 0);
    inventory.initialized = true; inventory.count = 33;
    CHECK(order(1, CERBERUS_BLE_DISCOVERY_LIST_ALL, CERBERUS_BLE_DISCOVERY_LIST_SIGNAL, false, 0) == 0);
    inventory.count = 1;
    for (unsigned receiver = 0; receiver <= 3; ++receiver) {
        for (unsigned index = 0; index <= UINT8_MAX; ++index) {
            cerberus_ble_discovery_list_ref_t item = ref(receiver, index);
            if ((receiver == 1 || receiver == 2) && index == 0) continue;
            CHECK(!cerberus_ble_discovery_list_ref_valid(&inventory, companion, 1, item));
            int8_t rssi = 77;
            CHECK(!cerberus_ble_discovery_list_rssi(&inventory, companion, 1, item, &rssi) && rssi == 77);
            char label[8]; memset(label, '!', sizeof(label));
            CHECK(cerberus_ble_discovery_list_label(&inventory, companion, 1, item, label, sizeof(label)) == 0);
            for (unsigned i = 0; i < sizeof(label); ++i) CHECK(label[i] == '!');
            cerberus_ble_discovery_list_age_t age = cerberus_ble_discovery_list_age(&inventory, companion, 1, item, 1000);
            CHECK(age.kind == CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN && !age.ms);
        }
    }
    CHECK(!cerberus_ble_discovery_list_ref_valid(NULL, companion, 1, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0)));
    CHECK(!cerberus_ble_discovery_list_ref_valid(&inventory, NULL, 0, ref(CERBERUS_BLE_DISCOVERY_LIST_C5, 0)));
    CHECK(!cerberus_ble_discovery_list_rssi(&inventory, companion, 1, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), NULL));
    CHECK(!cerberus_ble_discovery_list_label(&inventory, companion, 1, ref(CERBERUS_BLE_DISCOVERY_LIST_S3, 0), NULL, 1));
    for (unsigned i = 0; i < 64; ++i) expect(i, 0xA5, 0xA5);
    unchanged();
}

/* Independent qsort oracle over generated, already-known scalar keys. It does
 * not call the production label/comparator/age helpers to obtain expectations. */
typedef struct { unsigned ordinal, source, index; char name[16]; int rssi; bool measured; uint64_t age; } oracle_row_t;
static unsigned oracle_sort;
static int oracle_compare(const void *left, const void *right)
{
    const oracle_row_t *a = left, *b = right;
    if (oracle_sort == CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER)
        return a->ordinal < b->ordinal ? -1 : a->ordinal > b->ordinal;
    if (oracle_sort == CERBERUS_BLE_DISCOVERY_LIST_SIGNAL) {
        if (a->measured != b->measured) return a->measured ? -1 : 1;
        if (a->measured && a->rssi != b->rssi) return a->rssi > b->rssi ? -1 : 1;
    }
    int names = strcmp(a->name, b->name);
    if (names) return names;
    return a->ordinal < b->ordinal ? -1 : a->ordinal > b->ordinal;
}
static void generated_cross_product(void)
{
    for (unsigned seed = 0; seed < 80; ++seed) {
        unsigned s3_count = seed % 33, c5_count = (seed * 7U) % 33;
        initialize(s3_count, c5_count);
        oracle_row_t all[64]; unsigned total = 0;
        for (unsigned receiver = 1; receiver <= 2; ++receiver) {
            unsigned count = receiver == 1 ? s3_count : c5_count;
            for (unsigned i = 0; i < count; ++i) {
                unsigned key = (seed + i * 11U + receiver) % 13U;
                oracle_row_t *item = &all[total];
                *item = (oracle_row_t){.ordinal = total, .source = receiver, .index = i,
                    .rssi = (int)(key * 7U) - 100, .measured = key % 4U != 0U, .age = key * 5000U};
                snprintf(item->name, sizeof(item->name), "device-%02u", key % 5U);
                if (receiver == 1) {
                    cerberus_s3_ble_discovery_entry_t *entry = &inventory.entries[i];
                    strcpy(entry->record.peer.name, item->name);
                    if (i % 2U) entry->record.peer.name[0] = 'D';
                    entry->record.peer.rssi = item->measured ? item->rssi : 127;
                    entry->original_source_ms = 100000U - item->age;
                    entry->source_now_ms = 100000;
                } else {
                    strcpy(companion[i].name, item->name);
                    if (i % 2U) companion[i].name[0] = 'D';
                    companion[i].rssi = item->rssi;
                    companion[i].timing_flags = CERBERUS_DUAL_BLE_SEEN |
                        (item->measured ? CERBERUS_DUAL_BLE_RSSI_VALID : 0);
                    companion[i].request_started_ms = 100000;
                    companion[i].callback_age_ms = item->age;
                }
                ++total;
            }
        }
        freeze_inputs();
        for (unsigned source = 0; source < 3; ++source) {
            for (unsigned sort = 0; sort < 3; ++sort) {
                for (unsigned recent = 0; recent < 2; ++recent) {
                    oracle_row_t expected[64]; unsigned count = 0;
                    for (unsigned i = 0; i < total; ++i)
                        if ((!source || all[i].source == source) && (!recent || all[i].age <= 30000))
                            expected[count++] = all[i];
                    oracle_sort = sort; qsort(expected, count, sizeof(expected[0]), oracle_compare);
                    CHECK(order(c5_count, source, sort, recent != 0, 100000) == count);
                    uint64_t seen = 0;
                    for (unsigned i = 0; i < count; ++i) {
                        expect(i, expected[i].source, expected[i].index);
                        unsigned bit = (output.refs[i].source - 1U) * 32U + output.refs[i].index;
                        CHECK(!(seen & (UINT64_C(1) << bit))); seen |= UINT64_C(1) << bit;
                    }
                    unchanged();
                }
            }
        }
    }
}

static void source_masks(void)
{
    initialize(32U,32U);
    for(unsigned i=0;i<32U;++i){
        strcpy(inventory.entries[i].record.peer.name,"Equal");
        strcpy(companion[i].name,"Equal");
        inventory.entries[i].record.peer.rssi=companion[i].rssi=-50;
        companion[i].timing_flags=CERBERUS_DUAL_BLE_RSSI_VALID;
    }
    freeze_inputs();
    const uint32_t masks[]={0U,UINT32_MAX,UINT32_C(0xaaaaaaaa),UINT32_C(0x55555555),UINT32_C(0x80000001)};
    for(unsigned source=0;source<3U;++source)for(unsigned sort=0;sort<3U;++sort)
        for(unsigned a=0;a<5U;++a)for(unsigned b=0;b<5U;++b){
            memset(&output,0xa5,sizeof(output));
            unsigned count=cerberus_ble_discovery_list_order_masked(&inventory,companion,32U,
                source,sort,masks[a],masks[b],output.refs),expected=0U;
            for(unsigned receiver=1;receiver<=2U;++receiver){
                if(source&&source!=receiver)continue;
                for(unsigned i=0;i<32U;++i)if((receiver==1?masks[a]:masks[b])&(UINT32_C(1)<<i))
                    expect(expected++,receiver,i);
            }
            CHECK(count==expected);
            for(unsigned i=count;i<64U;++i)expect(i,0xa5,0xa5);
            unchanged();
        }
}

#include "dual_ble_response_ui_fixture.h"
static void response_name_order(void)
{
    const uint8_t bytes[]={6,9,'H','C','-','0','5'};
    initialize(1,2);strcpy(inventory.entries[0].record.peer.name,"Bravo");
    for(unsigned i=0;i<2;++i){
        fixture_response_row(&companion[i],bytes,sizeof(bytes),0,1000);
        memcpy(companion[i].response.name,i?"Zulu":"Alpha",i?4:5);
        companion[i].response.name_length=i?4:5;
    }
    freeze_inputs();CHECK(order(2,CERBERUS_BLE_DISCOVERY_LIST_ALL,CERBERUS_BLE_DISCOVERY_LIST_NAME,false,1000)==3);
    expect(0,CERBERUS_BLE_DISCOVERY_LIST_C5,0);expect(1,CERBERUS_BLE_DISCOVERY_LIST_S3,0);expect(2,CERBERUS_BLE_DISCOVERY_LIST_C5,1);
    char name[33];CHECK(cerberus_ble_discovery_list_label(&inventory,companion,2,ref(2,0),name,sizeof(name))==5);
    CHECK(!strcmp(name,"Alpha"));unchanged();
    strcpy(companion[0].name,"Omega");freeze_inputs();
    CHECK(order(2,CERBERUS_BLE_DISCOVERY_LIST_ALL,CERBERUS_BLE_DISCOVERY_LIST_NAME,false,1000)==3);
    expect(0,1,0);expect(1,2,0);expect(2,2,1);unchanged();
    companion[0].name[0]=0;companion[0].response.original_sequence=0;
    CHECK(!cerberus_dual_ble_list_response_name(&companion[0]));
    CHECK(cerberus_ble_discovery_list_label(&inventory,companion,2,ref(2,0),name,sizeof(name))==17);
    CHECK(!strcmp(name,"46:45:44:43:42:41"));
}

int main(void)
{
    capacity_and_sources(); typed_identity_and_ties(); signal_semantics(); labels();
    ages_and_recent(); invalid_arguments(); generated_cross_product();source_masks();response_name_order();
    printf("BLE discovery list PASS: %u checks; 64 source refs, stable ordering, typed identity and exact/upper ages\n", checks);
    return 0;
}
