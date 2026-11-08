#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS 32U
#define CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS 42U
#define CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS 250U

enum {
    CERBERUS_WIFI_CAPTURE_PLAN_BOTH,
    CERBERUS_WIFI_CAPTURE_PLAN_2G,
    CERBERUS_WIFI_CAPTURE_PLAN_5G,
};

/* Same channel-bit mapping as Packet Count. These are candidate channels;
 * the configured country and driver's successful tune determine coverage. */
typedef struct {
    uint64_t channel_mask;
    uint16_t dwell_ms;
    uint8_t target_count;
    uint8_t bssids[CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS][6];
} cerberus_wifi_capture_plan_t;

void cerberus_wifi_capture_plan_clear(cerberus_wifi_capture_plan_t *);
uint64_t cerberus_wifi_capture_plan_band_mask(uint8_t band);
uint8_t cerberus_wifi_capture_plan_channel(unsigned index);
uint64_t cerberus_wifi_capture_plan_channel_bit(uint8_t channel);
bool cerberus_wifi_capture_plan_mask_valid(uint64_t mask);
bool cerberus_wifi_capture_plan_valid(const cerberus_wifi_capture_plan_t *);
/* Adds a unique BSSID and unions its observed channel hint. Repeated BSSIDs
 * on different channels keep one target. Failure never modifies the plan. */
bool cerberus_wifi_capture_plan_add_target(cerberus_wifi_capture_plan_t *,
    const uint8_t bssid[6], uint8_t channel);
bool cerberus_wifi_capture_plan_matches(const cerberus_wifi_capture_plan_t *,
    const uint8_t bssid[6], uint8_t channel);
