#pragma once
#include "wardrive_stream.h"
#include "wifi_scan_observation.h"
static inline uint16_t wd_le16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
/* Bounded RSN AKM parsing. Unknown or malformed RSN remains protected/other;
 * in particular it must never be mislabeled as an open or WPA2-only network. */
static inline uint8_t cerberus_wd_rsn_security(const uint8_t *p,size_t n)
{
    if(n<8U||wd_le16(p)!=1U)return CERBERUS_WIFI_SECURITY_OTHER;
    size_t offset=8U;uint16_t pairs=wd_le16(p+6U);
    if(!pairs||pairs>(n-offset)/4U)return CERBERUS_WIFI_SECURITY_OTHER;
    offset+=(size_t)pairs*4U;if(n-offset<2U)return CERBERUS_WIFI_SECURITY_OTHER;
    uint16_t count=wd_le16(p+offset);offset+=2U;
    if(!count||count>(n-offset)/4U)return CERBERUS_WIFI_SECURITY_OTHER;
    bool psk=false,sae=false,enterprise=false,owe=false,unknown=false;
    for(unsigned i=0;i<count;++i,offset+=4U){
        if(memcmp(p+offset,"\x00\x0f\xac",3)){unknown=true;continue;}
        switch(p[offset+3U]){
        case 1:case 3:case 5:case 11:case 12:case 13:enterprise=true;break;
        case 2:case 4:case 6:psk=true;break;
        case 8:case 9:sae=true;break;
        case 18:owe=true;break;
        default:unknown=true;break;
        }
    }
    if(unknown)return CERBERUS_WIFI_SECURITY_OTHER;
    if(enterprise)return CERBERUS_WIFI_SECURITY_ENTERPRISE;
    if(owe)return CERBERUS_WIFI_SECURITY_OWE;
    if(sae)return psk?CERBERUS_WIFI_SECURITY_WPA2_WPA3:CERBERUS_WIFI_SECURITY_WPA3;
    return psk?CERBERUS_WIFI_SECURITY_WPA2:CERBERUS_WIFI_SECURITY_OTHER;
}
static inline bool cerberus_wd_wifi_decode(const uint8_t *bytes,size_t length,uint8_t channel,
    int rssi,uint32_t now,cerberus_wd_record_t *out)
{
    cerberus_wifi_observation_frame_t frame;
    if(!out||rssi < -127||rssi>0||!cerberus_wifi_observation_decode(bytes,length,channel,&frame))return false;
    memset(out,0,sizeof(*out));out->age_ms=now;out->kind=1;out->channel=channel;out->rssi=(int8_t)rssi;
    out->label_length=frame.key.ssid_length;memcpy(out->label,frame.key.ssid,out->label_length);
    memcpy(out->address,frame.key.bssid,6);
    out->security=(bytes[34]&0x10U)?CERBERUS_WIFI_SECURITY_WEP:CERBERUS_WIFI_SECURITY_OPEN;
    bool wpa=false,rsn=false;
    for(size_t pos=36U;pos<length;){
        uint8_t tag=bytes[pos],n=bytes[pos+1U];pos+=2U; /* Full IE stream validated above. */
        if(tag==48U){out->security=cerberus_wd_rsn_security(bytes+pos,n);rsn=true;}
        if(tag==221U&&n>=4U&&!memcmp(bytes+pos,"\x00\x50\xf2\x01",4))wpa=true;
        pos+=n;
    }
    if(wpa&&!rsn)out->security=CERBERUS_WIFI_SECURITY_WPA;
    else if(wpa&&out->security==CERBERUS_WIFI_SECURITY_WPA2)out->security=CERBERUS_WIFI_SECURITY_WPA_WPA2;
    return true;
}
