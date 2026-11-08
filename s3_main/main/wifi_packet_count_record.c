#include "wifi_packet_count_record.h"
#include "storage.h"
#include "wifi_pcap.h"
#include "cerberus_protocol.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* The native wrappers are deliberately separate from the worker body so its
 * actual ownership, transport and SD paths can run under deterministic faults. */
#include "wifi_packet_count_record_impl.h"
