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
- A different BSSID must be at least 4 dB better than the current AP
- The camera reconnects directly to the selected BSSID and channel
- If the roam does not succeed, normal reconnect/fallback handling resumes
- Roam attempts and successes are visible in `/status`

This hysteresis avoids unnecessary switching between access points with nearly
identical signal levels.

- Firmware v0.6.2 also logs the best roam candidate BSSID and its RSSI, even when the threshold is not met.


## Configurable roaming thresholds

Firmware v0.7.0 stores the roaming thresholds in NVS and exposes them on the
WLAN configuration page.

Defaults:

- Start roam scan below: -72 dBm
- Minimum improvement before switching AP: 4 dB

Allowed ranges:

- Trigger RSSI: -95 to -50 dBm
- Minimum improvement: 1 to 20 dB

The current values are also included in `/status`.


## NTP and snapshot timestamps

Firmware v0.8.0 adds local time synchronization using the same CET/CEST setup as
the MGE UPS controller:

- NTP servers: `pool.ntp.org`, `time.nist.gov`
- Time zone: Austria / Central Europe with automatic daylight-saving time
- Current synchronization state and local time are shown in `/status`
- NTP is synchronized after the initial Wi-Fi connection and after reconnects

A new camera setting can optionally burn a timestamp into `/jpg` snapshots.
The live MJPEG stream is intentionally left untouched.

Timestamp rendering requires decoding and re-encoding the JPEG. If the required
PSRAM buffer cannot be allocated at a large resolution, the firmware safely
falls back to returning the original snapshot without an overlay.


## v0.8.1 stability test

The NTP synchronization and local time reporting remain enabled, but the
experimental JPG timestamp overlay has been removed again. This returns
snapshot handling to the direct JPEG path used before v0.8.0 so stream
stability can be compared cleanly with the v0.7.x firmware.


## v0.8.2 stream baseline test

NTP/time support has been removed again for a clean A/B comparison with the
known-smooth v0.7.x stream behavior. Camera, roaming, flash LED, Wi-Fi settings
and diagnostics remain unchanged.


## v0.8.3 resolution-aware framebuffer policy

To reduce `cam_hal: FB-OVF` errors at larger resolutions, the camera now uses
different buffering modes depending on resolution:

- QQVGA through VGA: 2 framebuffers with `CAMERA_GRAB_LATEST` for smoother,
  lower-latency streaming.
- SVGA through UXGA: 1 framebuffer with `CAMERA_GRAB_WHEN_EMPTY` for improved
  stability at larger frame sizes.

When changing between these two resolution classes, the camera restarts once so
the framebuffer allocation is recreated with the correct policy. Changes within
the same class are still applied immediately.


## v0.8.5 non-blocking NTP and serial heartbeat

NTP support is enabled again, but unlike the earlier test it no longer waits
synchronously during startup. SNTP is configured after Wi-Fi connects and
synchronizes in the background.

- Time zone: CET/CEST with automatic daylight-saving time
- NTP servers: `pool.ntp.org`, `time.nist.gov`
- Local time and synchronization state are exposed in `/status`
- The root page shows the current local time when synchronized
- The JPG and MJPEG paths are unchanged; no image timestamp rendering is used
- The serial console prints a compact firmware/status heartbeat every 60 seconds
  with firmware version, uptime, IP/RSSI/BSSID, camera resolution/quality and
  memory information


## v0.8.6 persistent system log

The camera can now keep its own persistent application log in LittleFS so a
USB serial connection is no longer required for normal diagnostics.

- Log file: `/system.log`
- Maximum size: 128 KiB; when the limit is reached the file is rotated by
  starting a fresh log
- Web view: `/logs`
- Full download: `/logs/download`
- Clear log: `/logs/clear`
- The root page links directly to the system log
- Existing serial output remains active; application messages are written to
  both Serial and the persistent log
- Before NTP synchronization, entries use uptime-based timestamps. Afterwards
  they use local CET/CEST time.

Low-level ESP-IDF/camera-driver messages that are emitted directly by the
framework (for example some `cam_hal` diagnostics) are not guaranteed to pass
through the application logger.


## v0.8.8 RSSI range tracking and camera init retry

- Tracks minimum and maximum Wi-Fi RSSI since boot.
- The 60-second firmware heartbeat now includes current RSSI plus min/max values.
- `/status` exposes `rssi_min` and `rssi_max`.
- Camera startup now retries initialization up to three times.
- Between failed attempts, the OV2640 sensor is power-cycled through the PWDN pin.
- A partial camera-driver state is deinitialized before retrying.
- If all three attempts fail, the existing ESP restart fallback remains in place.
