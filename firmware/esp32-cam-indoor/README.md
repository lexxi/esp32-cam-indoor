# Firmware

Initial Arduino firmware for the AI-Thinker style ESP32-CAM + OV2640.

## Features

- Wi-Fi station mode
- Automatic reconnect
- OV2640 camera initialization
- Browser UI on port 80
- MJPEG stream on port 81
- JPEG snapshot endpoint
- JSON status endpoint with IP, RSSI, uptime and free heap
- No Wi-Fi credentials committed to Git

## Files

- `esp32-cam-indoor.ino` - main firmware
- `camera_pins.h` - AI-Thinker ESP32-CAM pin mapping
- `secrets.example.h` - Wi-Fi credential template

## First setup

1. Install ESP32 board support in the Arduino IDE.
2. Select an ESP32-CAM / AI-Thinker compatible board.
3. Copy `secrets.example.h` to `secrets.h`.
4. Enter the Wi-Fi SSID and password in `secrets.h`.
5. Compile and upload using the ESP32-CAM-MB board.
6. Open the serial monitor at 115200 baud.
7. After boot, open the displayed IP address in a browser.

The local `secrets.h` file is ignored by Git and must not be committed.

## Endpoints

- `http://<camera-ip>/` - browser UI
- `http://<camera-ip>/jpg` - single JPEG snapshot
- `http://<camera-ip>/status` - status JSON
- `http://<camera-ip>:81/stream` - MJPEG stream

## Initial camera settings

- Resolution: VGA (640x480)
- JPEG quality: 12
- Two frame buffers when PSRAM is available
- Wi-Fi power saving disabled for better stream stability

The first goal is stability, not maximum resolution. Resolution and quality can be increased after the power supply and Wi-Fi connection have been tested under continuous load.
