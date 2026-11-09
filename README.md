Matt's BLE Tracker
Matt's BLE Tracker

<img width="4032" height="511" alt="image" src="https://github.com/user-attachments/assets/1b933224-625f-4791-9b9d-7d782d018d1c" />


Matt's BLE Tracker is a portable ESP32-S3 based Bluetooth Low Energy scanning, identification, and proximity tracking platform.

The project is currently built around the LILYGO T-Display-S3 Touch, a custom carrier PCB, external indicator LEDs, LiPo battery, and a custom 3D-printed enclosure.

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
Automatic BLE rescanning
Touchscreen navigation
Three proximity LEDs
Dedicated STATUS LED
Battery monitoring
Display power management
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

The display also turns off automatically after approximately 90 seconds of inactivity.

Device Naming
The firmware attempts to identify BLE devices using:

Complete Local Name
Shortened Local Name
Scan-response Local Name
Manufacturer plus abbreviated BLE address
Abbreviated BLE address
Some BLE peripherals do not broadcast their user-assigned name. In those cases, the fallback identifier is expected behavior.

Proximity Tracking
A selected BLE device can be monitored using RSSI signal strength.

The tracking interface provides:

Current RSSI
Peak RSSI
RSSI trend graph
Observation count
GETTING CLOSER indication
STEADY indication
GETTING WEAKER indication
Physical LED proximity indicators
LED Thresholds
Indicator	Approximate RSSI
WEAK	-85 dBm or stronger
MED	-70 dBm or stronger
STRONG	-55 dBm or stronger
The STATUS LED provides general operating-state feedback.

Repository Structure
firmware/stable/ - Current supported firmware
firmware/archive/ - Previous known-good firmware
docs/ - Programming and troubleshooting documentation
hardware/ - Hardware and enclosure design files
images/ - Project photos and screenshots
Programming
Complete Arduino IDE installation, configuration, programming, upload, verification, and troubleshooting instructions are available in:

docs/Matts_BLE_Tracker_Programming_Guide.docx

Stable Firmware
Current stable firmware:

firmware/stable/ESP32PocketRF_v3_8_Intelligence.ino

Previous known-good versions are stored under:

firmware/archive/

New firmware revisions should receive a new version number rather than overwriting the stable release.

Releases
Current stable release:

v3.8 Intelligence

Stable packaged versions are available from the GitHub Releases section.

Responsible Use
Matt's BLE Tracker is intended for Bluetooth Low Energy experimentation, troubleshooting, device discovery, education, and authorized wireless analysis.

Users are responsible for complying with applicable laws and obtaining appropriate authorization when analyzing devices they do not own.

License
This project is released under the MIT License.

You are free to use, modify, distribute, and build upon this project in accordance with the terms of the MIT License.

See the LICENSE file for details.

Project Status
Active development

Current stable firmware:

v3.8 Intelligence
