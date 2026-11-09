Matt's BLE Tracker
Matt's BLE Tracker is a portable ESP32-S3 based Bluetooth Low Energy scanning, identification, and proximity tracking platform.

The project is currently built around the LILYGO T-Display-S3 Touch and a custom PCB and 3D-printed enclosure.

Current Firmware
Current stable release:

ESP32PocketRF_v3_8_Intelligence

Previous known-good firmware is retained in the firmware archive.

Features
Active Bluetooth Low Energy scanning
Complete Local Name parsing
Shortened Local Name parsing
BLE scan-response processing
Manufacturer identification
Intelligent fallback device names
Device-type recognition
Known/New device tagging
Device Details view
RSSI proximity tracking
RSSI trend graph
Automatic rescanning
Touchscreen navigation
Three proximity LEDs
Dedicated STATUS LED
Battery monitoring
Deep-sleep power management
Wake from either physical button
Hardware
LILYGO T-Display-S3 Touch
ESP32-S3
1.9-inch capacitive touchscreen
Custom carrier PCB
Four external indicator LEDs
LiPo battery
Custom 3D-printed enclosure
GPIO Assignments
Function	GPIO
WEAK proximity LED	11
MED proximity LED	12
STRONG proximity LED	13
STATUS LED	43
Physical Button 1	0
Physical Button 2	14
Display Power	15
Battery Monitor	4
Touch SDA	18
Touch SCL	17
Touch Interrupt	16
Touch Reset	21
Power Controls
Sleep
Hold both physical buttons for approximately 1.5 seconds.

Wake
Press either physical button.

The RESET button is not required for normal wake-up.

Device Naming
The firmware attempts to identify BLE devices using:

Complete Local Name
Shortened Local Name
Scan-response Local Name
Manufacturer plus abbreviated BLE address
Abbreviated BLE address
Some BLE peripherals do not broadcast their user-assigned name. In those cases, the fallback identifier is expected behavior.

Repository Structure
firmware/
    stable/
        Current supported firmware

    archive/
        Previous known-good firmware

docs/
    Programming and troubleshooting documentation
Programming
See:

docs/Matts_BLE_Tracker_Programming_Guide.docx

for complete Arduino IDE installation, configuration, programming, upload, verification, and troubleshooting instructions.

Project Status
Active development.

The stable firmware should not be modified directly. New development versions should receive a new firmware version number.
