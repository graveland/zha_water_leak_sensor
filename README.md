# Zigbee Water Leak Sensor

| Supported Targets | ESP32-C6 | ESP32-H2 |
| ----------------- | -------- | -------- |

This is a Zigbee water leak sensor using the IAS (Intruder Alarm System) Zone cluster. It's configured as a router device (powered by mains) and reports water leak status to your Zigbee coordinator.

## Hardware

- 1x ESP32-C6 (or ESP32-H2)
- 1x Water leak sensor probe (simple contact closure type)
- 1x 4.7-20k pull-up resistor to reduce interference / false triggers on the sensor. The built-in pull-up
  capabilities of the esp aren't good enough- I was getting constant false-positives without an external
  pull-up added.

The water leak sensor should be a simple two-wire probe that shorts to ground when water is detected.

## GPIO Configuration

- GPIO 14: Water leak sensor input (configurable via WATER_LEAK_GPIO in main/main.h)
  - HIGH (1) = No water detected (normal state with pull-up)
  - LOW (0) = Water detected (probe shorted to ground)

## Features

- **IAS Zone Device**: Properly reports as a water sensor (zone type 0x002a)
- **Router Device**: Mains-powered Zigbee router (not an end device)
- **Interrupt-Driven**: Immediate response using GPIO interrupts with 50ms debouncing
- **Low Power**: CPU-efficient interrupt-driven design instead of polling
- **OTA Updates**: Supports Over-The-Air firmware updates

## OTA Update Process

1. Increment OTA_UPGRADE_FILE_VERSION in main/main.h (e.g., from 2 to 3)
2. Build the firmware: `idf.py build`
3. Create OTA image: `python3 create_ota_image.py build/zha_water_leak_sensor.bin builds/water_leak_sensor_v3.zigbee 3`
4. Copy the .zigbee file to your coordinator's OTA directory
5. Trigger OTA update from your Zigbee coordinator (ZHA, Zigbee2MQTT, etc.)

## Building and Flashing

```bash
idf.py build
idf.py flash monitor
```

## Integration

This device will show up as an IAS Zone water sensor in:
- Home Assistant (ZHA)
- Zigbee2MQTT
- Other Zigbee coordinators supporting IAS zones

The sensor reports immediately when water is detected or cleared.
