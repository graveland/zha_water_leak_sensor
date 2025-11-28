#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"

/* Zigbee configuration */
#define INSTALLCODE_POLICY_ENABLE                                              \
  false /* enable the install code policy for security */
#define ED_AGING_TIMEOUT ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE 3000 /* 3000 millisecond */
#define HA_ESP_TEMP_START_ENDPOINT                                             \
  1 /* esp temperature sensor device endpoint, used for temperature            \
       measurement */
#define HA_ESP_NUM_T_SENSORS 1

#define ESP_ZB_PRIMARY_CHANNEL_MASK                                            \
  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK /* Zigbee primary channel mask use in   \
                                          the example */

#define ESP_TEMP_SENSOR_UPDATE_INTERVAL                                        \
  (10000000) /* Local sensor update interval (microsecond) */
#define ESP_TEMP_SENSOR_MIN_VALUE                                              \
  (-20) /* Local sensor min measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_MAX_VALUE                                              \
  (80) /* Local sensor max measured value (degree Celsius) */

/* Attribute values in ZCL string format
 * The string should be started with the length of its own.
 */
#define MANUFACTURER_NAME                                                      \
  "\x09"                                                                       \
  "graveland"
#define MODEL_IDENTIFIER                                                       \
  "\x12"                                                                       \
  "Temperature Sensor"

/* OTA Upgrade configuration */
#define OTA_UPGRADE_MANUFACTURER                                               \
  0x1234 /* Manufacturer code (must match OTA image) */
#define OTA_UPGRADE_IMAGE_TYPE 0x5679 /* Image type (must match OTA image) */
#define OTA_UPGRADE_FILE_VERSION 0x00000002 /* Current firmware version */
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

static void temp_timer_callback(void *arg);
