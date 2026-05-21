/*
 * ============================================================
 *  TROYIKZ — Smart Smoke Detection & Hazard Classification
 *  ESP32 Firmware  |  v1.0.3
 *
 *  CHANGE LOG v1.0.3:
 *    - FIX: sendLogToSupabase() now called every READ_INTERVAL_MS
 *      during active session, not only on stopSession()
 *    - FIX: session_status is "active" during testing,
 *      "completed" only on stop — dashboard now receives live rows
 *    - sendLogToSupabase() accepts sessStatus parameter
 *    - READ_INTERVAL_MS raised to 2000ms to avoid Supabase spam
 *    - SMOKE_THRESHOLD stays at 900
 *
 *  WARMUP IS NON-BLOCKING:
 *    - Dashboard fully usable during warmup
 *    - WiFi, heartbeats, serial all work during warmup
 *    - Only "Start Testing" blocked until warmup done
 *    - sensor_status.warmup_remaining sent every heartbeat
 *
 *  Hardware:
 *    ESP32 (38-pin) + MQ Smoke Sensor + Green LED + Red LED + Buzzer
 *
 *  Serial Commands (115200 baud):
 *    S  →  Start testing session (only works after warmup)
 *    X  →  Stop  testing session
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ── USER CONFIG ──────────────────────────────────────────────
const char* WIFI_SSID         = "2.4G";
const char* WIFI_PASSWORD     = "ROJOFAMS";
const char* SUPABASE_URL      = "https://crottwcwsasedjzvydbp.supabase.co";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImNyb3R0d2N3c2FzZWRqenZ5ZGJwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzkyODcxMzIsImV4cCI6MjA5NDg2MzEzMn0.qQU8eT4pkSIBILkLXy2gkE_4RDpybTxrfRjYD72MiGs";
const char* DEVICE_ID         = "esp32-troyikz-01";
const char* FIRMWARE_VER      = "v1.0.3";

// Timezone — Philippines UTC+8
const long  GMT_OFFSET_SEC  = 28800;
const int   DAYLIGHT_OFFSET = 0;
const char* NTP_SERVER      = "pool.ntp.org";

// ── Pins ─────────────────────────────────────────────────────
#define PIN_MQ_SENSOR  34
#define PIN_LED_GREEN  26
#define PIN_LED_RED    27
#define PIN_BUZZER     25

// ── Timing & Thresholds ──────────────────────────────────────
const unsigned long WARMUP_MS        = 180000UL; // 3 minutes
const unsigned long READ_INTERVAL_MS = 2000UL;   // send log every 2s during session
const unsigned long HEARTBEAT_MS     = 3000UL;
const unsigned long WIFI_RETRY_MS    = 10000UL;
const int           SMOKE_THRESHOLD  = 900;
const int           ADC_SAMPLES      = 10;

// ── Endpoints ────────────────────────────────────────────────
String EP_LOGS;
String EP_STATUS;

// ── State ────────────────────────────────────────────────────
bool          sensorReady    = false;
bool          isTesting      = false;
bool          wifiConnected  = false;
int           testCounter    = 0;
int           lastSmokeValue = 0;
String        currentSession = "";

unsigned long warmupStart    = 0;
unsigned long lastReadTime   = 0;
unsigned long lastHeartbeat  = 0;
unsigned long lastWifiRetry  = 0;

// ── Forward declarations ─────────────────────────────────────
void   connectWifi();
void   syncTime();
int    readSmokeSensor();
void   classifyAndActuate(int val, bool& isHaz);
void   sendLogToSupabase(int val, bool isHaz, const String& sid, const String& sessStatus);
void   sendHeartbeat();
void   setLEDs(bool g, bool r, bool b);
void   startSession();
void   stopSession();
void   handleSerial();
String buildTestId(int n);
String isoNow();

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(150);

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print(F("  TROYIKZ Smart Smoke Detection System  "));
  Serial.println(FIRMWARE_VER);
  Serial.println(F("============================================================"));

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_BUZZER,    OUTPUT);

  analogReadResolution(10);
  analogSetAttenuation(ADC_11db);
  setLEDs(false, false, false);

  EP_LOGS   = String(SUPABASE_URL) + "/rest/v1/smoke_logs";
  EP_STATUS = String(SUPABASE_URL) + "/rest/v1/sensor_status";

  // 3 quick green blinks = booted OK
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_GREEN, HIGH); delay(100);
    digitalWrite(PIN_LED_GREEN, LOW);  delay(100);
  }

  connectWifi();
  syncTime();

  warmupStart = millis();
  Serial.println(F("\n[WARMUP] Started — 3 minutes."));
  Serial.printf("[THRESHOLD] Smoke threshold set to: %d\n", SMOKE_THRESHOLD);
  Serial.println(F("[INFO] Dashboard is live. 'Start Testing' unlocks after warmup.\n"));

  sendHeartbeat();
}

// ============================================================
//  LOOP — fully non-blocking
// ============================================================
void loop() {
  unsigned long now = millis();

  // ── WiFi watchdog ─────────────────────────────────────────
  if (!wifiConnected && now - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = now;
    connectWifi();
  }

  // ── Serial commands ───────────────────────────────────────
  handleSerial();

  // ── Warmup progress ───────────────────────────────────────
  if (!sensorReady) {
    unsigned long elapsed = now - warmupStart;
    if (elapsed >= WARMUP_MS) {
      sensorReady = true;
      setLEDs(true, false, false);
      Serial.println(F("\n[WARMUP] Complete! Sensor ready."));
      Serial.println(F("[INFO] Type S to start, X to stop.\n"));
      sendHeartbeat();
      delay(800);
      setLEDs(false, false, false);
    } else {
      bool tick = (now / 800) % 2 == 0;
      digitalWrite(PIN_LED_GREEN, tick  ? HIGH : LOW);
      digitalWrite(PIN_LED_RED,   !tick ? HIGH : LOW);
    }
  }

  // ── Heartbeat ─────────────────────────────────────────────
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendHeartbeat();

    if (!sensorReady) {
      unsigned long rem = (WARMUP_MS - (now - warmupStart)) / 1000;
      Serial.printf("[WARMUP] %02lu:%02lu remaining\n", rem / 60, rem % 60);
    }
  }

  // ── Active test session — read + send every READ_INTERVAL_MS ──
  if (isTesting && now - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = now;
    int  val   = readSmokeSensor();
    bool isHaz = false;
    classifyAndActuate(val, isHaz);
    lastSmokeValue = val;

    Serial.printf("[READ] Smoke: %4d  |  %-10s  |  %s  (threshold: %d)\n",
      val,
      isHaz ? "Bad Smoke"  : "Good Smoke",
      isHaz ? "HAZARDOUS"  : "SAFE",
      SMOKE_THRESHOLD);

    // ✅ FIXED: send every live reading to Supabase as "active"
    sendLogToSupabase(val, isHaz, currentSession, "active");
  }

  // ── Idle ──────────────────────────────────────────────────
  if (sensorReady && !isTesting) {
    setLEDs(false, false, false);
  }
}

// ============================================================
//  WIFI
// ============================================================
void connectWifi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[WiFi] Connected  IP=%s  RSSI=%d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    wifiConnected = false;
    Serial.println(F("\n[WiFi] Failed — will retry in 10s"));
  }
}

// ============================================================
//  TIME SYNC
// ============================================================
void syncTime() {
  if (!wifiConnected) return;
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
  Serial.print(F("[NTP] Syncing"));
  struct tm t;
  for (int i = 0; i < 10 && !getLocalTime(&t); i++) {
    delay(400); Serial.print('.');
  }
  if (getLocalTime(&t))
    Serial.printf("\n[NTP] OK  %04d-%02d-%02d %02d:%02d:%02d PHT\n",
      t.tm_year+1900, t.tm_mon+1, t.tm_mday,
      t.tm_hour, t.tm_min, t.tm_sec);
  else
    Serial.println(F("\n[NTP] Failed — millis fallback active"));
}

// ============================================================
//  SENSOR READ — averaged over ADC_SAMPLES
// ============================================================
int readSmokeSensor() {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PIN_MQ_SENSOR);
    delay(4);
  }
  return (int)(sum / ADC_SAMPLES);
}

// ============================================================
//  CLASSIFY & ACTUATE
//  Good Smoke → Green LED ON,  Red OFF, Buzzer OFF
//  Bad Smoke  → Green LED OFF, Red ON,  Buzzer ON
// ============================================================
void classifyAndActuate(int val, bool& isHaz) {
  isHaz = (val > SMOKE_THRESHOLD);
  setLEDs(!isHaz, isHaz, isHaz);
}

void setLEDs(bool g, bool r, bool b) {
  digitalWrite(PIN_LED_GREEN, g ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,   r ? HIGH : LOW);
  digitalWrite(PIN_BUZZER,    b ? HIGH : LOW);
}

// ============================================================
//  SESSION CONTROL
// ============================================================
void startSession() {
  if (isTesting) {
    Serial.println(F("[SESSION] Already running. Send X to stop first."));
    return;
  }
  if (!sensorReady) {
    unsigned long rem = (WARMUP_MS - (millis() - warmupStart)) / 1000;
    Serial.printf("[SESSION] Still warming up — %lu seconds remaining.\n", rem);
    return;
  }
  isTesting      = true;
  lastReadTime   = 0; // trigger immediate first read
  currentSession = buildTestId(testCounter + 1);
  Serial.println(F("\n--------------------------------------------"));
  Serial.printf("[SESSION] Started  ID=%s  Threshold=%d\n",
    currentSession.c_str(), SMOKE_THRESHOLD);
  Serial.println(F("--------------------------------------------\n"));
}

void stopSession() {
  if (!isTesting) {
    Serial.println(F("[SESSION] Nothing to stop."));
    return;
  }
  isTesting = false;
  testCounter++;

  // Final reading saved as "completed"
  int  val   = readSmokeSensor();
  bool isHaz = (val > SMOKE_THRESHOLD);

  Serial.println(F("\n--------------------------------------------"));
  Serial.printf("[SESSION] Stopped  ID=%s\n", currentSession.c_str());
  Serial.printf("[SESSION] Final reading: %d  →  %s\n",
    val, isHaz ? "HAZARDOUS" : "SAFE");
  Serial.println(F("--------------------------------------------\n"));

  classifyAndActuate(val, isHaz);

  // ✅ FIXED: final log saved as "completed"
  sendLogToSupabase(val, isHaz, currentSession, "completed");

  delay(1500);
  setLEDs(false, false, false);
  currentSession = "";
}

// ============================================================
//  SUPABASE — save test log
//  sessStatus: "active" (during session) | "completed" (on stop)
// ============================================================
void sendLogToSupabase(int val, bool isHaz, const String& sid, const String& sessStatus) {
  if (!wifiConnected) {
    Serial.println(F("[DB] No WiFi — log skipped"));
    return;
  }

  HTTPClient http;
  http.begin(EP_LOGS);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer",        "return=minimal");

  StaticJsonDocument<256> doc;
  doc["test_id"]        = sid;
  doc["smoke_value"]    = val;
  doc["classification"] = isHaz ? "Bad Smoke"  : "Good Smoke";
  doc["status"]         = isHaz ? "Hazardous"  : "Safe";
  doc["green_led"]      = !isHaz;
  doc["red_led"]        =  isHaz;
  doc["buzzer"]         =  isHaz;
  doc["session_status"] = sessStatus;  // ✅ FIXED: dynamic
  doc["created_at"]     = isoNow();

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code == 201) {
    Serial.printf("[DB] OK  %s  val=%d  %s  (%s)\n",
      sid.c_str(), val, isHaz ? "HAZARDOUS" : "SAFE", sessStatus.c_str());
  } else {
    Serial.printf("[DB] FAIL  HTTP=%d  %s\n", code, http.getString().c_str());
  }
  http.end();
}

// ============================================================
//  SUPABASE — heartbeat with warmup countdown
// ============================================================
void sendHeartbeat() {
  if (!wifiConnected) return;

  unsigned long now     = millis();
  unsigned long elapsed = now - warmupStart;
  int remaining = sensorReady ? 0 : (int)((WARMUP_MS - elapsed) / 1000);
  if (remaining < 0) remaining = 0;

  HTTPClient http;
  String url = EP_STATUS + "?device_id=eq." + String(DEVICE_ID);
  http.begin(url);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer",        "resolution=merge-duplicates,return=minimal");

  StaticJsonDocument<256> doc;
  doc["device_id"]        = DEVICE_ID;
  doc["is_online"]        = true;
  doc["is_initialized"]   = sensorReady;
  doc["warmup_remaining"] = remaining;
  doc["rssi"]             = (int)WiFi.RSSI();
  doc["ip_address"]       = WiFi.localIP().toString();
  doc["firmware_ver"]     = FIRMWARE_VER;
  doc["last_seen"]        = isoNow();

  String body;
  serializeJson(doc, body);

  int code = http.PATCH(body);

  // Row doesn't exist yet — create it
  if (code != 200 && code != 204) {
    http.end();
    http.begin(EP_STATUS);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
    http.addHeader("Prefer",        "resolution=merge-duplicates,return=minimal");
    code = http.POST(body);
  }
  http.end();
}

// ============================================================
//  SERIAL HANDLER
// ============================================================
void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'S' || c == 's') startSession();
  if (c == 'X' || c == 'x') stopSession();
}

// ============================================================
//  HELPERS
// ============================================================
String buildTestId(int n) {
  char buf[12];
  snprintf(buf, sizeof(buf), "TEST-%04d", n);
  return String(buf);
}

String isoNow() {
  struct tm t;
  if (!getLocalTime(&t)) {
    unsigned long s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "1970-01-01T%02lu:%02lu:%02luZ",
      (s/3600)%24, (s/60)%60, s%60);
    return String(buf);
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+08:00", &t);
  return String(buf);
}

/*
  WIRING
  ──────────────────────────────────────
  GPIO 34 (ADC1) → MQ Sensor AOUT
  3.3V           → MQ Sensor VCC
  GND            → MQ Sensor GND
  GPIO 26        → 330Ω → Green LED +
  GPIO 27        → 330Ω → Red LED +
  GPIO 25        → Active Buzzer +
  GND            → all LED – and Buzzer –
*/
