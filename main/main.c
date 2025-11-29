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

#include "driver/gpio.h"
#include "led_strip.h"

static const char *TAG = "WATER_LEAK_SENSOR";

// IAS Zone status bits
#define IAS_ZONE_STATUS_ALARM1 (1 << 0)     // Water leak detected

// IAS Zone type for water sensor
#define IAS_ZONE_TYPE_WATER_SENSOR 0x002a

// Debounce settings
#define DEBOUNCE_TIME_MS 50
#define MIN_ALARM_DURATION_MS 5000 // Keep alarm active for at least 5 seconds

static bool last_leak_state = false;
static QueueHandle_t gpio_evt_queue = NULL;
static uint8_t ias_zone_id = 0xFF; // Will be set during enrollment
static int64_t alarm_start_time = 0; // Track when alarm started
static esp_timer_handle_t heartbeat_timer = NULL;
static led_strip_handle_t led_strip = NULL;
static esp_timer_handle_t led_flash_timer = NULL;
static bool led_flash_active = false;

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

// GPIO ISR handler - just sends event to queue
static void IRAM_ATTR gpio_isr_handler(void *arg) {
  uint32_t gpio_num = (uint32_t)arg;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void init_rgb_led(void) {
  // Configure LED strip (WS2812 or similar RGB LED on GPIO8)
  led_strip_config_t strip_config = {
      .strip_gpio_num = RGB_LED_GPIO,
      .max_leds = 1, // Single RGB LED
      .led_pixel_format = LED_PIXEL_FORMAT_GRB,
      .led_model = LED_MODEL_WS2812,
      .flags.invert_out = false,
  };

  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000, // 10MHz
      .flags.with_dma = false,
  };

  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

  // Turn off LED initially
  led_strip_clear(led_strip);
  ESP_LOGI(TAG, "RGB LED initialized on GPIO %d", RGB_LED_GPIO);
}

static void set_led_color(uint8_t red, uint8_t green, uint8_t blue) {
  if (led_strip) {
    led_strip_set_pixel(led_strip, 0, red, green, blue);
    led_strip_refresh(led_strip);
  }
}

static void led_flash_timer_callback(void *arg) {
  static bool flash_state = false;

  if (led_flash_active) {
    if (flash_state) {
      // Flash RED for leak alarm
      set_led_color(255, 0, 0);
    } else {
      set_led_color(0, 0, 0);
    }
    flash_state = !flash_state;
  }
}

static void start_led_flash(void) {
  if (!led_flash_active) {
    led_flash_active = true;

    // Create and start flash timer if not exists
    if (led_flash_timer == NULL) {
      const esp_timer_create_args_t flash_timer_args = {
          .callback = &led_flash_timer_callback,
          .name = "led_flash_timer"
      };
      ESP_ERROR_CHECK(esp_timer_create(&flash_timer_args, &led_flash_timer));
    }

    ESP_ERROR_CHECK(esp_timer_start_periodic(led_flash_timer, 500000)); // Flash every 500ms
    ESP_LOGI(TAG, "LED flashing started (RED)");
  }
}

static void stop_led_flash(void) {
  if (led_flash_active) {
    led_flash_active = false;

    if (led_flash_timer) {
      esp_timer_stop(led_flash_timer);
    }

    // Turn off LED
    set_led_color(0, 0, 0);
    ESP_LOGI(TAG, "LED flashing stopped");
  }
}

static void led_identify_blink(uint16_t identify_time) {
  // Blink BLUE for identify command
  ESP_LOGI(TAG, "Identify requested for %d seconds - blinking BLUE", identify_time);

  uint16_t blink_count = identify_time * 2; // Blink twice per second
  for (uint16_t i = 0; i < blink_count; i++) {
    set_led_color(0, 0, 255); // Blue
    vTaskDelay(pdMS_TO_TICKS(250));
    set_led_color(0, 0, 0); // Off
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  // Restore previous state
  if (led_flash_active) {
    // Will be restored by flash timer
  } else {
    set_led_color(0, 0, 0);
  }
}

static void init_water_leak_gpio(void) {
  // Create queue for GPIO events
  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

  // Configure GPIO
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << WATER_LEAK_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,  // Trigger on both rising and falling edges
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  // Install GPIO ISR service (may already be installed)
  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
  }

  // Attach interrupt handler
  ESP_ERROR_CHECK(gpio_isr_handler_add(WATER_LEAK_GPIO, gpio_isr_handler,
                                       (void *)WATER_LEAK_GPIO));

  // Read and log initial GPIO state
  int level = gpio_get_level(WATER_LEAK_GPIO);
  ESP_LOGI(TAG, "Water leak sensor GPIO %d initialized with interrupts, current level: %d",
           WATER_LEAK_GPIO, level);
}

void report_ias_zone_status(uint8_t ep, uint16_t zone_status, uint8_t zone_id) {
  // Use IAS Zone status change notification command
  esp_zb_zcl_ias_zone_status_change_notif_cmd_t notif_cmd;
  notif_cmd.zcl_basic_cmd.src_endpoint = ep;
  notif_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  notif_cmd.zone_status = zone_status;
  notif_cmd.extend_status = 0;
  notif_cmd.zone_id = zone_id;
  notif_cmd.delay = 0;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_ias_zone_status_change_notif_cmd_req(&notif_cmd);
  esp_zb_lock_release();

  ESP_LOGI(TAG, "Sent IAS Zone status change notification: status=0x%04x, zone_id=%d",
           zone_status, zone_id);
}

void esp_app_leak_sensor_handler(bool leak_detected, uint8_t endpoint) {
  uint16_t zone_status = 0;

  if (leak_detected) {
    zone_status = IAS_ZONE_STATUS_ALARM1;
    ESP_LOGW(TAG, "WATER LEAK DETECTED on endpoint %d!", endpoint);
  } else {
    zone_status = 0;
    ESP_LOGI(TAG, "No water leak on endpoint %d", endpoint);
  }

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_set_attribute_val(endpoint, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE,
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID,
                               &zone_status, false);
  esp_zb_lock_release();

  // Send zone status change notification
  report_ias_zone_status(endpoint, zone_status, ias_zone_id);
}

static bool read_water_leak_state(void) {
  // Read GPIO - LOW (0) indicates water detected (sensor shorted to ground)
  // HIGH (1) indicates no water (sensor open)
  int level = gpio_get_level(WATER_LEAK_GPIO);
  return (level == 0);
}

static void heartbeat_timer_callback(void *arg) {
  // Send periodic heartbeat to coordinator
  bool current_state = read_water_leak_state();

  ESP_LOGI(TAG, "Heartbeat: sending current status to coordinator (%s)",
           current_state ? "LEAK" : "NO LEAK");

  // Re-send current zone status as heartbeat
  uint16_t zone_status = current_state ? IAS_ZONE_STATUS_ALARM1 : 0;
  report_ias_zone_status(HA_ESP_LEAK_START_ENDPOINT, zone_status, ias_zone_id);
}

static void start_heartbeat_timer(void) {
  const esp_timer_create_args_t heartbeat_timer_args = {
      .callback = &heartbeat_timer_callback,
      .name = "heartbeat_timer"
  };

  ESP_ERROR_CHECK(esp_timer_create(&heartbeat_timer_args, &heartbeat_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeat_timer, HEARTBEAT_INTERVAL_US));
  ESP_LOGI(TAG, "Heartbeat timer started (interval: %d seconds)",
           (int)(HEARTBEAT_INTERVAL_US / 1000000));
}

static void handle_leak_state_change(bool leak_detected) {
  ESP_LOGI(TAG, "Water leak state changed: %s -> %s",
           last_leak_state ? "LEAK" : "NO LEAK",
           leak_detected ? "LEAK" : "NO LEAK");

  if (leak_detected) {
    // Water leak detected - start alarm
    alarm_start_time = esp_timer_get_time();
    last_leak_state = true;
    start_led_flash(); // Start flashing RED LED
    esp_app_leak_sensor_handler(true, HA_ESP_LEAK_START_ENDPOINT);
  } else {
    // Water cleared - check if minimum alarm duration has elapsed
    int64_t current_time = esp_timer_get_time();
    int64_t alarm_duration_ms = (current_time - alarm_start_time) / 1000;

    if (alarm_start_time > 0 && alarm_duration_ms < MIN_ALARM_DURATION_MS) {
      // Keep alarm active for minimum duration
      int64_t remaining_ms = MIN_ALARM_DURATION_MS - alarm_duration_ms;
      ESP_LOGI(TAG, "Keeping alarm active for %lld more ms (minimum duration)",
               remaining_ms);
      vTaskDelay(pdMS_TO_TICKS(remaining_ms));
    }

    // Clear alarm
    alarm_start_time = 0;
    last_leak_state = false;
    stop_led_flash(); // Stop flashing LED
    esp_app_leak_sensor_handler(false, HA_ESP_LEAK_START_ENDPOINT);
  }
}

// Task to handle GPIO events with debouncing
static void gpio_event_task(void *arg) {
  uint32_t gpio_num;
  TickType_t last_interrupt_time = 0;

  // Read initial state
  last_leak_state = read_water_leak_state();
  ESP_LOGI(TAG, "GPIO event task started. Initial water leak state: %s",
           last_leak_state ? "LEAK" : "NO LEAK");

  while (1) {
    if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY)) {
      ESP_LOGI(TAG, "GPIO interrupt received on pin %lu", gpio_num);
      TickType_t current_time = xTaskGetTickCount();

      // Debounce: ignore interrupts within DEBOUNCE_TIME_MS
      if ((current_time - last_interrupt_time) >
          pdMS_TO_TICKS(DEBOUNCE_TIME_MS)) {
        last_interrupt_time = current_time;

        // Read the current state immediately (debounce time check is sufficient)
        bool leak_detected = read_water_leak_state();
        ESP_LOGI(TAG, "Debounced GPIO read: %s (level=%d)",
                 leak_detected ? "LEAK" : "NO LEAK", gpio_get_level(WATER_LEAK_GPIO));

        // Only report if state actually changed
        if (leak_detected != last_leak_state) {
          handle_leak_state_change(leak_detected);
        } else {
          ESP_LOGI(TAG, "State unchanged, ignoring");
        }
      } else {
        ESP_LOGI(TAG, "Interrupt ignored due to debounce");
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
  ESP_LOGI(TAG, "Initializing RGB LED");
  init_rgb_led();

  ESP_LOGI(TAG, "Initializing water leak sensor GPIO with interrupts");
  init_water_leak_gpio();

  ESP_LOGI(TAG, "Starting GPIO event handler task");
  xTaskCreate(gpio_event_task, "gpio_event_task", 4096, NULL, 10, NULL);

  return ESP_OK;
}

static esp_err_t zb_ias_zone_cluster_attr_handler(
    esp_zb_zcl_set_attr_value_message_t *message) {
  esp_err_t ret = ESP_OK;

  ESP_LOGI(TAG, "Cluster attr write: cluster=0x%x, attr=0x%x",
           message->info.cluster, message->attribute.id);

  if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY) {
    // Handle identify command - attribute 0x0 is identify_time
    if (message->attribute.id == ESP_ZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID) {
      uint16_t identify_time = *(uint16_t *)message->attribute.data.value;
      ESP_LOGI(TAG, "Identify command received, time=%d seconds", identify_time);
      if (identify_time > 0) {
        led_identify_blink(identify_time);
      }
    }
  } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE) {
    if (message->attribute.id == ESP_ZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID) {
      ESP_LOGI(TAG, "IAS CIE address was set by coordinator");

      // Read the zone ID that was assigned during enrollment
      esp_zb_lock_acquire(portMAX_DELAY);
      esp_zb_zcl_attr_t *zone_id_attr = esp_zb_zcl_get_attribute(
          message->info.dst_endpoint, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE,
          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONEID_ID);
      if (zone_id_attr) {
        ias_zone_id = *(uint8_t *)zone_id_attr->data_p;
        ESP_LOGI(TAG, "IAS Zone ID: %d", ias_zone_id);
      }
      esp_zb_lock_release();

      // After CIE address is set, send initial zone status
      vTaskDelay(pdMS_TO_TICKS(500)); // Small delay to let enrollment complete
      bool leak_detected = read_water_leak_state();
      esp_app_leak_sensor_handler(leak_detected, message->info.dst_endpoint);
      ESP_LOGI(TAG, "Sent initial zone status: %s", leak_detected ? "LEAK" : "NO LEAK");

      // Start heartbeat timer now that we're enrolled
      if (heartbeat_timer == NULL) {
        start_heartbeat_timer();
      }
    }
  }

  return ret;
}

static esp_err_t zb_identify_handler(esp_zb_zcl_identify_effect_message_t *message) {
  ESP_LOGI(TAG, "Identify effect command received: effect_id=%d, effect_variant=%d",
           message->effect_id, message->effect_variant);

  // Blink LED for identify - use 5 seconds as default
  led_identify_blink(5);

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
  case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
    ret = zb_ias_zone_cluster_attr_handler(
        (esp_zb_zcl_set_attr_value_message_t *)message);
    break;
  case ESP_ZB_CORE_IDENTIFY_EFFECT_CB_ID:
    ret = zb_identify_handler((esp_zb_zcl_identify_effect_message_t *)message);
    break;
  case ESP_ZB_CORE_IAS_ZONE_ENROLL_RESPONSE_VALUE_CB_ID:
    // IAS Zone enrollment response - handled automatically by stack
    ESP_LOGI(TAG, "IAS Zone enrollment response received");
    break;
  case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
    // Default response from coordinator - normal Zigbee communication
    ESP_LOGD(TAG, "Received default response");
    break;
  case ESP_ZB_CORE_CMD_GREEN_POWER_RECV_CB_ID:
    // Green power cluster - not used, ignore silently
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

        // Check if already enrolled and start heartbeat timer
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_zcl_attr_t *zone_id_attr = esp_zb_zcl_get_attribute(
            HA_ESP_LEAK_START_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONEID_ID);
        if (zone_id_attr) {
          ias_zone_id = *(uint8_t *)zone_id_attr->data_p;
          ESP_LOGI(TAG, "Already enrolled with IAS Zone ID: %d", ias_zone_id);

          // Start heartbeat timer since we're already enrolled
          if (heartbeat_timer == NULL) {
            start_heartbeat_timer();
          }

          // Send current status
          vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for network to be fully ready
          bool leak_detected = read_water_leak_state();
          esp_app_leak_sensor_handler(leak_detected, HA_ESP_LEAK_START_ENDPOINT);
          ESP_LOGI(TAG, "Sent initial zone status on reboot: %s", leak_detected ? "LEAK" : "NO LEAK");
        }
        esp_zb_lock_release();
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

      // Set IAS Zone CIE address to the coordinator's address
      esp_zb_ieee_addr_t cie_addr;
      esp_zb_get_long_address(cie_addr);
      ESP_LOGI(TAG, "Setting IAS CIE address");
      esp_zb_lock_acquire(portMAX_DELAY);
      esp_zb_zcl_set_attribute_val(
          HA_ESP_LEAK_START_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE,
          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
          ESP_ZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID, cie_addr, false);
      esp_zb_lock_release();
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

static esp_zb_cluster_list_t *custom_water_leak_sensor_clusters_create(
    esp_zb_ias_zone_cluster_cfg_t *ias_zone_cfg) {
  esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

  // Add Basic cluster for device identification
  esp_zb_basic_cluster_cfg_t basic_cfg = {
      .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
      .power_source = 0x01,  // Mains (single phase) - this is a wall-powered router device
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

  // Add Identify cluster for device identification (LED blink)
  esp_zb_identify_cluster_cfg_t identify_cfg = {
      .identify_time = 0,
  };
  esp_zb_attribute_list_t *identify_cluster =
      esp_zb_identify_cluster_create(&identify_cfg);
  ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(
      cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

  // Add IAS Zone cluster for water leak detection
  esp_zb_attribute_list_t *ias_zone_cluster =
      esp_zb_ias_zone_cluster_create(ias_zone_cfg);
  ESP_ERROR_CHECK(esp_zb_cluster_list_add_ias_zone_cluster(
      cluster_list, ias_zone_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

  // Add Power Configuration cluster to report mains power
  esp_zb_power_config_cluster_cfg_t power_cfg = {
      .main_voltage = 0xffff, // Unknown mains voltage
      .main_alarm_mask = 0x00, // No alarms
  };
  esp_zb_attribute_list_t *power_cluster =
      esp_zb_power_config_cluster_create(&power_cfg);

  // Set battery percentage to 200 (0xC8) which indicates "AC/Mains powered" per Zigbee spec
  // This prevents battery low warnings in Zigbee2MQTT
  // Using raw attribute ID 0x0021 (BatteryPercentageRemaining)
  uint8_t battery_percentage = 200;
  ESP_ERROR_CHECK(esp_zb_power_config_cluster_add_attr(
      power_cluster, 0x0021,
      &battery_percentage));

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

static void custom_water_leak_sensor_ep_create(
    esp_zb_ep_list_t *ep_list, uint8_t endpoint_id,
    esp_zb_ias_zone_cluster_cfg_t *ias_zone_cfg) {
  esp_zb_endpoint_config_t endpoint_config = {
      .endpoint = endpoint_id,
      .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      .app_device_id = ESP_ZB_HA_IAS_ZONE_ID,
      .app_device_version = 0};
  esp_zb_cluster_list_t *cluster_list =
      custom_water_leak_sensor_clusters_create(ias_zone_cfg);
  esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);
}

static void esp_zb_task(void *pvParameters) {
  /* Initialize Zigbee stack */
  esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
  esp_zb_init(&zb_nwk_cfg);

  esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

  for (uint8_t ep = HA_ESP_LEAK_START_ENDPOINT;
       ep < (HA_ESP_LEAK_START_ENDPOINT + HA_ESP_NUM_LEAK_SENSORS); ep++) {
    esp_zb_ias_zone_cluster_cfg_t ias_zone_cfg = {
        .zone_state = 0x00,  // Not enrolled
        .zone_type = IAS_ZONE_TYPE_WATER_SENSOR,
        .zone_status = 0x0000,  // No alarm
    };

    ESP_LOGI(TAG, "Creating water leak sensor endpoint: %d", ep);
    custom_water_leak_sensor_ep_create(ep_list, ep, &ias_zone_cfg);
  }
  ESP_LOGI(TAG, "Total water leak sensor endpoints created: %d", HA_ESP_NUM_LEAK_SENSORS);

  // Register OTA upgrade action handler
  esp_zb_core_action_handler_register(esp_zb_action_handler);

  esp_zb_device_register(ep_list);

  // IAS Zone uses zone status change notifications, not periodic reporting
  // No need to configure reporting info like we do for temperature sensors

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
