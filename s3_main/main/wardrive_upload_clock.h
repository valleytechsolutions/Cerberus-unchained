/* Private to the upload worker. A plausible RTC date alone is insufficient:
 * reuse only a recent synchronization whose wall/monotonic clocks still agree. */
#include "wardrive_gps_uart.h"
#include <sys/time.h>

#define UPLOAD_CLOCK_HOLDOVER_US INT64_C(900000000)
#define UPLOAD_CLOCK_DRIFT_US INT64_C(2000000)
#define UPLOAD_NTP_WAIT_MS 15000U

static struct {
    int64_t monotonic_us, utc_us;
    bool valid;
} upload_clock;

static bool upload_utc_in_range(int64_t utc_us)
{
    return utc_us >= INT64_C(1704067200000000) &&
        utc_us < INT64_C(4102444800000000); /* 2024 through 2099. */
}

static bool upload_wall_time(int64_t *out)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) || tv.tv_sec < INT64_C(1704067200) ||
        tv.tv_sec >= INT64_C(4102444800) || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
        return false;
    *out = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    return true;
}

static bool upload_clock_recent(void)
{
    if (!upload_clock.valid) return false;
    int64_t now = esp_timer_get_time(), wall;
    bool valid = now >= upload_clock.monotonic_us &&
        now - upload_clock.monotonic_us <= UPLOAD_CLOCK_HOLDOVER_US &&
        upload_wall_time(&wall);
    if (valid) {
        int64_t expected = upload_clock.utc_us + now - upload_clock.monotonic_us;
        int64_t delta = wall - expected;
        valid = delta >= -UPLOAD_CLOCK_DRIFT_US && delta <= UPLOAD_CLOCK_DRIFT_US;
    }
    upload_clock.valid = valid;
    return valid;
}

static unsigned upload_utc_decimal(const char *text, unsigned count)
{
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i) value = value * 10U + (unsigned)(text[i] - '0');
    return value;
}

static bool upload_leap_year(unsigned year)
{ return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U); }

static bool upload_gps_utc(const cerberus_wardrive_gps_time_t *sample,
    int64_t now, int64_t *out)
{
    const char *s = sample->utc;
    if (now < 0 || sample->received_ms > (uint64_t)now / 1000U ||
        now - (int64_t)sample->received_ms * 1000 > UPLOAD_CLOCK_HOLDOVER_US ||
        s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':' || s[19] != '.' || s[23] != 'Z' || s[24]) return false;
    for (unsigned i = 0; i < 23U; ++i) {
        if (i == 4U || i == 7U || i == 10U || i == 13U || i == 16U || i == 19U) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    unsigned year = upload_utc_decimal(s, 4U), month = upload_utc_decimal(s + 5U, 2U);
    unsigned day = upload_utc_decimal(s + 8U, 2U), hour = upload_utc_decimal(s + 11U, 2U);
    unsigned minute = upload_utc_decimal(s + 14U, 2U), second = upload_utc_decimal(s + 17U, 2U);
    static const unsigned days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 2024U || year > 2099U || month < 1U || month > 12U || !day ||
        day > days_in_month[month - 1U] + (month == 2U && upload_leap_year(year)) ||
        hour > 23U || minute > 59U || second > 59U) return false;
    unsigned days = day - 1U;
    for (unsigned y = 1970U; y < year; ++y) days += 365U + upload_leap_year(y);
    for (unsigned m = 1U; m < month; ++m)
        days += days_in_month[m - 1U] + (m == 2U && upload_leap_year(year));
    int64_t utc_us = (((int64_t)days * 24 + hour) * 60 + minute) * 60 + second;
    utc_us = utc_us * 1000000 + upload_utc_decimal(s + 20U, 3U) * 1000U;
    utc_us += now - (int64_t)sample->received_ms * 1000;
    if (!upload_utc_in_range(utc_us)) return false;
    *out = utc_us;
    return true;
}

static void upload_sntp_end(void)
{
    if (sntp_owned) { esp_netif_sntp_deinit(); sntp_owned = false; }
}

static esp_err_t upload_clock_prepare(cerberus_upload_cancel_fn cancel,
    cerberus_upload_progress_fn progress, void *context)
{
    if (cancelled(cancel, context)) return ESP_ERR_INVALID_STATE;
    if (upload_clock_recent()) return ESP_OK;

    cerberus_wardrive_gps_time_t gps;
    int64_t utc_us, now = esp_timer_get_time();
    if (now < 0) return ESP_ERR_INVALID_STATE;
    if (cerberus_wardrive_gps_uart_recent_time(&gps) == ESP_OK) {
        now = esp_timer_get_time();
        if (upload_gps_utc(&gps, now, &utc_us)) {
            report(progress, context, "USING GPS UTC FOR TLS", 0, 0);
            struct timeval tv = {.tv_sec = (time_t)(utc_us / 1000000),
                .tv_usec = (suseconds_t)(utc_us % 1000000)};
            if (settimeofday(&tv, NULL)) return ESP_FAIL;
            upload_clock.monotonic_us = (int64_t)gps.received_ms * 1000;
            upload_clock.utc_us = utc_us - (now - upload_clock.monotonic_us);
            upload_clock.valid = true;
            if (!upload_clock_recent()) return ESP_ERR_INVALID_RESPONSE;
            return ESP_OK;
        }
    }

    static const char *const servers[] = {"pool.ntp.org", "time.cloudflare.com"};
    for (unsigned server = 0; server < sizeof(servers) / sizeof(servers[0]); ++server) {
        report(progress, context, server ? "VERIFYING UTC / BACKUP NTP" : "VERIFYING UTC FOR TLS", 0, 0);
        if (cancelled(cancel, context)) return ESP_ERR_INVALID_STATE;
        esp_sntp_config_t sync = ESP_NETIF_SNTP_DEFAULT_CONFIG(servers[server]);
        esp_err_t error = esp_netif_sntp_init(&sync);
        if (error != ESP_OK) return error;
        sntp_owned = true;
        int64_t previous = esp_timer_get_time();
        for (unsigned waited = 0U; waited < UPLOAD_NTP_WAIT_MS; waited += 100U) {
            if (cancelled(cancel, context)) { upload_sntp_end(); return ESP_ERR_INVALID_STATE; }
            error = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100));
            now = esp_timer_get_time();
            if (previous < 0 || now < previous) { upload_sntp_end(); return ESP_ERR_INVALID_STATE; }
            previous = now;
            if (error == ESP_OK) {
                /* Stop the client before sampling so a later callback cannot
                 * change the clock behind this verification's anchor. */
                upload_sntp_end();
                now = esp_timer_get_time();
                if (now < previous || !upload_wall_time(&utc_us)) return ESP_ERR_INVALID_RESPONSE;
                upload_clock.monotonic_us = now;
                upload_clock.utc_us = utc_us;
                upload_clock.valid = true;
                return ESP_OK;
            }
            if (error != ESP_ERR_TIMEOUT && error != ESP_ERR_NOT_FINISHED) {
                upload_sntp_end();
                return error;
            }
        }
        upload_sntp_end();
    }
    report(progress, context, "UTC TIMEOUT / LOG NOT SENT", 0, 0);
    return ESP_ERR_TIMEOUT;
}
