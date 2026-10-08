#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

#include "camera_pins.h"
#include "secrets.h"

static httpd_handle_t http_server = nullptr;
static httpd_handle_t stream_server = nullptr;

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-CAM Indoor</title>
  <style>
    body { font-family: system-ui, sans-serif; margin: 0; background: #111; color: #eee; }
    main { max-width: 900px; margin: auto; padding: 16px; }
    img { width: 100%; height: auto; border-radius: 8px; background: #222; }
    .meta { margin-top: 12px; color: #bbb; }
    a { color: #8ec5ff; }
  </style>
</head>
<body>
<main>
  <h1>ESP32-CAM Indoor</h1>
  <img id="stream" alt="camera stream">
  <div class="meta">
    <a href="/jpg">Snapshot</a> · <a href="/status">Status JSON</a>
  </div>
</main>
<script>
  document.getElementById('stream').src =
    'http://' + location.hostname + ':81/stream';
</script>
</body>
</html>
)HTML";

static esp_err_t root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return result;
}

static esp_err_t status_handler(httpd_req_t *req) {
  char body[256];
  const uint32_t uptime = millis() / 1000;
  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  snprintf(
      body, sizeof(body),
      "{\"wifi\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%u}",
      WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
      rssi,
      WiFi.localIP().toString().c_str(),
      static_cast<unsigned long>(uptime),
      ESP.getFreeHeap());

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, body);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t result = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) {
    return result;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      return ESP_FAIL;
    }

    char header[64];
    const int header_len = snprintf(header, sizeof(header), STREAM_PART, fb->len);

    result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(req, header, header_len);
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          req,
          reinterpret_cast<const char *>(fb->buf),
          fb->len);
    }

    esp_camera_fb_return(fb);

    if (result != ESP_OK) {
      break;
    }

    delay(1);
  }

  return result;
}

static void start_web_servers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 8;

  if (httpd_start(&http_server, &config) == ESP_OK) {
    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = root_handler;
    httpd_register_uri_handler(http_server, &root_uri);

    httpd_uri_t jpg_uri = {};
    jpg_uri.uri = "/jpg";
    jpg_uri.method = HTTP_GET;
    jpg_uri.handler = jpg_handler;
    httpd_register_uri_handler(http_server, &jpg_uri);

    httpd_uri_t status_uri = {};
    status_uri.uri = "/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = status_handler;
    httpd_register_uri_handler(http_server, &status_uri);
  }

  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;
  stream_config.max_uri_handlers = 4;

  if (httpd_start(&stream_server, &stream_config) == ESP_OK) {
    httpd_uri_t stream_uri = {};
    stream_uri.uri = "/stream";
    stream_uri.method = HTTP_GET;
    stream_uri.handler = stream_handler;
    httpd_register_uri_handler(stream_server, &stream_uri);
  }
}

static bool init_camera() {
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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_framesize(sensor, FRAMESIZE_VGA);
  }

  return true;
}

static void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - started > 30000) {
      Serial.println("\nWi-Fi connection timeout; restarting...");
      delay(1000);
      ESP.restart();
    }
  }

  Serial.println();
  Serial.printf("Wi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32-CAM Indoor starting...");

  if (!init_camera()) {
    delay(3000);
    ESP.restart();
  }

  connect_wifi();
  start_web_servers();

  Serial.printf("Web UI:   http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("Snapshot: http://%s/jpg\n", WiFi.localIP().toString().c_str());
  Serial.printf("Stream:   http://%s:81/stream\n", WiFi.localIP().toString().c_str());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  delay(1000);
}
