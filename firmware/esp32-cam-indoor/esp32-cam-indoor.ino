#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include <time.h>

#include "camera_pins.h"

static const char *APP_VERSION = "0.8.9";
static const char *AP_PASSWORD = "esp32cam123";
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
static const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
static const unsigned long WIFI_ROAM_CHECK_INTERVAL_MS = 60000;
static const unsigned long CONSOLE_STATUS_INTERVAL_MS = 60000;
static const int WIFI_ROAM_TRIGGER_RSSI_DEFAULT = -72;
static const int WIFI_ROAM_MIN_IMPROVEMENT_DB_DEFAULT = 4;
static const char *LOG_FILE = "/system.log";
static const size_t LOG_MAX_BYTES = 128 * 1024;
static const unsigned long STREAM_WATCHDOG_TIMEOUT_MS = 30000;

static httpd_handle_t http_server = nullptr;
static httpd_handle_t stream_server = nullptr;
static volatile int streamClientCount = 0;
static volatile unsigned long streamLastFrameMillis = 0;
static volatile unsigned long streamRestartCount = 0;
static volatile bool streamRestartRequested = false;

static String logBuffer;
static bool logReady = false;

static Preferences prefs;
static String wifiSsid;
static String wifiPassword;
static bool apMode = false;
static bool wifiWasConnected = false;
static unsigned long wifiLastRetryMillis = 0;
static unsigned long wifiReconnectAttempts = 0;
static unsigned long wifiReconnectSuccesses = 0;
static unsigned long wifiLastConnectedMillis = 0;
static unsigned long wifiLastRoamCheckMillis = 0;
static unsigned long wifiRoamAttempts = 0;
static unsigned long wifiRoamSuccesses = 0;
static bool wifiRoamInProgress = false;
static String wifiRoamTargetBssid;
static int wifiRoamTriggerRssi = WIFI_ROAM_TRIGGER_RSSI_DEFAULT;
static int wifiRoamMinImprovementDb = WIFI_ROAM_MIN_IMPROVEMENT_DB_DEFAULT;
static bool ntpStarted = false;
static bool ntpLoggedSynchronized = false;
static unsigned long consoleLastStatusMillis = 0;
static bool wifiRssiSeen = false;
static int wifiRssiMin = 0;
static int wifiRssiMax = 0;

struct CameraSettings {
  framesize_t frameSize = FRAMESIZE_VGA;
  int jpegQuality = 12;
  int brightness = 0;
  int contrast = 0;
  int saturation = 0;
  bool vflip = false;
  bool hmirror = false;
};

static CameraSettings cameraSettings;
static bool flashLedOn = false;

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static String logTimestamp() {
  if (time(nullptr) >= 1700000000) {
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmNow);
    return String(buffer);
  }

  return String("+") + String(millis() / 1000.0, 3) + "s";
}

static void writeLogLine(const String &line) {
  if (!logReady) return;

  if (LittleFS.exists(LOG_FILE)) {
    File existing = LittleFS.open(LOG_FILE, "r");
    if (existing) {
      const size_t currentSize = existing.size();
      existing.close();

      // Simple rotation: once the log reaches the limit, start a fresh file.
      if (currentSize >= LOG_MAX_BYTES) {
        LittleFS.remove(LOG_FILE);
      }
    }
  }

  File file = LittleFS.open(LOG_FILE, "a");
  if (!file) return;

  file.print("[");
  file.print(logTimestamp());
  file.print("] ");
  file.println(line);
  file.close();
}

static void appendLogText(const String &text) {
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];

    if (c == '\n') {
      if (logBuffer.endsWith("\r")) {
        logBuffer.remove(logBuffer.length() - 1);
      }
      writeLogLine(logBuffer);
      logBuffer = "";
    } else {
      logBuffer += c;

      if (logBuffer.length() > 1024) {
        writeLogLine(logBuffer);
        logBuffer = "";
      }
    }
  }
}

static void logPrint(const char *value) {
  Serial.print(value);
  appendLogText(String(value));
}

static void logPrint(char value) {
  Serial.print(value);
  appendLogText(String(value));
}

static void logPrintln() {
  Serial.println();
  writeLogLine(logBuffer);
  logBuffer = "";
}

static void logPrintln(const char *value) {
  Serial.println(value);
  appendLogText(String(value));
  writeLogLine(logBuffer);
  logBuffer = "";
}

static void logPrintln(const String &value) {
  Serial.println(value);
  appendLogText(value);
  writeLogLine(logBuffer);
  logBuffer = "";
}

static void logPrintf(const char *format, ...) {
  char buffer[512];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  Serial.print(buffer);
  appendLogText(String(buffer));
}

static void initLogger() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed; persistent log disabled");
    return;
  }

  File file;
  if (LittleFS.exists(LOG_FILE)) {
    file = LittleFS.open(LOG_FILE, "a");
  } else {
    file = LittleFS.open(LOG_FILE, "w");
  }

  if (file) {
    file.close();
    logReady = true;
    Serial.println("Persistent logger initialized");
  } else {
    Serial.println("ERROR: cannot create/open persistent log file");
  }
}

static String htmlEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static String urlDecode(const String &value) {
  String out;
  out.reserve(value.length());

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];

    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < value.length()) {
      char h1 = value[i + 1];
      char h2 = value[i + 2];

      auto hexValue = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        return -1;
      };

      int a = hexValue(h1);
      int b = hexValue(h2);

      if (a >= 0 && b >= 0) {
        out += char((a << 4) | b);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }

  return out;
}

static String formValue(const String &body, const String &key) {
  const String needle = key + "=";
  int start = body.indexOf(needle);

  if (start < 0) return "";

  start += needle.length();
  int end = body.indexOf('&', start);

  if (end < 0) end = body.length();

  return urlDecode(body.substring(start, end));
}

static String chipSuffix() {
  uint64_t chip = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06X", (uint32_t)(chip & 0xFFFFFF));
  return String(suffix);
}

static String fallbackApSsid() {
  return "ESP32-CAM-" + chipSuffix();
}

static String frameSizeName(framesize_t size);

static const char *wifiSignalRating(int32_t rssi) {
  if (rssi >= -50) return "sehr gut";
  if (rssi >= -60) return "gut";
  if (rssi >= -67) return "brauchbar";
  if (rssi >= -70) return "grenzwertig";
  return "schlecht";
}

static bool timeIsSynchronized() {
  return time(nullptr) >= 1700000000;
}

static String currentLocalTime() {
  if (!timeIsSynchronized()) return "";

  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmNow);
  return String(buffer);
}

static void startNtpSync() {
  if (WiFi.status() != WL_CONNECTED || ntpStarted) return;

  // Non-blocking NTP setup. SNTP synchronizes in the background.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0",
               "pool.ntp.org",
               "time.nist.gov");
  ntpStarted = true;
  ntpLoggedSynchronized = false;
  logPrintln("NTP synchronization started (background)");
}

static void maintainNtp() {
  if (WiFi.status() != WL_CONNECTED) {
    ntpStarted = false;
    ntpLoggedSynchronized = false;
    return;
  }

  if (!ntpStarted) {
    startNtpSync();
  }

  if (timeIsSynchronized() && !ntpLoggedSynchronized) {
    ntpLoggedSynchronized = true;
    logPrintf("NTP synchronized: %s\n", currentLocalTime().c_str());
  }
}

static void updateRssiStats() {
  if (WiFi.status() != WL_CONNECTED) return;

  const int rssi = WiFi.RSSI();

  if (!wifiRssiSeen) {
    wifiRssiMin = rssi;
    wifiRssiMax = rssi;
    wifiRssiSeen = true;
    return;
  }

  if (rssi < wifiRssiMin) wifiRssiMin = rssi;
  if (rssi > wifiRssiMax) wifiRssiMax = rssi;
}

static void logConsoleStatus() {
  if (millis() - consoleLastStatusMillis < CONSOLE_STATUS_INTERVAL_MS) return;
  consoleLastStatusMillis = millis();

  logPrintf("[FW %s] uptime=%lus", APP_VERSION, millis() / 1000UL);

  if (WiFi.status() == WL_CONNECTED) {
    updateRssiStats();
    logPrintf(" wifi=%s rssi=%d min=%d max=%d bssid=%s",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  wifiRssiSeen ? wifiRssiMin : 0,
                  wifiRssiSeen ? wifiRssiMax : 0,
                  WiFi.BSSIDstr().c_str());
  } else {
    logPrint(" wifi=disconnected");
  }

  logPrintf(" camera=%s q=%d heap=%u psram_free=%u",
                frameSizeName(cameraSettings.frameSize).c_str(),
                cameraSettings.jpegQuality,
                ESP.getFreeHeap(),
                ESP.getFreePsram());

  if (timeIsSynchronized()) {
    logPrintf(" time=%s", currentLocalTime().c_str());
  }

  logPrintln();
}


static bool loadWifiConfig() {
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("password", "");
  prefs.end();

  wifiSsid.trim();
  return wifiSsid.length() > 0;
}

static bool saveWifiConfig(const String &ssid, const String &password, bool updatePassword) {
  prefs.begin("wifi", false);
  bool ok1 = prefs.putString("ssid", ssid) > 0;
  bool ok2 = true;

  if (updatePassword) {
    ok2 = prefs.putString("password", password) >= 0;
  }

  prefs.end();
  return ok1 && ok2;
}


static void loadRoamingConfig() {
  prefs.begin("roaming", true);
  wifiRoamTriggerRssi = prefs.getInt("trigger_rssi", WIFI_ROAM_TRIGGER_RSSI_DEFAULT);
  wifiRoamMinImprovementDb = prefs.getInt("min_improve_db", WIFI_ROAM_MIN_IMPROVEMENT_DB_DEFAULT);
  prefs.end();

  wifiRoamTriggerRssi = constrain(wifiRoamTriggerRssi, -95, -50);
  wifiRoamMinImprovementDb = constrain(wifiRoamMinImprovementDb, 1, 20);
}

static void saveRoamingConfig() {
  prefs.begin("roaming", false);
  prefs.putInt("trigger_rssi", wifiRoamTriggerRssi);
  prefs.putInt("min_improve_db", wifiRoamMinImprovementDb);
  prefs.end();
}

static void loadCameraConfig() {
  prefs.begin("camera", true);
  cameraSettings.frameSize = static_cast<framesize_t>(
      prefs.getUChar("framesize", static_cast<uint8_t>(FRAMESIZE_VGA)));
  cameraSettings.jpegQuality = prefs.getInt("quality", 12);
  cameraSettings.brightness = prefs.getInt("brightness", 0);
  cameraSettings.contrast = prefs.getInt("contrast", 0);
  cameraSettings.saturation = prefs.getInt("saturation", 0);
  cameraSettings.vflip = prefs.getBool("vflip", false);
  cameraSettings.hmirror = prefs.getBool("hmirror", false);
  prefs.end();

  if (cameraSettings.jpegQuality < 4) cameraSettings.jpegQuality = 4;
  if (cameraSettings.jpegQuality > 63) cameraSettings.jpegQuality = 63;
  if (cameraSettings.brightness < -2) cameraSettings.brightness = -2;
  if (cameraSettings.brightness > 2) cameraSettings.brightness = 2;
  if (cameraSettings.contrast < -2) cameraSettings.contrast = -2;
  if (cameraSettings.contrast > 2) cameraSettings.contrast = 2;
  if (cameraSettings.saturation < -2) cameraSettings.saturation = -2;
  if (cameraSettings.saturation > 2) cameraSettings.saturation = 2;
}

static bool saveCameraConfig() {
  prefs.begin("camera", false);
  prefs.putUChar("framesize", static_cast<uint8_t>(cameraSettings.frameSize));
  prefs.putInt("quality", cameraSettings.jpegQuality);
  prefs.putInt("brightness", cameraSettings.brightness);
  prefs.putInt("contrast", cameraSettings.contrast);
  prefs.putInt("saturation", cameraSettings.saturation);
  prefs.putBool("vflip", cameraSettings.vflip);
  prefs.putBool("hmirror", cameraSettings.hmirror);
  prefs.end();
  return true;
}

static void applyCameraSettings() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;

  sensor->set_framesize(sensor, cameraSettings.frameSize);
  sensor->set_quality(sensor, cameraSettings.jpegQuality);
  sensor->set_brightness(sensor, cameraSettings.brightness);
  sensor->set_contrast(sensor, cameraSettings.contrast);
  sensor->set_saturation(sensor, cameraSettings.saturation);
  sensor->set_vflip(sensor, cameraSettings.vflip ? 1 : 0);
  sensor->set_hmirror(sensor, cameraSettings.hmirror ? 1 : 0);
}

static String frameSizeName(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QQVGA: return "160x120";
    case FRAMESIZE_QVGA: return "320x240";
    case FRAMESIZE_CIF: return "400x296";
    case FRAMESIZE_VGA: return "640x480";
    case FRAMESIZE_SVGA: return "800x600";
    case FRAMESIZE_XGA: return "1024x768";
    case FRAMESIZE_SXGA: return "1280x1024";
    case FRAMESIZE_UXGA: return "1600x1200";
    default: return "640x480";
  }
}

static framesize_t frameSizeFromValue(const String &value) {
  if (value == "QQVGA") return FRAMESIZE_QQVGA;
  if (value == "QVGA") return FRAMESIZE_QVGA;
  if (value == "CIF") return FRAMESIZE_CIF;
  if (value == "SVGA") return FRAMESIZE_SVGA;
  if (value == "XGA") return FRAMESIZE_XGA;
  if (value == "SXGA") return FRAMESIZE_SXGA;
  if (value == "UXGA") return FRAMESIZE_UXGA;
  return FRAMESIZE_VGA;
}

static bool isHighResolution(framesize_t size) {
  return size > FRAMESIZE_VGA;
}

static void setFlashLed(bool on) {
  flashLedOn = on;
  digitalWrite(FLASH_LED_GPIO_NUM, on ? HIGH : LOW);
}

static String frameSizeValue(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QQVGA: return "QQVGA";
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_CIF: return "CIF";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA: return "XGA";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    default: return "VGA";
  }
}

static void startAccessPoint() {
  apMode = true;

  // Keep the station interface enabled even when no WLAN is configured.
  // This is required so the configuration page can scan nearby networks
  // while the fallback AP stays available.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  const String ssid = fallbackApSsid();
  WiFi.softAP(ssid.c_str(), AP_PASSWORD);

  if (wifiSsid.length() > 0) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }

  logPrintln();
  logPrintln("Fallback AP started");
  logPrintf("SSID: %s\n", ssid.c_str());
  logPrintf("Password: %s\n", AP_PASSWORD);
  logPrintf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
}

static bool connectWifi() {
  if (!loadWifiConfig()) {
    logPrintln("No stored Wi-Fi configuration");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  logPrintf("Connecting to Wi-Fi SSID: %s", wifiSsid.c_str());

  const unsigned long started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    logPrint('.');
  }

  logPrintln();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    wifiWasConnected = true;
    wifiLastConnectedMillis = millis();

    logPrintln("Wi-Fi connected");
    updateRssiStats();
    startNtpSync();
    logPrintf("IP: %s\n", WiFi.localIP().toString().c_str());
    logPrintf("RSSI: %d dBm\n", WiFi.RSSI());
    return true;
  }

  logPrintln("Wi-Fi connection failed");
  return false;
}

static void maintainWifi() {
  const bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    updateRssiStats();

    if (!wifiWasConnected) {
      wifiWasConnected = true;
      if (wifiRoamInProgress) {
        wifiRoamSuccesses++;
        wifiRoamInProgress = false;
        logPrintln("Wi-Fi roam completed");
      } else {
        wifiReconnectSuccesses++;
        logPrintln("Wi-Fi reconnected");
      }

      wifiLastConnectedMillis = millis();
      ntpStarted = false;
      startNtpSync();
      logPrintf("IP: %s\n", WiFi.localIP().toString().c_str());
      logPrintf("RSSI: %d dBm\n", WiFi.RSSI());

      if (apMode) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        apMode = false;
        logPrintln("Fallback AP stopped");
      }
    }

    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    logPrintln("Wi-Fi connection lost");
  }

  if (wifiSsid.length() == 0) {
    if (!apMode) startAccessPoint();
    return;
  }

  if (millis() - wifiLastRetryMillis < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  if (wifiRoamInProgress) {
    logPrintln("Wi-Fi roam timed out; falling back to normal reconnect");
    wifiRoamInProgress = false;
  }

  wifiLastRetryMillis = millis();
  wifiReconnectAttempts++;

  logPrintf("Wi-Fi reconnect attempt %lu\n", wifiReconnectAttempts);

  if (!apMode) {
    startAccessPoint();
  } else {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }
}

static void checkForBetterAccessPoint() {
  if (WiFi.status() != WL_CONNECTED || apMode || wifiSsid.length() == 0) {
    return;
  }

  if (wifiRoamInProgress) {
    return;
  }

  if (millis() - wifiLastRoamCheckMillis < WIFI_ROAM_CHECK_INTERVAL_MS) {
    return;
  }

  wifiLastRoamCheckMillis = millis();

  const int currentRssi = WiFi.RSSI();

  // Avoid scan interruptions while the current AP is already good enough.
  if (currentRssi >= wifiRoamTriggerRssi) {
    return;
  }

  const String currentBssid = WiFi.BSSIDstr();

  logPrintf("Roam scan: current AP %s at %d dBm\n",
                currentBssid.c_str(), currentRssi);

  int networkCount = WiFi.scanNetworks(false, false);

  if (networkCount <= 0) {
    WiFi.scanDelete();
    return;
  }

  int bestRssi = currentRssi;
  int bestChannel = 0;
  uint8_t bestBssid[6] = {0};
  String bestBssidString;
  bool betterApFound = false;

  for (int i = 0; i < networkCount; i++) {
    if (WiFi.SSID(i) != wifiSsid) {
      continue;
    }

    String candidateBssid = WiFi.BSSIDstr(i);

    if (candidateBssid == currentBssid) {
      continue;
    }

    int candidateRssi = WiFi.RSSI(i);

    if (candidateRssi > bestRssi) {
      bestRssi = candidateRssi;
      bestChannel = WiFi.channel(i);

      const uint8_t *candidate = WiFi.BSSID(i);
      if (candidate) {
        memcpy(bestBssid, candidate, 6);
        bestBssidString = candidateBssid;
        betterApFound = true;
      }
    }
  }

  WiFi.scanDelete();

  if (!betterApFound) {
    logPrintln("Roam scan: no other AP with the configured SSID found");
    return;
  }

  logPrintf("Roam scan: best candidate %s at %d dBm (%+d dB)\n",
                bestBssidString.c_str(),
                bestRssi,
                bestRssi - currentRssi);

  if (bestRssi < currentRssi + wifiRoamMinImprovementDb) {
    logPrintf("Roam scan: improvement below %d dB threshold, staying on current AP\n",
                  wifiRoamMinImprovementDb);
    return;
  }

  wifiRoamAttempts++;
  wifiRoamInProgress = true;
  wifiRoamTargetBssid = bestBssidString;

  logPrintf("Roaming from %s (%d dBm) to %s (%d dBm), channel %d\n",
                currentBssid.c_str(),
                currentRssi,
                bestBssidString.c_str(),
                bestRssi,
                bestChannel);

  // Give the roam attempt a grace period before normal reconnect/fallback
  // handling takes over.
  wifiLastRetryMillis = millis();
  wifiWasConnected = false;

  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(
      wifiSsid.c_str(),
      wifiPassword.c_str(),
      bestChannel,
      bestBssid,
      true);
}


static String rootPage() {
  String html;
  html.reserve(3000);

  html += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>ESP32-CAM Indoor</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}");
  html += F("main{max-width:900px;margin:auto;padding:16px}img{width:100%;height:auto;border-radius:8px;background:#222}");
  html += F(".meta{margin-top:12px;color:#bbb}a{color:#8ec5ff}.warn{color:#ffcb6b}.ok{color:#8bd450}");
  html += F(".settings{margin-top:22px;padding:14px;background:#1b1b1b;border-radius:8px}");
  html += F(".settings table{width:100%;border-collapse:collapse}.settings td{padding:5px 0;border-bottom:1px solid #333}");
  html += F(".settings td:last-child{text-align:right;color:#fff}</style>");
  html += F("</head><body><main><h1>ESP32-CAM Indoor</h1>");

  html += F("<p>Firmware <b>v");
  html += APP_VERSION;
  html += F("</b></p>");

  html += F("<p class='meta'>Zeit: ");
  if (timeIsSynchronized()) {
    html += currentLocalTime();
  } else {
    html += F("NTP noch nicht synchronisiert");
  }
  html += F("</p>");

  if (apMode) {
    html += F("<p class='warn'><b>Fallback AP aktiv</b></p>");
  } else {
    html += F("<p class='ok'><b>WLAN verbunden</b> · ");
    html += WiFi.localIP().toString();
    html += F(" · ");
    html += String(WiFi.RSSI());
    html += F(" dBm</p>");
  }

  if (WiFi.status() == WL_CONNECTED) {
    html += F("<img id='stream' alt='camera stream'>");
  } else {
    html += F("<p>Kein Kamera-Stream, solange keine WLAN-Verbindung besteht.</p>");
  }

  html += F("<div class='meta'><a href='/jpg'>Snapshot</a> · ");
  html += F("<a href='/status'>Status JSON</a> · ");
  html += F("<a href='/camera'>Kamera-Settings</a> · ");
  html += F("<a href='/config'>WLAN-Konfiguration</a> · ");
  html += F("<a href='/logs'>System-Log</a></div>");

  html += F("<div class='meta' style='margin-top:18px'><b>Flash-LED:</b> ");
  html += flashLedOn ? "AN" : "AUS";
  html += F(" &nbsp; <form method='POST' action='/flash/on' style='display:inline'>");
  html += F("<button type='submit'>LED an</button></form> ");
  html += F("<form method='POST' action='/flash/off' style='display:inline'>");
  html += F("<button type='submit'>LED aus</button></form></div>");

  html += F("<div class='settings'><h2>Kamera-Settings</h2><table>");
  html += F("<tr><td>Auflösung</td><td>");
  html += frameSizeName(cameraSettings.frameSize);
  html += F("</td></tr>");
  html += F("<tr><td>JPEG Qualität</td><td>");
  html += String(cameraSettings.jpegQuality);
  html += F("</td></tr>");
  html += F("<tr><td>Helligkeit</td><td>");
  html += String(cameraSettings.brightness);
  html += F("</td></tr>");
  html += F("<tr><td>Kontrast</td><td>");
  html += String(cameraSettings.contrast);
  html += F("</td></tr>");
  html += F("<tr><td>Sättigung</td><td>");
  html += String(cameraSettings.saturation);
  html += F("</td></tr>");
  html += F("<tr><td>Vertikal drehen</td><td>");
  html += cameraSettings.vflip ? "ja" : "nein";
  html += F("</td></tr>");
  html += F("<tr><td>Horizontal spiegeln</td><td>");
  html += cameraSettings.hmirror ? "ja" : "nein";
  html += F("</td></tr>");
  html += F("<tr><td>Capture-Modus</td><td>");
  html += isHighResolution(cameraSettings.frameSize)
      ? "High-Resolution / stabil"
      : "Stream / niedrige Latenz";
  html += F("</td></tr>");
  html += F("</table><p><a href='/camera'>Kamera-Settings ändern</a></p></div>");

  if (WiFi.status() == WL_CONNECTED) {
    html += F("<script>document.getElementById('stream').src='http://'+location.hostname+':81/stream';</script>");
  }

  html += F("</main></body></html>");
  return html;
}

static String configPage() {
  const bool connected = WiFi.status() == WL_CONNECTED;

  // Scan while keeping the fallback AP online. ESP32 is 2.4 GHz only.
  // show_hidden=true makes hidden SSIDs visible as empty names.
  int networkCount = WiFi.scanNetworks(false, true);

  String html;
  html.reserve(9000);

  html += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>WLAN - ESP32-CAM Indoor</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;max-width:820px;margin:30px auto;padding:0 18px;background:#f4f4f4;color:#222}");
  html += F(".card{background:#fff;padding:20px;margin-bottom:18px;border-radius:10px;box-shadow:0 1px 5px #bbb}");
  html += F("input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}button{padding:9px 14px;cursor:pointer}");
  html += F("table{width:100%;border-collapse:collapse}td,th{padding:7px;border-bottom:1px solid #ddd;text-align:left}");
  html += F(".ssidbtn{width:100%;text-align:left;background:#fff;border:1px solid #bbb;border-radius:5px}.muted{color:#666}a{color:#06c}</style></head><body>");

  html += F("<div class='card'><h1>WLAN</h1><table>");
  html += F("<tr><td>Status</td><td><b>");
  if (connected) html += F("verbunden");
  else if (apMode) html += F("Fallback AP aktiv");
  else html += F("nicht verbunden");
  html += F("</b></td></tr>");

  html += F("<tr><td>Konfigurierte SSID</td><td>");
  html += wifiSsid.length() ? htmlEscape(wifiSsid) : String("-");
  html += F("</td></tr>");

  html += F("<tr><td>Betriebsart</td><td>");
  if (apMode && connected) html += F("STA + AP");
  else if (apMode) html += F("Fallback AP (AP + STA)");
  else html += F("STA");
  html += F("</td></tr>");

  html += F("<tr><td>IP-Adresse</td><td>");
  html += connected ? WiFi.localIP().toString() : String("-");
  html += F("</td></tr>");

  html += F("<tr><td>Fallback AP</td><td>");
  html += fallbackApSsid();
  html += F("</td></tr>");

  if (connected) {
    int32_t rssi = WiFi.RSSI();
    html += F("<tr><td>Signalstärke</td><td>");
    html += String(rssi);
    html += F(" dBm (");
    html += wifiSignalRating(rssi);
    html += F(")</td></tr>");

    html += F("<tr><td>BSSID / Access Point</td><td>");
    html += WiFi.BSSIDstr();
    html += F("</td></tr>");

    html += F("<tr><td>Kanal</td><td>");
    html += String(WiFi.channel());
    html += F("</td></tr>");
  }

  html += F("<tr><td>Reconnect-Versuche</td><td>");
  html += String(wifiReconnectAttempts);
  html += F("</td></tr>");

  html += F("<tr><td>Erfolgreiche Reconnects</td><td>");
  html += String(wifiReconnectSuccesses);
  html += F("</td></tr>");
  html += F("</table></div>");

  html += F("<div class='card'><h2>Roaming</h2>");
  html += F("<form method='POST' action='/roaming/save'>");
  html += F("<label>Roaming-Scan ab RSSI schlechter als (dBm)</label>");
  html += F("<input type='number' min='-95' max='-50' name='trigger_rssi' value='");
  html += String(wifiRoamTriggerRssi);
  html += F("' required>");
  html += F("<label>Mindestverbesserung für AP-Wechsel (dB)</label>");
  html += F("<input type='number' min='1' max='20' name='min_improve_db' value='");
  html += String(wifiRoamMinImprovementDb);
  html += F("' required>");
  html += F("<button type='submit'>Roaming speichern</button></form>");
  html += F("<p><small>Standard: -72 dBm / 4 dB. Niedrigere dBm-Schwelle = später scannen; kleinere dB-Differenz = aggressiver wechseln.</small></p></div>");

  html += F("<div class='card'><h2>Gefundene WLANs</h2>");
  html += F("<p><small>ESP32-CAM unterstützt nur 2,4-GHz-WLAN. Netzwerk anklicken, dann Passwort eingeben.</small></p>");

  if (networkCount <= 0) {
    html += F("<p>Keine WLANs gefunden.</p>");
  } else {
    html += F("<table><tr><th>SSID</th><th>BSSID</th><th>Signal</th><th>Kanal</th><th>Sicherheit</th></tr>");

    for (int i = 0; i < networkCount; i++) {
      String scannedSsid = WiFi.SSID(i);
      String displaySsid = scannedSsid.length() ? htmlEscape(scannedSsid) : String("<i>versteckt</i>");
      bool openNetwork = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;

      html += F("<tr><td>");
      if (scannedSsid.length()) {
        html += F("<button type='button' class='ssidbtn' onclick=\"selectSsid('");
        String jsSsid = scannedSsid;
        jsSsid.replace("\\", "\\\\");
        jsSsid.replace("'", "\\'");
        html += jsSsid;
        html += F("')\">");
        html += displaySsid;
        html += F("</button>");
      } else {
        html += displaySsid;
      }
      html += F("</td><td><code>");
      html += WiFi.BSSIDstr(i);
      html += F("</code></td><td>");
      html += String(WiFi.RSSI(i));
      html += F(" dBm</td><td>");
      html += String(WiFi.channel(i));
      html += F("</td><td>");
      html += openNetwork ? "offen" : "geschützt";
      html += F("</td></tr>");
    }

    html += F("</table>");
  }

  html += F("<p><a href='/config'>Erneut scannen</a></p></div>");
  WiFi.scanDelete();

  html += F("<div class='card'><h2>WLAN verbinden</h2>");
  html += F("<form method='POST' action='/save'>");
  html += F("<label>SSID</label><input id='ssid' name='ssid' value='");
  html += htmlEscape(wifiSsid);
  html += F("' required>");
  html += F("<label>Passwort</label>");
  html += F("<input type='password' name='password' value='' placeholder='Passwort eingeben'>");
  html += F("<button type='submit'>Speichern und verbinden</button></form>");
  html += F("<p><small>Bei derselben SSID bedeutet ein leeres Passwort: gespeichertes Passwort beibehalten. Bei einer neuen SSID wird ein leeres Passwort als offenes WLAN gespeichert.</small></p>");
  html += F("<p><a href='/'>Zurück zur Kamera</a></p></div>");
  html += F("<script>function selectSsid(s){document.getElementById('ssid').value=s;document.getElementById('ssid').scrollIntoView({behavior:'smooth',block:'center'});}</script>");
  html += F("</body></html>");

  return html;
}

static String cameraPage() {
  String html;
  html.reserve(6000);

  html += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Kamera - ESP32-CAM Indoor</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;max-width:820px;margin:30px auto;padding:0 18px;background:#f4f4f4;color:#222}");
  html += F(".card{background:#fff;padding:20px;margin-bottom:18px;border-radius:10px;box-shadow:0 1px 5px #bbb}");
  html += F("select,input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}");
  html += F("button{padding:10px 18px;cursor:pointer}label{font-weight:600}a{color:#06c}</style></head><body>");

  html += F("<div class='card'><h1>Kamera-Settings</h1>");
  html += F("<p>Firmware <b>v");
  html += APP_VERSION;
  html += F("</b></p>");
  html += F("<form method='POST' action='/camera/save'>");

  html += F("<label>Auflösung</label><select name='framesize'>");
  const char *values[] = {"QQVGA","QVGA","CIF","VGA","SVGA","XGA","SXGA","UXGA"};
  const char *labels[] = {"160x120","320x240","400x296","640x480","800x600","1024x768","1280x1024","1600x1200"};
  String currentFrameSize = frameSizeValue(cameraSettings.frameSize);

  for (size_t i = 0; i < 8; i++) {
    html += F("<option value='");
    html += values[i];
    html += F("'");
    if (currentFrameSize == values[i]) html += F(" selected");
    html += F(">");
    html += labels[i];
    html += F("</option>");
  }
  html += F("</select>");
  html += F("<p><small>Bis VGA: flüssiger Stream mit 2 Framebuffern. Ab SVGA: stabiler High-Resolution-Modus mit 1 Framebuffer. Beim Wechsel zwischen den Bereichen erfolgt ein Neustart.</small></p>");

  html += F("<label>JPEG Qualität (4 = beste Qualität, 63 = stärkste Kompression)</label>");
  html += F("<input type='number' min='4' max='63' name='quality' value='");
  html += String(cameraSettings.jpegQuality);
  html += F("'>");

  html += F("<label>Helligkeit (-2 bis 2)</label>");
  html += F("<input type='number' min='-2' max='2' name='brightness' value='");
  html += String(cameraSettings.brightness);
  html += F("'>");

  html += F("<label>Kontrast (-2 bis 2)</label>");
  html += F("<input type='number' min='-2' max='2' name='contrast' value='");
  html += String(cameraSettings.contrast);
  html += F("'>");

  html += F("<label>Sättigung (-2 bis 2)</label>");
  html += F("<input type='number' min='-2' max='2' name='saturation' value='");
  html += String(cameraSettings.saturation);
  html += F("'>");

  html += F("<label><input type='checkbox' name='vflip' value='1' style='width:auto'");
  if (cameraSettings.vflip) html += F(" checked");
  html += F("> Bild vertikal drehen</label><br>");

  html += F("<label><input type='checkbox' name='hmirror' value='1' style='width:auto'");
  if (cameraSettings.hmirror) html += F(" checked");
  html += F("> Bild horizontal spiegeln</label><br><br>");

  html += F("<button type='submit'>Speichern und anwenden</button></form>");
  html += F("<p><small>Die Werte werden im NVS des ESP32 gespeichert und nach jedem Neustart wieder geladen.</small></p>");
  html += F("<p><a href='/'>Zurück zur Kamera</a></p></div></body></html>");

  return html;
}

static esp_err_t camera_handler(httpd_req_t *req) {
  String html = cameraPage();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t camera_save_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
    return ESP_FAIL;
  }

  String body;
  body.reserve(req->content_len + 1);

  int remaining = req->content_len;
  char buffer[256];

  while (remaining > 0) {
    int received = httpd_req_recv(req, buffer, min(remaining, (int)sizeof(buffer)));
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }

    body.concat(buffer, received);
    remaining -= received;
  }

  const bool wasHighResolution = isHighResolution(cameraSettings.frameSize);

  cameraSettings.frameSize = frameSizeFromValue(formValue(body, "framesize"));
  cameraSettings.jpegQuality = constrain(formValue(body, "quality").toInt(), 4, 63);
  cameraSettings.brightness = constrain(formValue(body, "brightness").toInt(), -2, 2);
  cameraSettings.contrast = constrain(formValue(body, "contrast").toInt(), -2, 2);
  cameraSettings.saturation = constrain(formValue(body, "saturation").toInt(), -2, 2);
  cameraSettings.vflip = formValue(body, "vflip") == "1";
  cameraSettings.hmirror = formValue(body, "hmirror") == "1";

  saveCameraConfig();

  const bool isNowHighResolution = isHighResolution(cameraSettings.frameSize);
  const bool bufferingModeChanged = wasHighResolution != isNowHighResolution;

  if (!bufferingModeChanged) {
    applyCameraSettings();
  }

  String response;
  response.reserve(1400);
  response += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  response += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (bufferingModeChanged) {
    response += F("<meta http-equiv='refresh' content='6;url=/'>");
  }
  response += F("<title>Kamera gespeichert</title></head><body>");
  response += F("<h1>Kamera-Settings gespeichert</h1>");
  if (bufferingModeChanged) {
    response += F("<p>Framebuffer-Modus wird angepasst. Die Kamera startet einmal neu...</p>");
  } else {
    response += F("<p>Die Einstellungen wurden im ESP32 gespeichert und sofort angewendet.</p>");
  }
  response += F("<p><a href='/camera'>Zurück zu den Kamera-Settings</a> · <a href='/'>Zum Stream</a></p>");
  response += F("</body></html>");

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  esp_err_t result = httpd_resp_send(req, response.c_str(), response.length());

  if (bufferingModeChanged) {
    delay(1000);
    ESP.restart();
  }

  return result;
}

static esp_err_t flash_on_handler(httpd_req_t *req) {
  setFlashLed(true);
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t flash_off_handler(httpd_req_t *req) {
  setFlashLed(false);
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t roaming_save_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 512) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
    return ESP_FAIL;
  }

  String body;
  body.reserve(req->content_len + 1);

  int remaining = req->content_len;
  char buffer[256];

  while (remaining > 0) {
    int received = httpd_req_recv(req, buffer, min(remaining, (int)sizeof(buffer)));
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }

    body.concat(buffer, received);
    remaining -= received;
  }

  int triggerRssi = formValue(body, "trigger_rssi").toInt();
  int minImprovement = formValue(body, "min_improve_db").toInt();

  if (triggerRssi < -95 || triggerRssi > -50 ||
      minImprovement < 1 || minImprovement > 20) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Roaming values out of range");
    return ESP_FAIL;
  }

  wifiRoamTriggerRssi = triggerRssi;
  wifiRoamMinImprovementDb = minImprovement;
  saveRoamingConfig();

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/config");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t root_handler(httpd_req_t *req) {
  String html = rootPage();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t config_handler(httpd_req_t *req) {
  String html = configPage();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t save_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
    return ESP_FAIL;
  }

  String body;
  body.reserve(req->content_len + 1);

  int remaining = req->content_len;
  char buffer[256];

  while (remaining > 0) {
    int received = httpd_req_recv(req, buffer, min(remaining, (int)sizeof(buffer)));
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form data");
      return ESP_FAIL;
    }

    body.concat(buffer, received);
    remaining -= received;
  }

  String newSsid = formValue(body, "ssid");
  String newPassword = formValue(body, "password");
  newSsid.trim();

  if (newSsid.length() == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
    return ESP_FAIL;
  }

  // When selecting a different SSID, an empty password must mean
  // "open network", not "reuse the password from the old WLAN".
  const bool updatePassword =
      newPassword.length() > 0 || wifiSsid.length() == 0 || newSsid != wifiSsid;

  if (!saveWifiConfig(newSsid, newPassword, updatePassword)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save Wi-Fi configuration");
    return ESP_FAIL;
  }

  const char *response =
      "<!doctype html><html lang='de'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='8;url=/'>"
      "<title>WLAN gespeichert</title></head><body>"
      "<h1>Gespeichert</h1>"
      "<p>WLAN-Konfiguration gespeichert. Neustart...</p>"
      "<p>Die Seite versucht in 8 Sekunden automatisch zur Kamera zurückzukehren.</p>"
      "<p><small>Wenn du gerade vom Fallback-AP in dein normales WLAN wechselst, "
      "ändert sich die IP-Adresse. In diesem Fall kann die automatische Weiterleitung "
      "nicht funktionieren; öffne dann die neue Kamera-IP im normalen WLAN.</small></p>"
      "<p><a href='/'>Jetzt versuchen</a></p>"
      "</body></html>";

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);

  delay(1000);
  ESP.restart();
  return ESP_OK;
}

static esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(
      req,
      reinterpret_cast<const char *>(fb->buf),
      fb->len);
  esp_camera_fb_return(fb);
  return result;
}

static esp_err_t logs_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");

  String header;
  header.reserve(1400);
  header += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  header += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  header += F("<title>ESP32-CAM System-Log</title>");
  header += F("<style>body{font-family:system-ui,sans-serif;max-width:1000px;margin:20px auto;padding:0 15px;background:#111;color:#eee}");
  header += F("pre{background:#1b1b1b;color:#ddd;padding:15px;border-radius:8px;white-space:pre-wrap;overflow:auto}");
  header += F("a{color:#8ec5ff;margin-right:14px}</style></head><body>");
  header += F("<h1>System-Log</h1><p>Firmware v");
  header += APP_VERSION;
  header += F("</p><p><a href='/logs/download'>Log herunterladen</a>");
  header += F("<a href='/logs/clear' onclick=\"return confirm('Log wirklich löschen?')\">Log löschen</a>");
  header += F("<a href='/'>Zurück</a></p><pre>");

  esp_err_t result = httpd_resp_send_chunk(req, header.c_str(), header.length());
  if (result != ESP_OK) return result;

  File file = LittleFS.open(LOG_FILE, "r");
  if (!file) {
    result = httpd_resp_send_chunk(req, "No log entries.\n", HTTPD_RESP_USE_STRLEN);
  } else {
    const size_t size = file.size();

    // Keep the browser view compact; the download endpoint always returns the
    // complete persistent file.
    if (size > 20000) {
      file.seek(size - 20000, SeekSet);
      result = httpd_resp_send_chunk(
          req,
          "[... Logansicht gekürzt; vollständiger Log über Download ...]\n",
          HTTPD_RESP_USE_STRLEN);
    }

    String chunk;
    chunk.reserve(768);

    while (result == ESP_OK && file.available()) {
      const char c = static_cast<char>(file.read());

      if (c == '&') chunk += F("&amp;");
      else if (c == '<') chunk += F("&lt;");
      else if (c == '>') chunk += F("&gt;");
      else chunk += c;

      if (chunk.length() >= 512) {
        result = httpd_resp_send_chunk(req, chunk.c_str(), chunk.length());
        chunk = "";
      }
    }

    if (result == ESP_OK && chunk.length() > 0) {
      result = httpd_resp_send_chunk(req, chunk.c_str(), chunk.length());
    }

    file.close();
  }

  if (result == ESP_OK) {
    const char *footer = "</pre><p><a href='/logs'>Refresh</a> · <a href='/'>Zurück</a></p></body></html>";
    result = httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_send_chunk(req, nullptr, 0);
  return result;
}

static esp_err_t logs_download_handler(httpd_req_t *req) {
  File file = LittleFS.open(LOG_FILE, "r");
  if (!file) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Log file not found");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=esp32-cam-system.log");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  uint8_t buffer[768];
  esp_err_t result = ESP_OK;

  while (file.available() && result == ESP_OK) {
    const size_t count = file.read(buffer, sizeof(buffer));
    if (count > 0) {
      result = httpd_resp_send_chunk(
          req,
          reinterpret_cast<const char *>(buffer),
          count);
    }
  }

  file.close();
  httpd_resp_send_chunk(req, nullptr, 0);
  return result;
}

static esp_err_t logs_clear_handler(httpd_req_t *req) {
  LittleFS.remove(LOG_FILE);

  File file = LittleFS.open(LOG_FILE, "w");
  if (file) file.close();

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/logs");
  esp_err_t result = httpd_resp_send(req, nullptr, 0);

  logPrintln("System log cleared");
  return result;
}

static esp_err_t status_handler(httpd_req_t *req) {
  String body;
  body.reserve(1200);

  sensor_t *sensor = esp_camera_sensor_get();
  const bool cameraDetected = sensor != nullptr;

  body += F("{\"version\":\"");
  body += APP_VERSION;
  body += F("\",\"wifi\":\"");
  body += (WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  body += F("\",\"ap_mode\":");
  body += apMode ? "true" : "false";
  body += F(",\"configured_ssid\":\"");
  body += wifiSsid;
  body += F("\",\"rssi\":");
  body += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("null");
  body += F(",\"rssi_min\":");
  body += wifiRssiSeen ? String(wifiRssiMin) : String("null");
  body += F(",\"rssi_max\":");
  body += wifiRssiSeen ? String(wifiRssiMax) : String("null");
  body += F(",\"ip\":\"");
  body += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
  body += F("\",\"ap_ip\":\"");
  body += apMode ? WiFi.softAPIP().toString() : String("");
  body += F("\",\"uptime_s\":");
  body += String(millis() / 1000UL);
  body += F(",\"free_heap\":");
  body += String(ESP.getFreeHeap());
  body += F(",\"time_synchronized\":");
  body += timeIsSynchronized() ? "true" : "false";
  body += F(",\"local_time\":\"");
  body += currentLocalTime();
  body += F("\"");
  body += F(",\"psram\":");
  body += psramFound() ? "true" : "false";
  body += F(",\"psram_size\":");
  body += String(ESP.getPsramSize());
  body += F(",\"free_psram\":");
  body += String(ESP.getFreePsram());
  body += F(",\"reconnect_attempts\":");
  body += String(wifiReconnectAttempts);
  body += F(",\"reconnect_successes\":");
  body += String(wifiReconnectSuccesses);
  body += F(",\"bssid\":\"");
  body += WiFi.status() == WL_CONNECTED ? WiFi.BSSIDstr() : String("");
  body += F("\",\"channel\":");
  body += WiFi.status() == WL_CONNECTED ? String(WiFi.channel()) : String("null");
  body += F(",\"roam_attempts\":");
  body += String(wifiRoamAttempts);
  body += F(",\"roam_successes\":");
  body += String(wifiRoamSuccesses);
  body += F(",\"roam_trigger_rssi\":");
  body += String(wifiRoamTriggerRssi);
  body += F(",\"roam_min_improvement_db\":");
  body += String(wifiRoamMinImprovementDb);
  body += F(",\"roam_in_progress\":");
  body += wifiRoamInProgress ? "true" : "false";
  body += F(",\"roam_target_bssid\":\"");
  body += wifiRoamTargetBssid;
  body += F("\"");
  body += F(",\"stream_clients\":");
  body += String(streamClientCount);
  body += F(",\"stream_restarts\":");
  body += String(streamRestartCount);
  body += F(",\"last_frame_age_s\":");
  if (streamLastFrameMillis > 0) {
    body += String((millis() - streamLastFrameMillis) / 1000UL);
  } else {
    body += F("null");
  }

  body += F(",\"camera\":{\"detected\":");
  body += cameraDetected ? "true" : "false";
  body += F(",\"sensor_pid\":");
  body += cameraDetected ? String(sensor->id.PID) : String("null");
  body += F(",\"resolution\":\"");
  body += frameSizeName(cameraSettings.frameSize);
  body += F("\",\"jpeg_quality\":");
  body += String(cameraSettings.jpegQuality);
  body += F(",\"brightness\":");
  body += String(cameraSettings.brightness);
  body += F(",\"contrast\":");
  body += String(cameraSettings.contrast);
  body += F(",\"saturation\":");
  body += String(cameraSettings.saturation);
  body += F(",\"vflip\":");
  body += cameraSettings.vflip ? "true" : "false";
  body += F(",\"hmirror\":");
  body += cameraSettings.hmirror ? "true" : "false";
  body += F(",\"flash_led\":");
  body += flashLedOn ? "true" : "false";
  body += F("}}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t stream_handler(httpd_req_t *req) {
  streamClientCount++;
  logPrintf("Stream client connected, clients=%d\n", streamClientCount);

  esp_err_t result = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) {
    streamClientCount--;
    logPrintf("Stream client disconnected, clients=%d\n", streamClientCount);
    return result;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      logPrintln("Camera capture failed");
      result = ESP_FAIL;
      break;
    }

    char header[64];
    const int headerLen = snprintf(header, sizeof(header), STREAM_PART, fb->len);

    result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, header, headerLen);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          req,
          reinterpret_cast<const char *>(fb->buf),
          fb->len);
    }

    esp_camera_fb_return(fb);

    if (result != ESP_OK) break;

    streamLastFrameMillis = millis();

    if (streamRestartRequested) {
      logPrintln("Stream client closed for watchdog restart");
      result = ESP_FAIL;
      break;
    }

    delay(1);
  }

  if (streamClientCount > 0) streamClientCount--;
  logPrintf("Stream client disconnected, clients=%d\n", streamClientCount);
  return result;
}

static bool startStreamServer();

static void startWebServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 16;

  if (httpd_start(&http_server, &config) == ESP_OK) {
    httpd_uri_t rootUri = {};
    rootUri.uri = "/";
    rootUri.method = HTTP_GET;
    rootUri.handler = root_handler;
    httpd_register_uri_handler(http_server, &rootUri);

    httpd_uri_t cameraUri = {};
    cameraUri.uri = "/camera";
    cameraUri.method = HTTP_GET;
    cameraUri.handler = camera_handler;
    httpd_register_uri_handler(http_server, &cameraUri);

    httpd_uri_t cameraSaveUri = {};
    cameraSaveUri.uri = "/camera/save";
    cameraSaveUri.method = HTTP_POST;
    cameraSaveUri.handler = camera_save_handler;
    httpd_register_uri_handler(http_server, &cameraSaveUri);

    httpd_uri_t configUri = {};
    configUri.uri = "/config";
    configUri.method = HTTP_GET;
    configUri.handler = config_handler;
    httpd_register_uri_handler(http_server, &configUri);

    httpd_uri_t saveUri = {};
    saveUri.uri = "/save";
    saveUri.method = HTTP_POST;
    saveUri.handler = save_handler;
    httpd_register_uri_handler(http_server, &saveUri);

    httpd_uri_t jpgUri = {};
    jpgUri.uri = "/jpg";
    jpgUri.method = HTTP_GET;
    jpgUri.handler = jpg_handler;
    httpd_register_uri_handler(http_server, &jpgUri);

    httpd_uri_t statusUri = {};
    statusUri.uri = "/status";
    statusUri.method = HTTP_GET;
    statusUri.handler = status_handler;
    httpd_register_uri_handler(http_server, &statusUri);

    httpd_uri_t logsUri = {};
    logsUri.uri = "/logs";
    logsUri.method = HTTP_GET;
    logsUri.handler = logs_handler;
    httpd_register_uri_handler(http_server, &logsUri);

    httpd_uri_t logsDownloadUri = {};
    logsDownloadUri.uri = "/logs/download";
    logsDownloadUri.method = HTTP_GET;
    logsDownloadUri.handler = logs_download_handler;
    httpd_register_uri_handler(http_server, &logsDownloadUri);

    httpd_uri_t logsClearUri = {};
    logsClearUri.uri = "/logs/clear";
    logsClearUri.method = HTTP_GET;
    logsClearUri.handler = logs_clear_handler;
    httpd_register_uri_handler(http_server, &logsClearUri);

    httpd_uri_t flashOnUri = {};
    flashOnUri.uri = "/flash/on";
    flashOnUri.method = HTTP_POST;
    flashOnUri.handler = flash_on_handler;
    httpd_register_uri_handler(http_server, &flashOnUri);

    httpd_uri_t flashOffUri = {};
    flashOffUri.uri = "/flash/off";
    flashOffUri.method = HTTP_POST;
    flashOffUri.handler = flash_off_handler;
    httpd_register_uri_handler(http_server, &flashOffUri);

    httpd_uri_t roamingSaveUri = {};
    roamingSaveUri.uri = "/roaming/save";
    roamingSaveUri.method = HTTP_POST;
    roamingSaveUri.handler = roaming_save_handler;
    httpd_register_uri_handler(http_server, &roamingSaveUri);
  }

  startStreamServer();
}

static bool startStreamServer() {
  if (stream_server != nullptr) return true;

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = 32769;
  streamConfig.max_uri_handlers = 4;

  if (httpd_start(&stream_server, &streamConfig) != ESP_OK) {
    stream_server = nullptr;
    logPrintln("Stream server start failed");
    return false;
  }

  httpd_uri_t streamUri = {};
  streamUri.uri = "/stream";
  streamUri.method = HTTP_GET;
  streamUri.handler = stream_handler;

  if (httpd_register_uri_handler(stream_server, &streamUri) != ESP_OK) {
    logPrintln("Stream handler registration failed");
    httpd_stop(stream_server);
    stream_server = nullptr;
    return false;
  }

  logPrintln("Stream server started");
  return true;
}

static bool restartStreamServer() {
  logPrintln("Restarting stream server");
  streamRestartRequested = true;

  delay(50);

  if (stream_server != nullptr) {
    httpd_stop(stream_server);
    stream_server = nullptr;
  }

  streamClientCount = 0;
  streamRestartRequested = false;
  streamLastFrameMillis = millis();

  if (!startStreamServer()) {
    logPrintln("Stream server restart failed");
    return false;
  }

  streamRestartCount++;
  logPrintln("Stream server restarted");
  return true;
}

static void maintainStreamWatchdog() {
  if (streamClientCount <= 0 || streamLastFrameMillis == 0) return;

  const unsigned long age = millis() - streamLastFrameMillis;
  if (age < STREAM_WATCHDOG_TIMEOUT_MS) return;

  logPrintf("Stream watchdog: no successful frame for %lu s\n",
            age / 1000UL);

  if (!restartStreamServer()) {
    logPrintln("Rebooting ESP32 after stream recovery failure");
    delay(500);
    ESP.restart();
  }
}

static bool initCameraOnce() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = cameraSettings.frameSize;
  config.jpeg_quality = cameraSettings.jpegQuality;

  const bool highResolution = isHighResolution(cameraSettings.frameSize);

  if (psramFound()) {
    config.fb_count = highResolution ? 1 : 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  config.grab_mode =
      highResolution ? CAMERA_GRAB_WHEN_EMPTY : CAMERA_GRAB_LATEST;

  logPrintf("Camera buffering: %s, fb_count=%d\n",
            highResolution ? "stable high-resolution" : "low-latency stream",
            config.fb_count);

  const esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    logPrintf("Camera init failed: 0x%x\n", err);
    return false;
  }

  applyCameraSettings();
  return true;
}

static void powerCycleCamera() {
#if PWDN_GPIO_NUM >= 0
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(250);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(500);
#else
  delay(750);
#endif
}

static bool initCamera() {
  static const int CAMERA_INIT_ATTEMPTS = 3;

  for (int attempt = 1; attempt <= CAMERA_INIT_ATTEMPTS; attempt++) {
    logPrintf("Camera init attempt %d/%d\n", attempt, CAMERA_INIT_ATTEMPTS);

    if (initCameraOnce()) {
      if (attempt > 1) {
        logPrintf("Camera init recovered on attempt %d\n", attempt);
      }
      return true;
    }

    // Clean up any partial driver state before retrying.
    esp_camera_deinit();

    if (attempt < CAMERA_INIT_ATTEMPTS) {
      logPrintln("Camera init retry after sensor power-cycle");
      powerCycleCamera();
    }
  }

  logPrintln("Camera init failed after 3 attempts");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  initLogger();

  logPrintln();
  logPrintln("================================");
  logPrintln("ESP32-CAM Indoor");
  logPrintf("Firmware version: v%s\n", APP_VERSION);
  logPrintln("================================");

  loadCameraConfig();
  loadRoamingConfig();

  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  setFlashLed(false);

  if (!initCamera()) {
    delay(3000);
    ESP.restart();
  }

  if (!connectWifi()) {
    startAccessPoint();
  }

  startWebServers();

  if (WiFi.status() == WL_CONNECTED) {
    logPrintf("Web UI:   http://%s/\n", WiFi.localIP().toString().c_str());
    logPrintf("Snapshot: http://%s/jpg\n", WiFi.localIP().toString().c_str());
    logPrintf("Stream:   http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  }

  if (apMode) {
    logPrintf("Config UI: http://%s/config\n", WiFi.softAPIP().toString().c_str());
  }
}

void loop() {
  maintainWifi();
  maintainNtp();
  checkForBetterAccessPoint();
  maintainStreamWatchdog();
  logConsoleStatus();
  delay(250);
}
