#pragma once
#include <stdbool.h>
/* Boundary for unrelated-owner fixtures. The real Wardrive engine is exercised
 * by wardrive_radio_check.c; these excerpts must retain foreign-owner guards. */
static bool fixture_wardrive_held;
static __attribute__((unused)) bool cerberus_wardrive_radio_held(void)
{return fixture_wardrive_held;}
static __attribute__((unused)) void cerberus_wardrive_radio_wifi_rx(void *packet,int type)
{(void)packet;(void)type;}
