#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include <time.h>

#include "camera_pins.h"

static const char *APP_VERSION = "0.8.0";
static const char *AP_PASSWORD = "esp32cam123";
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
static const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
static const unsigned long WIFI_ROAM_CHECK_INTERVAL_MS = 60000;
static const int WIFI_ROAM_TRIGGER_RSSI_DEFAULT = -72;
static const int WIFI_ROAM_MIN_IMPROVEMENT_DB_DEFAULT = 4;

static httpd_handle_t http_server = nullptr;
static httpd_handle_t stream_server = nullptr;

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

struct CameraSettings {
  framesize_t frameSize = FRAMESIZE_VGA;
  int jpegQuality = 12;
  int brightness = 0;
  int contrast = 0;
  int saturation = 0;
  bool vflip = false;
  bool hmirror = false;
  bool timestamp = false;
};

static CameraSettings cameraSettings;
static bool flashLedOn = false;

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

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

static void syncClock() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Austria: CET/CEST with automatic daylight-saving transition.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0",
               "pool.ntp.org",
               "time.nist.gov");

  unsigned long started = millis();
  while (!timeIsSynchronized() && millis() - started < 5000) {
    delay(100);
  }

  if (timeIsSynchronized()) {
    Serial.printf("NTP time synchronized: %s\n", currentLocalTime().c_str());
  } else {
    Serial.println("NTP synchronization unavailable");
  }
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
  cameraSettings.timestamp = prefs.getBool("timestamp", false);
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
  prefs.putBool("timestamp", cameraSettings.timestamp);
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

  Serial.println();
  Serial.println("Fallback AP started");
  Serial.printf("SSID: %s\n", ssid.c_str());
  Serial.printf("Password: %s\n", AP_PASSWORD);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
}

static bool connectWifi() {
  if (!loadWifiConfig()) {
    Serial.println("No stored Wi-Fi configuration");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  Serial.printf("Connecting to Wi-Fi SSID: %s", wifiSsid.c_str());

  const unsigned long started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    wifiWasConnected = true;
    wifiLastConnectedMillis = millis();

    Serial.println("Wi-Fi connected");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    return true;
  }

  Serial.println("Wi-Fi connection failed");
  return false;
}

static void maintainWifi() {
  const bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      if (wifiRoamInProgress) {
        wifiRoamSuccesses++;
        wifiRoamInProgress = false;
        Serial.println("Wi-Fi roam completed");
      } else {
        wifiReconnectSuccesses++;
        Serial.println("Wi-Fi reconnected");
      }

      wifiLastConnectedMillis = millis();
      syncClock();
      Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());

      if (apMode) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        apMode = false;
        Serial.println("Fallback AP stopped");
      }
    }

    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("Wi-Fi connection lost");
  }

  if (wifiSsid.length() == 0) {
    if (!apMode) startAccessPoint();
    return;
  }

  if (millis() - wifiLastRetryMillis < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  if (wifiRoamInProgress) {
    Serial.println("Wi-Fi roam timed out; falling back to normal reconnect");
    wifiRoamInProgress = false;
  }

  wifiLastRetryMillis = millis();
  wifiReconnectAttempts++;

  Serial.printf("Wi-Fi reconnect attempt %lu\n", wifiReconnectAttempts);

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

  Serial.printf("Roam scan: current AP %s at %d dBm\n",
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
    Serial.println("Roam scan: no other AP with the configured SSID found");
    return;
  }

  Serial.printf("Roam scan: best candidate %s at %d dBm (%+d dB)\n",
                bestBssidString.c_str(),
                bestRssi,
                bestRssi - currentRssi);

  if (bestRssi < currentRssi + wifiRoamMinImprovementDb) {
    Serial.printf("Roam scan: improvement below %d dB threshold, staying on current AP\n",
                  wifiRoamMinImprovementDb);
    return;
  }

  wifiRoamAttempts++;
  wifiRoamInProgress = true;
  wifiRoamTargetBssid = bestBssidString;

  Serial.printf("Roaming from %s (%d dBm) to %s (%d dBm), channel %d\n",
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


struct Glyph5x7 {
  char c;
  uint8_t rows[7];
};

static const Glyph5x7 TIMESTAMP_FONT[] = {
  {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
  {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
  {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
  {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
  {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
  {'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
  {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
  {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
  {'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
  {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
  {':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
  {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}
};

static const uint8_t *glyphFor(char c) {
  for (size_t i = 0; i < sizeof(TIMESTAMP_FONT) / sizeof(TIMESTAMP_FONT[0]); i++) {
    if (TIMESTAMP_FONT[i].c == c) return TIMESTAMP_FONT[i].rows;
  }
  return nullptr;
}

static void setRgbPixel(uint8_t *rgb, int width, int height,
                        int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= width || y >= height) return;
  size_t offset = (static_cast<size_t>(y) * width + x) * 3;
  rgb[offset] = r;
  rgb[offset + 1] = g;
  rgb[offset + 2] = b;
}

static void drawTimestampText(uint8_t *rgb, int width, int height, const String &text) {
  const int scale = width >= 800 ? 3 : 2;
  const int charWidth = 6 * scale;
  const int textWidth = text.length() * charWidth;
  const int textHeight = 7 * scale;
  const int margin = 8;
  const int x0 = margin;
  const int y0 = max(margin, height - textHeight - margin);

  // Black background for readability.
  for (int y = y0 - 4; y < y0 + textHeight + 4; y++) {
    for (int x = x0 - 4; x < min(width, x0 + textWidth + 4); x++) {
      setRgbPixel(rgb, width, height, x, y, 0, 0, 0);
    }
  }

  int cursorX = x0;
  for (size_t i = 0; i < text.length(); i++) {
    const uint8_t *glyph = glyphFor(text[i]);
    if (!glyph) {
      cursorX += charWidth;
      continue;
    }

    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 5; col++) {
        if ((glyph[row] >> (4 - col)) & 0x01) {
          for (int sy = 0; sy < scale; sy++) {
            for (int sx = 0; sx < scale; sx++) {
              setRgbPixel(rgb, width, height,
                          cursorX + col * scale + sx,
                          y0 + row * scale + sy,
                          255, 255, 255);
            }
          }
        }
      }
    }
    cursorX += charWidth;
  }
}

static bool makeTimestampedJpeg(camera_fb_t *fb, uint8_t **outJpg, size_t *outLen) {
  if (!fb || !outJpg || !outLen || !timeIsSynchronized()) return false;

  const size_t rgbLen = static_cast<size_t>(fb->width) * fb->height * 3;
  uint8_t *rgb = static_cast<uint8_t *>(
      heap_caps_malloc(rgbLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (!rgb) {
    Serial.printf("Timestamp: RGB buffer allocation failed (%u bytes)\n",
                  static_cast<unsigned>(rgbLen));
    return false;
  }

  if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb)) {
    Serial.println("Timestamp: JPEG decode failed");
    free(rgb);
    return false;
  }

  drawTimestampText(rgb, fb->width, fb->height, currentLocalTime());

  bool ok = fmt2jpg(rgb,
                    rgbLen,
                    fb->width,
                    fb->height,
                    PIXFORMAT_RGB888,
                    cameraSettings.jpegQuality,
                    outJpg,
                    outLen);

  free(rgb);

  if (!ok) Serial.println("Timestamp: JPEG encode failed");
  return ok;
}

static String rootPage() {
  String html;
  html.reserve(3000);

  html += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>ESP32-CAM Indoor</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}");
  html += F("main{max-width:900px;margin:auto;padding:16px}img{width:100%;height:auto;border-radius:8px;background:#222}");
  html += F(".meta{margin-top:12px;color:#bbb}a{color:#8ec5ff}.warn{color:#ffcb6b}.ok{color:#8bd450}</style>");
  html += F("</head><body><main><h1>ESP32-CAM Indoor</h1>");

  html += F("<p>Firmware <b>v");
  html += APP_VERSION;
  html += F("</b></p>");

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
  html += F("<a href='/config'>WLAN-Konfiguration</a></div>");

  html += F("<div class='meta' style='margin-top:18px'><b>Flash-LED:</b> ");
  html += flashLedOn ? "AN" : "AUS";
  html += F(" &nbsp; <form method='POST' action='/flash/on' style='display:inline'>");
  html += F("<button type='submit'>LED an</button></form> ");
  html += F("<form method='POST' action='/flash/off' style='display:inline'>");
  html += F("<button type='submit'>LED aus</button></form></div>");

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

  html += F("<label><input type='checkbox' name='timestamp' value='1' style='width:auto'");
  if (cameraSettings.timestamp) html += F(" checked");
  html += F("> Zeitstempel in JPG-Snapshots einblenden</label><br><br>");

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

  cameraSettings.frameSize = frameSizeFromValue(formValue(body, "framesize"));
  cameraSettings.jpegQuality = constrain(formValue(body, "quality").toInt(), 4, 63);
  cameraSettings.brightness = constrain(formValue(body, "brightness").toInt(), -2, 2);
  cameraSettings.contrast = constrain(formValue(body, "contrast").toInt(), -2, 2);
  cameraSettings.saturation = constrain(formValue(body, "saturation").toInt(), -2, 2);
  cameraSettings.vflip = formValue(body, "vflip") == "1";
  cameraSettings.hmirror = formValue(body, "hmirror") == "1";
  cameraSettings.timestamp = formValue(body, "timestamp") == "1";

  saveCameraConfig();
  applyCameraSettings();

  String response;
  response.reserve(1200);
  response += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  response += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  response += F("<title>Kamera gespeichert</title></head><body>");
  response += F("<h1>Kamera-Settings gespeichert</h1>");
  response += F("<p>Die Einstellungen wurden im ESP32 gespeichert und sofort angewendet.</p>");
  response += F("<p><a href='/camera'>Zurück zu den Kamera-Settings</a> · <a href='/'>Zum Stream</a></p>");
  response += F("</body></html>");

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, response.c_str(), response.length());
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

  uint8_t *timestampedJpg = nullptr;
  size_t timestampedLen = 0;
  esp_err_t result;

  if (cameraSettings.timestamp &&
      makeTimestampedJpeg(fb, &timestampedJpg, &timestampedLen)) {
    result = httpd_resp_send(
        req,
        reinterpret_cast<const char *>(timestampedJpg),
        timestampedLen);
    free(timestampedJpg);
  } else {
    result = httpd_resp_send(
        req,
        reinterpret_cast<const char *>(fb->buf),
        fb->len);
  }

  esp_camera_fb_return(fb);
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
  body += F(",\"timestamp\":");
  body += cameraSettings.timestamp ? "true" : "false";
  body += F("}}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t result = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) return result;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      return ESP_FAIL;
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
    delay(1);
  }

  return result;
}

static void startWebServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 12;

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

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = 32769;
  streamConfig.max_uri_handlers = 4;

  if (httpd_start(&stream_server, &streamConfig) == ESP_OK) {
    httpd_uri_t streamUri = {};
    streamUri.uri = "/stream";
    streamUri.method = HTTP_GET;
    streamUri.handler = stream_handler;
    httpd_register_uri_handler(stream_server, &streamUri);
  }
}

static bool initCamera() {
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
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  applyCameraSettings();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32-CAM Indoor");
  Serial.printf("Firmware version: v%s\n", APP_VERSION);
  Serial.println("================================");

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
  } else {
    syncClock();
  }

  startWebServers();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Web UI:   http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("Snapshot: http://%s/jpg\n", WiFi.localIP().toString().c_str());
    Serial.printf("Stream:   http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  }

  if (apMode) {
    Serial.printf("Config UI: http://%s/config\n", WiFi.softAPIP().toString().c_str());
  }
}

void loop() {
  maintainWifi();
  checkForBetterAccessPoint();
  delay(250);
}
