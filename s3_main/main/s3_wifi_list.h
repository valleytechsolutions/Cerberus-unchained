#pragma once

#include "c5_spi_link.h"
#include "s3_wifi_observation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CERBERUS_S3_WIFI_SORT_SIGNAL_DESC = 0,
    CERBERUS_S3_WIFI_SORT_NAME_ASC,
    CERBERUS_S3_WIFI_SORT_CHANNEL_ASC,
    CERBERUS_S3_WIFI_SORT_COUNT,
} cerberus_s3_wifi_sort_t;

typedef enum {
    CERBERUS_S3_WIFI_BAND_ALL = 0,
    CERBERUS_S3_WIFI_BAND_24_GHZ,
    CERBERUS_S3_WIFI_BAND_5_GHZ,
    CERBERUS_S3_WIFI_BAND_COUNT,
} cerberus_s3_wifi_band_t;

/* Match the existing Wi-Fi observation convention. Positive values and -128
 * are unavailable, never stronger than an admitted measurement. */
bool cerberus_s3_wifi_list_signal_valid(int rssi);
/* ALL denotes unknown here; it is never evidence of a received band. */
cerberus_s3_wifi_band_t cerberus_s3_wifi_list_channel_band(uint8_t channel);

/* Build a presentation-only map for one immutable completed AP snapshot.
 * No transport, RF, allocation or task operations; at most 32 source rows.
 * Matching returns the total count; capacity limits the copied sorted prefix.
 * A zero-capacity call may use indexes=NULL. count=0 permits rows=NULL.
 * Observations are required only for a nonempty recent-only request.
 *
 * Band filtering recognizes only the supported 2.4/5 GHz channel plan. An
 * unknown channel is retained in ALL and sorted after recognized channels;
 * it is never guessed into a band. Recent filtering retains the existing
 * conservative request-start/source-age predicate without refreshing age.
 *
 * Names use ASCII case folding of unsanitized received bytes before NUL;
 * an empty name uses the BSSID's hexadecimal colon-delimited display string.
 * This is received-string ordering, not full binary
 * on-air SSID identity: the SDK/wire format already truncates embedded NUL.
 * Signal/channel ties use this name ordering, matching upstream TargetListSort.
 * Equal name keys retain original source order. Unknown signals/channels sort
 * after measurements/recognized channels, then by the same name key.
 *
 * Invalid arguments, security or unterminated names fail atomically: matched
 * becomes zero and indexes are unchanged. All rows are validated even when
 * filtered out. Outputs must not overlap each other or either input bank.
 * The existing cerberus_s3_wifi_observation_indexes API remains unchanged. */
bool cerberus_s3_wifi_list_indexes(const cerberus_c5_wifi_result_t *rows,
    const cerberus_s3_wifi_observation_t *observations, size_t count,
    cerberus_s3_wifi_sort_t sort, cerberus_s3_wifi_band_t band,
    bool recent_only, uint64_t now_us, uint8_t *indexes, size_t capacity,
    size_t *matched);
