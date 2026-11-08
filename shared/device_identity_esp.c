#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include <string.h>

static void copy_text(char *destination, size_t capacity, const char *source)
{
    for (size_t i = 0; i + 1U < capacity && source[i]; ++i) {
        unsigned char c = (unsigned char)source[i];
        destination[i] = c >= 32U && c <= 126U ? (char)c : '?';
    }
}

void cerberus_device_identity_collect(cerberus_device_identity_t *out, uint32_t capability)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = CERBERUS_DEVICE_IDENTITY_MAGIC;
    out->capability_version = capability;
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    out->chip_model = (uint16_t)chip.model;
    out->chip_revision = chip.revision;
    out->cores = chip.cores;
    out->chip_features = chip.features;
    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK && flash_bytes) {
        out->flags |= CERBERUS_ID_FLASH;
        out->flash_bytes = flash_bytes;
    }
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (part) {
        out->flags |= CERBERUS_ID_PARTITION;
        out->partition_subtype = part->subtype;
        out->partition_address = part->address;
        out->partition_bytes = part->size;
        copy_text(out->partition_label, sizeof(out->partition_label), part->label);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    copy_text(out->project, sizeof(out->project), app->project_name);
    copy_text(out->version, sizeof(out->version), app->version);
    copy_text(out->idf, sizeof(out->idf), app->idf_ver);
    copy_text(out->date, sizeof(out->date), app->date);
    copy_text(out->time, sizeof(out->time), app->time);
    memcpy(out->elf_sha256, app->app_elf_sha256, sizeof(out->elf_sha256));
    if (esp_efuse_mac_get_default(out->base_mac) == ESP_OK) out->flags |= CERBERUS_ID_BASE_MAC;
    else memset(out->base_mac, 0, sizeof(out->base_mac));
    if (esp_wifi_get_mac(WIFI_IF_STA, out->sta_mac) == ESP_OK)
        out->flags |= CERBERUS_ID_STA_MAC | CERBERUS_ID_STA_CURRENT;
    else if (esp_read_mac(out->sta_mac, ESP_MAC_WIFI_STA) == ESP_OK) out->flags |= CERBERUS_ID_STA_MAC;
    else memset(out->sta_mac, 0, sizeof(out->sta_mac));
    if (esp_wifi_get_mac(WIFI_IF_AP, out->ap_mac) == ESP_OK)
        out->flags |= CERBERUS_ID_AP_MAC | CERBERUS_ID_AP_CURRENT;
    else if (esp_read_mac(out->ap_mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) out->flags |= CERBERUS_ID_AP_MAC;
    else memset(out->ap_mac, 0, sizeof(out->ap_mac));
}
