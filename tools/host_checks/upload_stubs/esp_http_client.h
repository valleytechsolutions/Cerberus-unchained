#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct test_http *esp_http_client_handle_t;
typedef struct {const char *url,*user_agent;int method,timeout_ms,buffer_size,buffer_size_tx;bool disable_auto_redirect;esp_err_t (*crt_bundle_attach)(void *);} esp_http_client_config_t;
#define HTTP_METHOD_POST 1
#define ESP_ERR_HTTP_CONNECT 0x7002
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);
int esp_http_client_write(esp_http_client_handle_t,const char *,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char *,int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_get_errno(esp_http_client_handle_t);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,int *,int *);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
