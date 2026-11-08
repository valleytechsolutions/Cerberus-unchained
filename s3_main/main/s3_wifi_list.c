#include "s3_wifi_list.h"

#include "wifi_scan_observation.h"
#include <string.h>

bool cerberus_s3_wifi_list_signal_valid(int rssi)
{
    return rssi >= -127 && rssi <= 0;
}

cerberus_s3_wifi_band_t cerberus_s3_wifi_list_channel_band(uint8_t channel)
{
    if (!cerberus_wifi_observation_channel_valid(channel)) return CERBERUS_S3_WIFI_BAND_ALL;
    return channel <= 14U ? CERBERUS_S3_WIFI_BAND_24_GHZ : CERBERUS_S3_WIFI_BAND_5_GHZ;
}

static unsigned char fold_ascii(unsigned char byte)
{
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte + ('a' - 'A')) : byte;
}

static unsigned char name_byte(const cerberus_c5_wifi_result_t *row, size_t index)
{
    if (row->ssid[0]) return (unsigned char)row->ssid[index];
    if (index >= 17U) return 0U;
    if (index % 3U == 2U) return ':';
    static const char hex[] = "0123456789ABCDEF";
    uint8_t byte = row->bssid[index / 3U];
    return (unsigned char)hex[index % 3U == 0U ? byte >> 4U : byte & 15U];
}

static bool name_before(const cerberus_c5_wifi_result_t *left,
    const cerberus_c5_wifi_result_t *right)
{
    /* Both arrays were bounded and validated before this comparison. */
    for (size_t i = 0U; i <= CERBERUS_WIFI_SSID_MAX_LENGTH; ++i) {
        unsigned char a = fold_ascii(name_byte(left, i));
        unsigned char b = fold_ascii(name_byte(right, i));
        if (a != b) return a < b;
        if (!a) return false;
    }
    return false;
}

static bool before(const cerberus_c5_wifi_result_t *left,
    const cerberus_c5_wifi_result_t *right, cerberus_s3_wifi_sort_t sort)
{
    if (sort == CERBERUS_S3_WIFI_SORT_NAME_ASC)
        return name_before(left, right);
    if (sort == CERBERUS_S3_WIFI_SORT_CHANNEL_ASC) {
        bool a = cerberus_wifi_observation_channel_valid(left->channel);
        bool b = cerberus_wifi_observation_channel_valid(right->channel);
        if (a != b) return a;
        if (a && left->channel != right->channel) return left->channel < right->channel;
        return name_before(left, right);
    }
    bool a = cerberus_s3_wifi_list_signal_valid(left->rssi);
    bool b = cerberus_s3_wifi_list_signal_valid(right->rssi);
    if (a != b) return a;
    if (a && left->rssi != right->rssi) return left->rssi > right->rssi;
    return name_before(left, right);
}

bool cerberus_s3_wifi_list_indexes(const cerberus_c5_wifi_result_t *rows,
    const cerberus_s3_wifi_observation_t *observations, size_t count,
    cerberus_s3_wifi_sort_t sort, cerberus_s3_wifi_band_t band,
    bool recent_only, uint64_t now_us, uint8_t *indexes, size_t capacity,
    size_t *matched)
{
    if (!matched) return false;
    *matched = 0U;
    if (count > CERBERUS_WIFI_SCAN_MAX_RESULTS ||
        (count && (!rows || (recent_only && !observations))) ||
        (capacity && !indexes) || (unsigned)sort >= CERBERUS_S3_WIFI_SORT_COUNT ||
        (unsigned)band >= CERBERUS_S3_WIFI_BAND_COUNT) return false;

    for (size_t i = 0U; i < count; ++i)
        if (!memchr(rows[i].ssid, '\0', sizeof(rows[i].ssid)) ||
            rows[i].security > CERBERUS_WIFI_SECURITY_OTHER) return false;

    uint8_t ordered[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    size_t used = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (band != CERBERUS_S3_WIFI_BAND_ALL &&
            cerberus_s3_wifi_list_channel_band(rows[i].channel) != band)
            continue;
        if (recent_only && !cerberus_s3_wifi_observation_recent(&observations[i], now_us))
            continue;
        size_t position = used;
        while (position && before(&rows[i], &rows[ordered[position - 1U]], sort)) {
            ordered[position] = ordered[position - 1U];
            --position;
        }
        ordered[position] = (uint8_t)i;
        ++used;
    }
    size_t copied = used < capacity ? used : capacity;
    if (copied) memcpy(indexes, ordered, copied);
    *matched = used;
    return true;
}
