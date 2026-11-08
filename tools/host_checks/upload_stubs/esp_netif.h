#pragma once
#include "esp_err.h"
typedef struct {int id;} esp_netif_t;
typedef struct {struct {unsigned addr;} ip;} esp_netif_ip_info_t;
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(esp_netif_t *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *);
