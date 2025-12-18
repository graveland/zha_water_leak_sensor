#!/usr/bin/env python3
"""
Generate Zigbee OTA image file from ESP32 binary using zigpy.
This wraps the ESP32 firmware binary in Zigbee OTA format.

Requires: pip install zigpy
"""

import hashlib
import json
import os
import sys

try:
    import zigpy.ota.image
except ImportError:
    print("Error: zigpy is required. Install with: pip install zigpy")
    sys.exit(1)

# From main.h
MANUFACTURER_CODE = 0x1234
IMAGE_TYPE = 0x567a
DEFAULT_FILE_VERSION = 0x00000008

def create_zigbee_ota_file(input_bin, output_ota, file_version=DEFAULT_FILE_VERSION):
    """Create a Zigbee OTA file from ESP32 binary using zigpy."""

    if not os.path.exists(input_bin):
        print(f"Error: Input file {input_bin} not found")
        sys.exit(1)

    # Read the ESP32 binary
    with open(input_bin, 'rb') as f:
        firmware_data = f.read()

    firmware_size = len(firmware_data)

    print(f"Creating Zigbee OTA image...")
    print(f"  Manufacturer Code: 0x{MANUFACTURER_CODE:04X}")
    print(f"  Image Type: 0x{IMAGE_TYPE:04X}")
    print(f"  File Version: 0x{file_version:08X}")
    print(f"  Firmware Size: {firmware_size} bytes")

    # Create OTA header using zigpy
    header = zigpy.ota.image.OTAImageHeader(
        upgrade_file_id=zigpy.ota.image.OTAImageHeader.MAGIC_VALUE,
        header_version=0x0100,
        header_length=0,  # Will be calculated
        field_control=zigpy.ota.image.FieldControl(0),
        manufacturer_id=MANUFACTURER_CODE,
        image_type=IMAGE_TYPE,
        file_version=file_version,
        stack_version=2,  # Zigbee PRO
        header_string="ESP32-H2 Water Leak Sensor",
        image_size=0,  # Will be calculated
    )

    # Create the OTA image with subelements
    image = zigpy.ota.image.OTAImage(
        header,
        subelements=[
            zigpy.ota.image.SubElement(
                tag_id=zigpy.ota.image.ElementTagId.UPGRADE_IMAGE,
                data=firmware_data,
            )
        ],
    )

    # Calculate header length and image size
    image.header.header_length = len(image.header.serialize())
    image.header.image_size = image.header.header_length + len(image.subelements.serialize())

    print(f"  Header Length: {image.header.header_length} bytes")
    print(f"  Total OTA Size: {image.header.image_size} bytes")

    # Serialize and write
    ota_data = image.serialize()
    with open(output_ota, 'wb') as f:
        f.write(ota_data)

    print(f"✓ Created OTA image: {output_ota}")

    # Calculate SHA512 hash
    with open(output_ota, 'rb') as f:
        sha512_hash = hashlib.sha512(f.read()).hexdigest()

    # Generate index.json entry for zigbee2mqtt
    index_entry = {
        "fileName": os.path.basename(output_ota),
        "fileVersion": file_version,
        "fileSize": image.header.image_size,
        "url": f"./{os.path.basename(output_ota)}",  # Relative path for local files
        "imageType": IMAGE_TYPE,
        "manufacturerCode": MANUFACTURER_CODE,
        "sha512": sha512_hash,
        "otaHeaderString": str(image.header.header_string)
    }

    print(f"\nZigbee2MQTT index.json entry:")
    print(json.dumps(index_entry, indent=2))

    print(f"\nTo use this OTA image:")
    print(f"  1. Copy {output_ota} to your zigbee2mqtt data/ota/ directory")
    print(f"  2. Create or update data/ota/index.json with the entry above")
    print(f"  3. In configuration.yaml, set:")
    print(f"     ota:")
    print(f"       zigbee_ota_override_index_location: index.json")
    print(f"  4. Restart Zigbee2MQTT")
    print(f"  5. Trigger OTA update via frontend or MQTT")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 create_ota_image.py <input_binary> [output_ota] [file_version]")
        print("\nExample:")
        print("  python3 create_ota_image.py build/zha_water_leak_sensor.bin")
        print("  python3 create_ota_image.py build/zha_water_leak_sensor.bin output.ota 2")
        sys.exit(1)

    input_bin = sys.argv[1]
    output_ota = sys.argv[2] if len(sys.argv) > 2 else 'graveland_temperature_monitor.ota'
    file_version = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_FILE_VERSION

    create_zigbee_ota_file(input_bin, output_ota, file_version)
