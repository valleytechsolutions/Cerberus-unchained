#include "ssid_list.h"
#include <string.h>

static uint64_t row_mask(unsigned count)
{ return count == 64U ? UINT64_MAX : (UINT64_C(1) << count) - 1U; }

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    if (!an || !bn) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y ||
        (x < y + bn && y < x + an);
}

static cerberus_ssid_list_result_t document_result(const cerberus_wifi_marauder_ssid_t *document)
{
    switch (cerberus_wifi_marauder_ssid_validate(document)) {
    case CERBERUS_WIFI_MARAUDER_OK: return CERBERUS_SSID_LIST_OK;
    case CERBERUS_WIFI_MARAUDER_LIMIT: return CERBERUS_SSID_LIST_LIMIT;
    case CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE: return CERBERUS_SSID_LIST_UNREPRESENTABLE;
    default: return CERBERUS_SSID_LIST_INVALID;
    }
}

void cerberus_ssid_list_init(cerberus_ssid_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
    list->revision = 1U;
}

bool cerberus_ssid_list_valid(const cerberus_ssid_list_t *list)
{
    if (!list || !list->revision || *(const unsigned char *)&list->dirty > 1U ||
        document_result(&list->document) != CERBERUS_SSID_LIST_OK) return false;
    return !(list->selected & ~row_mask(list->document.count));
}

static cerberus_ssid_list_result_t name_copy(cerberus_wifi_marauder_ssid_row_t *row,
    const uint8_t *bytes, size_t length)
{
    if (length > CERBERUS_WIFI_MARAUDER_SSID_CAPACITY) return CERBERUS_SSID_LIST_LIMIT;
    if (!bytes && length) return CERBERUS_SSID_LIST_INVALID;
    if (length && memchr(bytes, '\n', length)) return CERBERUS_SSID_LIST_UNREPRESENTABLE;
    memset(row, 0, sizeof(*row));
    row->length = (uint8_t)length;
    if (length) memcpy(row->bytes, bytes, length);
    return CERBERUS_SSID_LIST_OK;
}

static bool same_name(const cerberus_wifi_marauder_ssid_row_t *a,
    const cerberus_wifi_marauder_ssid_row_t *b)
{ return a->length == b->length && !memcmp(a->bytes, b->bytes, a->length); }

static bool contains(const cerberus_wifi_marauder_ssid_t *document, unsigned count,
    const cerberus_wifi_marauder_ssid_row_t *row)
{
    for (unsigned i = 0; i < count; ++i)
        if (same_name(&document->rows[i], row)) return true;
    return false;
}

static void changed(cerberus_ssid_list_t *list, bool content)
{
    ++list->revision;
    if (content) list->dirty = true;
}

cerberus_ssid_list_result_t cerberus_ssid_list_add(cerberus_ssid_list_t *list,
    const uint8_t *bytes, size_t length)
{
    if (!cerberus_ssid_list_valid(list)) return CERBERUS_SSID_LIST_INVALID;
    cerberus_wifi_marauder_ssid_row_t row;
    cerberus_ssid_list_result_t result = name_copy(&row, bytes, length);
    if (result) return result;
    if (list->document.count == CERBERUS_WIFI_MARAUDER_MAX_RECORDS) return CERBERUS_SSID_LIST_LIMIT;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    list->document.rows[list->document.count++] = row;
    list->document.final_lf = true;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_edit(cerberus_ssid_list_t *list,
    uint8_t index, const uint8_t *bytes, size_t length)
{
    if (!cerberus_ssid_list_valid(list) || index >= list->document.count) return CERBERUS_SSID_LIST_INVALID;
    cerberus_wifi_marauder_ssid_row_t row;
    cerberus_ssid_list_result_t result = name_copy(&row, bytes, length);
    if (result) return result;
    if (same_name(&list->document.rows[index], &row)) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    list->document.rows[index] = row;
    list->document.final_lf = true;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_remove(cerberus_ssid_list_t *list, uint8_t index)
{
    if (!cerberus_ssid_list_valid(list) || index >= list->document.count) return CERBERUS_SSID_LIST_INVALID;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    unsigned remaining = --list->document.count - index;
    memmove(list->document.rows + index, list->document.rows + index + 1U,
        remaining * sizeof(list->document.rows[0]));
    memset(list->document.rows + list->document.count, 0, sizeof(list->document.rows[0]));
    uint64_t above = index == 63U ? 0U : list->selected >> (index + 1U);
    list->selected = (list->selected & row_mask(index)) | (above << index);
    list->document.final_lf = list->document.count != 0U;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_remove_selected(cerberus_ssid_list_t *list)
{
    if (!cerberus_ssid_list_valid(list)) return CERBERUS_SSID_LIST_INVALID;
    if (!list->selected) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    unsigned retained = 0U, original = list->document.count;
    for (unsigned i = 0; i < original; ++i)
        if (!(list->selected & (UINT64_C(1) << i)))
            list->document.rows[retained++] = list->document.rows[i];
    memset(list->document.rows + retained, 0,
        (original - retained) * sizeof(list->document.rows[0]));
    list->document.count = (uint8_t)retained;
    list->document.final_lf = retained != 0U;
    list->selected = 0U;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_clear(cerberus_ssid_list_t *list)
{
    if (!cerberus_ssid_list_valid(list)) return CERBERUS_SSID_LIST_INVALID;
    if (!list->document.count) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    memset(&list->document, 0, sizeof(list->document));
    list->selected = 0U;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_select(cerberus_ssid_list_t *list,
    uint8_t index, bool selected)
{
    if (!cerberus_ssid_list_valid(list) || index >= list->document.count) return CERBERUS_SSID_LIST_INVALID;
    uint64_t bit = UINT64_C(1) << index;
    uint64_t mask = selected ? list->selected | bit : list->selected & ~bit;
    if (mask == list->selected) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    list->selected = mask;
    changed(list, false);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_select_all(cerberus_ssid_list_t *list, bool selected)
{
    if (!cerberus_ssid_list_valid(list)) return CERBERUS_SSID_LIST_INVALID;
    uint64_t mask = selected ? row_mask(list->document.count) : 0U;
    if (mask == list->selected) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    list->selected = mask;
    changed(list, false);
    return CERBERUS_SSID_LIST_OK;
}

static cerberus_ssid_list_result_t import_valid(const cerberus_ssid_list_t *list,
    const cerberus_wifi_marauder_ssid_t *document)
{
    if (!cerberus_ssid_list_valid(list) || !document ||
        overlaps(list, sizeof(*list), document, sizeof(*document))) return CERBERUS_SSID_LIST_INVALID;
    return document_result(document);
}

cerberus_ssid_list_result_t cerberus_ssid_list_replace(cerberus_ssid_list_t *list,
    const cerberus_wifi_marauder_ssid_t *document)
{
    cerberus_ssid_list_result_t result = import_valid(list, document);
    if (result) return result;
    bool content = list->document.count != document->count ||
        list->document.final_lf != document->final_lf ||
        memcmp(list->document.rows, document->rows, document->count * sizeof(document->rows[0]));
    if (!content && !list->selected) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    memset(&list->document, 0, sizeof(list->document));
    list->document.count = document->count;
    list->document.final_lf = document->final_lf;
    memcpy(list->document.rows, document->rows, document->count * sizeof(document->rows[0]));
    list->selected = 0U;
    changed(list, content);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_append(cerberus_ssid_list_t *list,
    const cerberus_wifi_marauder_ssid_t *document, bool unique)
{
    cerberus_ssid_list_result_t result = import_valid(list, document);
    if (result) return result;
    uint64_t accepted = 0U;
    unsigned count = 0U;
    for (unsigned i = 0; i < document->count; ++i) {
        if (unique && (contains(&list->document, list->document.count, &document->rows[i]) ||
            contains(document, i, &document->rows[i]))) continue;
        accepted |= UINT64_C(1) << i;
        ++count;
    }
    if (count > CERBERUS_WIFI_MARAUDER_MAX_RECORDS - list->document.count) return CERBERUS_SSID_LIST_LIMIT;
    if (!count) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    for (unsigned i = 0; i < document->count; ++i)
        if (accepted & (UINT64_C(1) << i)) list->document.rows[list->document.count++] = document->rows[i];
    list->document.final_lf = true;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_generate(cerberus_ssid_list_t *list,
    size_t count, const uint8_t *entropy, size_t length)
{
    if (!cerberus_ssid_list_valid(list)) return CERBERUS_SSID_LIST_INVALID;
    if (count > CERBERUS_WIFI_MARAUDER_MAX_RECORDS - list->document.count) return CERBERUS_SSID_LIST_LIMIT;
    if (length != count * CERBERUS_SSID_LIST_ENTROPY_PER_NAME || (!entropy && length) ||
        overlaps(list, sizeof(*list), entropy, length)) return CERBERUS_SSID_LIST_INVALID;
    if (!count) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0U; i < count; ++i) {
        cerberus_wifi_marauder_ssid_row_t *row = &list->document.rows[list->document.count++];
        memset(row, 0, sizeof(*row));
        memcpy(row->bytes, "CERB-", 5U);
        row->length = 13U;
        for (unsigned j = 0U; j < CERBERUS_SSID_LIST_ENTROPY_PER_NAME; ++j) {
            uint8_t byte = entropy[i * CERBERUS_SSID_LIST_ENTROPY_PER_NAME + j];
            row->bytes[5U + 2U * j] = (uint8_t)hex[byte >> 4U];
            row->bytes[6U + 2U * j] = (uint8_t)hex[byte & 15U];
        }
    }
    list->document.final_lf = true;
    changed(list, true);
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_copy(const cerberus_ssid_list_t *list,
    cerberus_wifi_marauder_ssid_t *document, uint64_t *revision)
{
    if (!cerberus_ssid_list_valid(list) || !document || !revision ||
        overlaps(list, sizeof(*list), document, sizeof(*document)) ||
        overlaps(list, sizeof(*list), revision, sizeof(*revision)) ||
        overlaps(document, sizeof(*document), revision, sizeof(*revision))) return CERBERUS_SSID_LIST_INVALID;
    *document = list->document;
    *revision = list->revision;
    return CERBERUS_SSID_LIST_OK;
}

cerberus_ssid_list_result_t cerberus_ssid_list_saved(cerberus_ssid_list_t *list, uint64_t revision)
{
    if (!cerberus_ssid_list_valid(list)) return CERBERUS_SSID_LIST_INVALID;
    if (revision != list->revision) return CERBERUS_SSID_LIST_STALE;
    if (!list->dirty) return CERBERUS_SSID_LIST_OK;
    if (list->revision == UINT64_MAX) return CERBERUS_SSID_LIST_EXHAUSTED;
    list->dirty = false;
    changed(list, false);
    return CERBERUS_SSID_LIST_OK;
}
