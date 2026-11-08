#pragma once
#include "wifi_archive.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Offline interchange only. These records have UNKNOWN receiver, generation,
 * clock, coverage and acquisition provenance; they are never live targets.
 * Bounds are Cerberus resource limits, not upstream or Wi-Fi format maxima. */
enum {
    CERBERUS_WIFI_MARAUDER_MAX_RECORDS = 64,
    CERBERUS_WIFI_MARAUDER_SSID_CAPACITY = 32,
    CERBERUS_WIFI_MARAUDER_MAN_CAPACITY = 64,
    CERBERUS_WIFI_MARAUDER_MAX_STATIONS = 256,
    /* Worst canonical in-bound model is52,224 bytes: all64 full escaped
     * names/manufacturer strings and all256 stations in one row. */
    CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES = 65536,
    CERBERUS_WIFI_MARAUDER_SSID_MAX_BYTES = 2112,
    CERBERUS_WIFI_MARAUDER_MAX_DEPTH = 12,
};
typedef enum {
    CERBERUS_WIFI_MARAUDER_OK = 0,
    CERBERUS_WIFI_MARAUDER_INVALID,
    CERBERUS_WIFI_MARAUDER_LIMIT,
    CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE,
    CERBERUS_WIFI_MARAUDER_NO_ELIGIBLE,
} cerberus_wifi_marauder_result_t;
enum {
    CERBERUS_WIFI_MARAUDER_AP_ESSID = 1U << 0,
    CERBERUS_WIFI_MARAUDER_AP_CHANNEL = 1U << 1,
    CERBERUS_WIFI_MARAUDER_AP_BSSID = 1U << 2,
    CERBERUS_WIFI_MARAUDER_AP_RSSI = 1U << 3,
    CERBERUS_WIFI_MARAUDER_AP_PACKETS = 1U << 4,
    CERBERUS_WIFI_MARAUDER_AP_SEC = 1U << 5,
    CERBERUS_WIFI_MARAUDER_AP_WPS = 1U << 6,
    CERBERUS_WIFI_MARAUDER_AP_MAN = 1U << 7,
    CERBERUS_WIFI_MARAUDER_AP_STATIONS = 1U << 8,
    CERBERUS_WIFI_MARAUDER_AP_ALL = 511U,
};
typedef struct {
    uint32_t packets;
    int32_t rssi; /* Imported integer, not necessarily a valid RF measurement. */
    uint16_t present, station_offset, station_count;
    uint8_t essid_length, man_length;
    uint8_t essid[32], man[64]; /* Decoded UTF-8 bytes; NUL/control bytes retained. */
    uint8_t bssid[6]; /* Display/network order, not BLE reversed byte order. */
    uint8_t channel, security; /* Raw upstream values; no live admission meaning. */
    bool wps;
} cerberus_wifi_marauder_ap_row_t;
typedef struct {
    uint16_t station_count;
    uint16_t ignored_members; /* Validated unknown JSON members not retained. */
    uint8_t count;
    cerberus_wifi_marauder_ap_row_t rows[64];
    uint8_t stations[256][6]; /* Contiguous slices; duplicates/order preserved. */
} cerberus_wifi_marauder_ap_t;
typedef struct { uint8_t length, bytes[32]; } cerberus_wifi_marauder_ssid_row_t;
typedef struct {
    uint8_t count;
    bool final_lf;
    cerberus_wifi_marauder_ssid_row_t rows[64];
} cerberus_wifi_marauder_ssid_t;
enum {
    CERBERUS_WIFI_MARAUDER_LOSS_PROVENANCE = 1U << 0,
    CERBERUS_WIFI_MARAUDER_LOSS_TIMING = 1U << 1,
    CERBERUS_WIFI_MARAUDER_LOSS_COVERAGE = 1U << 2,
    CERBERUS_WIFI_MARAUDER_LOSS_AP_DETAILS = 1U << 3,
    CERBERUS_WIFI_MARAUDER_LOSS_EXTENSIONS = 1U << 4,
};
typedef struct {
    uint32_t loss_flags;
    uint16_t records, security_unmapped, rssi_omitted, names_absent;
} cerberus_wifi_marauder_export_stats_t;

/* All active strings have canonical zero tails; absent fields are zero.
 * JSON strings are strict Unicode/UTF-8, including ignored members. Known
 * schema duplicates reject except equal packet/packets aliases. Canonical
 * encoding emits BOTH aliases when the counter is present: pinned Marauder's
 * saver uses packets but its loader reads packet. Unknown fields are counted
 * and discarded explicitly. Channel/security/RSSI do not acquire RF validity.
 * No heap/clock/filesystem/radio. Every failed output is unchanged; encode
 * sets disjoint *written=0. All input/output/stats regions must not overlap. */
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_validate(const cerberus_wifi_marauder_ap_t *document);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_decode(const uint8_t *input, size_t length,
    cerberus_wifi_marauder_ap_t *document);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_encode(const cerberus_wifi_marauder_ap_t *document,
    uint8_t *output, size_t capacity, size_t *written);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ap_from_archive(const cerberus_wifi_archive_t *archive,
    cerberus_wifi_marauder_ap_t *document, cerberus_wifi_marauder_export_stats_t *stats);

/* SSID text splits ONLY LF, matching upstream readStringUntil('\n'). CR, NUL,
 * non-UTF8, empty rows and duplicate names remain payload. final_lf preserves
 * framing: empty file is zero rows; "a\n" is one row with final_lf; "\n" is
 * one empty row with final_lf. A final empty row requires final_lf. No trim or
 * CRLF normalization. Embedded LF in a model/native name is unrepresentable.
 * Native export retains all names including empty and sets final_lf for any
 * nonempty list. AP conversion omits ABSENT essid (names_absent), not empty. */
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_validate(const cerberus_wifi_marauder_ssid_t *document);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_decode(const uint8_t *input, size_t length,
    cerberus_wifi_marauder_ssid_t *document);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_encode(const cerberus_wifi_marauder_ssid_t *document,
    uint8_t *output, size_t capacity, size_t *written);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_from_archive(const cerberus_wifi_archive_t *archive,
    cerberus_wifi_marauder_ssid_t *document, cerberus_wifi_marauder_export_stats_t *stats);
cerberus_wifi_marauder_result_t cerberus_wifi_marauder_ssid_from_ap(const cerberus_wifi_marauder_ap_t *ap,
    cerberus_wifi_marauder_ssid_t *document, cerberus_wifi_marauder_export_stats_t *stats);

/* Maps only native OPEN/WEP/WPA/WPA2/WPA3/WPA-WPA2 exactly. Unsupported mixed,
 * OWE, collapsed enterprise or OTHER map to upstream UNKNOWN255, never an
 * invented narrower authentication mode. false means the mapping lost detail. */
bool cerberus_wifi_marauder_security_from_archive(uint8_t native_security, uint8_t *upstream_security);
