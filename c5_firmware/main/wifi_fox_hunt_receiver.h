/* Included by the shared receive worker. Commands take scanner_mutex before
 * packet_monitor_mux; callback state below is protected by the latter. */
#include "wifi_fox_hunt_core.h"

static bool wifi_fox_hunt_mode;
static cerberus_wifi_fox_hunt_start_t wifi_fox_hunt_request;
static cerberus_wifi_fox_hunt_signal_t wifi_fox_hunt_latest;
static int64_t wifi_fox_hunt_observed_us, wifi_fox_hunt_armed_us, wifi_fox_hunt_clock_us;
static uint16_t wifi_fox_hunt_sweeps;
static bool wifi_fox_hunt_searching, wifi_fox_hunt_skipped;
static esp_err_t wifi_fox_hunt_tune_error;
static uint8_t wifi_fox_hunt_channels[42], wifi_fox_hunt_channel_count;

static void wifi_fox_hunt_receive(const wifi_promiscuous_pkt_t *packet,
    wifi_promiscuous_pkt_type_t type, int64_t entered, uint32_t token)
{
    if (!packet || !token || packet->rx_ctrl.rx_state != 0U ||
        packet->rx_ctrl.sig_len < 20U || packet->rx_ctrl.rssi < -127 ||
        packet->rx_ctrl.rssi > 20) return;
    unsigned frame_type = type == WIFI_PKT_MGMT ? 0U : type == WIFI_PKT_CTRL ? 1U :
        type == WIFI_PKT_DATA ? 2U : 3U;
    uint8_t address[6], kind;
    if (!cerberus_wifi_transmitter(packet->payload, packet->rx_ctrl.sig_len - 4U,
            frame_type, address, &kind)) return;
    portENTER_CRITICAL(&packet_monitor_mux);
    int64_t now = esp_timer_get_time();
    if (wifi_fox_hunt_mode && packet_monitor_receiving &&
        !atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire) &&
        token == atomic_load_explicit(&wifi_fox_hunt_admission_token, memory_order_acquire) &&
        packet->rx_ctrl.channel == packet_monitor_receive_channel &&
        entered >= wifi_fox_hunt_armed_us) {
        if (now < entered || now < wifi_fox_hunt_clock_us) {
            atomic_store_explicit(&channel_analyzer_clock_invalid, true, memory_order_release);
            atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
            packet_monitor_receiving = false;
        } else {
            wifi_fox_hunt_clock_us = now;
            if (entered >= wifi_fox_hunt_observed_us &&
                memcmp(address, wifi_fox_hunt_request.target, sizeof(address)) == 0) {
                if (wifi_fox_hunt_latest.sequence == UINT32_MAX) {
                    atomic_store_explicit(&channel_analyzer_clock_invalid, true, memory_order_release);
                    atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
                    packet_monitor_receiving = false;
                } else {
                    ++wifi_fox_hunt_latest.sequence;
                    wifi_fox_hunt_latest.observed_ms = (uint32_t)((uint64_t)entered / 1000U);
                    wifi_fox_hunt_latest.rssi = packet->rx_ctrl.rssi;
                    wifi_fox_hunt_latest.channel = packet->rx_ctrl.channel;
                    wifi_fox_hunt_observed_us = entered;
                    wifi_fox_hunt_searching = false;
                }
            }
        }
    }
    portEXIT_CRITICAL(&packet_monitor_mux);
}
