#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"

/* Zigbee configuration */
#define INSTALLCODE_POLICY_ENABLE                                              \
  false /* enable the install code policy for security */
#define ED_AGING_TIMEOUT ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE 3000 /* 3000 millisecond */
#define HA_ESP_LEAK_START_ENDPOINT 1 /* esp water leak sensor device endpoint */
#define HA_ESP_NUM_LEAK_SENSORS 1

#define ESP_ZB_PRIMARY_CHANNEL_MASK                                            \
  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK /* Zigbee primary channel mask use in   \
                                          the example */

#define WATER_LEAK_GPIO 14 /* GPIO pin for water leak sensor */
#define RGB_LED_GPIO 8 /* GPIO pin for RGB LED */
#define HEARTBEAT_INTERVAL_US (120000000) /* Heartbeat every 2m (microseconds) */
#define REPORT_COOLDOWN_MS 60000 /* 1 minute cooldown between reports */
#define SUPPRESSION_COUNTER_ATTR_ID 0xC000 /* Cumulative suppression counter */

#define HA_ESP_REBOOT_ENDPOINT 10 /* Separate endpoint for reboot switch */

/* Attribute values in ZCL string format
 * The string should be started with the length of its own.
 */
#define MANUFACTURER_NAME                                                      \
  "\x09"                                                                       \
  "graveland"
#define MODEL_IDENTIFIER                                                       \
  "\x11"                                                                       \
  "Water Leak Sensor"

/* OTA Upgrade configuration */
#define OTA_UPGRADE_MANUFACTURER                                               \
  0x1234 /* Manufacturer code (must match OTA image) */
#define OTA_UPGRADE_IMAGE_TYPE 0x567a /* Image type (must match OTA image) */
#define OTA_UPGRADE_FILE_VERSION 0x00000006 /* Current firmware version */
#define OTA_UPGRADE_HW_VERSION 0x0001       /* Hardware version */
#define OTA_UPGRADE_MAX_DATA_SIZE 64        /* OTA image block size */

/* OTA element format */
#define OTA_ELEMENT_HEADER_LEN                                                 \
  6 /* Header size: tag identifier (2) + length (4) */

typedef enum {
  UPGRADE_IMAGE = 0x0000, /* Upgrade image tag */
} esp_ota_element_tag_id_t;

#define ESP_ZB_ZED_CONFIG()                                                    \
  {                                                                            \
      .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                                    \
      .install_code_policy = INSTALLCODE_POLICY_ENABLE,                        \
      .nwk_cfg.zed_cfg =                                                       \
          {                                                                    \
              .ed_timeout = ED_AGING_TIMEOUT,                                  \
              .keep_alive = ED_KEEP_ALIVE,                                     \
          },                                                                   \
  }

#define ESP_ZB_ZR_CONFIG()                                                     \
  {                                                                            \
      .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,                                \
      .install_code_policy = INSTALLCODE_POLICY_ENABLE,                        \
      .nwk_cfg.zczr_cfg =                                                      \
          {                                                                    \
              .max_children = 10,                                              \
          },                                                                   \
  }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                                          \
  {                                                                            \
      .radio_mode = ZB_RADIO_MODE_NATIVE,                                      \
  }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                                           \
  {                                                                            \
      .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,                    \
  }

