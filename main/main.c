#include "main.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"
#include "string.h"

#include "nvs_functions.h"
#include "temp_sensor.h"

static const char *TAG = "TEMP_SENSOR";
uint8_t read_failures = 0;

uint8_t ds18b20_device_num = 0;
ds18b20_device_handle_t ds18b20s[HA_ESP_NUM_T_SENSORS];
uint8_t ep_to_ds[HA_ESP_NUM_T_SENSORS]; // Use 0xff to indicate no link

static int16_t zb_temperature_to_s16(float temp) {
  return (int16_t)(temp * 100);
}

// OTA partition and handle (must be static for persistence across callbacks)
static const esp_partition_t *s_ota_partition = NULL;
static esp_ota_handle_t s_ota_handle = 0;
static bool s_tagid_received = false;

static esp_err_t esp_element_ota_data(uint32_t total_size, const void *payload,
                                      uint16_t payload_size, void **outbuf,
                                      uint16_t *outlen) {
  static uint16_t tagid = 0;
  void *data_buf = NULL;
  uint16_t data_len;

  if (!s_tagid_received) {
    uint32_t length = 0;
    if (!payload || payload_size <= OTA_ELEMENT_HEADER_LEN) {
      ESP_LOGE(TAG, "Invalid element format");
      return ESP_ERR_INVALID_ARG;
    }

    tagid = *(const uint16_t *)payload;
    length = *(const uint32_t *)(payload + sizeof(tagid));
    if ((length + OTA_ELEMENT_HEADER_LEN) != total_size) {
      ESP_LOGE(TAG, "Invalid element length [%ld/%ld]", length, total_size);
      return ESP_ERR_INVALID_ARG;
    }

    s_tagid_received = true;

    data_buf = (void *)(payload + OTA_ELEMENT_HEADER_LEN);
    data_len = payload_size - OTA_ELEMENT_HEADER_LEN;
  } else {
    data_buf = (void *)payload;
    data_len = payload_size;
  }

  switch (tagid) {
  case UPGRADE_IMAGE:
    *outbuf = data_buf;
    *outlen = data_len;
    break;
  default:
    ESP_LOGE(TAG, "Unsupported element tag identifier %d", tagid);
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

static esp_err_t
zb_ota_upgrade_status_handler(esp_zb_zcl_ota_upgrade_value_message_t message) {
  static uint32_t total_size = 0;
  static uint32_t offset = 0;
  static int64_t start_time = 0;
  esp_err_t ret = ESP_OK;

  if (message.info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    switch (message.upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
      ESP_LOGI(TAG, "-- OTA upgrade start");
      start_time = esp_timer_get_time();
      s_ota_partition = esp_ota_get_next_update_partition(NULL);
      if (!s_ota_partition) {
        ESP_LOGE(TAG, "Failed to find OTA partition");
        return ESP_FAIL;
      }
      ret = esp_ota_begin(s_ota_partition, 0, &s_ota_handle);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin OTA partition: %s",
                 esp_err_to_name(ret));
        return ret;
      }
      break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
      total_size = message.ota_header.image_size;
      offset += message.payload_size;
      ESP_LOGI(TAG, "-- OTA Client receives data: progress [%ld/%ld]", offset,
               total_size);
      if (message.payload_size && message.payload) {
        uint16_t payload_size = 0;
        void *payload = NULL;
        ret =
            esp_element_ota_data(total_size, message.payload,
                                 message.payload_size, &payload, &payload_size);
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Failed to element OTA data: %s", esp_err_to_name(ret));
          return ret;
        }
        ret = esp_ota_write(s_ota_handle, (const void *)payload, payload_size);
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Failed to write OTA data to partition: %s",
                   esp_err_to_name(ret));
          return ret;
        }
      }
      break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
      ESP_LOGI(TAG, "-- OTA upgrade apply");
      break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
      ret = offset == total_size ? ESP_OK : ESP_FAIL;
      offset = 0;
      total_size = 0;
      s_tagid_received = false;
      ESP_LOGI(TAG, "-- OTA upgrade check status: %s", esp_err_to_name(ret));
      break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
      ESP_LOGI(TAG, "-- OTA Finish");
      ESP_LOGI(TAG,
               "-- OTA Information: version: 0x%lx, manufacturer code: 0x%x, "
               "image type: 0x%x, total size: %ld bytes, cost time: %lld ms",
               message.ota_header.file_version,
               message.ota_header.manufacturer_code,
               message.ota_header.image_type, message.ota_header.image_size,
               (esp_timer_get_time() - start_time) / 1000);
      ret = esp_ota_end(s_ota_handle);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to end OTA partition: %s", esp_err_to_name(ret));
        return ret;
      }
      ret = esp_ota_set_boot_partition(s_ota_partition);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set OTA boot partition: %s",
                 esp_err_to_name(ret));
        return ret;
      }
      ESP_LOGW(TAG, "Prepare to restart system");
      esp_restart();
      break;

    default:
      ESP_LOGI(TAG, "OTA status: %d", message.upgrade_status);
      break;
    }
  }
  return ret;
}

static esp_err_t zb_ota_upgrade_query_image_resp_handler(
    esp_zb_zcl_ota_upgrade_query_image_resp_message_t message) {
  ESP_LOGI(TAG,
           "OTA query response: status(%d), size(%lu), version(0x%lx), "
           "type(0x%x), manufacturer(0x%x)",
           message.query_status, message.image_size, message.file_version,
           message.image_type, message.manufacturer_code);

  if (message.query_status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    ESP_LOGI(TAG, "OTA image available, upgrade will start automatically");
    return ESP_OK;
  } else {
    ESP_LOGI(TAG, "No OTA image available");
    return ESP_FAIL;
  }
}

static void start_temp_timer() {
  const esp_timer_create_args_t periodic_timer_args = {
      .callback = &temp_timer_callback, .name = "temp_timer"};

  esp_timer_handle_t periodic_timer;
  ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer,
                                           ESP_TEMP_SENSOR_UPDATE_INTERVAL));
}

void report_temp_attr(uint8_t ep) {
  esp_zb_zcl_report_attr_cmd_t report_attr_cmd;
  report_attr_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report_attr_cmd.attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
  report_attr_cmd.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
  report_attr_cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
  report_attr_cmd.zcl_basic_cmd.src_endpoint = ep;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_report_attr_cmd_req(&report_attr_cmd);
  esp_zb_lock_release();
  ESP_EARLY_LOGI(TAG, "Send 'report attributes' command");
}

void esp_app_temp_sensor_handler(float temperature, uint8_t endpoint) {
  int16_t measured_value = zb_temperature_to_s16(temperature);
  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_set_attribute_val(endpoint, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                               &measured_value, false);
  esp_zb_lock_release();
}

void read_temps(float *temp_results) {
  float temperature;
  vTaskDelay(pdMS_TO_TICKS(200));
  for (uint8_t i = 0; i < ds18b20_device_num; i++) {
    ESP_LOGI(TAG, "Setting Temp Conv");
    esp_err_t err = ds18b20_trigger_temperature_conversion(ds18b20s[i]);
    if (err == ESP_OK) {
      err = ds18b20_get_temperature(ds18b20s[i], &temperature);
      if (err == ESP_OK) {
        temp_results[i] = temperature;
      } else {
        temp_results[i] = DSB1820_BAD_TEMP;
        read_failures++;
      }
    } else {
      temp_results[i] = DSB1820_BAD_TEMP;
      read_failures++;
    }
  }
  if (read_failures > (4 * HA_ESP_NUM_T_SENSORS)) {
    // Had to be a way to reset the bus, but I haven't found it.
    onewire_bus_reset(ds18b20s[0]->bus);
  }
}
static void temp_timer_callback(void *arg) {
  float temp_results[HA_ESP_NUM_T_SENSORS] = {DSB1820_BAD_TEMP};
  read_temps(temp_results);

  for (int i = 0; i < HA_ESP_NUM_T_SENSORS; i++) {
    uint8_t dsb_index = ep_to_ds[i];
    ESP_LOGI(TAG, "EP: %d, dsb_index: %d", i, dsb_index);

    if (dsb_index == 0xff) {
      ESP_LOGI(TAG, "No sensor for EP: %d", i);
    } else {
      ESP_LOGI(TAG, "temperature read from DS18B20[%d], for EP: %d,  %.2fC",
               dsb_index, i, temp_results[dsb_index]);

      if (temp_results[dsb_index] != DSB1820_BAD_TEMP) {
        esp_app_temp_sensor_handler(temp_results[dsb_index],
                                    (HA_ESP_TEMP_START_ENDPOINT + i));
        report_temp_attr(HA_ESP_TEMP_START_ENDPOINT + i);
      }
    }
  }
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
  ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) ==
                          ESP_OK,
                      , TAG, "Failed to start Zigbee bdb commissioning");
}

static esp_err_t deferred_driver_init(void) {
  memset(ep_to_ds, 0xff, HA_ESP_NUM_T_SENSORS);

  find_onewire(ds18b20s, &ds18b20_device_num);
  // We have an array of all found ds18b20s
  ESP_LOGI(TAG, "One Wire Count: %i", ds18b20_device_num);

  uint8_t associated_eps_count = 0;
  // uint8_t unassociated_sensor_count = 0;
  //  For each found ds18b20, we need to try to match it to an endpoint
  for (uint8_t i = 0; i < HA_ESP_NUM_T_SENSORS; i++) {
    // for each endpoint see if we have a saved address
    onewire_device_address_t found_addr = 0;
    uint8_t *ep_index = &i;
    esp_err_t r_result = get_address_for_ep(&found_addr, ep_index);
    ESP_LOGI(TAG, "Err Result %d, address: %016llX", r_result, found_addr);
    if (r_result == ESP_OK && found_addr != 0) {
      // We have identified a saved address, but do we have a sensor that
      // matches?
      bool dev_exists = 0x00;
      for (uint8_t j = 0; j < ds18b20_device_num; j++) {

        if (found_addr == ds18b20s[j]->addr) {
          dev_exists = 0x01;
          // associate dev with correct endpoint
          ep_to_ds[j] = i;
          ESP_LOGI(
              TAG,
              "DS18B20[%d], address: %016llX, associated with EP index: %d", j,
              found_addr, i);
          associated_eps_count++;

          break;
        }
      }

      if (!dev_exists) {
        // We no longer have it, so remove it from the saved state.
        ESP_LOGI(TAG, "Address: %016llX, associated with EP index: %d, deleted",
                 found_addr, i);
        found_addr = 0;
        r_result = set_address_for_ep(&found_addr, ep_index);
      }
    }
  }

  // Ok, we have cleaned our list, now to assign extra sensors to endpoints
  // We know that anything with 0xff in ep_to_ds needs a sensor
  uint8_t available_sensors = ds18b20_device_num - associated_eps_count;
  uint8_t required_sensors = HA_ESP_NUM_T_SENSORS - associated_eps_count;

  ESP_LOGI(TAG, "There are %d sensors available to assign, need %d.",
           available_sensors, required_sensors);
  uint8_t av_sensor_index[available_sensors];
  memset(av_sensor_index, 0xff, available_sensors);

  uint8_t sensors_to_assign = available_sensors;
  if (available_sensors >= required_sensors) {
    sensors_to_assign = required_sensors;
  }

  for (uint8_t i = 0; i < sensors_to_assign; i++) {
    for (uint8_t j = 0; j < HA_ESP_NUM_T_SENSORS; j++) {
      if (ep_to_ds[j] == 0xff) {
        // Needs a sensor
        for (uint8_t k = 0; k < ds18b20_device_num; k++) {
          bool sens_av = 0x01;
          for (uint8_t l = 0; l < HA_ESP_NUM_T_SENSORS; l++) {
            if (ep_to_ds[l] == k) {
              sens_av = 0x00;
              break;
            }
          }
          if (sens_av) {
            ep_to_ds[j] = k;
            uint64_t set_addr = (uint64_t)ds18b20s[k]->addr;
            esp_err_t r_result = set_address_for_ep(&set_addr, &j);
            if (r_result != ESP_OK) {
              ESP_LOGW(TAG, "Failed to save ep %d.", j);
            }
            ESP_LOGI(TAG, "Setting EP %d to Sensor index %d, address: %016llX",
                     j, k, ds18b20s[k]->addr);
            break;
          }
        }
      }
    }
  }

  // Just print the final result so we can watch it over time.
  for (uint8_t i = 0; (i < HA_ESP_NUM_T_SENSORS) && (i < ds18b20_device_num);
       i++) {
    ESP_LOGI(TAG, "EP %d,  address: %016llX", i, ds18b20s[ep_to_ds[i]]->addr);
  }

  ESP_LOGI(TAG, "Starting Timers");
  start_temp_timer();
  return ESP_OK;
}

static esp_err_t
esp_zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                      const void *message) {
  esp_err_t ret = ESP_OK;

  switch (callback_id) {
  case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
    ret = zb_ota_upgrade_status_handler(
        *(esp_zb_zcl_ota_upgrade_value_message_t *)message);
    break;
  case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
    ret = zb_ota_upgrade_query_image_resp_handler(
        *(esp_zb_zcl_ota_upgrade_query_image_resp_message_t *)message);
    break;
  case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
    // Default response from coordinator - normal Zigbee communication
    ESP_LOGD(TAG, "Received default response");
    break;
  default:
    ESP_LOGW(TAG, "Unhandled action callback: %d", callback_id);
    break;
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Action handler failed with error: %s", esp_err_to_name(ret));
  }

  return ret;
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
  uint32_t *p_sg_p = signal_struct->p_app_signal;
  esp_err_t err_status = signal_struct->esp_err_status;
  esp_zb_app_signal_type_t sig_type = *p_sg_p;
  switch (sig_type) {
  case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
    ESP_LOGI(TAG, "Initialize Zigbee stack");
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
    break;
  case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
  case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
    if (err_status == ESP_OK) {
      ESP_LOGI(TAG, "Deferred driver initialization %s",
               deferred_driver_init() ? "failed" : "successful");
      ESP_LOGI(TAG, "Device started up in %s factory-reset mode",
               esp_zb_bdb_is_factory_new() ? "" : "non");
      if (esp_zb_bdb_is_factory_new()) {
        ESP_LOGI(TAG, "Start network steering");
        esp_zb_bdb_start_top_level_commissioning(
            ESP_ZB_BDB_MODE_NETWORK_STEERING);
      } else {
        ESP_LOGI(TAG, "Device rebooted");
      }
    } else {
      /* commissioning failed */
      ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)",
               esp_err_to_name(err_status));
    }
    break;
  case ESP_ZB_BDB_SIGNAL_STEERING:
    if (err_status == ESP_OK) {
      esp_zb_ieee_addr_t extended_pan_id;
      esp_zb_get_extended_pan_id(extended_pan_id);
      ESP_LOGI(TAG,
               "Joined network successfully (Extended PAN ID: "
               "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, "
               "Channel:%d, Short Address: 0x%04hx)",
               extended_pan_id[7], extended_pan_id[6], extended_pan_id[5],
               extended_pan_id[4], extended_pan_id[3], extended_pan_id[2],
               extended_pan_id[1], extended_pan_id[0], esp_zb_get_pan_id(),
               esp_zb_get_current_channel(), esp_zb_get_short_address());
    } else {
      ESP_LOGI(TAG, "Network steering was not successful (status: %s)",
               esp_err_to_name(err_status));
      esp_zb_scheduler_alarm(
          (esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
          ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
    }
    break;
  default:
    ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
             esp_zb_zdo_signal_to_string(sig_type), sig_type,
             esp_err_to_name(err_status));
    break;
  }
}

static esp_zb_cluster_list_t *custom_temperature_sensor_clusters_create(
    esp_zb_temperature_meas_cluster_cfg_t *temperature_sensor) {
  esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

  // Add Basic cluster for device identification
  esp_zb_basic_cluster_cfg_t basic_cfg = {
      .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
      .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
  };
  esp_zb_attribute_list_t *basic_cluster =
      esp_zb_basic_cluster_create(&basic_cfg);
  ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
      basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
      MANUFACTURER_NAME));
  ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
      basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
      MODEL_IDENTIFIER));
  ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(
      cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

  // Add temperature measurement cluster
  esp_zb_attribute_list_t *t_attr_list =
      esp_zb_temperature_meas_cluster_create(temperature_sensor);
  ESP_ERROR_CHECK(esp_zb_cluster_list_add_temperature_meas_cluster(
      cluster_list, t_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

  // Add Power Configuration cluster to report mains power
  esp_zb_power_config_cluster_cfg_t power_cfg = {
      .main_voltage =
          0xffff, // Unknown mains voltage (connected but voltage unknown)
      .main_alarm_mask = 0x00, // No alarms
  };
  esp_zb_attribute_list_t *power_cluster =
      esp_zb_power_config_cluster_create(&power_cfg);
  ESP_ERROR_CHECK(esp_zb_cluster_list_add_power_config_cluster(
      cluster_list, power_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

  // Add OTA upgrade client cluster
  esp_zb_ota_cluster_cfg_t ota_cluster_cfg = {
      .ota_upgrade_file_version = OTA_UPGRADE_FILE_VERSION,
      .ota_upgrade_downloaded_file_ver = OTA_UPGRADE_FILE_VERSION,
      .ota_upgrade_manufacturer = OTA_UPGRADE_MANUFACTURER,
      .ota_upgrade_image_type = OTA_UPGRADE_IMAGE_TYPE,
  };
  esp_zb_attribute_list_t *ota_cluster =
      esp_zb_ota_cluster_create(&ota_cluster_cfg);

  // Add additional required OTA attributes
  esp_zb_zcl_ota_upgrade_client_variable_t variable_config = {
      .timer_query = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,
      .hw_version = OTA_UPGRADE_HW_VERSION,
      .max_data_size = OTA_UPGRADE_MAX_DATA_SIZE,
  };
  uint16_t ota_upgrade_server_addr = 0xffff;
  uint8_t ota_upgrade_server_ep = 0xff;

  ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(
      ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,
      (void *)&variable_config));
  ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(
      ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID,
      (void *)&ota_upgrade_server_addr));
  ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(
      ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID,
      (void *)&ota_upgrade_server_ep));

  ESP_ERROR_CHECK(esp_zb_cluster_list_add_ota_cluster(
      cluster_list, ota_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));

  return cluster_list;
}

static void custom_temperature_sensor_ep_create(
    esp_zb_ep_list_t *ep_list, uint8_t endpoint_id,
    esp_zb_temperature_meas_cluster_cfg_t *temperature_sensor) {
  esp_zb_endpoint_config_t endpoint_config = {
      .endpoint = endpoint_id,
      .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
      .app_device_version = 0};
  esp_zb_cluster_list_t *t_cl =
      custom_temperature_sensor_clusters_create(temperature_sensor);
  esp_zb_ep_list_add_ep(ep_list, t_cl, endpoint_config);
}

static void esp_zb_task(void *pvParameters) {
  /* Initialize Zigbee stack */
  esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
  esp_zb_init(&zb_nwk_cfg);

  esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

  for (uint8_t ep = HA_ESP_TEMP_START_ENDPOINT;
       ep < (HA_ESP_TEMP_START_ENDPOINT + HA_ESP_NUM_T_SENSORS); ep++) {
    esp_zb_temperature_meas_cluster_cfg_t temp_sensor_cfg = {
        .max_value = zb_temperature_to_s16(ESP_TEMP_SENSOR_MAX_VALUE),
        .min_value = zb_temperature_to_s16(ESP_TEMP_SENSOR_MIN_VALUE),
        .measured_value = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN};

    ESP_LOGI(TAG, "Creating temperature endpoint: %d", ep);
    custom_temperature_sensor_ep_create(ep_list, ep, &temp_sensor_cfg);
  }
  ESP_LOGI(TAG, "Total temperature endpoints created: %d", HA_ESP_NUM_T_SENSORS);

  // Register OTA upgrade action handler
  esp_zb_core_action_handler_register(esp_zb_action_handler);

  esp_zb_device_register(ep_list);
  for (uint8_t ep = HA_ESP_TEMP_START_ENDPOINT;
       ep < (HA_ESP_TEMP_START_ENDPOINT + HA_ESP_NUM_T_SENSORS); ep++) {
    esp_zb_zcl_reporting_info_t reporting_info = {
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep = ep,
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .dst.endpoint = 1,
        .u.send_info.min_interval = 5,
        .u.send_info.max_interval = 300,
        .u.send_info.def_min_interval = 5,
        .u.send_info.def_max_interval = 300,
        .u.send_info.delta.u16 = 50,
        .attr_id = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };

    ESP_ERROR_CHECK(esp_zb_zcl_update_reporting_info(&reporting_info));
  }

  esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
  ESP_ERROR_CHECK(esp_zb_start(false));

  esp_zb_main_loop_iteration();
}

void app_main(void) {
  esp_zb_platform_config_t config = {
      .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
      .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
  };
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));

  /* Start Zigbee stack task */
  xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
