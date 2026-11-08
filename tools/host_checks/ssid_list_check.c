#include "ssid_list.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(expression) do { ++checks; assert(expression); } while (0)
#define UNCHANGED(expression, expected) do { \
    cerberus_ssid_list_t before = list; \
    CHECK((expression) == (expected)); \
    CHECK(!memcmp(&list, &before, sizeof(list))); \
} while (0)

static cerberus_ssid_list_t list;
static cerberus_wifi_marauder_ssid_t document, saved;

static void add_byte(uint8_t value)
{ CHECK(cerberus_ssid_list_add(&list, &value, 1U) == CERBERUS_SSID_LIST_OK); }

static void row(cerberus_wifi_marauder_ssid_t *doc, unsigned index,
    const uint8_t *bytes, size_t length)
{
    memset(&doc->rows[index], 0, sizeof(doc->rows[index]));
    doc->rows[index].length = (uint8_t)length;
    if (length) memcpy(doc->rows[index].bytes, bytes, length);
}

static void roundtrip(void)
{
    uint8_t bytes[2112];
    size_t written = SIZE_MAX;
    uint64_t revision = 0U;
    CHECK(cerberus_ssid_list_copy(&list, &saved, &revision) == CERBERUS_SSID_LIST_OK);
    CHECK(revision == list.revision);
    CHECK(cerberus_wifi_marauder_ssid_encode(&saved, bytes, sizeof(bytes), &written) == CERBERUS_WIFI_MARAUDER_OK);
    CHECK(cerberus_wifi_marauder_ssid_decode(bytes, written, &document) == CERBERUS_WIFI_MARAUDER_OK);
    CHECK(!memcmp(&document, &saved, sizeof(document)));
}

static void names_and_framing(void)
{
    cerberus_ssid_list_init(NULL);
    cerberus_ssid_list_init(&list);
    CHECK(cerberus_ssid_list_valid(&list) && list.revision == 1U && !list.dirty);
    UNCHANGED(cerberus_ssid_list_clear(&list), CERBERUS_SSID_LIST_OK);
    UNCHANGED(cerberus_ssid_list_remove_selected(&list), CERBERUS_SSID_LIST_OK);
    UNCHANGED(cerberus_ssid_list_select_all(&list, true), CERBERUS_SSID_LIST_OK);
    CHECK(cerberus_ssid_list_add(&list, NULL, 0U) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.count == 1U && list.document.final_lf && list.dirty && list.revision == 2U);
    uint8_t binary[] = {'A', 0, '\r', 0xffU, 0x80U, 1U, '\t'};
    CHECK(cerberus_ssid_list_add(&list, binary, sizeof(binary)) == CERBERUS_SSID_LIST_OK);
    CHECK(!memcmp(list.document.rows[1].bytes, binary, sizeof(binary)));
    uint8_t maximum[32]; memset(maximum, 0xfe, sizeof(maximum));
    CHECK(cerberus_ssid_list_add(&list, maximum, sizeof(maximum)) == CERBERUS_SSID_LIST_OK);
    roundtrip();
    UNCHANGED(cerberus_ssid_list_add(&list, maximum, 33U), CERBERUS_SSID_LIST_LIMIT);
    UNCHANGED(cerberus_ssid_list_add(&list, NULL, 1U), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_add(&list, (const uint8_t *)"x\ny", 3U), CERBERUS_SSID_LIST_UNREPRESENTABLE);
    UNCHANGED(cerberus_ssid_list_edit(&list, 1U, binary, sizeof(binary)), CERBERUS_SSID_LIST_OK);
    UNCHANGED(cerberus_ssid_list_edit(&list, 64U, binary, 1U), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_edit(&list, 1U, (const uint8_t *)"\n", 1U), CERBERUS_SSID_LIST_UNREPRESENTABLE);
    CHECK(cerberus_ssid_list_add(&list, list.document.rows[1].bytes, list.document.rows[1].length) == CERBERUS_SSID_LIST_OK);
    CHECK(!memcmp(&list.document.rows[1], &list.document.rows[3], sizeof(list.document.rows[0])));
    CHECK(cerberus_ssid_list_edit(&list, 1U, list.document.rows[1].bytes + 1U, 3U) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.rows[1].length == 3U && !memcmp(list.document.rows[1].bytes, binary + 1U, 3U));
    for (unsigned i = 3U; i < 32U; ++i) CHECK(!list.document.rows[1].bytes[i]);
    roundtrip();
    CHECK(cerberus_wifi_marauder_ssid_decode((const uint8_t *)"one\r\ntwo", 8U, &document) == CERBERUS_WIFI_MARAUDER_OK);
    CHECK(cerberus_ssid_list_replace(&list, &document) == CERBERUS_SSID_LIST_OK);
    CHECK(!list.document.final_lf && list.document.rows[0].bytes[3] == '\r');
    UNCHANGED(cerberus_ssid_list_edit(&list, 1U, (const uint8_t *)"two", 3U), CERBERUS_SSID_LIST_OK);
    CHECK(cerberus_ssid_list_edit(&list, 1U, NULL, 0U) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.final_lf);
    roundtrip();
}

static void selection_and_removal(void)
{
    for (unsigned removed = 0U; removed < 64U; ++removed) {
        cerberus_ssid_list_init(&list);
        for (unsigned i = 0U; i < 64U; ++i) {
            add_byte((uint8_t)(i + 32U));
            if (i % 3U == 0U || i == 63U)
                CHECK(cerberus_ssid_list_select(&list, (uint8_t)i, true) == CERBERUS_SSID_LIST_OK);
        }
        UNCHANGED(cerberus_ssid_list_add(&list, NULL, 0U), CERBERUS_SSID_LIST_LIMIT);
        uint64_t original = list.selected;
        CHECK(cerberus_ssid_list_remove(&list, (uint8_t)removed) == CERBERUS_SSID_LIST_OK);
        CHECK(list.document.count == 63U && cerberus_ssid_list_valid(&list));
        unsigned dst = 0U;
        for (unsigned src = 0U; src < 64U; ++src) {
            if (src == removed) continue;
            CHECK(list.document.rows[dst].length == 1U && list.document.rows[dst].bytes[0] == src + 32U);
            CHECK(((list.selected >> dst) & 1U) == ((original >> src) & 1U));
            ++dst;
        }
        cerberus_wifi_marauder_ssid_row_t zero = {0};
        CHECK(!memcmp(&list.document.rows[63], &zero, sizeof(zero)));
    }
    cerberus_ssid_list_init(&list);
    for (unsigned i = 0U; i < 64U; ++i) add_byte((uint8_t)(i + 32U));
    CHECK(cerberus_ssid_list_select_all(&list, true) == CERBERUS_SSID_LIST_OK && list.selected == UINT64_MAX);
    for (unsigned i = 1U; i < 64U; i += 2U)
        CHECK(cerberus_ssid_list_select(&list, (uint8_t)i, false) == CERBERUS_SSID_LIST_OK);
    CHECK(cerberus_ssid_list_remove_selected(&list) == CERBERUS_SSID_LIST_OK);
    CHECK(!list.selected && list.document.count == 32U);
    for (unsigned i = 0U; i < 32U; ++i) CHECK(list.document.rows[i].bytes[0] == 33U + 2U * i);
    CHECK(cerberus_ssid_list_select_all(&list, true) == CERBERUS_SSID_LIST_OK);
    CHECK(cerberus_ssid_list_remove_selected(&list) == CERBERUS_SSID_LIST_OK);
    CHECK(!list.document.count && !list.document.final_lf && !list.selected);
    UNCHANGED(cerberus_ssid_list_remove(&list, 0U), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_select(&list, 0U, true), CERBERUS_SSID_LIST_INVALID);
}

static void imports(void)
{
    cerberus_ssid_list_init(&list);
    add_byte('A');
    CHECK(cerberus_ssid_list_select(&list, 0U, true) == CERBERUS_SSID_LIST_OK);
    const uint8_t raw[] = {'A','\n',0,'\n','B','\n',0,'\n','\n','B','\n'};
    CHECK(cerberus_wifi_marauder_ssid_decode(raw, sizeof(raw), &document) == CERBERUS_WIFI_MARAUDER_OK);
    CHECK(cerberus_ssid_list_append(&list, &document, true) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.count == 4U && list.selected == 1U);
    CHECK(list.document.rows[1].length == 1U && !list.document.rows[1].bytes[0]);
    CHECK(list.document.rows[2].bytes[0] == 'B' && !list.document.rows[3].length);
    UNCHANGED(cerberus_ssid_list_append(&list, &document, true), CERBERUS_SSID_LIST_OK);
    CHECK(cerberus_ssid_list_append(&list, &document, false) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.count == 10U && list.selected == 1U);
    CHECK(!memcmp(list.document.rows + 4U, document.rows, 6U * sizeof(document.rows[0])));
    roundtrip();
    document = list.document;
    document.rows[0].length = 33U;
    UNCHANGED(cerberus_ssid_list_replace(&list, &document), CERBERUS_SSID_LIST_LIMIT);
    UNCHANGED(cerberus_ssid_list_append(&list, &document, false), CERBERUS_SSID_LIST_LIMIT);
    document = list.document; document.rows[0].bytes[0] = '\n';
    UNCHANGED(cerberus_ssid_list_append(&list, &document, true), CERBERUS_SSID_LIST_UNREPRESENTABLE);
    document = list.document; document.rows[0].bytes[31] = 1U;
    UNCHANGED(cerberus_ssid_list_replace(&list, &document), CERBERUS_SSID_LIST_INVALID);
    document = list.document; document.count = 65U;
    UNCHANGED(cerberus_ssid_list_replace(&list, &document), CERBERUS_SSID_LIST_LIMIT);
    UNCHANGED(cerberus_ssid_list_replace(&list, &list.document), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_append(&list, &list.document, false), CERBERUS_SSID_LIST_INVALID);
    memset(&document, 0, sizeof(document)); document.count = 1U; document.final_lf = true;
    row(&document, 0U, (const uint8_t *)"Z", 1U);
    memset(document.rows + 1U, 0xa5, 63U * sizeof(document.rows[0]));
    CHECK(cerberus_ssid_list_replace(&list, &document) == CERBERUS_SSID_LIST_OK);
    CHECK(!list.selected && list.document.count == 1U);
    for (unsigned i = 1U; i < 64U; ++i) CHECK(!list.document.rows[i].length && !list.document.rows[i].bytes[0]);
    UNCHANGED(cerberus_ssid_list_replace(&list, &document), CERBERUS_SSID_LIST_OK);
    for (unsigned i = 1U; i < 63U; ++i) add_byte('Z');
    memset(&document, 0, sizeof(document)); document.count = 2U; document.final_lf = true;
    row(&document, 0U, (const uint8_t *)"Y", 1U); row(&document, 1U, (const uint8_t *)"X", 1U);
    UNCHANGED(cerberus_ssid_list_append(&list, &document, false), CERBERUS_SSID_LIST_LIMIT);
    UNCHANGED(cerberus_ssid_list_append(&list, &document, true), CERBERUS_SSID_LIST_LIMIT);
    row(&document, 0U, (const uint8_t *)"Z", 1U);
    CHECK(cerberus_ssid_list_append(&list, &document, true) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.count == 64U && list.document.rows[63].bytes[0] == 'X');
    UNCHANGED(cerberus_ssid_list_append(&list, &document, true), CERBERUS_SSID_LIST_OK);
    roundtrip();
}

static void snapshots_and_generation(void)
{
    cerberus_ssid_list_init(&list);
    uint8_t entropy[256]; for (unsigned i = 0U; i < sizeof(entropy); ++i) entropy[i] = (uint8_t)i;
    UNCHANGED(cerberus_ssid_list_generate(&list, 0U, NULL, 0U), CERBERUS_SSID_LIST_OK);
    UNCHANGED(cerberus_ssid_list_generate(&list, SIZE_MAX, entropy, sizeof(entropy)), CERBERUS_SSID_LIST_LIMIT);
    UNCHANGED(cerberus_ssid_list_generate(&list, 1U, NULL, 4U), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_generate(&list, 1U, entropy, 3U), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_generate(&list, 1U, (const uint8_t *)&list, 4U), CERBERUS_SSID_LIST_INVALID);
    CHECK(cerberus_ssid_list_generate(&list, 64U, entropy, sizeof(entropy)) == CERBERUS_SSID_LIST_OK);
    CHECK(list.document.count == 64U && list.revision == 2U);
    CHECK(!memcmp(list.document.rows[0].bytes, "CERB-00010203", 13U));
    CHECK(!memcmp(list.document.rows[63].bytes, "CERB-FCFDFEFF", 13U));
    for (unsigned i = 0U; i < 64U; ++i) {
        CHECK(list.document.rows[i].length == 13U);
        for (unsigned j = 0U; j < 13U; ++j)
            CHECK(list.document.rows[i].bytes[j] >= 32U && list.document.rows[i].bytes[j] <= 126U);
    }
    roundtrip();
    uint64_t revision;
    CHECK(cerberus_ssid_list_copy(&list, &saved, &revision) == CERBERUS_SSID_LIST_OK);
    cerberus_wifi_marauder_ssid_t frozen = saved;
    CHECK(cerberus_ssid_list_edit(&list, 0U, (const uint8_t *)"changed", 7U) == CERBERUS_SSID_LIST_OK);
    CHECK(!memcmp(&saved, &frozen, sizeof(saved)));
    UNCHANGED(cerberus_ssid_list_saved(&list, revision), CERBERUS_SSID_LIST_STALE);
    revision = list.revision;
    CHECK(cerberus_ssid_list_saved(&list, revision) == CERBERUS_SSID_LIST_OK);
    CHECK(!list.dirty && list.revision == revision + 1U);
    UNCHANGED(cerberus_ssid_list_saved(&list, revision), CERBERUS_SSID_LIST_STALE);
    UNCHANGED(cerberus_ssid_list_saved(&list, list.revision), CERBERUS_SSID_LIST_OK);
    revision = list.revision;
    CHECK(cerberus_ssid_list_select(&list, 63U, true) == CERBERUS_SSID_LIST_OK);
    CHECK(!list.dirty && list.revision == revision + 1U);
    UNCHANGED(cerberus_ssid_list_saved(&list, revision), CERBERUS_SSID_LIST_STALE);
    CHECK(cerberus_ssid_list_select_all(&list, false) == CERBERUS_SSID_LIST_OK && !list.dirty);
    CHECK(cerberus_ssid_list_clear(&list) == CERBERUS_SSID_LIST_OK && list.dirty);
    CHECK(cerberus_ssid_list_generate(&list, 2U, entropy, 8U) == CERBERUS_SSID_LIST_OK);
    memset(&saved, 0x5a, sizeof(saved)); frozen = saved; revision = 99U;
    CHECK(cerberus_ssid_list_copy(NULL, &saved, &revision) == CERBERUS_SSID_LIST_INVALID);
    CHECK(!memcmp(&saved, &frozen, sizeof(saved)) && revision == 99U);
    UNCHANGED(cerberus_ssid_list_copy(&list, &list.document, &revision), CERBERUS_SSID_LIST_INVALID);
    UNCHANGED(cerberus_ssid_list_copy(&list, &saved, &list.revision), CERBERUS_SSID_LIST_INVALID);
    CHECK(cerberus_ssid_list_copy(&list, &saved, (uint64_t *)(void *)&saved) == CERBERUS_SSID_LIST_INVALID);
    CHECK(!memcmp(&saved, &frozen, sizeof(saved)));
}

static void exhaustion_and_corruption(void)
{
    cerberus_ssid_list_init(&list); add_byte('A'); add_byte('B');
    CHECK(cerberus_ssid_list_select(&list, 0U, true) == CERBERUS_SSID_LIST_OK);
    memset(&document, 0, sizeof(document)); document.count = 1U; document.final_lf = true;
    row(&document, 0U, (const uint8_t *)"C", 1U);
    uint8_t entropy[4] = {0}; list.revision = UINT64_MAX;
    UNCHANGED(cerberus_ssid_list_add(&list, NULL, 0U), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_edit(&list, 0U, NULL, 0U), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_remove(&list, 0U), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_remove_selected(&list), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_clear(&list), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_select(&list, 1U, true), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_select_all(&list, true), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_replace(&list, &document), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_append(&list, &document, false), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_generate(&list, 1U, entropy, 4U), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_saved(&list, UINT64_MAX), CERBERUS_SSID_LIST_EXHAUSTED);
    UNCHANGED(cerberus_ssid_list_edit(&list, 0U, (const uint8_t *)"A", 1U), CERBERUS_SSID_LIST_OK);
    UNCHANGED(cerberus_ssid_list_select(&list, 0U, true), CERBERUS_SSID_LIST_OK);
    list.revision = 0U;
    CHECK(!cerberus_ssid_list_valid(&list));
    UNCHANGED(cerberus_ssid_list_clear(&list), CERBERUS_SSID_LIST_INVALID);
    list.revision = 1U; list.selected = UINT64_C(1) << 63U;
    CHECK(!cerberus_ssid_list_valid(&list));
    list.selected = 0U; list.document.count = 65U;
    CHECK(!cerberus_ssid_list_valid(&list));
    cerberus_ssid_list_init(&list);
    *(unsigned char *)&list.dirty = 2U;
    CHECK(!cerberus_ssid_list_valid(&list));
}

int main(void)
{
    names_and_framing(); selection_and_removal(); imports();
    snapshots_and_generation(); exhaustion_and_corruption();
    printf("SSID list: %u checks; %zu-byte model; binary names, atomic edits, selection and exact save revisions\n",
        checks, sizeof(list));
    return 0;
}
