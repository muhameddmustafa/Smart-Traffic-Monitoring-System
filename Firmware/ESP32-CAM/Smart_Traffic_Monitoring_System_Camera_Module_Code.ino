// =====================================================
// @https://www.linkedin.com/in/muhameddmustafa/
// Smart-Traffic-Monitoring-System - ESP32-CAM VIOLATION CAMERA
// -----------------------------------------------------
// 1. Waits for a "violation" message from the main ESP32-S3 (ESP-NOW,
//    wireless, no pins or wires between the boards).
// 2. Takes a photo immediately.
// 3. Reads the QR code on the car (CAR1 - CAR4).
// 4. Sends the photo to the dashboard on the main ESP32-S3.
// 5. Sends the photo to the phones with the free "ntfy" app.
//
// Board: "AI Thinker ESP32-CAM"   Partition: "Huge APP"   PSRAM: Enabled
// Keep ALL files of this folder together (quirc*.c / .h = QR decoder).
// =====================================================
#include <Arduino.h>
#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "quirc.h"

// =====================================================
// 1) SETTINGS - CHANGE ONLY THIS PART - USE YOUR OWN CREDENTIALS 
// =====================================================
// Same hotspot as the main ESP32-S3 (copy the name exactly).
const char* WIFI_SSID =  // "Wi-Fi Name";
const char* WIFI_PASS =  // "Wi-Fi Password";

// ntfy topics = "channels" the phones subscribe to in the ntfy app.
// Anyone who knows a topic name can read it, so keep the random part.
const char* NTFY_SERVER  = "https://ntfy.sh";
// One car per team member: each person subscribes to their own topic.
const char* TOPIC_CAR1   = "smarttraffic-6agqzh-car1";    // QR "CAR1"
const char* TOPIC_CAR2   = "smarttraffic-6agqzh-car2";    // QR "CAR2"
const char* TOPIC_CAR3   = "smarttraffic-6agqzh-car3";    // QR "CAR3"
const char* TOPIC_CAR4   = "smarttraffic-6agqzh-car4";    // QR "CAR4"
const char* TOPIC_POLICE = "smarttraffic-6agqzh-police";  // gets EVERY violation

// Car owners (shown in the phone alert). - Use Your Own names.
const char* OWNER_CAR1 = "Muhamed Mustafa";
const char* OWNER_CAR2 = "Mohamed Saeed";
const char* OWNER_CAR3 = "Mohamed Taha";
const char* OWNER_CAR4 = "Tasnim Sabry";

// Camera
const framesize_t FRAME_SIZE = FRAMESIZE_SVGA;  // 800x600. Try FRAMESIZE_XGA if QR is far.
const int  JPEG_QUALITY = 12;                   // 10 = better, 20 = smaller
const bool FLIP_VERTICAL = false;               // picture upside down? -> true
const bool MIRROR = false;                      // picture mirrored?    -> true

// How long to keep looking for the QR code after the photo (ms).
const uint32_t QR_SEARCH_MS = 1500;

// FAST CARS: after the trigger, record frames for BURST_MS, then read the QR
// on them (sharpest first). SHUTTER: 0 = auto, 100-300 = less blur (needs a lamp).
const uint32_t BURST_MS = 500;
const uint32_t QR_BUDGET_MS = 8000;   // enough to try every burst frame
const int SHUTTER = 200;

// Cairo: UTC+2
const long GMT_OFFSET_SEC = 7200;

// =====================================================
// AI-THINKER ESP32-CAM PINS (do not change)
// =====================================================
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

#define FLASH_LED  4    // big white LED (kept OFF)
#define BUSY_LED  33    // small red LED on the back, LOW = ON

// =====================================================
// ESP-NOW MESSAGE - must match the main ESP32-S3 sketch
// =====================================================
#define TL_MAGIC 0x31564C54UL   // "TLV1"
enum : uint8_t { PKT_TRIGGER = 1, PKT_RESULT = 2, PKT_HEARTBEAT = 3 };

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  type;
  uint32_t id;          // violation ID
  char     lane;        // 'A' or 'B'
  uint32_t ip;          // trigger: main board IP / heartbeat: camera IP
  char     car[12];     // result: "CAR1", "CAR2" or "UNKNOWN"
  uint8_t  photoSaved;  // result: 1 = photo uploaded to the dashboard
  char     url[100];    // result: photo link on ntfy.sh (backup)
} TLPacket;

struct Trigger {
  TLPacket pkt;
  uint8_t mac[6];       // main board MAC (to answer it)
  uint32_t receivedAt;  // millis() when the message arrived
};

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t triggerQueue = nullptr;

// =====================================================
// BUFFERS (in PSRAM)
// =====================================================
const size_t PHOTO_MAX = 200000;   // main board accepts up to 200 KB
const size_t HEAD_ROOM = 256;      // space for the upload header

uint8_t* bodyBuf = nullptr;        // [header][photo][footer]
uint8_t* photo = nullptr;          // = bodyBuf + HEAD_ROOM
size_t photoLen = 0;

const int BURST_MAX = 10;
const size_t BURST_SLOT = 90000;
uint8_t* burstBuf[BURST_MAX];
size_t burstLen[BURST_MAX];
int burstCount = 0;

uint8_t* rgbBuf = nullptr;         // decoded picture for the QR reader
int imgW = 0, imgH = 0;
struct quirc* qr = nullptr;

WebServer web(80);

// Bigger stack for loop(): QR decoding + HTTPS uploads need more than the default 8 KB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

uint32_t lastTriggerID = 0;
uint32_t lastTriggerAt = 0;
uint32_t lastHeartbeat = 0;
uint32_t wifiLostAt = 0;

// =====================================================
// CAMERA
// =====================================================
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAME_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = 2;                       // always one fresh frame ready
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;     // newest frame = smallest delay

  esp_err_t err = esp_camera_init(&c);
  if(err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, FLIP_VERTICAL ? 1 : 0);
  s->set_hmirror(s, MIRROR ? 1 : 0);

  if(SHUTTER > 0) {
    s->set_exposure_ctrl(s, 0);         // fixed short exposure = less blur
    s->set_aec_value(s, SHUTTER);
    s->set_gain_ctrl(s, 1);
    s->set_gainceiling(s, GAINCEILING_16X);
  }

  // Let auto exposure settle and learn the real picture size.
  for(int i = 0; i < 8; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if(fb) {
      imgW = fb->width;
      imgH = fb->height;
      esp_camera_fb_return(fb);
    }
    delay(60);
  }

  Serial.printf("Camera ready: %dx%d\n", imgW, imgH);
  return imgW > 0;
}


// =====================================================
// QR CODE READER
// =====================================================
bool initQR() {
  rgbBuf = (uint8_t*)ps_malloc((size_t)imgW * imgH * 3);
  qr = quirc_new();

  if(!rgbBuf || !qr || quirc_resize(qr, imgW, imgH) < 0) {
    Serial.println("Not enough PSRAM for the QR reader. Use a smaller FRAME_SIZE.");
    return false;
  }
  return true;
}

// ---------- QR helpers ----------
Preferences prefs;
float modulePx = 0;     // size of one QR square in the picture (learned, saved)
String lastRawQR;       // last QR text that was not a car code

// Accepts CAR1..CAR4 (also "car 1", "CAR-1", "1" ...). Anything else = not a car.
bool normalizeCar(String& t) {
  t.trim();
  t.toUpperCase();
  t.replace(" ", "");
  t.replace("-", "");
  t.replace("_", "");
  if(t.length() == 1) t = "CAR" + t;
  return t == "CAR1" || t == "CAR2" || t == "CAR3" || t == "CAR4";
}

// Learns how big one QR square is in the picture (used by the deblur pass).
void learnModuleSize(const struct quirc_code& code) {
  float side = 0;
  for(int i = 0; i < 4; i++) {
    float dx = code.corners[(i + 1) % 4].x - code.corners[i].x;
    float dy = code.corners[(i + 1) % 4].y - code.corners[i].y;
    side += sqrtf(dx * dx + dy * dy);
  }
  float m = side / 4.0f / code.size;
  if(m < 1.5f || m > 40.0f) return;

  modulePx = (modulePx <= 0) ? m : modulePx * 0.7f + m * 0.3f;
  if(fabsf(modulePx - prefs.getFloat("mod", 0)) > 0.5f) prefs.putFloat("mod", modulePx);
}

// Decodes the codes quirc found. True when one of them is a car code.
bool decodeFound(String& text) {
  // ~13 KB together: keep them OUT of the task stack.
  static struct quirc_code code;
  static struct quirc_data data;

  int count = quirc_count(qr);
  for(int i = 0; i < count; i++) {
    quirc_extract(qr, i, &code);
    quirc_decode_error_t err = quirc_decode(&code, &data);

    if(err == QUIRC_ERROR_DATA_ECC) {      // mirrored code? try flipped
      quirc_flip(&code);
      err = quirc_decode(&code, &data);
    }

    if(err == QUIRC_SUCCESS && data.payload_len > 0) {
      String p = String((const char*)data.payload);
      if(normalizeCar(p)) {
        text = p;
        learnModuleSize(code);
        return true;
      }
      lastRawQR = p;
    }
  }
  return false;
}

// Sharpens with a spacing of d pixels (= one QR square). This undoes part of
// the motion blur: tested to read codes with ~20% more blur than before.
void sharpenInto(const uint8_t* src, uint8_t* dst, int w, int h, int d) {
  for(int y = 0; y < h; y++) {
    const uint8_t* row = src + y * w;
    const uint8_t* up  = src + (y - d < 0 ? 0 : y - d) * w;
    const uint8_t* dn  = src + (y + d >= h ? h - 1 : y + d) * w;
    uint8_t* o = dst + y * w;
    for(int x = 0; x < w; x++) {
      int c = row[x];
      int l = row[x - d < 0 ? 0 : x - d];
      int r = row[x + d >= w ? w - 1 : x + d];
      int v = c + (4 * c - l - r - up[x] - dn[x]) / 2;
      o[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
  }
}

// Returns true and fills "text" (CAR1..CAR4) when a car QR is read in this JPEG.
// Pass 1: normal picture. Pass 2: deblurred picture (only if pass 1 failed).
bool readQR(const uint8_t* jpg, size_t len, String& text) {
  if(!qr || !rgbBuf) return false;
  if(!fmt2rgb888(jpg, len, PIXFORMAT_JPEG, rgbBuf)) return false;

  int w, h;
  uint8_t* img = quirc_begin(qr, &w, &h);
  if(w != imgW || h != imgH) {
    quirc_end(qr);
    return false;
  }

  // RGB -> gray. A copy is kept at the start of rgbBuf for pass 2
  // (quirc changes its own image while searching).
  uint8_t* gray = rgbBuf;
  const uint8_t* p = rgbBuf;
  for(int i = 0; i < w * h; i++, p += 3) {
    uint8_t g = (uint8_t)((p[0] + 2 * p[1] + p[2]) >> 2);
    gray[i] = g;
    img[i] = g;
  }
  quirc_end(qr);
  if(decodeFound(text)) return true;

  // Pass 2: square size learned -> one try; not learned yet -> two sizes.
  int tries[2] = {5, 8};
  int nTries = 2;
  if(modulePx > 0) {
    tries[0] = (int)(modulePx + 0.5f);
    nTries = 1;
  }

  for(int k = 0; k < nTries; k++) {
    if(tries[k] < 2) continue;
    img = quirc_begin(qr, nullptr, nullptr);
    sharpenInto(gray, img, w, h, tries[k]);
    quirc_end(qr);
    if(decodeFound(text)) return true;
  }
  return false;
}

// =====================================================
// HELPERS
// =====================================================
void busyLed(bool on) {
  digitalWrite(BUSY_LED, on ? LOW : HIGH);
}

String nowText() {
  time_t t = time(nullptr);
  if(t < 1700000000) return "time not synced";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(buf);
}

int carNumber(const String& car) {
  if(car.length() == 4 && car.startsWith("CAR")) {
    int n = car.charAt(3) - '0';
    if(n >= 1 && n <= 4) return n;
  }
  return 0;
}

String carName(const String& car) {
  switch(carNumber(car)) {
    case 1: return String("Car 1 (") + OWNER_CAR1 + ")";
    case 2: return String("Car 2 (") + OWNER_CAR2 + ")";
    case 3: return String("Car 3 (") + OWNER_CAR3 + ")";
    case 4: return String("Car 4 (") + OWNER_CAR4 + ")";
  }
  return "Unknown car";
}

const char* carTopic(const String& car) {
  switch(carNumber(car)) {
    case 1: return TOPIC_CAR1;
    case 2: return TOPIC_CAR2;
    case 3: return TOPIC_CAR3;
    case 4: return TOPIC_CAR4;
  }
  return nullptr;
}

void addPeer(const uint8_t* mac) {
  if(esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;          // follow the hotspot's channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void sendResult(const Trigger& t, const String& car, bool saved, const String& url) {
  TLPacket r = {};
  r.magic = TL_MAGIC;
  r.type = PKT_RESULT;
  r.id = t.pkt.id;
  r.lane = t.pkt.lane;
  r.photoSaved = saved ? 1 : 0;
  strncpy(r.car, car.c_str(), sizeof(r.car) - 1);
  strncpy(r.url, url.c_str(), sizeof(r.url) - 1);

  addPeer(t.mac);
  esp_now_send(t.mac, (const uint8_t*)&r, sizeof(r));
}

// =====================================================
// SEND PHOTO TO THE DASHBOARD (main ESP32-S3)
// =====================================================
bool uploadToDashboard(uint32_t mainIP, uint32_t id) {
  if(mainIP == 0 || WiFi.status() != WL_CONNECTED || photoLen == 0) return false;

  const char* boundary = "----SmartTrafficCam7d1f";
  String head = String("--") + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"photo\"; filename=\"" +
                String(id) + ".jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = String("\r\n--") + boundary + "--\r\n";

  if(head.length() > HEAD_ROOM) return false;

  uint8_t* start = photo - head.length();
  memcpy(start, head.c_str(), head.length());
  memcpy(photo + photoLen, tail.c_str(), tail.length());
  size_t total = head.length() + photoLen + tail.length();

  String url = "http://" + IPAddress(mainIP).toString() + "/api/photo";

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(8000);
  http.begin(url);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);
  int code = http.POST(start, total);
  http.end();

  Serial.printf("Dashboard upload -> HTTP %d\n", code);
  return code == 200;
}

// =====================================================
// SEND PHOTO TO PHONES (ntfy app)
// =====================================================
// Uploads the photo to one topic. Returns the photo link from ntfy (or "").
String ntfyPhoto(const char* topic, const String& title, const String& msg,
                 const String& fileName) {
  WiFiClientSecure client;
  client.setInsecure();   // school project: skip certificate check

  HTTPClient https;
  https.setTimeout(15000);
  if(!https.begin(client, String(NTFY_SERVER) + "/" + topic)) return "";

  https.addHeader("Title", title);
  https.addHeader("Message", msg);
  https.addHeader("Filename", fileName);
  https.addHeader("Tags", "rotating_light,camera");
  https.addHeader("Priority", "high");

  int code = https.PUT(photo, photoLen);
  String resp = (code > 0) ? https.getString() : "";
  https.end();

  Serial.printf("ntfy photo -> %s : HTTP %d\n", topic, code);
  if(code != 200) return "";

  // Response contains: "attachment":{ ... "url":"https://ntfy.sh/file/xxxx.jpg" ...}
  int a = resp.indexOf("\"attachment\"");
  int u = resp.indexOf("\"url\":\"", a < 0 ? 0 : a);
  if(u < 0) return "";
  u += 7;
  int e = resp.indexOf('"', u);
  if(e < 0) return "";
  return resp.substring(u, e);
}

// Sends a notification that points to an already-uploaded photo (no re-upload).
bool ntfyLink(const char* topic, const String& title, const String& msg,
              const String& photoUrl) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(15000);
  if(!https.begin(client, String(NTFY_SERVER) + "/" + topic)) return false;

  https.addHeader("Title", title);
  https.addHeader("Message", msg);
  https.addHeader("Attach", photoUrl);
  https.addHeader("Tags", "rotating_light,camera");
  https.addHeader("Priority", "high");

  int code = https.POST("");
  https.end();

  Serial.printf("ntfy link -> %s : HTTP %d\n", topic, code);
  return code == 200;
}

// Car owner's phone + police phone. Returns the photo link.
String notifyPhones(uint32_t id, char lane, const String& car, bool isTest) {
  if(WiFi.status() != WL_CONNECTED) return "";

  String title = isTest ? String("TEST - camera works")
                        : String("RED LIGHT VIOLATION - ") + carName(car);
  String msg = carName(car) + " passed the RED light on Lane " +
               String(lane) + ". Violation #" + String(id) + " at " + nowText() + ".";
  String file = "violation_" + String(id) + "_" + car + ".jpg";

  const char* ownerTopic = isTest ? nullptr : carTopic(car);
  String url;

  if(ownerTopic) {
    url = ntfyPhoto(ownerTopic, title, msg, file);
    // Police gets the same photo by link (fast, no second upload).
    if(url.length() == 0 || !ntfyLink(TOPIC_POLICE, title, msg, url)) {
      String u2 = ntfyPhoto(TOPIC_POLICE, title, msg, file);
      if(url.length() == 0) url = u2;
    }
  } else {
    url = ntfyPhoto(TOPIC_POLICE, title, msg, file);
  }

  return url;
}

// =====================================================
// THE VIOLATION JOB
// =====================================================
void handleViolation(const Trigger& t, bool isTest) {
  busyLed(true);

  Serial.printf("\n=== VIOLATION #%lu  Lane %c ===\n",
                (unsigned long)t.pkt.id, t.pkt.lane);

  // ---- 1. BURST: record frames for BURST_MS right after the trigger ----
  int n = 0;
  uint32_t burstStart = millis();
  while(n < burstCount && millis() - burstStart < BURST_MS) {
    camera_fb_t* fb = esp_camera_fb_get();
    if(!fb) break;
    if(fb->len <= BURST_SLOT) {
      memcpy(burstBuf[n], fb->buf, fb->len);
      burstLen[n] = fb->len;
      n++;
    }
    esp_camera_fb_return(fb);
  }

  if(n == 0) {
    Serial.println("ERROR: camera frame not available");
    busyLed(false);
    return;
  }

  // ---- 2. QR CODE: sharpest frames first (a blurred JPEG is smaller) ----
  int order[BURST_MAX];
  for(int i = 0; i < n; i++) order[i] = i;
  for(int i = 1; i < n; i++) {
    int k = order[i];
    int j = i - 1;
    while(j >= 0 && burstLen[order[j]] < burstLen[k]) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }

  String car;
  bool found = false;
  int evidence = 0;          // default photo = first frame after the trigger
  int tried = 0;
  uint32_t qrStart = millis();

  for(int i = 0; i < n && !found && millis() - qrStart < QR_BUDGET_MS; i++) {
    tried++;
    if(readQR(burstBuf[order[i]], burstLen[order[i]], car)) {
      found = true;
      evidence = order[i];   // the plate is readable on this frame
    }
  }

  if(!found) car = "UNKNOWN";
  Serial.printf("Burst: %d frames, QR %s after %d tries (%lu ms)\n",
                n, car.c_str(), tried, (unsigned long)(millis() - qrStart));

  photoLen = min(burstLen[evidence], PHOTO_MAX);
  memcpy(photo, burstBuf[evidence], photoLen);

  if(!isTest) {
    // ---- 3. Car name to the dashboard right away ----
    sendResult(t, car, false, "");

    // ---- 4. Photo to the dashboard ----
    bool saved = uploadToDashboard(t.pkt.ip, t.pkt.id);

    // ---- 5. Photo to the phones ----
    String url = notifyPhones(t.pkt.id, t.pkt.lane, car, false);

    // ---- 6. Final result (photo link = backup if dashboard upload failed) ----
    sendResult(t, car, saved, url);
  } else {
    notifyPhones(0, 'T', car, true);
  }

  Serial.printf("Done in %lu ms\n", (unsigned long)(millis() - t.receivedAt));
  busyLed(false);
}

// =====================================================
// ESP-NOW
// =====================================================
void queueTrigger(const uint8_t* mac, const uint8_t* data, int len) {
  if(len != (int)sizeof(TLPacket)) return;

  Trigger t;
  memcpy(&t.pkt, data, sizeof(TLPacket));
  if(t.pkt.magic != TL_MAGIC || t.pkt.type != PKT_TRIGGER) return;

  memcpy(t.mac, mac, 6);
  t.receivedAt = millis();
  xQueueSend(triggerQueue, &t, 0);
}

void initEspNow() {
  triggerQueue = xQueueCreate(4, sizeof(Trigger));

  if(esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t* info,
                              const uint8_t* data, int len) {
    queueTrigger(info->src_addr, data, len);
  });
#else
  esp_now_register_recv_cb([](const uint8_t* mac, const uint8_t* data, int len) {
    queueTrigger(mac, data, len);
  });
#endif

  addPeer(BROADCAST_MAC);
  Serial.println("ESP-NOW ready, waiting for violations...");
}

void sendHeartbeat() {
  TLPacket h = {};
  h.magic = TL_MAGIC;
  h.type = PKT_HEARTBEAT;
  h.ip = (WiFi.status() == WL_CONNECTED) ? (uint32_t)WiFi.localIP() : 0;
  esp_now_send(BROADCAST_MAC, (const uint8_t*)&h, sizeof(h));
}

// =====================================================
// AIMING PAGE  (open http://<camera IP>/ on the phone)
// =====================================================
const char aimHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Violation Camera</title>
<style>
body{margin:0;background:#0b1120;color:#edf2fa;font-family:Arial;text-align:center}
img{width:100%;max-width:800px;border-radius:10px;margin-top:10px}
button{border:0;border-radius:8px;padding:12px 16px;margin:8px;font-weight:bold;
color:#fff;background:#2563eb;font-size:15px}
#r{font-size:22px;font-weight:bold;color:#facc15;min-height:30px}
</style></head><body>
<h2>VIOLATION CAMERA - AIMING</h2>
<img id="v" src="/jpg">
<div>
<button onclick="readQR()">READ QR NOW</button>
<button onclick="testPhones()">TEST PHONE ALERT</button>
</div>
<div id="r">Hold a QR card where the car stops, then press READ QR NOW</div>
<script>
const v=document.getElementById('v');
v.onload=()=>setTimeout(()=>v.src='/jpg?t='+Date.now(),300);
v.onerror=()=>setTimeout(()=>v.src='/jpg?t='+Date.now(),1000);
async function readQR(){document.getElementById('r').textContent='Reading...';
const r=await fetch('/qr');document.getElementById('r').textContent=await r.text();}
async function testPhones(){document.getElementById('r').textContent='Sending test...';
const r=await fetch('/test');document.getElementById('r').textContent=await r.text();}
</script></body></html>
)rawliteral";

void setupWeb() {
  web.on("/", HTTP_GET, []() {
    web.send_P(200, "text/html", aimHTML);
  });

  web.on("/jpg", HTTP_GET, []() {
    camera_fb_t* fb = esp_camera_fb_get();
    if(!fb) {
      web.send(503, "text/plain", "No frame");
      return;
    }
    web.sendHeader("Cache-Control", "no-store");
    web.setContentLength(fb->len);
    web.send(200, "image/jpeg", "");
    web.sendContent((const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
  });

  web.on("/qr", HTTP_GET, []() {
    uint32_t t0 = millis();
    String text;
    bool ok = false;
    // Try a few frames, like during a real violation.
    while(!ok && millis() - t0 < QR_SEARCH_MS) {
      camera_fb_t* fb = esp_camera_fb_get();
      if(!fb) break;
      ok = readQR(fb->buf, fb->len, text);
      esp_camera_fb_return(fb);
    }
    String answer;
    if(ok) {
      answer = "QR FOUND: " + text + " = " + carName(text) +
               " | QR square = " + String(modulePx, 1) + " px (6+ is best)";
    } else if(lastRawQR.length()) {
      answer = "QR read but it is not CAR1-CAR4: " + lastRawQR;
      lastRawQR = "";
    } else {
      answer = "No QR found - move closer / more light";
    }
    answer += "  (" + String(millis() - t0) + " ms)";
    web.send(200, "text/plain", answer);
  });

  web.on("/test", HTTP_GET, []() {
    Trigger t = {};
    t.receivedAt = millis();
    t.pkt.lane = 'T';
    handleViolation(t, true);
    web.send(200, "text/plain",
             "Test sent to the POLICE topic. Check the phone (ntfy app).");
  });

  web.begin();
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  // Prevents resets from short voltage dips (still use a good 5V supply).
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(300);
  Serial.println("\nSmart Traffic - ESP32-CAM violation camera");
  Serial.printf("Reset reason: %d (1 = power on, 4/5/6/7 = crash/watchdog)\n",
                (int)esp_reset_reason());

  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);
  pinMode(BUSY_LED, OUTPUT);
  busyLed(true);

  if(!psramFound()) {
    Serial.println("ERROR: PSRAM not found. Tools > PSRAM > Enabled");
  }

  bodyBuf = (uint8_t*)ps_malloc(HEAD_ROOM + PHOTO_MAX + 128);
  photo = bodyBuf ? bodyBuf + HEAD_ROOM : nullptr;

  if(!bodyBuf || !initCamera() || !initQR()) {
    Serial.println("Camera setup FAILED. Restarting in 5 s...");
    delay(5000);
    ESP.restart();
  }

  // QR square size learned earlier (used by the deblur pass).
  prefs.begin("cam", false);
  modulePx = prefs.getFloat("mod", 0);
  Serial.printf("QR square size: %.1f px (0 = not learned yet)\n", modulePx);

  for(int i = 0; i < BURST_MAX; i++) {
    burstBuf[i] = (uint8_t*)ps_malloc(BURST_SLOT);
    if(!burstBuf[i]) break;
    burstCount++;
  }
  Serial.printf("Burst buffer: %d frames\n", burstCount);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // must stay awake to hear the main board
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to hotspot");
  uint32_t t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if(WiFi.status() == WL_CONNECTED) {
    Serial.print("Camera IP: http://");
    Serial.println(WiFi.localIP());
    Serial.printf("Wi-Fi channel: %d\n", WiFi.channel());
  } else {
    Serial.println("Hotspot not found yet - will keep trying.");
  }

  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");

  initEspNow();
  setupWeb();

  Serial.printf("Free PSRAM: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
  Serial.println("Serial commands: t = test phone alert, q = read QR now");
  busyLed(false);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // A violation from the main ESP32-S3?
  Trigger t;
  if(xQueueReceive(triggerQueue, &t, pdMS_TO_TICKS(10)) == pdTRUE) {
    bool duplicate = (t.pkt.id == lastTriggerID) &&
                     (t.receivedAt - lastTriggerAt < 10000);
    if(!duplicate) {
      lastTriggerID = t.pkt.id;
      lastTriggerAt = t.receivedAt;
      handleViolation(t, false);
    }
  }

  web.handleClient();

  // "I'm alive" message so the dashboard can show CAMERA ONLINE.
  if(millis() - lastHeartbeat > 3000) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

  // Wi-Fi recovery
  if(WiFi.status() != WL_CONNECTED) {
    if(wifiLostAt == 0) wifiLostAt = millis();
    if(millis() - wifiLostAt > 20000) {
      Serial.println("Wi-Fi lost - reconnecting...");
      WiFi.reconnect();
      wifiLostAt = millis();
    }
  } else {
    wifiLostAt = 0;
  }

  // Serial test commands
  if(Serial.available()) {
    char c = Serial.read();
    if(c == 't') {
      Trigger test = {};
      test.receivedAt = millis();
      test.pkt.lane = 'T';
      handleViolation(test, true);
    } else if(c == 'q') {
      camera_fb_t* fb = esp_camera_fb_get();
      if(fb) {
        String text;
        uint32_t t0 = millis();
        bool ok = readQR(fb->buf, fb->len, text);
        esp_camera_fb_return(fb);
        Serial.printf("QR: %s (%lu ms)\n", ok ? text.c_str() : "not found",
                      (unsigned long)(millis() - t0));
      }
    }
  }
}
