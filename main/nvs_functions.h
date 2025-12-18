#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *NTAG = "NVS";

static void init_flash(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS partition was truncated and needs to be erased
    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

uint16_t increment_and_get_reset_count(void)
{
    init_flash();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("diagnostics", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(NTAG, "Error (%s) opening NVS handle for reset count!", esp_err_to_name(err));
        return 0;
    }

    uint16_t reset_count = 0;
    err = nvs_get_u16(nvs_handle, "reset_count", &reset_count);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        reset_count = 0;
    }

    reset_count++;

    err = nvs_set_u16(nvs_handle, "reset_count", reset_count);
    if (err == ESP_OK)
    {
        nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(NTAG, "Reset count: %d", reset_count);
    return reset_count;
}
