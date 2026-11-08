/* Included by c5_spi_link.c. Only incremental passive readers opt into this
 * budget. The shared mutex owns both the gate and every exchange below. */
#define C5_PASSIVE_READY_BUDGET_US 5000U
static bool passive_ready_pending;
static uint32_t passive_ready_epoch;

typedef struct {
    int64_t started_us, last_us;
    uint32_t deadline_ms;
} passive_ready_budget_t;

static esp_err_t passive_ready_remaining(passive_ready_budget_t *budget, uint32_t *remaining)
{
    int64_t now = esp_timer_get_time();
    if (now < 0 || now < budget->last_us) return ESP_ERR_INVALID_STATE;
    budget->last_us = now;
    uint64_t elapsed = (uint64_t)(now - budget->started_us);
    uint32_t left_ms = budget->deadline_ms - (uint32_t)((uint64_t)now / 1000U);
    if (elapsed >= C5_PASSIVE_READY_BUDGET_US || !left_ms || left_ms > INT32_MAX)
        return ESP_ERR_TIMEOUT;
    uint64_t left_us = (uint64_t)left_ms * 1000U - (uint64_t)now % 1000U;
    uint32_t software_left = C5_PASSIVE_READY_BUDGET_US - (uint32_t)elapsed;
    if (remaining) *remaining = left_us < software_left ? (uint32_t)left_us : software_left;
    return ESP_OK;
}

static esp_err_t passive_ready_wait(passive_ready_budget_t *budget, bool falling,
    uint32_t baseline, uint32_t *timeout_counter)
{
    uint32_t remaining = 0U;
    esp_err_t error = passive_ready_remaining(budget, &remaining);
    if (error != ESP_OK) return error;
    bool ready = falling ? c5_ready_wait_fall(&ready_io, baseline, remaining, C5_READY_POLL_US) :
        c5_ready_wait_high(&ready_io, remaining, C5_READY_POLL_US);
    if (!ready) diagnostic_increment(timeout_counter);
    error = passive_ready_remaining(budget, NULL);
    return error != ESP_OK ? error : ready ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t passive_ready_finish(passive_ready_budget_t *budget)
{
    if (!passive_ready_pending) return ESP_OK;
    esp_err_t error = passive_ready_wait(budget, true, passive_ready_epoch,
        &transport_diagnostics.ready_fall_timeouts);
    if (error != ESP_OK) return error;
    if (ready_high()) diagnostic_increment(&transport_diagnostics.ready_latched_pulses);
    error = passive_ready_wait(budget, false, 0U, &transport_diagnostics.ready_high_timeouts);
    if (error == ESP_OK) passive_ready_pending = false;
    return error;
}

/* A bounded failure may leave the C5 processing the last transfer. Its exact
 * falling edge must be observed before another caller can trust READY-high.
 * Ordinary callers retain their existing wait policy only for this recovery. */
static bool passive_ready_recover_legacy(void)
{
    if (!passive_ready_pending) return true;
    if (!wait_for_ready_cycle(passive_ready_epoch)) return false;
    passive_ready_pending = false;
    return true;
}

static esp_err_t spi_passive_transaction(const void *tx_data, void *rx_data, size_t length)
{
    memset(rx_data, 0, length);
    if (!c5_spi || !length) return ESP_ERR_INVALID_STATE;
    spi_transaction_t transaction = {
        .length = length * 8U, .tx_buffer = tx_data, .rx_buffer = rx_data,
    };
    uint32_t baseline = ready_epoch();
    /* IDF 6.0.1 polling_start errors occur before spi_new_trans. Splitting the
     * normal polling_transmit pair distinguishes those no-clock rejections
     * from an error after transfer, without guessing from an ambiguous errno. */
    esp_err_t error = spi_device_polling_start(c5_spi, &transaction, portMAX_DELAY);
    if (error == ESP_OK) {
        passive_ready_epoch = baseline;
        passive_ready_pending = true;
        /* Always finish a started transfer before returning stack storage.
         * portMAX_DELAY retains the existing non-preemptible hardware policy.
         * A completed end error keeps the READY gate for later recovery. */
        error = spi_device_polling_end(c5_spi, portMAX_DELAY);
    }
    if (error != ESP_OK) {
        diagnostic_increment(&transport_diagnostics.spi_errors);
        ESP_LOGW(TAG, "passive hardware SPI transaction failed: %s", esp_err_to_name(error));
        memset(rx_data, 0, length);
    }
    return error;
}

static esp_err_t spi_request_response_passive(const void *request, void *discarded,
    void *response, size_t length, uint32_t deadline_ms)
{
    memset(discarded, 0, length);
    memset(response, 0, length);
    int64_t started = esp_timer_get_time();
    if (started < 0) return ESP_ERR_INVALID_STATE;
    passive_ready_budget_t budget = {started, started, deadline_ms};
    diagnostic_increment(&transport_diagnostics.exchanges);
    esp_err_t error = passive_ready_remaining(&budget, NULL);
    if (error == ESP_OK) error = passive_ready_finish(&budget);
    if (error == ESP_OK) error = passive_ready_wait(&budget, false, 0U,
        &transport_diagnostics.ready_initial_timeouts);
    if (error == ESP_OK) {
        error = spi_passive_transaction(request, discarded, length);
    }
    if (error == ESP_OK) error = passive_ready_finish(&budget);
    if (error == ESP_OK) {
        /* The preceding finish checked the budget after READY-high. Hardware
         * polling SPI itself is not preemptible by this software deadline. */
        error = spi_passive_transaction(NULL, response, length);
    }
    if (error == ESP_OK) error = passive_ready_finish(&budget);
    if (error == ESP_OK) error = passive_ready_remaining(&budget, NULL);
    if (error != ESP_OK) {
        memset(discarded, 0, length);
        memset(response, 0, length);
    }
    int64_t finished = esp_timer_get_time();
    if (finished >= started) {
        uint64_t elapsed = (uint64_t)(finished - started);
        diagnostic_maximum(&transport_diagnostics.max_exchange_us,
            elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
    }
    return error;
}
