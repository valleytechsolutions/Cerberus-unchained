#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CERBERUS_GPS_AUTO, CERBERUS_GPS_LC29H_AA, CERBERUS_GPS_ATGM336H,
    CERBERUS_GPS_MANUAL, CERBERUS_GPS_PROFILE_COUNT };
/* Stable NVS layout. 0 baud means receive-only detection; selecting a profile
 * changes the host UART, never sends persistent receiver configuration. */
typedef struct {
    uint32_t baud;
    uint8_t profile, enabled, rx_gpio;
    int8_t tx_gpio, pps_gpio;
    uint8_t reserved[3];
} cerberus_gps_config_t;
_Static_assert(sizeof(cerberus_gps_config_t)==12U,"GPS settings layout");
static inline bool cerberus_gps_baud_valid(uint32_t baud)
{
    return baud==0U||baud==4800U||baud==9600U||baud==19200U||baud==38400U||
        baud==57600U||baud==115200U||baud==230400U||baud==460800U||baud==921600U;
}
/* Only otherwise unassigned board pins are offered. GPIO46 is input-only.
 * USB, display, SD, C5 link/reset, expansion SPI and flash/PSRAM are excluded. */
static inline bool cerberus_gps_input_pin_valid(int pin)
{ return pin==1||pin==2||pin==46; }
static inline bool cerberus_gps_config_valid(const cerberus_gps_config_t *c)
{
    if(!c||c->profile>=CERBERUS_GPS_PROFILE_COUNT||c->enabled>1U||
       !cerberus_gps_baud_valid(c->baud)||!cerberus_gps_input_pin_valid(c->rx_gpio)||
       c->reserved[0]||c->reserved[1]||c->reserved[2])return false;
    if(c->tx_gpio!=-1&&(!cerberus_gps_input_pin_valid(c->tx_gpio)||c->tx_gpio==46||c->tx_gpio==c->rx_gpio))return false;
    if(c->pps_gpio!=-1&&(!cerberus_gps_input_pin_valid(c->pps_gpio)||c->pps_gpio==c->rx_gpio||c->pps_gpio==c->tx_gpio))return false;
    if(c->profile==CERBERUS_GPS_AUTO&&c->baud!=0U)return false;
    if(c->profile==CERBERUS_GPS_LC29H_AA&&c->baud!=115200U)return false;
    if(c->profile==CERBERUS_GPS_ATGM336H&&c->baud!=9600U)return false;
    return true;
}
static inline cerberus_gps_config_t cerberus_gps_config_default(void)
{
    const cerberus_gps_config_t c={.baud=0U,.profile=CERBERUS_GPS_AUTO,
        .enabled=1U,.rx_gpio=1U,.tx_gpio=2,.pps_gpio=46};
    return c;
}
static inline void cerberus_gps_config_profile(cerberus_gps_config_t *c,uint8_t profile)
{
    if(!c||profile>=CERBERUS_GPS_PROFILE_COUNT)return;
    c->profile=profile;
    if(profile==CERBERUS_GPS_AUTO)c->baud=0U;
    else if(profile==CERBERUS_GPS_LC29H_AA)c->baud=115200U;
    else if(profile==CERBERUS_GPS_ATGM336H)c->baud=9600U;
    else if(!c->baud)c->baud=115200U;
}
static inline const char *cerberus_gps_profile_name(uint8_t profile)
{
    static const char *const names[]={"AUTO NMEA","QUECTEL LC29H-AA","ATGM336H","MANUAL NMEA"};
    return profile<CERBERUS_GPS_PROFILE_COUNT?names[profile]:"INVALID";
}
