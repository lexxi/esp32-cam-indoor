# ESP32-CAM Indoor

A small DIY indoor Wi-Fi camera project based on the ESP32-CAM and OV2640 camera module.

The goal is a compact, maintainable camera for indoor use, powered by a standard 5 V USB wall adapter and housed in a 3D-printed enclosure.

## Current hardware

- ESP32-CAM (AI-Thinker style)
- OV2640 camera module
- ESP32-CAM-MB USB programmer / power board
- 5 V / 1 A USB wall adapter
- Internal PCB Wi-Fi antenna
- Optional U.FL/IPEX to SMA pigtail with external antenna

## Project goals

- Stable indoor video streaming over Wi-Fi
- Simple USB power supply
- No exposed mains voltage inside the DIY enclosure
- Compact 3D-printable housing
- Maintain access to USB / reset for service and reflashing
- Optional external Wi-Fi antenna
- Public documentation of hardware, firmware and enclosure

## Repository structure

```text
firmware/   ESP32-CAM firmware
hardware/   Wiring, hardware notes and measurements
enclosure/  3D-printable enclosure files
docs/       Additional documentation and images
```

## Power

The current test setup uses a 5 V / 1 A USB wall adapter connected to the ESP32-CAM-MB via a short USB cable.

Before designing the final enclosure, the camera should be tested under continuous Wi-Fi/video load to verify that the power supply is stable. If brownouts or spontaneous resets occur, a higher-current 5 V supply should be tested.

## Wi-Fi antenna

The ESP32-CAM board provides both a PCB antenna and a U.FL/IPEX connector. The board must be configured for the selected antenna path; simply attaching an external antenna does not automatically switch from the PCB antenna.

For the first prototype, the internal PCB antenna will be used. The external antenna is optional if signal quality at the final installation location is insufficient.

## Safety

This project intentionally keeps mains voltage inside a complete, enclosed USB wall adapter. The 230 V power supply is not opened or integrated as a bare module into the 3D-printed camera enclosure.

## Status

Early prototype / hardware evaluation.

## License

MIT License.
