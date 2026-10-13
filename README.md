# ESP32 Health Monitor — MAX30102 + OLED + DFPlayer Mini

A simple accessibility-focused health monitor prototype for ESP32 that reads heart rate (BPM) and estimates SpO2 using a MAX30102 sensor, displays results on an OLED, and gives spoken guidance and numeric readouts using a DFPlayer Mini and recorded voice prompts (Indonesian). The device is designed for low-vision users: spoken prompts guide the user to place a finger, confirm detection, announce results, and prompt to remove the finger.

> Important: This is a hobby/prototype project and NOT a clinical device. SpO2 calculation is an approximation using AC/DC ratio (R) and requires calibration and validated algorithms for medical use.

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
  - 10: "nol" (zero)
  - 11: "satu"
  - 12: "dua"
  - 13: "tiga"
  - 14: "empat"
  - 15: "lima"
  - 16: "enam"
  - 17: "tujuh"
  - 18: "delapan"
  - 19: "sembilan"
- 20: "sepuluh"
- 21: "sebelas"
- 22: "belas" (suffix for 12–19)
- 23: "puluh" (suffix for tens: 20,30,...)
- 24: "seratus"
- 25: "bpm"
- 26: "persen"
- 30: "Letakkan jari Anda di sensor"
- 31: "Jari terdeteksi, mohon tahan sebentar"
- 32: "Pembacaan selesai, Anda boleh angkat jari"

Customize track numbers by editing the #define constants at the top of the sketch.

## Accessibility voice flow
1. When idle, the device says: "Letakkan jari Anda di sensor" every 15 seconds.
2. On finger detection: "Jari terdeteksi, mohon tahan sebentar".
3. After sufficient data is captured (buffer full + heartbeat seen): "Pembacaan selesai, Anda boleh angkat jari".
4. Immediately announces BPM (number + "bpm") then SpO2 (number + "persen").
5. Cycle resets and prompts again.

## How the detection works (brief)
- Heartbeats: Uses SparkFun's checkForBeat() on IR waveform. On each beat, the sketch calculates instantaneous BPM from beat-to-beat interval and applies a simple jump-rejection filter (reject >40% jump vs last valid).
- SpO2: Buffers 100 samples of IR and Red, computes DC (mean) and AC (RMS) then R = (redAC/redDC) / (irAC/irDC). Estimates SpO2 via an empirical formula est = 110 - 25 * R, constrained to 0–100. This is an approximation and needs calibration for accurate results.
- Both signals use simple rejection streaks and reset logic to reduce transient noise.

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
- Replace Indonesian voice clips with other languages if desired; adapt DFPlayer track mapping accordingly.

## Files
- health_monitor_esp32-1.ino — main Arduino sketch (this project)
- Audio files for DFPlayer — place in `/mp3` folder on SD card, numbered `0001.mp3`, `0002.mp3`, ...

## License & Credits
- This project is provided as-is for educational use. No warranty.
- Libraries used: Adafruit GFX, Adafruit SH110X/SSD1306, SparkFun MAX3010x, DFRobotDFPlayerMini.
- Author: (Your name / project author). Adapt as you like.

---

If you'd like, I can:
- Produce a shorter README or a localized Indonesian version.
- Generate a ready-to-commit file and create a commit/branch in your GitHub repo (I can do that if you provide repo owner/name).
- Create a checklist for recording the DFPlayer voice clips (timings, recommended formats).