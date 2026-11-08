#pragma once
#include "wardrive_stream.h"
#include "esp_err.h"
/* Native adapter used by both chips. SPI commands and the local S3 caller use
 * the same session/ACK contract. No transmit, connect, or advertising API. */
esp_err_t cerberus_wardrive_radio_exchange(uint8_t command,const cerberus_wd_request_t *request,void *reply);
bool cerberus_wardrive_radio_held(void);
void cerberus_wardrive_radio_wifi_rx(void *packet,int type);
