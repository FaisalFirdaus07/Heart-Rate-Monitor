#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>   // OLED saya pakai SH1106, kalau SSD1306 ganti library

#include "MAX30105.h"
#include "heartRate.h"

#include <DFRobotDFPlayerMini.h>

// isi sendiri sebelum upload
#define WIFI_SSID               "name_wifi"
#define WIFI_PASSWORD           "password_wifi"

#define FIREBASE_API_KEY        "api_key_firebase"
#define FIREBASE_DB_URL         "https://nama-project-default-rtdb.firebaseio.com/"
#define FIREBASE_USER_EMAIL     "email@example.com"
#define FIREBASE_USER_PASSWORD  "password_account"

// pin
#define PIN_SDA   21
#define PIN_SCL   22
#define DF_RX_PIN 16
#define DF_TX_PIN 17

// track DFPlayer
#define TRACK_BOOT         1
#define TRACK_HR_TINGGI    2
#define TRACK_SPO2_RENDAH  3

#define TRACK_UNIT_BASE     10  // angka 0-9 = 10 + digit
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
#define TRACK_BELAS         22
#define TRACK_PULUH         23
#define TRACK_SERATUS       24
#define TRACK_BPM_SUFFIX    25
#define TRACK_PERSEN_SUFFIX 26

#define TRACK_PROMPT_LETAKKAN    30
#define TRACK_PROMPT_TERDETEKSI  31
#define TRACK_PROMPT_ANGKAT      32

#define MAX_SPEECH_TRACKS   6

Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire, -1);
MAX30105 particleSensor;
DFRobotDFPlayerMini dfPlayer;

void processData(AsyncResult &aResult);
UserAuth user_auth(FIREBASE_API_KEY, FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD);
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

// heart rate
const byte RATE_ARR_SIZE = 4;
byte  rateArr[RATE_ARR_SIZE];
byte  rateSpot = 0;
unsigned long lastBeatMs = 0;
int   bpmAvg = 0;

// SpO2 - masih estimasi kasar dari rasio AC/DC, belum dikalibrasi
const int BUF_SIZE = 100;
uint32_t irBuf[BUF_SIZE];
uint32_t redBuf[BUF_SIZE];
int   bufIdx = 0;
bool  bufFull = false;
float spo2Estimate = 0;
bool  fingerDetected = false;

// timer
unsigned long lastFirebaseMs = 0;
const unsigned long FIREBASE_INTERVAL_MS = 3000;
unsigned long lastDisplayMs = 0;
const unsigned long DISPLAY_INTERVAL_MS = 300;
unsigned long lastAlertMs = 0;
const unsigned long ALERT_COOLDOWN_MS = 30000;

// antrean suara
int  speechQueue[MAX_SPEECH_TRACKS];
int  speechQueueLen = 0;
int  speechIndex = 0;
bool speechPlaying = false;
unsigned long speechTrackStartMs = 0;
const unsigned long SPEECH_TRACK_TIMEOUT_MS = 4000;

unsigned long lastAnnounceMs = 0;
const unsigned long ANNOUNCE_INTERVAL_MS = 8000;
bool announceSpo2Next = false;   // gantian bpm / spo2

// panduan suara
bool prevFingerDetected = false;
bool readingAnnouncedThisSession = false;
unsigned long lastPlacementPromptMs = 0;
const unsigned long PLACEMENT_PROMPT_INTERVAL_MS = 15000;

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  initOLED();
  initMAX30102();
  initDFPlayer();
  connectWiFi();
  initFirebase();

  delay(500);
  int bootSeq[2] = { TRACK_BOOT, TRACK_PROMPT_LETAKKAN };
  queueTracks(bootSeq, 2);
  lastPlacementPromptMs = millis();
}

void loop() {
  app.loop();   // harus jalan terus buat auth firebase

  uint32_t irValue  = particleSensor.getIR();
  uint32_t redValue = particleSensor.getRed();
  fingerDetected = (irValue > 50000);

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
  checkAlerts();

  if (millis() - lastFirebaseMs > FIREBASE_INTERVAL_MS && app.ready()) {
    lastFirebaseMs = millis();
    sendToFirebase();
  }
}

// ---------------- init ----------------

void initOLED() {
  if (!display.begin(0x3C, true)) {
    Serial.println("OLED tidak terdeteksi (0x3C)");
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
    Serial.println("MAX30102 tidak terdeteksi, cek SDA/SCL");
    while (true) delay(1000);
  }
  // power, avg, ledMode (2 = red+IR), sample rate, pulse width, adc range
  particleSensor.setup(0x1F, 4, 2, 400, 411, 4096);
}

void initDFPlayer() {
  Serial2.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  delay(500);   // nunggu DFPlayer baca SD card

  if (!dfPlayer.begin(Serial2)) {
    Serial.println("DFPlayer tidak terdeteksi, cek SD card & RX/TX");
  } else {
    Serial.println("DFPlayer OK");
    delay(200);   // tanpa delay kadang perintah volume bikin DFPlayer hang
    dfPlayer.volume(20);
    delay(200);
  }
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" OK: " + WiFi.localIP().toString());
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // buat timestamp
}

void initFirebase() {
  ssl_client.setInsecure();   // TODO: pakai sertifikat kalau sudah bukan prototipe
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(FIREBASE_DB_URL);
}

// ---------------- HR & SpO2 ----------------

void updateHeartRate(uint32_t irValue) {
  if (checkForBeat(irValue)) {
    unsigned long now = millis();
    unsigned long delta = now - lastBeatMs;
    lastBeatMs = now;
    float bpm = 60.0 / (delta / 1000.0);
    if (bpm > 20 && bpm < 255) {
      rateArr[rateSpot++] = (byte)bpm;
      rateSpot %= RATE_ARR_SIZE;
      int sum = 0;
      for (byte i = 0; i < RATE_ARR_SIZE; i++) sum += rateArr[i];
      bpmAvg = sum / RATE_ARR_SIZE;
    }
  }
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
  double est = 110.0 - 25.0 * R;   // rumus empiris, perlu kalibrasi
  spo2Estimate = constrain(est, 0, 100);
}

// ---------------- display & alert ----------------

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Health Monitor");
  display.drawLine(0, 10, 128, 10, SH110X_WHITE);

  display.setCursor(0, 18);
  if (!fingerDetected) {
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

void checkAlerts() {
  if (!fingerDetected) return;

  bool bpmDarurat  = bpmAvg > 120;
  bool spo2Darurat = bufFull && spo2Estimate > 0 && spo2Estimate < 92;
  if (!speechPlaying && (bpmDarurat || spo2Darurat) && millis() - lastAlertMs > ALERT_COOLDOWN_MS) {
    lastAlertMs = millis();
    lastAnnounceMs = millis();
    if (bpmDarurat) {
      speakNumber(bpmAvg, TRACK_HR_TINGGI, TRACK_BPM_SUFFIX);
    } else {
      speakNumber((int)round(spo2Estimate), TRACK_SPO2_RENDAH, TRACK_PERSEN_SUFFIX);
    }
    return;
  }

  // normal: sebut bpm dan spo2 gantian tiap 8 detik
  if (!speechPlaying && millis() - lastAnnounceMs > ANNOUNCE_INTERVAL_MS) {
    lastAnnounceMs = millis();
    if (announceSpo2Next && bufFull) {
      speakNumber((int)round(spo2Estimate), 0, TRACK_PERSEN_SUFFIX);
    } else {
      speakNumber(bpmAvg, 0, TRACK_BPM_SUFFIX);
    }
    announceSpo2Next = !announceSpo2Next;
  }
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
    // pas 100
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
  dfPlayer.playMp3Folder(speechQueue[0]);
}

// prefix = nada alert, suffix = "bpm"/"persen", isi 0 kalau tidak dipakai
void speakNumber(int n, int prefixTrack, int suffixTrack) {
  if (speechPlaying) return;

  int tmp[MAX_SPEECH_TRACKS];
  int len = 0;
  if (prefixTrack > 0 && len < MAX_SPEECH_TRACKS) tmp[len++] = prefixTrack;
  len += numberToTracks(n, &tmp[len], MAX_SPEECH_TRACKS - len);
  if (suffixTrack > 0 && len < MAX_SPEECH_TRACKS) tmp[len++] = suffixTrack;

  queueTracks(tmp, len);
}

void speakSingle(int track) {
  int tmp[1] = { track };
  queueTracks(tmp, 1);
}


void voiceGuidance() {
  // jari baru ditempel
  if (fingerDetected && !prevFingerDetected) {
    readingAnnouncedThisSession = false;
    speakSingle(TRACK_PROMPT_TERDETEKSI);
  }

  // jari baru diangkat
  if (!fingerDetected && prevFingerDetected) {
    lastPlacementPromptMs = millis();
  }

  // belum ada jari, ingatkan tiap 15 detik
  if (!fingerDetected && !speechPlaying && millis() - lastPlacementPromptMs > PLACEMENT_PROMPT_INTERVAL_MS) {
    lastPlacementPromptMs = millis();
    speakSingle(TRACK_PROMPT_LETAKKAN);
  }

  // buffer sudah penuh, bacaan dianggap stabil
  if (fingerDetected && bufFull && !readingAnnouncedThisSession && !speechPlaying) {
    readingAnnouncedThisSession = true;
    speakSingle(TRACK_PROMPT_ANGKAT);
    lastAnnounceMs = millis();
  }

  prevFingerDetected = fingerDetected;
}

void advanceSpeechQueue() {
  speechIndex++;
  if (speechIndex < speechQueueLen) {
    speechTrackStartMs = millis();
    dfPlayer.playMp3Folder(speechQueue[speechIndex]);
  } else {
    speechPlaying = false;
  }
}

void pollDFPlayer() {
  if (dfPlayer.available()) {
    uint8_t type = dfPlayer.readType();
    dfPlayer.read();
    if (type == DFPlayerPlayFinished && speechPlaying) {
      advanceSpeechQueue();
    }
  }
  // kadang notif selesai dari DFPlayer tidak masuk, jadi pakai timeout
  if (speechPlaying && millis() - speechTrackStartMs > SPEECH_TRACK_TIMEOUT_MS) {
    advanceSpeechQueue();
  }
}


void sendToFirebase() {
  Database.set<int>(aClient, "/healthMonitor/latest/bpm", bpmAvg, processData, "setBpm");
  Database.set<float>(aClient, "/healthMonitor/latest/spo2", spo2Estimate, processData, "setSpo2");
  Database.set<bool>(aClient, "/healthMonitor/latest/finger_detected", fingerDetected, processData, "setFinger");
  Database.set<int>(aClient, "/healthMonitor/latest/timestamp", (int)time(nullptr), processData, "setTs");
}

void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;
  if (aResult.isError())
    Firebase.printf("Error task: %s, msg: %s, code: %d\n",
                     aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
  if (aResult.available())
    Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
}
