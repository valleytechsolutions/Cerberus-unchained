#include "gps_recorder.h"
static cerberus_gps_recorder_status_t gps_log_runtime;
static unsigned gps_log_starts,gps_log_stops,gps_log_marks;
static esp_err_t gps_log_result;
bool cerberus_gps_recorder_busy(void){return gps_log_runtime.busy;}
void cerberus_gps_recorder_status(cerberus_gps_recorder_status_t *out){*out=gps_log_runtime;}
esp_err_t cerberus_gps_recorder_start(cerberus_gpx_mode_t mode,uint64_t *job)
{
    ++gps_log_starts;*job=0;if(gps_log_result)return gps_log_result;
    if(gps_log_runtime.busy)return ESP_ERR_INVALID_STATE;
    gps_log_runtime=(cerberus_gps_recorder_status_t){.job=gps_log_starts,.busy=true,
        .mode=mode,.state=CERBERUS_GPS_LOG_WAITING};*job=gps_log_runtime.job;return ESP_OK;
}
esp_err_t cerberus_gps_recorder_stop(uint64_t job)
{
    ++gps_log_stops;if(job!=gps_log_runtime.job)return ESP_ERR_INVALID_STATE;
    gps_log_runtime.state=CERBERUS_GPS_LOG_STOPPING;return ESP_OK;
}
esp_err_t cerberus_gps_recorder_mark(uint64_t job,const char *label)
{
    (void)label;++gps_log_marks;
    if(job!=gps_log_runtime.job||!gps_log_runtime.busy||gps_log_runtime.state==CERBERUS_GPS_LOG_STOPPING)
        return ESP_ERR_INVALID_STATE;
    if(!gps_log_runtime.fix_ready)return ESP_ERR_NOT_FINISHED;
    ++gps_log_runtime.queued;return ESP_OK;
}
