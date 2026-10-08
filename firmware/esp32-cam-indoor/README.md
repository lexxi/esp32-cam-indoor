# Firmware

Arduino firmware for the AI-Thinker style ESP32-CAM + OV2640.

## Features

- OV2640 camera initialization
- Browser UI on port 80
- MJPEG stream on port 81
- JPEG snapshot endpoint
- JSON status endpoint
- Persistent Wi-Fi configuration in ESP32 NVS
- 30 second initial Wi-Fi connection timeout
- Automatic fallback access point if Wi-Fi is missing or unavailable
- RSSI-based roaming between access points that broadcast the same SSID
- Background Wi-Fi retry every 30 seconds while the fallback AP is active
- Fallback AP shuts down automatically after the configured Wi-Fi reconnects
- Wi-Fi configuration page with RSSI, BSSID, channel and reconnect counters
- Wi-Fi scan directly from fallback AP mode
- Scan table includes BSSID/MAC to help identify multiple SSIDs from the same access point
- Click a detected SSID and join it from the configuration page
- Persistent camera settings stored in ESP32 NVS
- Camera configuration page under `/camera`
- Resolution, JPEG quality, brightness, contrast, saturation, vertical flip and horizontal mirror
- Flash LED on/off control from the main page
- Extended status JSON with camera settings, sensor state and PSRAM diagnostics

## First setup

1. Install ESP32 board support in the Arduino IDE.
2. Select an ESP32-CAM / AI-Thinker compatible board.
3. Compile and upload using the ESP32-CAM-MB board.
4. Open the serial monitor at 115200 baud.
5. On first boot the camera starts a fallback AP because no Wi-Fi configuration exists.
6. Connect to the AP `ESP32-CAM-XXXXXX`.
7. Password: `esp32cam123`
8. Open `http://192.168.4.1/config`.
9. The page scans nearby 2.4-GHz WLANs.
10. Click the desired SSID, enter its password and select **Speichern und verbinden**.
11. The ESP32-CAM restarts and connects to the selected WLAN.

The Wi-Fi credentials are stored locally in ESP32 NVS and are not committed to Git.

## Fallback behavior

The Wi-Fi behavior intentionally follows the pattern used in the MGE UPS Controller project:

- Stored WLAN available: connect in station mode.
- No configuration or no connection within 30 seconds: start fallback AP.
- If credentials exist, fallback mode uses AP+STA so the configured WLAN can still be retried.
- Retry interval: 30 seconds.
- After a successful reconnect, the fallback AP is stopped automatically.
- The configuration page remains available under `/config`.
- In fallback mode the AP and station interfaces run together, so scanning and joining are possible without first leaving the camera AP.

## Camera configuration

Camera settings are stored in the ESP32 NVS namespace `camera` and survive reboot/power loss.

Current configurable values:

- Resolution: 160x120 up to 1600x1200
- JPEG quality: 4 to 63 (lower value = better image quality / larger frames)
- Brightness: -2 to 2
- Contrast: -2 to 2
- Saturation: -2 to 2
- Vertical flip
- Horizontal mirror

Changes are applied immediately and also used after the next reboot.

## Endpoints

- `http://<camera-ip>/` - browser UI
- `http://<camera-ip>/config` - Wi-Fi configuration
- `http://<camera-ip>/camera` - camera settings
- `http://<camera-ip>/jpg` - single JPEG snapshot
- `http://<camera-ip>/status` - extended status JSON
- `POST http://<camera-ip>/flash/on` - switch flash LED on
- `POST http://<camera-ip>/flash/off` - switch flash LED off
- `http://<camera-ip>:81/stream` - MJPEG stream

## Initial camera settings

- Resolution: VGA (640x480)
- JPEG quality: 12
- Two frame buffers when PSRAM is available
- Wi-Fi power saving disabled for better stream stability

The first goal is stability, not maximum resolution. Resolution and quality can be increased after the power supply and Wi-Fi connection have been tested under continuous load.


## Diagnostics

The `/status` endpoint includes:

- firmware version
- Wi-Fi state, RSSI, IP and reconnect counters
- free heap
- PSRAM detected, total PSRAM and free PSRAM
- camera sensor detected and sensor PID
- configured resolution and JPEG quality
- brightness, contrast and saturation
- vertical flip and horizontal mirror
- flash LED state

The flash LED uses GPIO 4 on the AI-Thinker ESP32-CAM. It is deliberately OFF after every reboot.


## Wi-Fi roaming

Firmware v0.6.0 adds simple client-side roaming for installations with multiple
access points using the same SSID.

- Roam check interval: 60 seconds
- A roam scan is only started when the current RSSI is below -72 dBm
- A different BSSID must be at least 6 dB better than the current AP
- The camera reconnects directly to the selected BSSID and channel
- If the roam does not succeed, normal reconnect/fallback handling resumes
- Roam attempts and successes are visible in `/status`

This hysteresis avoids unnecessary switching between access points with nearly
identical signal levels.
