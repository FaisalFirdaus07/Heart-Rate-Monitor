# ESP32 Health Monitor — MAX30102 + OLED + DFPlayer Mini

A simple accessibility-focused health monitor prototype for ESP32 that reads heart rate (BPM) and estimates SpO2 using a MAX30102 sensor, displays results on an OLED, and gives spoken guidance and numeric readouts using a DFPlayer Mini and recorded voice prompts. The device is designed for low-vision users: spoken prompts guide the user to place a finger, confirm detection, announce results, and prompt to remove the finger.

> Important: This is a hobby/prototype project and NOT a clinical device. SpO2 calculation is an approximation using AC/DC ratio (R) and requires calibration and validated algorithms for medical use.

## Versions
This repository contains two sketches:

| Sketch | Purpose | Signal processing |
|---|---|---|
| `health_monitor_esp32-1.ino` | Standalone device (OLED + voice) | Instant BPM without smoothing, debounced finger detection (300 ms), jump rejection vs. last valid reading (>40% BPM, >8 points SpO2) with automatic baseline re-acquisition |
| `firebase_health_monitor_esp32.ino` | Connected version for remote monitoring | Same sensor setup and voice flow; BPM averaged over the last 4 beats, simple IR-threshold finger detection; pushes readings to Firebase Realtime Database every 3 s |

The sections below describe the standalone sketch unless stated otherwise. The Firebase-specific setup is in [Firebase version](#firebase-version).

## Features
- Real-time heart rate detection (instant BPM) using SparkFun MAX3010x library.
- SpO2 estimation using AC/DC ratio across IR and Red channels (buffer-based).
- OLED status and numeric display (SH1106/SSD1306 compatible).
- Non-blocking queued voice prompts and numeric speech using DFPlayer Mini and short recorded clips.
- Accessibility voice flow: prompts to place finger, confirmation, read results, and repeat flow.
- Noise filtering heuristics for both BPM and SpO2 to reject unlikely jumps.

## Hardware
- ESP32 (Dev Module)
- MAX30102 (heart-rate / SpO2 sensor)
- OLED display 128x64 (SH1106 or SSD1306)
- DFPlayer Mini + microSD card
- Speaker (e.g., 8Ω, 0.5W)
- Power supply / battery shield with 5V and common ground
- Wires / breadboard

## Wiring (as used in the sketch)
- I2C (MAX30102 + OLED):
  - SDA -> GPIO21
  - SCL -> GPIO22
- DFPlayer Mini (Serial2):
  - DFPlayer TX -> ESP32 RX2 (GPIO16)
  - DFPlayer RX -> ESP32 TX2 (GPIO17)
- Power:
  - MAX30102 VCC -> 3.3V
  - OLED VCC -> 3.3V
  - DFPlayer VCC -> 5V (from battery shield)
  - All grounds tied together (common GND)
- Speaker connected to DFPlayer SPK+/SPK- (or use DAC pin + amplifier)

Adjust pins in the sketch if your wiring differs.

## Software / Libraries (Arduino IDE)
Install via Library Manager:
- Adafruit GFX
- Adafruit SH110X (or Adafruit SSD1306 if your OLED uses SSD1306)
- SparkFun MAX3010x Pulse and Proximity Sensor Library
- DFRobotDFPlayerMini

Board: Select `ESP32 Dev Module`.

## DFPlayer / SD Card setup
- DFPlayer reads files from the `mp3` folder (uses dfPlayer.playMp3Folder()).
- Files must be named with 4-digit numeric names and `.mp3` extension: `0001.mp3`, `0002.mp3`, ...
- Track numbers used in the code map to file indices on the SD card. Update tracks to match your SD card layout.

Recommended tracks for accessibility and speech (the project expects these indices by default):
- 1: Boot greeting
- 2: Heart-rate alert tone (also used as prefix before BPM)
- 3: SpO2 alert tone
- 10–19: spoken units for digits 0–9 (TRACK_UNIT_BASE = 10)
  - 10: zero
  - 11: one
  - 12: two
  - 13: three
  - 14: four
  - 15: five
  - 16: six
  - 17: seven
  - 18: eight
  - 19: nine
- 20: ten
- 21: eleven
- 22: "-teen" suffix (used for 12–19, e.g. "two" + "-teen" = twelve)
- 23: "-ty" suffix (used for tens, e.g. "two" + "-ty" = twenty)
- 24: one hundred
- 25: "bpm"
- 26: "percent"
- 30: "Place your finger on the sensor"
- 31: "Finger detected, please hold still"
- 32: "Reading complete, you may remove your finger"

Note: the default clips were recorded in Indonesian, so the number composition (digit + "-teen" / "-ty" suffix) follows Indonesian grammar, where it is regular. To use English clips, adapt `speakNumber()` so that 11–19 and the tens use their own clips.

Customize track numbers by editing the #define constants at the top of the sketch.

## Accessibility voice flow
1. When idle, the device says "Place your finger on the sensor" every 15 seconds.
2. On finger detection: "Finger detected, please hold still".
3. After sufficient data is captured (buffer full + heartbeat seen): "Reading complete, you may remove your finger".
4. Immediately announces BPM (number + "bpm") then SpO2 (number + "percent").
5. Cycle resets and prompts again.

## How the detection works (brief)
- Heartbeats: Uses SparkFun's checkForBeat() on IR waveform. On each beat, the sketch calculates instantaneous BPM from beat-to-beat interval and applies a simple jump-rejection filter (reject >40% jump vs last valid).
- SpO2: Buffers 100 samples of IR and Red, computes DC (mean) and AC (RMS) then R = (redAC/redDC) / (irAC/irDC). Estimates SpO2 via an empirical formula est = 110 - 25 * R, constrained to 0–100. This is an approximation and needs calibration for accurate results.
- Both signals use simple rejection streaks and reset logic to reduce transient noise.

## Firebase version
`firebase_health_monitor_esp32.ino` adds Wi-Fi and Firebase Realtime Database on top of the same hardware.

1. Create a Firebase project, enable **Realtime Database** and **Authentication > Email/Password**, and create a user account for the device.
2. Install the extra library **FirebaseClient** (by Mobizt) from the Library Manager.
3. Fill in the placeholders at the top of the sketch before uploading:
   - `WIFI_SSID`, `WIFI_PASSWORD`
   - `FIREBASE_API_KEY`, `FIREBASE_DB_URL`
   - `FIREBASE_USER_EMAIL`, `FIREBASE_USER_PASSWORD`
4. Every 3 seconds the device writes to `/healthMonitor/latest/`:
   - `bpm` (int), `spo2` (float), `finger_detected` (bool)
   - `timestamp` (Unix time, synchronised with NTP at boot)

Notes:
- Never commit your real Wi-Fi or Firebase credentials. Keep the placeholders in the repository.
- The TLS client uses `setInsecure()` for prototyping (no certificate validation). Use a root certificate for anything beyond a prototype.
- Set Realtime Database rules so only the authenticated device user can write.

## Important notes & tips
- OLED driver: The sketch uses Adafruit SH110X by default (Adafruit_SH110X.h). Many 1.3" 128x64 modules use SH1106, not SSD1306. If your OLED is SSD1306-based, change the include and display object appropriately (see top-of-file comment).
- MAX30102 I2C address: The sketch expects address 0x57. Confirm yours with an I2C scanner if detection fails.
- Sensor setup: MAX30102 settings in sketch are tuned to avoid ADC saturation; you can adjust particleSensor.setup() parameters if you get flatlined IR values.
- DFPlayer: Some DFPlayer modules are sensitive to power and SD card format. Use good-quality 5V supply, and FAT32 SD card with properly named files. Set volume via dfPlayer.volume() if needed.
- Debug: Serial output prints finger states, reject events, and queue info — use the Serial Monitor at 115200 baud to see internal logs.

## Safety & Legal
- This project is for educational, prototyping, and personal use only.
- Not certified for medical diagnosis or monitoring. Do not rely on this device for critical health decisions. For medical purposes, use certified medical devices.

## Troubleshooting
- OLED shows shifted/garbled text: try switching between SH1106 and SSD1306 libraries/drivers.
- MAX30102 not detected: check SDA/SCL wiring, sensor VCC (3.3V), and I2C address.
- DFPlayer not detected: check Serial2 wiring (RX/TX swapped for DFPlayer) and SD card content/naming.
- No voice: verify speaker wiring and volume, confirm DFPlayer returns a successful init on Serial.

## Customization
- Change pin definitions at the top of the sketch to match your wiring.
- Tune noise thresholds and buffer size (BUF_SIZE) to balance latency vs stability.
- Record the voice clips in any language; adapt the DFPlayer track mapping and `speakNumber()` accordingly.

## Files
- `health_monitor_esp32-1.ino` — standalone sketch with the full signal-quality logic
- `firebase_health_monitor_esp32.ino` — connected sketch that streams readings to Firebase Realtime Database
- Audio files for DFPlayer — place in `/mp3` folder on SD card, numbered `0001.mp3`, `0002.mp3`, ...

## License & Credits
- This project is provided as-is for educational use. No warranty.
- Libraries used: Adafruit GFX, Adafruit SH110X/SSD1306, SparkFun MAX3010x, DFRobotDFPlayerMini, FirebaseClient (Firebase version only).
- Author: Faisal Firdaus
