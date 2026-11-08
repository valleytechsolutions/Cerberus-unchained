/* Included in ui.c; one Settings branch and the existing Wardriving GPS view. */
#include "wardrive_gps_uart.h"
static cerberus_gps_config_t gps_edit;
static esp_err_t gps_settings_error,gps_preview_error;
static bool gps_preview_attached,gps_preview_cleanup;
static uint32_t gps_preview_retry_ms;

static void gps_settings_open(void)
{
    gps_edit=cerberus_settings_gps_get();gps_settings_error=ESP_OK;
    settings_mode=UI_SETTINGS_MODE_GPS;
}
static esp_err_t draw_settings_gps(void)
{
    char line[64];
    ESP_RETURN_ON_ERROR(draw_centered(58,"GPS RECEIVER",UI_TEXT,2),TAG,"GPS settings title");
    ESP_RETURN_ON_ERROR(draw_small_button(16,88,288,38,cerberus_gps_profile_name(gps_edit.profile)),TAG,"GPS preset");
    if(gps_edit.baud)snprintf(line,sizeof(line),"BAUD: %lu",(unsigned long)gps_edit.baud);
    else snprintf(line,sizeof(line),"BAUD: AUTOMATIC");
    ESP_RETURN_ON_ERROR(draw_small_button(16,134,288,34,line),TAG,"GPS baud");
    snprintf(line,sizeof(line),"S3 RX: GPIO %u  <- GPS TX",gps_edit.rx_gpio);
    ESP_RETURN_ON_ERROR(draw_small_button(16,176,288,34,line),TAG,"GPS RX");
    if(gps_edit.tx_gpio<0)snprintf(line,sizeof(line),"S3 TX: NOT WIRED");
    else snprintf(line,sizeof(line),"S3 TX: GPIO %d  -> GPS RX",gps_edit.tx_gpio);
    ESP_RETURN_ON_ERROR(draw_small_button(16,218,288,34,line),TAG,"GPS TX");
    if(gps_edit.pps_gpio<0)snprintf(line,sizeof(line),"PPS: NOT WIRED");
    else snprintf(line,sizeof(line),"PPS: GPIO %d / RESERVED",gps_edit.pps_gpio);
    ESP_RETURN_ON_ERROR(draw_small_button(16,260,288,34,line),TAG,"GPS PPS");
    ESP_RETURN_ON_ERROR(draw_centered(306,"RECEIVE NMEA / NO MODULE REPROGRAMMING",UI_MUTED,1),TAG,"GPS config scope");
    ESP_RETURN_ON_ERROR(draw_small_button(16,330,136,34,gps_edit.enabled?"PREVIEW: ON":"PREVIEW: OFF"),TAG,"GPS preview");
    ESP_RETURN_ON_ERROR(draw_small_button(168,330,136,34,"SAVE / APPLY"),TAG,"GPS save");
    if(gps_settings_error!=ESP_OK){
        snprintf(line,sizeof(line),"SAVE: %s",esp_err_to_name(gps_settings_error));
        ESP_RETURN_ON_ERROR(draw_centered(382,line,UI_BAD,1),TAG,"GPS settings error");
    }else ESP_RETURN_ON_ERROR(draw_centered(382,"PRESETS KEEP YOUR PIN ASSIGNMENTS",UI_MUTED,1),TAG,"GPS settings help");
    return ESP_OK;
}
static int gps_next_pin(int current,bool optional,bool output)
{
    static const int8_t pins[]={-1,1,2,46};
    for(size_t attempt=1U;attempt<=sizeof(pins);++attempt){
        size_t index=0U;while(index<sizeof(pins)&&pins[index]!=current)++index;
        current=pins[(index+1U)%sizeof(pins)];
        if((optional&&current<0)||(current>=0&&(!output||current!=46)))return current;
    }
    return current;
}
static void gps_settings_tap(uint16_t x,uint16_t y)
{
    if(x<16U||x>=304U)return;
    if(y>=88U&&y<126U)cerberus_gps_config_profile(&gps_edit,(uint8_t)((gps_edit.profile+1U)%CERBERUS_GPS_PROFILE_COUNT));
    else if(y>=134U&&y<168U){
        static const uint32_t rates[]={4800U,9600U,19200U,38400U,57600U,115200U,230400U,460800U,921600U};
        size_t i=0;while(i<sizeof(rates)/sizeof(rates[0])&&rates[i]!=gps_edit.baud)++i;
        gps_edit.profile=CERBERUS_GPS_MANUAL;gps_edit.baud=rates[(i+1U)%(sizeof(rates)/sizeof(rates[0]))];
    }else if(y>=176U&&y<210U)gps_edit.rx_gpio=(uint8_t)gps_next_pin(gps_edit.rx_gpio,false,false);
    else if(y>=218U&&y<252U)gps_edit.tx_gpio=(int8_t)gps_next_pin(gps_edit.tx_gpio,true,true);
    else if(y>=260U&&y<294U)gps_edit.pps_gpio=(int8_t)gps_next_pin(gps_edit.pps_gpio,true,false);
    else if(y>=330U&&y<364U&&x<152U)gps_edit.enabled=!gps_edit.enabled;
    else if(y>=330U&&y<364U&&x>=168U){
        cerberus_wardrive_status_t run;cerberus_wardrive_get_status(&run);
        if(run.state==CERBERUS_WARDRIVE_STARTING||run.state==CERBERUS_WARDRIVE_RUNNING||run.state==CERBERUS_WARDRIVE_STOPPING){gps_settings_error=ESP_ERR_INVALID_STATE;return;}
        if(!cerberus_gps_config_valid(&gps_edit)){gps_settings_error=ESP_ERR_INVALID_ARG;return;}
        if(gps_preview_attached||gps_preview_cleanup){
            gps_settings_error=cerberus_wardrive_gps_uart_preview(false);
            if(gps_settings_error!=ESP_OK)return;
            gps_preview_attached=false;gps_preview_cleanup=false;
        }
        gps_settings_error=cerberus_settings_gps_set(&gps_edit);
    }
}
static void gps_preview_poll(uint32_t now_ms,bool asleep)
{
    bool want=current_page==UI_PAGE_WARDRIVE || (!asleep&&
        current_page==UI_PAGE_SETTINGS&&settings_mode==UI_SETTINGS_MODE_GPS&&cerberus_settings_gps_get().enabled);
    if(want==gps_preview_attached&&!gps_preview_cleanup)return;
    if((uint32_t)(now_ms-gps_preview_retry_ms)<250U)return;
    gps_preview_retry_ms=now_ms;
    if(gps_preview_cleanup){
        gps_preview_error=cerberus_wardrive_gps_uart_preview(false);
        if(gps_preview_error==ESP_OK){gps_preview_cleanup=false;gps_preview_attached=false;}
        return;
    }
    gps_preview_error=cerberus_wardrive_gps_uart_preview(want);
    if(gps_preview_error==ESP_OK)gps_preview_attached=want;
    else if(want)gps_preview_cleanup=true; /* Failed attach can retain UART cleanup. */
}
