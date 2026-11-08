#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
/* Unrelated SDK excerpts leave the new owner inactive. The real receiver,
 * worker and wire path are exercised by wifi_ap_survey_c5_check.c. */
static __attribute__((unused)) atomic_bool ap_survey_radio_reserved;
static __attribute__((unused)) atomic_uint ap_survey_admission;
static __attribute__((unused)) bool ap_survey_retained(void) { return false; }
static __attribute__((unused)) void ap_survey_worker_tick(void) { }
static __attribute__((unused)) void ap_survey_receive(const void *packet,unsigned type,
    int64_t entered,uint32_t token)
{ (void)packet;(void)type;(void)entered;(void)token; }
