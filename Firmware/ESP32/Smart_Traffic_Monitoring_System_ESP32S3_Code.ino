// =====================================================
// Smart-Traffic-Monitoring-System @https://www.linkedin.com/in/muhameddmustafa/
// ESP32-CAM (photo + QR plate)
// Main board: ESP32-S3. Talks to the ESP32-CAM wirelessly
// with ESP-NOW (no extra pins, no wires between boards).
// =====================================================
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <TM1637Display.h>
#include <time.h>
#include <esp_now.h>
#include <esp_wifi.h>

// =====================================================
// WIFI - USE YOUR OWN CREDENTIALS 
// =====================================================
const char* ssid =      // "Wi-Fi Name";
const char* password =  // "Wi-Fi Password";

// Cairo: UTC+2
const long GMT_OFFSET_SEC = 7200;
const int DAYLIGHT_OFFSET_SEC = 0;

// =====================================================
// TRAFFIC LIGHT PINS
// =====================================================
#define RED_A     20
#define YELLOW_A  47
#define GREEN_A   21

#define RED_B     4
#define YELLOW_B  5
#define GREEN_B   6

// =====================================================
// TM1637 DISPLAYS
// =====================================================
#define DIO_A 48
#define CLK_A 45

#define DIO_B 7
#define CLK_B 15

TM1637Display displayA(CLK_A, DIO_A);
TM1637Display displayB(CLK_B, DIO_B);

// =====================================================
// IR SENSORS
// =====================================================
// NOTE: GPIO20 is also the native USB D+ pin on ESP32-S3.
// If your board uses native USB for Serial/USB connection, GPIO20 may conflict.
// Keep this pin only if your board's USB interface does not use GPIO20.
#define IR_A 1
#define IR_B 16

// Sensor polarity is detected automatically at startup.
// Power on with BOTH IR sensors clear (no hand/object in front).

// =====================================================
// TIMING
// =====================================================
const uint32_t GREEN_TIME = 10000;
const uint32_t YELLOW_TIME = 3000;

const uint32_t DEBOUNCE_MS = 15;

// =====================================================
// SERVER AND STORAGE
// =====================================================
WebServer server(80);

const char* LOG_FILE = "/violations.txt";
const char* TEMP_FILE = "/temp.txt";

bool fsReady = false;
bool timeSynced = false;
bool wifiConnected = false;

// Wi-Fi recovery: retry the saved hotspot credentials without blocking traffic.
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
const uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
uint32_t wifiAttemptStarted = 0;
uint32_t nextWifiRetry = 0;
bool wifiAttemptInProgress = false;


// =====================================================
// TRAFFIC STATE MACHINE
// =====================================================
// 0 = A Green, B Red
// 1 = A Yellow, B Red
// 2 = A Red, B Green
// 3 = A Red, B Yellow

uint8_t currentPhase = 0;
uint32_t phaseStart = 0;
uint32_t phaseDuration = GREEN_TIME;

// =====================================================
// SENSOR DEBOUNCE
// =====================================================
struct SensorState {
  bool stableState;       // Last confirmed stable digital reading
  bool lastReading;       // Most recent raw reading
  bool activeState;       // Detected automatically from the idle reading
  uint32_t lastChange;    // Time the raw reading last changed
};

SensorState sensorA;
SensorState sensorB;

// =====================================================
// VIOLATION DATA
// File format:
// ID|LANE|UNIX_TIMESTAMP
// =====================================================
uint32_t nextViolationID = 1;

struct Violation {
  uint32_t id;
  char lane;
  time_t timestamp;
};

// =====================================================
// CAMERA LINK (ESP-NOW) - must match the ESP32-CAM sketch
// =====================================================
// Log line format is now:  ID|LANE|UNIX_TIMESTAMP|CAR|PHOTO_URL
// (old 3-field lines still work; CAR/URL are filled in by the camera)
const char* PHOTO_DIR = "/photos";
const size_t PHOTO_MAX_BYTES = 200000;      // reject anything bigger
const size_t PHOTO_MIN_FREE_BYTES = 220000;  // delete oldest photos below this

#define TL_MAGIC 0x31564C54UL   // "TLV1"
enum : uint8_t { PKT_TRIGGER = 1, PKT_RESULT = 2, PKT_HEARTBEAT = 3 };

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  type;
  uint32_t id;          // violation ID
  char     lane;        // 'A' or 'B'
  uint32_t ip;          // trigger: main board IP / heartbeat: camera IP
  char     car[12];     // result: "CAR1", "CAR2" or "UNKNOWN"
  uint8_t  photoSaved;  // result: 1 = photo uploaded to this dashboard
  char     url[100];    // result: photo link on ntfy.sh (backup)
} TLPacket;

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool espNowReady = false;

// Results arrive in the Wi-Fi task; they are queued and saved in loop().
QueueHandle_t camResultQueue = nullptr;
volatile uint32_t lastCameraSeen = 0;
volatile uint32_t cameraIP = 0;

bool updateViolationCar(uint32_t targetID, const String& car, const String& url);

// =====================================================
// DASHBOARD HTML
// =====================================================
const char dashboardHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Traffic System</title>

<style>
:root {
  --bg: #0b1120;
  --panel: #151f32;
  --panel2: #1c2940;
  --text: #edf2fa;
  --muted: #8fa0b8;
  --green: #22c55e;
  --red: #ef4444;
  --yellow: #facc15;
  --blue: #38bdf8;
  --border: #2a3850;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: Arial, sans-serif;
}

.container {
  max-width: 1200px;
  margin: auto;
  padding: 22px;
}

header {
  text-align: center;
  padding: 24px 12px;
  background: linear-gradient(135deg,#16243a,#101a2b);
  border: 1px solid var(--border);
  border-radius: 18px;
  margin-bottom: 20px;
}

h1 {
  margin: 0 0 14px;
  letter-spacing: 2px;
  font-size: clamp(23px,4vw,34px);
}

.designed {
  color: var(--muted);
  font-size: 11px;
  letter-spacing: 3px;
  margin-bottom: 12px;
}

.team {
  display: flex;
  justify-content: center;
  align-items: center;
  gap: 22px;
  flex-wrap: wrap;
}

.team span {
  font-size: 13px;
  font-weight: bold;
  color: #dce8f8;
}

.info {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
  margin-bottom: 20px;
}

.info-item {
  background: var(--panel);
  border: 1px solid var(--border);
  padding: 13px 18px;
  border-radius: 12px;
  flex: 1;
  min-width: 180px;
}

.label {
  color: var(--muted);
  font-size: 12px;
}

.value {
  font-weight: bold;
  margin-top: 7px;
}

.lanes {
  display: grid;
  grid-template-columns: repeat(2,minmax(0,1fr));
  gap: 18px;
}

.card {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 16px;
  padding: 20px;
}

.card h2 {
  margin: 0 0 16px;
  font-size: 19px;
}

.lights {
  display: flex;
  gap: 12px;
  justify-content: center;
  margin: 20px 0;
}

.light {
  width: 40px;
  height: 40px;
  border-radius: 50%;
  background: #293449;
  border: 3px solid #3b475c;
}

.light.red.on {
  background: var(--red);
  box-shadow: 0 0 18px var(--red);
}

.light.yellow.on {
  background: var(--yellow);
  box-shadow: 0 0 18px var(--yellow);
}

.light.green.on {
  background: var(--green);
  box-shadow: 0 0 18px var(--green);
}

.status {
  text-align: center;
  font-weight: bold;
  font-size: 13px;
  color: var(--muted);
}

.countdown {
  text-align: center;
  font-size: 55px;
  font-weight: bold;
  margin: 12px 0;
  color: var(--blue);
  font-variant-numeric: tabular-nums;
}

.unit {
  text-align: center;
  color: var(--muted);
  font-size: 12px;
}

.stats {
  display: grid;
  grid-template-columns: repeat(2,minmax(0,1fr));
  gap: 18px;
  margin-top: 20px;
}

.number {
  font-size: 38px;
  font-weight: bold;
  margin-top: 10px;
}

.section {
  margin-top: 20px;
}

.section-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  flex-wrap: wrap;
  margin-bottom: 15px;
}

button {
  border: 0;
  border-radius: 8px;
  padding: 10px 15px;
  font-weight: bold;
  cursor: pointer;
  color: white;
}

.delete {
  background: #b91c1c;
  padding: 7px 10px;
}

.clear {
  background: #dc2626;
}

.table-wrap {
  overflow-x: auto;
}

table {
  width: 100%;
  border-collapse: collapse;
  min-width: 530px;
}

th,td {
  padding: 13px 10px;
  border-bottom: 1px solid var(--border);
  text-align: left;
  font-size: 13px;
}

th {
  color: var(--muted);
}

.laneA {
  color: #38bdf8;
  font-weight: bold;
}

.laneB {
  color: #c084fc;
  font-weight: bold;
}

.empty {
  text-align: center;
  color: var(--muted);
  padding: 25px;
}

footer {
  text-align: center;
  color: var(--muted);
  padding: 24px;
  font-size: 12px;
}

.photo-btn {
  background: #2563eb;
  padding: 7px 10px;
}

.car {
  font-weight: bold;
  color: var(--yellow);
}

.capture {
  display: grid;
  grid-template-columns: minmax(0,2fr) minmax(0,1fr);
  gap: 18px;
  align-items: center;
}

.capture img {
  width: 100%;
  border-radius: 12px;
  border: 1px solid var(--border);
  background: #000;
  cursor: zoom-in;
}

.capture .none {
  aspect-ratio: 4 / 3;
  display: flex;
  align-items: center;
  justify-content: center;
  border: 1px dashed var(--border);
  border-radius: 12px;
  color: var(--muted);
}

.big {
  font-size: 30px;
  font-weight: bold;
  margin: 6px 0 14px;
}

.cam a {
  color: var(--blue);
}

#viewer {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,.88);
  display: none;
  align-items: center;
  justify-content: center;
  flex-direction: column;
  gap: 12px;
  z-index: 10;
  padding: 16px;
}

#viewer img {
  max-width: 100%;
  max-height: 82vh;
  border-radius: 10px;
}

@media(max-width:650px) {
  .capture {
    grid-template-columns: 1fr;
  }
}

@media(max-width:650px) {
  .lanes, .stats {
    grid-template-columns: 1fr;
  }

  .container {
    padding: 12px;
  }

  .team {
    gap: 12px;
  }
}
</style>
</head>

<body>
<div class="container">

<header>
  <h1>SMART TRAFFIC SYSTEM</h1>
  <div class="designed">DESIGNED BY</div>
  <div class="team">
    <span>Muhamed Mustafa</span>
    <span>Mohamed Saeed</span>
    <span>Mohamed Taha</span>
    <span>Tasnim Sabry</span>
  </div>
</header>

<div class="info">
  <div class="info-item">
    <div class="label">SYSTEM CONNECTION</div>
    <div class="value" id="wifi">CONNECTING...</div>
  </div>

  <div class="info-item">
    <div class="label">ESP32 IP ADDRESS</div>
    <div class="value" id="ip">--</div>
  </div>

  <div class="info-item">
    <div class="label">DATE & TIME</div>
    <div class="value" id="datetime">--</div>
  </div>

  <div class="info-item cam">
    <div class="label">VIOLATION CAMERA</div>
    <div class="value" id="cam">--</div>
  </div>
</div>

<div class="lanes">

  <!-- Lane B is first so it appears on the LEFT on desktop -->
  <div class="card">
    <h2>LANE B</h2>
    <div class="lights">
      <div class="light red" id="br"></div>
      <div class="light yellow" id="by"></div>
      <div class="light green" id="bg"></div>
    </div>
    <div class="status" id="bstatus">--</div>
    <div class="countdown" id="btime">--</div>
    <div class="unit">SECONDS REMAINING</div>
  </div>

  <!-- Lane A is second so it appears on the RIGHT on desktop -->
  <div class="card">
    <h2>LANE A</h2>
    <div class="lights">
      <div class="light red" id="ar"></div>
      <div class="light yellow" id="ay"></div>
      <div class="light green" id="ag"></div>
    </div>
    <div class="status" id="astatus">--</div>
    <div class="countdown" id="atime">--</div>
    <div class="unit">SECONDS REMAINING</div>
  </div>

</div>

<div class="stats">
  <!-- Lane B left -->
  <div class="card">
    <div class="label">LANE B VIOLATIONS</div>
    <div class="number" id="countB">0</div>
  </div>

  <!-- Lane A right -->
  <div class="card">
    <div class="label">LANE A VIOLATIONS</div>
    <div class="number" id="countA">0</div>
  </div>
</div>

<div class="card section">
  <div class="section-header">
    <h2>LATEST CAPTURE</h2>
  </div>
  <div class="capture">
    <div id="lastPhoto"><div class="none">No photo yet</div></div>
    <div>
      <div class="label">CAR (QR CODE)</div>
      <div class="big" id="lastCar">--</div>
      <div class="label">LANE</div>
      <div class="big" id="lastLane">--</div>
      <div class="label">VIOLATION / TIME</div>
      <div class="value" id="lastInfo">--</div>
    </div>
  </div>
</div>

<div class="card section">
  <div class="section-header">
    <h2>VIOLATION RECORDS</h2>
    <button class="clear" onclick="clearAll()">CLEAR ALL</button>
  </div>

  <div class="table-wrap">
    <table>
      <thead>
        <tr>
          <th>ID</th>
          <th>LANE</th>
          <th>CAR</th>
          <th>DATE</th>
          <th>TIME</th>
          <th>PHOTO</th>
          <th>ACTION</th>
        </tr>
      </thead>
      <tbody id="logs">
        <tr><td colspan="7" class="empty">Loading records...</td></tr>
      </tbody>
    </table>
  </div>
</div>

<div id="viewer" onclick="this.style.display='none'">
  <img id="viewerImg" alt="Violation photo">
  <div id="viewerText" class="status"></div>
</div>

<footer>
  SMART TRAFFIC SYSTEM | ESP32-S3
</footer>

</div>

<script>
let previousLogs = '';

function photoSrc(v) {
  if(v.photo) return '/photo?id=' + v.id + '&k=' + encodeURIComponent(v.car || '');
  if(v.url) return v.url;
  return '';
}

function showPhoto(v) {
  const src = photoSrc(v);
  if(!src) return;
  document.getElementById('viewerImg').src = src;
  document.getElementById('viewerText').textContent =
    '#' + v.id + '  |  LANE ' + v.lane + '  |  ' + (v.car || 'NOT READ') +
    '  |  ' + v.date + ' ' + v.time + '  (tap to close)';
  document.getElementById('viewer').style.display = 'flex';
}

function updateLatest(logs) {
  const box = document.getElementById('lastPhoto');
  if(logs.length === 0) {
    box.innerHTML = '<div class="none">No photo yet</div>';
    document.getElementById('lastCar').textContent = '--';
    document.getElementById('lastLane').textContent = '--';
    document.getElementById('lastInfo').textContent = '--';
    return;
  }

  const v = logs[logs.length - 1];
  const src = photoSrc(v);
  box.innerHTML = '';

  if(src) {
    const img = document.createElement('img');
    img.src = src;
    img.alt = 'Violation photo';
    img.onclick = () => showPhoto(v);
    box.appendChild(img);
  } else {
    const none = document.createElement('div');
    none.className = 'none';
    none.textContent = 'Waiting for camera photo...';
    box.appendChild(none);
  }

  document.getElementById('lastCar').textContent = v.car || 'WAITING...';
  document.getElementById('lastLane').textContent = 'LANE ' + v.lane;
  document.getElementById('lastInfo').textContent =
    '#' + v.id + '  ' + v.date + ' ' + v.time;
}

async function updateStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();

    const cam = document.getElementById('cam');
    if(d.camOnline) {
      cam.innerHTML = 'ONLINE - <a target="_blank" href="http://' +
        d.camIP + '/">' + d.camIP + '</a>';
    } else {
      cam.textContent = 'OFFLINE';
    }

    document.getElementById('wifi').textContent =
      d.wifi ? 'CONNECTED' : 'DISCONNECTED';

    document.getElementById('ip').textContent = d.ip;
    document.getElementById('datetime').textContent = d.datetime;

    document.getElementById('atime').textContent = d.timeA;
    document.getElementById('btime').textContent = d.timeB;

    document.getElementById('astatus').textContent = d.statusA;
    document.getElementById('bstatus').textContent = d.statusB;

    ['ar','ay','ag','br','by','bg'].forEach(id => {
      document.getElementById(id).classList.remove('on');
    });

    if(d.redA) document.getElementById('ar').classList.add('on');
    if(d.yellowA) document.getElementById('ay').classList.add('on');
    if(d.greenA) document.getElementById('ag').classList.add('on');

    if(d.redB) document.getElementById('br').classList.add('on');
    if(d.yellowB) document.getElementById('by').classList.add('on');
    if(d.greenB) document.getElementById('bg').classList.add('on');

  } catch(e) {
    document.getElementById('wifi').textContent = 'SERVER OFFLINE';
  }
}

async function updateLogs() {
  try {
    const r = await fetch('/api/logs');
    const d = await r.json();

    document.getElementById('countA').textContent = d.countA;
    document.getElementById('countB').textContent = d.countB;

    // Redraw when anything changes (new record, car read, photo arrived).
    const signature = JSON.stringify(d.logs);

    if(previousLogs !== signature) {
      previousLogs = signature;

      updateLatest(d.logs);

      const tbody = document.getElementById('logs');
      tbody.innerHTML = '';

      if(d.logs.length === 0) {
        tbody.innerHTML =
          '<tr><td colspan="7" class="empty">No violations recorded</td></tr>';
        return;
      }

      d.logs.slice().reverse().forEach(v => {
        const tr = document.createElement('tr');

        const values = [
          '#' + v.id,
          'LANE ' + v.lane,
          v.car || '...',
          v.date,
          v.time
        ];

        values.forEach((value, index) => {
          const td = document.createElement('td');
          td.textContent = value;

          if(index === 1) {
            td.className = v.lane === 'A' ? 'laneA' : 'laneB';
          }
          if(index === 2) {
            td.className = 'car';
          }

          tr.appendChild(td);
        });

        const photoCell = document.createElement('td');
        if(photoSrc(v)) {
          const pbtn = document.createElement('button');
          pbtn.className = 'photo-btn';
          pbtn.textContent = 'VIEW';
          pbtn.onclick = () => showPhoto(v);
          photoCell.appendChild(pbtn);
        } else {
          photoCell.textContent = '--';
        }
        tr.appendChild(photoCell);

        const action = document.createElement('td');
        const btn = document.createElement('button');
        btn.className = 'delete';
        btn.textContent = 'DELETE';
        btn.onclick = () => deleteLog(v.id);

        action.appendChild(btn);
        tr.appendChild(action);
        tbody.appendChild(tr);
      });
    }
  } catch(e) {
    console.error(e);
  }
}

async function deleteLog(id) {
  if(!confirm('Delete violation #' + id + '?')) return;

  const r = await fetch('/api/delete?id=' + id, {
    method: 'POST'
  });

  if(r.ok) {
    previousLogs = '';
    updateLogs();
  } else {
    alert('Delete failed');
  }
}

async function clearAll() {
  if(!confirm('Are you sure you want to delete ALL violation records?')) return;
  if(!confirm('This action cannot be undone. Continue?')) return;

  const r = await fetch('/api/clear', {
    method: 'POST'
  });

  if(r.ok) {
    previousLogs = '';
    updateLogs();
  } else {
    alert('Clear failed');
  }
}

updateStatus();
updateLogs();

setInterval(updateStatus, 500);
setInterval(updateLogs, 2000);
</script>

</body>
</html>
)rawliteral";

// =====================================================
// HELPERS
// =====================================================
String jsonEscape(const String& input) {
  String output;

  for(size_t i = 0; i < input.length(); i++) {
    char c = input[i];

    if(c == '"' || c == '\\') {
      output += '\\';
      output += c;
    } else if(c == '\n') {
      output += "\\n";
    } else if(c == '\r') {
      output += "\\r";
    } else {
      output += c;
    }
  }

  return output;
}

String getDateTime(time_t timestamp) {
  if(timestamp < 1700000000) return "NOT SYNCED";

  struct tm t;
  localtime_r(&timestamp, &t);

  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &t);

  return String(buffer);
}

bool clockIsValid() {
  return time(nullptr) > 1700000000;
}

// =====================================================
// TRAFFIC LIGHT CONTROL
// =====================================================
void setLights(
  bool ra, bool ya, bool ga,
  bool rb, bool yb, bool gb
) {
  digitalWrite(RED_A, ra);
  digitalWrite(YELLOW_A, ya);
  digitalWrite(GREEN_A, ga);

  digitalWrite(RED_B, rb);
  digitalWrite(YELLOW_B, yb);
  digitalWrite(GREEN_B, gb);
}

void applyPhase() {
  switch(currentPhase) {

    case 0:
      setLights(LOW, LOW, HIGH,
                HIGH, LOW, LOW);
      phaseDuration = GREEN_TIME;
      break;

    case 1:
      setLights(LOW, HIGH, LOW,
                HIGH, LOW, LOW);
      phaseDuration = YELLOW_TIME;
      break;

    case 2:
      setLights(HIGH, LOW, LOW,
                LOW, LOW, HIGH);
      phaseDuration = GREEN_TIME;
      break;

    case 3:
      setLights(HIGH, LOW, LOW,
                LOW, HIGH, LOW);
      phaseDuration = YELLOW_TIME;
      break;
  }

  phaseStart = millis();
}

void updateTraffic() {
  uint32_t elapsed = millis() - phaseStart;

  if(elapsed >= phaseDuration) {
    currentPhase = (currentPhase + 1) % 4;
    applyPhase();
  }
}

uint8_t getRemaining(uint32_t duration, uint32_t elapsed) {
  if(elapsed >= duration) return 0;

  uint32_t remaining = duration - elapsed;
  return (remaining + 999) / 1000;
}

void updateDisplays() {
  uint32_t elapsed = millis() - phaseStart;

  uint8_t timeA = 0;
  uint8_t timeB = 0;

  switch(currentPhase) {

    case 0:
      timeA = getRemaining(GREEN_TIME, elapsed);
      timeB = getRemaining(GREEN_TIME + YELLOW_TIME, elapsed);
      break;

    case 1:
      timeA = getRemaining(YELLOW_TIME, elapsed);
      timeB = getRemaining(YELLOW_TIME, elapsed);
      break;

    case 2:
      timeA = getRemaining(GREEN_TIME + YELLOW_TIME, elapsed);
      timeB = getRemaining(GREEN_TIME, elapsed);
      break;

    case 3:
      timeA = getRemaining(YELLOW_TIME, elapsed);
      timeB = getRemaining(YELLOW_TIME, elapsed);
      break;
  }

  displayA.showNumberDec(timeA, true);
  displayB.showNumberDec(timeB, true);
}

// =====================================================
// PHOTO STORAGE  (LittleFS: /photos/<violation id>.jpg)
// =====================================================
const char* PHOTO_TEMP = "/photo_upload.tmp";

File uploadFile;
uint32_t uploadID = 0;
size_t uploadSize = 0;
bool uploadOK = false;

String photoPath(uint32_t id) {
  return String(PHOTO_DIR) + "/" + String(id) + ".jpg";
}

uint32_t idFromPath(const char* path) {
  String p = String(path);
  int slash = p.lastIndexOf('/');
  return p.substring(slash + 1).toInt();   // "12.jpg" -> 12
}

void removePhoto(uint32_t id) {
  if(!fsReady) return;
  String p = photoPath(id);
  if(LittleFS.exists(p)) LittleFS.remove(p);
}

void removeAllPhotos() {
  if(!fsReady) return;

  File dir = LittleFS.open(PHOTO_DIR);
  if(!dir || !dir.isDirectory()) return;

  // Collect the names first, then delete them.
  String names[64];
  int count = 0;

  File f = dir.openNextFile();
  while(f && count < 64) {
    names[count++] = String(PHOTO_DIR) + "/" + String(idFromPath(f.path())) + ".jpg";
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  for(int i = 0; i < count; i++) LittleFS.remove(names[i]);

  // If there were more than 64 files, run again.
  if(count == 64) removeAllPhotos();
}

// Deletes the oldest photos until there is room for a new one.
void makeRoomForPhoto() {
  for(int guard = 0; guard < 64; guard++) {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if(freeBytes >= PHOTO_MIN_FREE_BYTES) return;

    File dir = LittleFS.open(PHOTO_DIR);
    if(!dir || !dir.isDirectory()) return;

    uint32_t oldest = UINT32_MAX;
    File f = dir.openNextFile();
    while(f) {
      uint32_t id = idFromPath(f.path());
      if(id > 0 && id < oldest) oldest = id;
      f.close();
      f = dir.openNextFile();
    }
    dir.close();

    if(oldest == UINT32_MAX) return;   // nothing left to delete

    Serial.printf("Storage almost full: deleting old photo #%lu\n",
                  (unsigned long)oldest);
    removePhoto(oldest);
  }
}

// Receives the photo from the ESP32-CAM (multipart upload, file "<id>.jpg").
// Written straight to flash in small chunks, so it needs almost no RAM.
void handlePhotoUpload() {
  HTTPUpload& up = server.upload();

  if(up.status == UPLOAD_FILE_START) {
    uploadOK = false;
    uploadSize = 0;
    uploadID = up.filename.toInt();

    if(!fsReady || uploadID == 0) return;

    if(!LittleFS.exists(PHOTO_DIR)) LittleFS.mkdir(PHOTO_DIR);
    makeRoomForPhoto();

    uploadFile = LittleFS.open(PHOTO_TEMP, "w");
    uploadOK = (bool)uploadFile;

  } else if(up.status == UPLOAD_FILE_WRITE) {
    if(!uploadOK) return;

    uploadSize += up.currentSize;

    if(uploadSize > PHOTO_MAX_BYTES ||
       uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
      uploadOK = false;
      uploadFile.close();
      LittleFS.remove(PHOTO_TEMP);
    }

  } else if(up.status == UPLOAD_FILE_END) {
    if(!uploadOK) return;

    uploadFile.close();

    String finalPath = photoPath(uploadID);
    if(LittleFS.exists(finalPath)) LittleFS.remove(finalPath);
    uploadOK = LittleFS.rename(PHOTO_TEMP, finalPath);

  } else if(up.status == UPLOAD_FILE_ABORTED) {
    if(uploadOK) uploadFile.close();
    LittleFS.remove(PHOTO_TEMP);
    uploadOK = false;
  }
}

void handlePhotoUploadDone() {
  if(uploadOK) {
    Serial.printf("CAMERA: photo for violation #%lu saved (%u bytes)\n",
                  (unsigned long)uploadID, (unsigned)uploadSize);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(500, "text/plain", "Photo not saved");
  }
}

void handlePhoto() {
  if(!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing ID");
    return;
  }

  String p = photoPath(server.arg("id").toInt());

  if(!fsReady || !LittleFS.exists(p)) {
    server.send(404, "text/plain", "No photo");
    return;
  }

  File f = LittleFS.open(p, "r");
  server.sendHeader("Cache-Control", "no-cache");
  server.streamFile(f, "image/jpeg");
  f.close();
}

// =====================================================
// CAMERA LINK (ESP-NOW)
// =====================================================
void sendCameraTrigger(uint32_t id, char lane) {
  if(!espNowReady) return;

  TLPacket p = {};
  p.magic = TL_MAGIC;
  p.type = PKT_TRIGGER;
  p.id = id;
  p.lane = lane;
  p.ip = WiFi.isConnected() ? (uint32_t)WiFi.localIP() : 0;

  // Broadcast has no delivery receipt, so send it 3 times.
  // The camera ignores the copies (same ID).
  for(int i = 0; i < 3; i++) {
    esp_now_send(BROADCAST_MAC, (const uint8_t*)&p, sizeof(p));
  }

  Serial.printf("CAMERA: trigger sent for violation #%lu (Lane %c)\n",
                (unsigned long)id, lane);
}

void initEspNow() {
  camResultQueue = xQueueCreate(6, sizeof(TLPacket));

  if(esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed. Camera will not be triggered.");
    return;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t* info,
                              const uint8_t* data, int len) {
#else
  esp_now_register_recv_cb([](const uint8_t* mac,
                              const uint8_t* data, int len) {
#endif
    if(len != (int)sizeof(TLPacket)) return;

    TLPacket p;
    memcpy(&p, data, sizeof(p));
    if(p.magic != TL_MAGIC) return;

    uint32_t now = millis();
    lastCameraSeen = now ? now : 1;

    if(p.type == PKT_HEARTBEAT && p.ip != 0) {
      cameraIP = p.ip;
    } else if(p.type == PKT_RESULT && camResultQueue) {
      xQueueSend(camResultQueue, &p, 0);   // saved later in loop()
    }
  });

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;            // 0 = follow the hotspot's channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  if(esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW: could not add broadcast peer.");
    return;
  }

  espNowReady = true;
  Serial.println("ESP-NOW ready (camera link).");
}

String cleanText(const char* in, size_t maxLen, bool isUrl) {
  String out;
  for(size_t i = 0; i < maxLen && in[i]; i++) {
    char c = in[i];
    bool ok = isalnum((unsigned char)c) || c == '_' || c == '-';
    if(isUrl) ok = ok || c == ':' || c == '/' || c == '.' || c == '?' ||
                   c == '=' || c == '&' || c == '%' || c == '~';
    if(ok) out += c;
  }
  if(isUrl && !out.startsWith("https://")) out = "";
  return out;
}

// Saves car names / photo links reported by the camera.
void processCameraResults() {
  if(!camResultQueue) return;

  TLPacket p;
  while(xQueueReceive(camResultQueue, &p, 0) == pdTRUE) {
    String car = cleanText(p.car, sizeof(p.car), false);
    String url = cleanText(p.url, sizeof(p.url), true);

    Serial.printf("CAMERA RESULT: violation #%lu -> %s%s\n",
                  (unsigned long)p.id,
                  car.length() ? car.c_str() : "?",
                  p.photoSaved ? " (photo on dashboard)" : "");

    updateViolationCar(p.id, car, url);
  }
}

bool cameraOnline() {
  uint32_t seen = lastCameraSeen;
  return seen != 0 && (uint32_t)(millis() - seen) < 10000;
}

// =====================================================
// VIOLATION STORAGE
// =====================================================
void initializeStorage() {
  if(!fsReady) return;

  if(!LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "w");
    if(f) f.close();
  }

  nextViolationID = 1;

  File f = LittleFS.open(LOG_FILE, "r");
  if(!f) return;

  while(f.available()) {
    String line = f.readStringUntil('\n');
    uint32_t id = line.substring(0, line.indexOf('|')).toInt();

    if(id >= nextViolationID) {
      nextViolationID = id + 1;
    }
  }

  f.close();
}

bool addViolation(char lane) {
  if(!fsReady) return false;

  File f = LittleFS.open(LOG_FILE, "a");
  if(!f) return false;

  uint32_t id = nextViolationID++;
  time_t timestamp = clockIsValid() ? time(nullptr) : 0;

  // Fire the camera FIRST so the photo is taken as fast as possible.
  sendCameraTrigger(id, lane);

  f.printf("%lu|%c|%lld\n",
           (unsigned long)id,
           lane,
           (long long)timestamp);

  f.close();

  Serial.printf("VIOLATION: Lane %c, ID %lu\n",
                lane, (unsigned long)id);

  return true;
}

bool deleteViolation(uint32_t targetID) {
  if(!fsReady) return false;

  File source = LittleFS.open(LOG_FILE, "r");
  if(!source) return false;

  File temp = LittleFS.open(TEMP_FILE, "w");
  if(!temp) {
    source.close();
    return false;
  }

  bool found = false;

  while(source.available()) {
    String line = source.readStringUntil('\n');
    if(line.length() == 0) continue;

    int separator = line.indexOf('|');
    uint32_t id = line.substring(0, separator).toInt();

    if(id == targetID) {
      found = true;
      continue;
    }

    temp.println(line);
  }

  source.close();
  temp.close();

  if(found) {
    LittleFS.remove(LOG_FILE);
    LittleFS.rename(TEMP_FILE, LOG_FILE);
    removePhoto(targetID);
  } else {
    LittleFS.remove(TEMP_FILE);
  }

  return found;
}

// Adds/updates the CAR and PHOTO_URL fields of one record.
// Only rewrites the file when something really changed.
bool updateViolationCar(uint32_t targetID, const String& car, const String& url) {
  if(!fsReady) return false;

  File source = LittleFS.open(LOG_FILE, "r");
  if(!source) return false;

  File temp = LittleFS.open(TEMP_FILE, "w");
  if(!temp) {
    source.close();
    return false;
  }

  bool changed = false;

  while(source.available()) {
    String line = source.readStringUntil('\n');
    line.trim();
    if(line.length() == 0) continue;

    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    uint32_t id = line.substring(0, p1).toInt();

    if(id == targetID && p1 > 0 && p2 > 0) {
      int p3 = line.indexOf('|', p2 + 1);
      int p4 = (p3 >= 0) ? line.indexOf('|', p3 + 1) : -1;

      String head = (p3 >= 0) ? line.substring(0, p3) : line;   // ID|LANE|TS
      String oldCar = (p3 >= 0) ? line.substring(p3 + 1, p4 >= 0 ? p4 : line.length()) : "";
      String oldUrl = (p4 >= 0) ? line.substring(p4 + 1) : "";

      String newCar = car.length() ? car : oldCar;
      String newUrl = url.length() ? url : oldUrl;

      String newLine = head + "|" + newCar + "|" + newUrl;
      if(newLine != line) changed = true;
      line = newLine;
    }

    temp.println(line);
  }

  source.close();
  temp.close();

  if(changed) {
    LittleFS.remove(LOG_FILE);
    LittleFS.rename(TEMP_FILE, LOG_FILE);
  } else {
    LittleFS.remove(TEMP_FILE);
  }

  return changed;
}

bool clearViolations() {
  if(!fsReady) return false;

  File f = LittleFS.open(LOG_FILE, "w");
  if(!f) return false;

  f.close();
  removeAllPhotos();

  // Reset the next violation ID only after the log file was cleared.
  // The next new violation will be assigned ID #1.
  nextViolationID = 1;
  Serial.println("All violation records cleared. Next ID reset to #1.");

  return true;
}

// =====================================================
// SENSOR HANDLING
// =====================================================
void initializeSensor(SensorState &sensor, int pin, char lane) {
  // IMPORTANT: Keep the sensor clear during startup.
  // The idle level is sampled, then the opposite level is treated as detection.
  bool reading = digitalRead(pin);

  sensor.stableState = reading;
  sensor.lastReading = reading;
  sensor.activeState = !reading;
  sensor.lastChange = millis();

  Serial.printf(
    "IR %c initialized: idle=%s, detection=%s\n",
    lane,
    reading ? "HIGH" : "LOW",
    sensor.activeState ? "HIGH" : "LOW"
  );
}

void updateSensor(
  SensorState &sensor,
  int pin,
  char lane,
  bool laneIsRed
) {
  const uint32_t now = millis();
  const bool reading = digitalRead(pin);

  // Restart the debounce timer whenever the raw signal changes.
  if(reading != sensor.lastReading) {
    sensor.lastReading = reading;
    sensor.lastChange = now;
  }

  // Accept the new state only after it remains stable for DEBOUNCE_MS.
  if((uint32_t)(now - sensor.lastChange) >= DEBOUNCE_MS &&
     sensor.lastReading != sensor.stableState) {

    sensor.stableState = sensor.lastReading;

    // Register only the transition into the detected/active state.
    // Holding a finger/object in front of the sensor cannot create
    // repeated records; the sensor must return to idle first.
    if(sensor.stableState == sensor.activeState) {
      Serial.printf(
        "IR %c DETECTED | Lane is %s\n",
        lane,
        laneIsRed ? "RED" : "NOT RED"
      );

      if(laneIsRed) {
        if(!addViolation(lane)) {
          Serial.printf(
            "ERROR: Could not save Lane %c violation. Check LittleFS.\n",
            lane
          );
        }
      } else {
        Serial.printf(
          "Lane %c detection ignored because its light is not RED.\n",
          lane
        );
      }
    }
  }
}

// =====================================================
// JSON API
// =====================================================
void handleStatus() {
  uint32_t elapsed = millis() - phaseStart;

  uint8_t a = 0;
  uint8_t b = 0;

  switch(currentPhase) {
    case 0:
      a = getRemaining(GREEN_TIME, elapsed);
      b = getRemaining(GREEN_TIME + YELLOW_TIME, elapsed);
      break;

    case 1:
      a = getRemaining(YELLOW_TIME, elapsed);
      b = getRemaining(YELLOW_TIME, elapsed);
      break;

    case 2:
      a = getRemaining(GREEN_TIME + YELLOW_TIME, elapsed);
      b = getRemaining(GREEN_TIME, elapsed);
      break;

    case 3:
      a = getRemaining(YELLOW_TIME, elapsed);
      b = getRemaining(YELLOW_TIME, elapsed);
      break;
  }

  bool redA = (currentPhase == 2 || currentPhase == 3);
  bool yellowA = (currentPhase == 1);
  bool greenA = (currentPhase == 0);

  bool redB = (currentPhase == 0 || currentPhase == 1);
  bool yellowB = (currentPhase == 3);
  bool greenB = (currentPhase == 2);

  String statusA = greenA ? "GREEN" : yellowA ? "YELLOW" : "RED";
  String statusB = greenB ? "GREEN" : yellowB ? "YELLOW" : "RED";

  String ip = WiFi.isConnected()
    ? WiFi.localIP().toString()
    : "Not connected";

  String datetime = clockIsValid()
    ? getDateTime(time(nullptr))
    : "WAITING FOR NTP";

  String json = "{";

  json += "\"wifi\":";
  json += WiFi.isConnected() ? "true" : "false";

  json += ",\"ip\":\"" + ip + "\"";
  json += ",\"datetime\":\"" + datetime + "\"";

  json += ",\"timeA\":" + String(a);
  json += ",\"timeB\":" + String(b);

  json += ",\"statusA\":\"" + statusA + "\"";
  json += ",\"statusB\":\"" + statusB + "\"";

  json += ",\"redA\":" + String(redA ? "true" : "false");
  json += ",\"yellowA\":" + String(yellowA ? "true" : "false");
  json += ",\"greenA\":" + String(greenA ? "true" : "false");

  json += ",\"redB\":" + String(redB ? "true" : "false");
  json += ",\"yellowB\":" + String(yellowB ? "true" : "false");
  json += ",\"greenB\":" + String(greenB ? "true" : "false");

  json += ",\"camOnline\":" + String(cameraOnline() ? "true" : "false");
  json += ",\"camIP\":\"" + IPAddress((uint32_t)cameraIP).toString() + "\"";

  json += "}";

  server.send(200, "application/json", json);
}

void handleLogs() {
  String json = "{\"countA\":0,\"countB\":0,\"logs\":[";

  uint32_t countA = 0;
  uint32_t countB = 0;

  bool first = true;

  if(fsReady) {
    File f = LittleFS.open(LOG_FILE, "r");

    if(f) {
      while(f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();

        if(line.length() == 0) continue;

        int p1 = line.indexOf('|');
        int p2 = line.indexOf('|', p1 + 1);

        if(p1 < 0 || p2 < 0) continue;

        int p3 = line.indexOf('|', p2 + 1);
        int p4 = (p3 >= 0) ? line.indexOf('|', p3 + 1) : -1;

        uint32_t id = line.substring(0, p1).toInt();
        char lane = line.substring(p1 + 1, p2).charAt(0);
        time_t timestamp = (time_t)line.substring(
          p2 + 1, p3 >= 0 ? p3 : line.length()).toInt();

        String car = (p3 >= 0)
          ? line.substring(p3 + 1, p4 >= 0 ? p4 : line.length())
          : "";
        String url = (p4 >= 0) ? line.substring(p4 + 1) : "";
        bool hasPhoto = LittleFS.exists(photoPath(id));

        if(lane == 'A') countA++;
        if(lane == 'B') countB++;

        String date = "--";
        String clock = "--";

        if(timestamp > 1700000000) {
          struct tm t;
          localtime_r(&timestamp, &t);

          char dateBuffer[12];
          char timeBuffer[12];

          strftime(dateBuffer, sizeof(dateBuffer),
                   "%Y-%m-%d", &t);

          strftime(timeBuffer, sizeof(timeBuffer),
                   "%H:%M:%S", &t);

          date = dateBuffer;
          clock = timeBuffer;
        } else {
          date = "NOT SYNCED";
          clock = "NOT SYNCED";
        }

        if(!first) json += ",";
        first = false;

        json += "{\"id\":" + String(id);
        json += ",\"lane\":\"" + String(lane) + "\"";
        json += ",\"date\":\"" + date + "\"";
        json += ",\"time\":\"" + clock + "\"";
        json += ",\"car\":\"" + jsonEscape(car) + "\"";
        json += ",\"url\":\"" + jsonEscape(url) + "\"";
        json += ",\"photo\":" + String(hasPhoto ? "true" : "false") + "}";
      }

      f.close();
    }
  }

  json += "]}";

  json.replace("\"countA\":0", "\"countA\":" + String(countA));
  json.replace("\"countB\":0", "\"countB\":" + String(countB));

  server.send(200, "application/json", json);
}

void handleDelete() {
  if(!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing ID");
    return;
  }

  uint32_t id = server.arg("id").toInt();

  if(deleteViolation(id)) {
    server.send(200, "text/plain", "Deleted");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void handleClear() {
  if(clearViolations()) {
    server.send(200, "text/plain", "Cleared");
  } else {
    server.send(500, "text/plain", "Storage error");
  }
}

// =====================================================
// WIFI MANAGEMENT
// =====================================================
// Auto-reconnect is enabled in setup(). If the hotspot disappears,
// the ESP32 keeps the traffic controller running and periodically asks
// the Wi-Fi driver to reconnect using the saved SSID/password.
void manageWiFi() {
  const uint32_t now = millis();
  const bool connectedNow = (WiFi.status() == WL_CONNECTED);

  if(connectedNow) {
    wifiAttemptInProgress = false;

    if(!wifiConnected) {
      wifiConnected = true;
      Serial.println("WiFi connected successfully.");
      Serial.print("Dashboard IP: http://");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if(wifiConnected) {
    wifiConnected = false;
    Serial.println("WiFi disconnected. Automatic reconnection will start.");
    wifiAttemptInProgress = false;
    nextWifiRetry = now; // Try recovery promptly after a real disconnect.
  }

  // Do not repeatedly change station configuration while a connection
  // attempt is still running. Retry only after the attempt timeout.
  if(wifiAttemptInProgress &&
     (uint32_t)(now - wifiAttemptStarted) < WIFI_CONNECT_TIMEOUT_MS) {
    return;
  }

  if((int32_t)(now - nextWifiRetry) >= 0) {
    Serial.println("Hotspot not connected. Searching/reconnecting...");

    // WiFi.reconnect() reuses the station credentials already configured
    // by WiFi.begin() in setup(), avoiding repeated WiFi.begin() calls.
    bool reconnectStarted = WiFi.reconnect();

    if(reconnectStarted) {
      Serial.println("WiFi reconnection attempt started.");
    } else {
      Serial.println("WiFi reconnect request was not accepted; will retry.");
    }

    wifiAttemptInProgress = true;
    wifiAttemptStarted = now;
    nextWifiRetry = now + WIFI_RETRY_INTERVAL_MS;
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  pinMode(RED_A, OUTPUT);
  pinMode(YELLOW_A, OUTPUT);
  pinMode(GREEN_A, OUTPUT);

  pinMode(RED_B, OUTPUT);
  pinMode(YELLOW_B, OUTPUT);
  pinMode(GREEN_B, OUTPUT);

  pinMode(IR_A, INPUT_PULLUP);
  pinMode(IR_B, INPUT_PULLUP);

  setLights(HIGH, LOW, LOW,
            HIGH, LOW, LOW);

  displayA.setBrightness(5);
  displayB.setBrightness(5);

  displayA.clear();
  displayB.clear();

  // First try to mount without formatting so existing records are preserved.
  fsReady = LittleFS.begin(false);

  if(!fsReady) {
    Serial.println("LittleFS mount failed. Filesystem may be corrupted.");
    Serial.println("Attempting to format LittleFS...");

    // Formatting erases old filesystem contents. This is the recovery
    // path for the corrupted directory pair shown in Serial Monitor.
    if(LittleFS.format()) {
      Serial.println("LittleFS formatted successfully.");
      fsReady = LittleFS.begin(false);
    } else {
      Serial.println("LittleFS format failed.");
    }
  }

  if(fsReady) {
    Serial.println("LittleFS Ready");
    initializeStorage();
    if(!LittleFS.exists(PHOTO_DIR)) LittleFS.mkdir(PHOTO_DIR);
    Serial.printf("LittleFS: %u KB used of %u KB\n",
                  (unsigned)(LittleFS.usedBytes() / 1024),
                  (unsigned)(LittleFS.totalBytes() / 1024));
  } else {
    Serial.println("LittleFS is unavailable. Violations cannot be saved.");
  }

  initializeSensor(sensorA, IR_A, 'A');
  initializeSensor(sensorB, IR_B, 'B');

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  Serial.println("Connecting to iPhone Hotspot...");
  WiFi.begin(ssid, password);  // Initial connection; retries use WiFi.reconnect().
  WiFi.setSleep(false);        // keep the radio awake so camera messages are not missed
  wifiAttemptInProgress = true;
  wifiAttemptStarted = millis();
  nextWifiRetry = wifiAttemptStarted + WIFI_CONNECT_TIMEOUT_MS;

  // Wireless link to the ESP32-CAM (works on the hotspot's channel).
  initEspNow();

  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  currentPhase = 0;
  applyPhase();

  // Dashboard routes
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", dashboardHTML);
  });

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/logs", HTTP_GET, handleLogs);

  server.on("/api/delete", HTTP_POST, handleDelete);
  server.on("/api/clear", HTTP_POST, handleClear);

  // Camera photos
  server.on("/photo", HTTP_GET, handlePhoto);
  server.on("/api/photo", HTTP_POST, handlePhotoUploadDone, handlePhotoUpload);

  server.begin();

  Serial.println("Web Server Started");
  Serial.println("Smart Traffic System Ready");
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  manageWiFi();

  if(WiFi.isConnected() && clockIsValid()) {
    timeSynced = true;
  }

  updateTraffic();

  bool redA = (currentPhase == 2 || currentPhase == 3);
  bool redB = (currentPhase == 0 || currentPhase == 1);

  updateSensor(sensorA, IR_A, 'A', redA);
  updateSensor(sensorB, IR_B, 'B', redB);

  // Save car names / photo links reported by the ESP32-CAM.
  processCameraResults();

  // Periodic diagnostics to verify the actual electrical output of each IR module.
  static uint32_t lastSensorDebug = 0;
  if((uint32_t)(millis() - lastSensorDebug) >= 1000) {
    lastSensorDebug = millis();

    Serial.printf(
      "IR RAW | A=%s (RED=%d) | B=%s (RED=%d)\n",
      digitalRead(IR_A) ? "HIGH" : "LOW",
      redA ? 1 : 0,
      digitalRead(IR_B) ? "HIGH" : "LOW",
      redB ? 1 : 0
    );
  }

  updateDisplays();

  server.handleClient();
}
