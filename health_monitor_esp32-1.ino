#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>     
#include "MAX30105.h"            
#include "heartRate.h"
#include <DFRobotDFPlayerMini.h>

#define PIN_SDA   21
#define PIN_SCL   22
#define DF_RX_PIN 16   // ESP32 RX2 <- TX DFPlayer
#define DF_TX_PIN 17   // ESP32 TX2 -> RX DFPlayer

#define TRACK_BOOT         1
#define TRACK_HR_TINGGI    2   // dipakai jg sbg nada alert sebelum sebut angka saat BPM tinggi
#define TRACK_SPO2_RENDAH  3   // dipakai jg sbg nada alert sebelum sebut angka saat SpO2 rendah

#define TRACK_UNIT_BASE     10  // track angka 0-9 = TRACK_UNIT_BASE + digit
#define TRACK_NOL           10
#define TRACK_SATU          11
#define TRACK_DUA           12
#define TRACK_TIGA          13
#define TRACK_EMPAT         14
#define TRACK_LIMA          15
#define TRACK_ENAM          16
#define TRACK_TUJUH         17
#define TRACK_DELAPAN       18
#define TRACK_SEMBILAN      19
#define TRACK_SEPULUH       20
#define TRACK_SEBELAS       21
#define TRACK_BELAS         22  // sufiks "...belas" (dipakai utk 12-19)
#define TRACK_PULUH         23  // sufiks "...puluh" (dipakai utk 20,30,...,90)
#define TRACK_SERATUS       24
#define TRACK_BPM_SUFFIX    25  // kata "bpm"
#define TRACK_PERSEN_SUFFIX 26  // kata "persen"

#define TRACK_PROMPT_LETAKKAN    30  // "Letakkan jari Anda di sensor"
#define TRACK_PROMPT_TERDETEKSI  31  // "Jari terdeteksi, mohon tahan sebentar"
#define TRACK_PROMPT_ANGKAT      32  // "Pembacaan selesai, Anda boleh angkat jari"

#define MAX_SPEECH_TRACKS   6   // maks token per ucapan (nada alert + angka + sufiks satuan)

Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire, -1);
MAX30105 particleSensor;
DFRobotDFPlayerMini dfPlayer;

unsigned long lastBeatMs = 0;
int   bpmAvg = 0; // nama variabel dipertahankan (dipakai di banyak tempat), tapi isinya murni bacaan instan, tanpa rata-rata

int   lastValidBpm = 0;
byte  hrRejectStreak = 0;
const byte HR_REJECT_STREAK_RESET = 2;  // ditolak 2x beruntun -> acuan lama dianggap basi, ganti ke bacaan berikutnya
const float HR_MAX_JUMP = 0.4;          // tolak lompatan >40% dari bacaan valid sebelumnya (ciri noise, bukan detak asli)
bool  bpmCapturedThisCycle = false;     // sudah ada 1 bacaan BPM valid di siklus taruh-jari saat ini?

const int BUF_SIZE = 100;
uint32_t irBuf[BUF_SIZE];
uint32_t redBuf[BUF_SIZE];
int   bufIdx = 0;
bool  bufFull = false;
float spo2Estimate = 0;

float spo2LastValid = 0;
byte  spo2RejectStreak = 0;
const byte SPO2_REJECT_STREAK_RESET = 3;
const float SPO2_MAX_JUMP = 8.0; // tolak lompatan >8 poin persen dari bacaan valid sebelumnya
bool  fingerDetected = false;
bool  max30102Ok = false; // false kalau MAX30102 gagal terdeteksi saat setup()

bool  rawFingerNow = false;
unsigned long fingerStateChangeMs = 0;
const unsigned long FINGER_DEBOUNCE_MS = 300; // status baru dianggap sah kalau bertahan minimal segini lama

unsigned long lastDisplayMs = 0;
const unsigned long DISPLAY_INTERVAL_MS = 300;

int  speechQueue[MAX_SPEECH_TRACKS];
int  speechQueueLen = 0;
int  speechIndex = 0;
bool speechPlaying = false;
unsigned long speechTrackStartMs = 0;
const unsigned long SPEECH_TRACK_TIMEOUT_MS = 4000; // jaga-jaga kalau notifikasi "selesai" dari DFPlayer tidak sampai

enum GuidanceStage { STAGE_LETAKKAN, STAGE_TERDETEKSI, STAGE_SELESAI, STAGE_BPM, STAGE_SPO2 };
GuidanceStage stage = STAGE_LETAKKAN;

unsigned long lastPlacementPromptMs = 0;
const unsigned long PLACEMENT_PROMPT_INTERVAL_MS = 15000; // ingatkan "letakkan jari" tiap 15 detik saat idle

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  initOLED();
  initMAX30102();
  initDFPlayer();

  delay(500); // jeda sebelum suara perdana diputar
  int bootSeq[2] = { TRACK_BOOT, TRACK_PROMPT_LETAKKAN };
  queueTracks(bootSeq, 2);          // sapaan awal, lanjut langsung ke instruksi "letakkan jari"
  lastPlacementPromptMs = millis(); // reminder berkala berikutnya dihitung mulai dari sini
}

void loop() {
  uint32_t irValue  = 0;
  uint32_t redValue = 0;

  if (max30102Ok) {
    irValue  = particleSensor.getIR();
    redValue = particleSensor.getRed();
  }
  bool rawFinger = max30102Ok && (irValue > 50000);
  if (rawFinger != rawFingerNow) {
    rawFingerNow = rawFinger;
    fingerStateChangeMs = millis();
  }
  if (millis() - fingerStateChangeMs >= FINGER_DEBOUNCE_MS && fingerDetected != rawFingerNow) {
    fingerDetected = rawFingerNow;
    Serial.print("[Jari] Status berubah -> ");
    Serial.print(fingerDetected ? "TERDETEKSI" : "TIDAK ADA");
    Serial.print(" (irValue=");
    Serial.print(irValue);
    Serial.println(")");
  }

  if (fingerDetected) {
    updateHeartRate(irValue);
    updateSpO2Buffer(irValue, redValue);
  } else {
    bufIdx = 0;
    bufFull = false;
  }

  if (millis() - lastDisplayMs > DISPLAY_INTERVAL_MS) {
    lastDisplayMs = millis();
    updateDisplay();
  }

  pollDFPlayer();
  voiceGuidance();
}

void initOLED() {
  // begin(alamat_i2c, reset). 1.3" 128x64 umumnya SH1106 @ 0x3C.
  if (!display.begin(0x3C, true)) {
    Serial.println("OLED tidak terdeteksi di alamat 0x3C");
  }
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20);
  display.setTextSize(1);
  display.println("Menginisialisasi...");
  display.display();
}

void initMAX30102() {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST, 0x57)) {
    Serial.println("MAX30102 tidak terdeteksi di alamat 0x57 - cek wiring SDA/SCL");
    max30102Ok = false;
    return; // TIDAK hang lagi - program lanjut jalan supaya OLED & fitur lain tetap bisa dites
  }
  max30102Ok = true;

  particleSensor.setup(0x3F, 4, 2, 400, 411, 16384);
}

void initDFPlayer() {
  Serial2.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  delay(500); // beri waktu DFPlayer boot & baca kartu MicroSD sebelum dikirimi perintah

  if (!dfPlayer.begin(Serial2)) {
    Serial.println("DFPlayer Mini tidak terdeteksi - cek SD card & RX/TX");
  } else {
    Serial.println("DFPlayer berhasil terhubung!");
    delay(200);           // jeda sebelum perintah volume, cegah cip audio error/hang
    dfPlayer.volume(20);  // dinaikkan sementara utk debug (skala 0-30)
    delay(200);           // jeda sesudah perintah volume
  }
}

void updateHeartRate(uint32_t irValue) {
  if (!checkForBeat(irValue)) return;

  unsigned long now = millis();
  unsigned long delta = now - lastBeatMs;
  lastBeatMs = now;
  float bpm = 60.0 / (delta / 1000.0);

  if (bpm <= 50 || bpm >= 230) return; // di bawah 50 dianggap nggak wajar buat kondisi normal, abaikan total

  // Noise check: bandingkan ke 1 bacaan valid terakhir saja (bukan rata-rata beberapa bacaan)
  if (lastValidBpm > 0) {
    float jump = fabs(bpm - lastValidBpm) / lastValidBpm;
    if (jump > HR_MAX_JUMP) {
      hrRejectStreak++;
      Serial.print("[HR] Instan ");
      Serial.print(bpm);
      Serial.print(" bpm ditolak (noise, lompat ");
      Serial.print(jump * 100, 0);
      Serial.print("% dari bacaan valid terakhir ");
      Serial.println(lastValidBpm);

      // Ditolak terus-menerus -> acuan lama dianggap basi. Pakai bacaan INI sbg baseline baru, jangan dibuang juga
      if (hrRejectStreak >= HR_REJECT_STREAK_RESET) {
        Serial.println("[HR] Ditolak beruntun, ganti ke baseline baru dari bacaan ini");
        hrRejectStreak = 0;
        lastValidBpm = (int)bpm;
        bpmAvg = (int)bpm;
        bpmCapturedThisCycle = true;
        Serial.print("[HR] Detak terdeteksi, instan: ");
        Serial.println(bpmAvg);
      }
      return;
    }
  }

  hrRejectStreak = 0;
  lastValidBpm = (int)bpm;
  bpmAvg = (int)bpm; // tetap murni instan - cuma dipastikan bukan noise dulu, tanpa rata-rata/smoothing
  bpmCapturedThisCycle = true;

  Serial.print("[HR] Detak terdeteksi, instan: ");
  Serial.println(bpmAvg);
}

void updateSpO2Buffer(uint32_t irValue, uint32_t redValue) {
  irBuf[bufIdx]  = irValue;
  redBuf[bufIdx] = redValue;
  bufIdx++;
  if (bufIdx >= BUF_SIZE) {
    bufIdx = 0;
    bufFull = true;
  }
  if (bufFull) computeSpO2();
}

void computeSpO2() {
  double irDC = 0, redDC = 0;
  for (int i = 0; i < BUF_SIZE; i++) { irDC += irBuf[i]; redDC += redBuf[i]; }
  irDC /= BUF_SIZE;
  redDC /= BUF_SIZE;

  double irAC = 0, redAC = 0;
  for (int i = 0; i < BUF_SIZE; i++) {
    irAC  += sq((double)irBuf[i]  - irDC);
    redAC += sq((double)redBuf[i] - redDC);
  }
  irAC  = sqrt(irAC  / BUF_SIZE);
  redAC = sqrt(redAC / BUF_SIZE);

  if (irDC < 1 || redDC < 1 || irAC < 1) return;

  double R   = (redAC / redDC) / (irAC / irDC);
  double est = 110.0 - 25.0 * R;   // rumus rasio empiris umum -> PERKIRAAN, perlu kalibrasi utk akurasi medis
  est = constrain(est, 0, 100);

  // Noise filter: tolak lompatan besar dari bacaan valid sebelumnya (gerbang validasi, bukan rata-rata tambahan)
  if (spo2LastValid > 0) {
    float jump = fabs(est - spo2LastValid);
    if (jump > SPO2_MAX_JUMP) {
      spo2RejectStreak++;
      Serial.print("[SpO2] ");
      Serial.print(est, 2);
      Serial.print("% ditolak (noise, lompat ");
      Serial.print(jump, 1);
      Serial.print(" poin dari bacaan valid terakhir ");
      Serial.print(spo2LastValid, 1);
      Serial.println("%)");

      if (spo2RejectStreak >= SPO2_REJECT_STREAK_RESET) {
        Serial.println("[SpO2] Ditolak beruntun, reset acuan bacaan valid");
        spo2LastValid = 0;
      }
      return;
    }
  }

  spo2RejectStreak = 0;
  spo2LastValid = est;
  spo2Estimate = est;
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Health Monitor");
  display.drawLine(0, 10, 128, 10, SH110X_WHITE);

  display.setCursor(0, 18);
  if (!max30102Ok) {
    display.println("MAX30102 GAGAL!");
    display.println("Cek wiring SDA/SCL");
    display.println("& alamat 0x57");
  } else if (!fingerDetected) {
    display.println("Letakkan jari di sensor...");
  } else {
    display.setTextSize(2);
    display.print(bpmAvg);
    display.println(" bpm");
    display.setTextSize(1);
    display.setCursor(0, 42);
    display.print("SpO2 ~");
    display.print(spo2Estimate, 0);
    display.println("% (estimasi)");
  }
  display.display();
}

int numberToTracks(int n, int *out, int maxOut) {
  int count = 0;
  if (n < 0) n = 0;
  if (n > 199) n = 199;

  if (n == 0) {
    if (count < maxOut) out[count++] = TRACK_UNIT_BASE + 0;
    return count;
  }

  int hundreds  = n / 100;
  int remainder = n % 100;

  if (hundreds == 1 && count < maxOut) out[count++] = TRACK_SERATUS;

  if (remainder == 0) {
    // pas 100 - tidak ada token tambahan
  } else if (remainder < 10) {
    if (count < maxOut) out[count++] = TRACK_UNIT_BASE + remainder;
  } else if (remainder == 10) {
    if (count < maxOut) out[count++] = TRACK_SEPULUH;
  } else if (remainder == 11) {
    if (count < maxOut) out[count++] = TRACK_SEBELAS;
  } else if (remainder < 20) {
    if (count < maxOut) out[count++] = TRACK_UNIT_BASE + (remainder - 10);
    if (count < maxOut) out[count++] = TRACK_BELAS;
  } else {
    int tens  = remainder / 10;
    int units = remainder % 10;
    if (count < maxOut) out[count++] = TRACK_UNIT_BASE + tens;
    if (count < maxOut) out[count++] = TRACK_PULUH;
    if (units > 0 && count < maxOut) out[count++] = TRACK_UNIT_BASE + units;
  }
  return count;
}

void queueTracks(int *tracks, int count) {
  if (speechPlaying || count <= 0) return;

  int n = (count < MAX_SPEECH_TRACKS) ? count : MAX_SPEECH_TRACKS;
  for (int i = 0; i < n; i++) speechQueue[i] = tracks[i];

  speechQueueLen = n;
  speechIndex = 0;
  speechPlaying = true;
  speechTrackStartMs = millis();

  Serial.print("[DFPlayer] Antrean track: ");
  for (int i = 0; i < n; i++) { Serial.print(speechQueue[i]); Serial.print(" "); }
  Serial.println();

  dfPlayer.playMp3Folder(speechQueue[0]); // baca dari folder "mp3", bukan root SD card
}

// prefixTrack: nada alert opsional sebelum angka (0 = tanpa nada alert)
// suffixTrack: kata satuan sesudah angka, mis. TRACK_BPM_SUFFIX (0 = tanpa sufiks)
void speakNumber(int n, int prefixTrack, int suffixTrack) {
  if (speechPlaying) return; // jangan interupsi urutan yg sedang jalan

  int tmp[MAX_SPEECH_TRACKS];
  int len = 0;
  if (prefixTrack > 0 && len < MAX_SPEECH_TRACKS) tmp[len++] = prefixTrack;
  len += numberToTracks(n, &tmp[len], MAX_SPEECH_TRACKS - len);
  if (suffixTrack > 0 && len < MAX_SPEECH_TRACKS) tmp[len++] = suffixTrack;

  queueTracks(tmp, len);
}

// Putar satu klip panduan (mis. "letakkan jari") lewat antrean non-blocking yang sama
void speakSingle(int track) {
  int tmp[1] = { track };
  queueTracks(tmp, 1);
}

void voiceGuidance() {
  switch (stage) {

    case STAGE_LETAKKAN: // Tahap 1: menunggu jari diletakkan di sensor
      if (fingerDetected && !speechPlaying) {
        stage = STAGE_TERDETEKSI;
        speakSingle(TRACK_PROMPT_TERDETEKSI); // Tahap 2: "jari terdeteksi, mohon tahan sebentar"
      } else if (!fingerDetected && !speechPlaying &&
                 millis() - lastPlacementPromptMs > PLACEMENT_PROMPT_INTERVAL_MS) {
        lastPlacementPromptMs = millis();
        speakSingle(TRACK_PROMPT_LETAKKAN); // ingatkan berkala selagi idle
      }
      break;

    case STAGE_TERDETEKSI: // Tahap 2: menunggu pembacaan stabil (buffer penuh DAN sudah ada BPM valid)
      if (!fingerDetected) {
        stage = STAGE_LETAKKAN; // jari diangkat sebelum stabil -> ulang dari tahap 1
        lastPlacementPromptMs = millis();
      } else if (bufFull && bpmCapturedThisCycle && !speechPlaying) {
        stage = STAGE_SELESAI;
        speakSingle(TRACK_PROMPT_ANGKAT); // Tahap 3: "pembacaan selesai, anda boleh angkat jari"
      }
      break;

    case STAGE_SELESAI: // Tahap 3 baru selesai diputar
      if (!speechPlaying) {
        stage = STAGE_BPM;
        speakNumber(bpmAvg, 0, TRACK_BPM_SUFFIX); // Tahap 4: LANGSUNG, tanpa delay - angka BPM + "bpm"
      }
      break;

    case STAGE_BPM: // Tahap 4 baru selesai diucapkan -> LANGSUNG lanjut tahap 5, tanpa delay
      if (!speechPlaying) {
        stage = STAGE_SPO2;
        speakNumber((int)round(spo2Estimate), 0, TRACK_PERSEN_SUFFIX); // Tahap 5: angka SpO2 + "persen"
      }
      break;

    case STAGE_SPO2: // Tahap 5 baru selesai diucapkan -> siklus komplit, reset semua total
      if (!speechPlaying) {
        stage = STAGE_LETAKKAN; // reset ke tahap 1

        // Reset acuan noise-filter & angka yg ditampilkan/dikirim, biar tidak kebawa ke siklus berikutnya
        lastValidBpm = 0;
        hrRejectStreak = 0;
        bpmCapturedThisCycle = false;
        spo2LastValid = 0;
        spo2RejectStreak = 0;
        lastBeatMs = millis();
        bpmAvg = 0;
        spo2Estimate = 0;

        // Reset status deteksi jari juga -> siklus berikutnya wajib deteksi ulang dari nol
        fingerDetected = false;
        rawFingerNow = false;
        fingerStateChangeMs = millis();

        lastPlacementPromptMs = millis();
        speakSingle(TRACK_PROMPT_LETAKKAN);
      }
      break;
  }
}

void advanceSpeechQueue() {
  speechIndex++;
  if (speechIndex < speechQueueLen) {
    speechTrackStartMs = millis();
    dfPlayer.playMp3Folder(speechQueue[speechIndex]); // baca dari folder "mp3", bukan root SD card
  } else {
    speechPlaying = false;
  }
}


void pollDFPlayer() {
  if (dfPlayer.available()) {
    uint8_t type = dfPlayer.readType();
    dfPlayer.read(); // ambil nilai terkait supaya state internal library konsisten (tidak dipakai di sini)
    if (type == DFPlayerPlayFinished && speechPlaying) {
      advanceSpeechQueue();
    }
  }
  // jaga-jaga: kalau notifikasi "selesai" tidak pernah sampai, jangan macet selamanya
  if (speechPlaying && millis() - speechTrackStartMs > SPEECH_TRACK_TIMEOUT_MS) {
    advanceSpeechQueue();
  }
}
