/**
 * ESP32 Wi-Fi Diagnostic Hub
 * Heavy project for bare ESP32 (no external hardware needed)
 *
 * Written by Grok for Agnia (Kandelsbreit)
 * August 2026
 *
 * Features:
 *  - SoftAP "ESP32-Diag-Hub"
 *  - Full web dashboard with dark theme
 *  - Live WebSocket telemetry
 *  - Wi-Fi network scanner
 *  - System & chip info
 *  - Circular event log
 *  - OTA via web
 *  - HTTP Basic Auth
 *  - FreeRTOS monitoring task
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_chip_info.h>

// ===================== CONFIG =====================
const char* AP_SSID     = "ESP32-Diag-Hub";
const char* AP_PASSWORD = "diagnostic123";

const char* AUTH_USER   = "admin";
const char* AUTH_PASS   = "grok2026";

const int   WS_INTERVAL_MS = 1500;   // WebSocket push interval
const int   LOG_CAPACITY   = 40;     // circular log size

// ===================== GLOBALS =====================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

struct LogEntry {
  unsigned long ts;      // millis
  char msg[96];
};

LogEntry logBuf[LOG_CAPACITY];
int logHead = 0;
int logCount = 0;

struct ScanResult {
  String ssid;
  int32_t rssi;
  uint8_t channel;
  wifi_auth_mode_t enc;
  bool isHidden;
};

std::vector<ScanResult> lastScan;
bool scanInProgress = false;
unsigned long lastScanTime = 0;

// Live stats
struct {
  uint32_t freeHeap;
  uint32_t minFreeHeap;
  uint32_t uptimeSec;
  int8_t   rssi;          // if connected as STA (optional)
  uint8_t  apClients;
  uint32_t wifiChannel;
  float    temperature;   // internal sensor approx
} liveStats;

// ===================== UTILS =====================
void addLog(const char* fmt, ...) {
  char buf[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  logBuf[logHead].ts = millis();
  strncpy(logBuf[logHead].msg, buf, sizeof(logBuf[logHead].msg) - 1);
  logBuf[logHead].msg[sizeof(logBuf[logHead].msg) - 1] = 0;

  logHead = (logHead + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) logCount++;

  Serial.printf("[LOG] %s\n", buf);
}

String authModeToStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:         return "OPEN";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default:                     return "?";
  }
}

bool checkAuth(AsyncWebServerRequest *request) {
  if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

// ===================== LIVE STATS TASK =====================
void updateLiveStats() {
  liveStats.freeHeap    = ESP.getFreeHeap();
  liveStats.minFreeHeap = ESP.getMinFreeHeap();
  liveStats.uptimeSec   = millis() / 1000;
  liveStats.apClients   = WiFi.softAPgetStationNum();
  liveStats.wifiChannel = WiFi.channel();
  // Internal temperature (rough, ESP32)
  liveStats.temperature = temperatureRead();
}

void broadcastStats() {
  if (ws.count() == 0) return;

  updateLiveStats();

  JsonDocument doc;
  doc["type"] = "stats";
  doc["freeHeap"] = liveStats.freeHeap;
  doc["minFreeHeap"] = liveStats.minFreeHeap;
  doc["uptime"] = liveStats.uptimeSec;
  doc["apClients"] = liveStats.apClients;
  doc["channel"] = liveStats.wifiChannel;
  doc["temp"] = liveStats.temperature;
  doc["scanInProgress"] = scanInProgress;
  doc["lastScanAge"] = lastScanTime ? (millis() - lastScanTime) / 1000 : -1;

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// ===================== SCANNER =====================
void doScan() {
  if (scanInProgress) return;
  scanInProgress = true;
  addLog("Wi-Fi scan started...");

  // Run in separate task-ish (async)
  int n = WiFi.scanNetworks(false, true); // async=false, show_hidden=true
  lastScan.clear();

  for (int i = 0; i < n; ++i) {
    ScanResult r;
    r.ssid    = WiFi.SSID(i);
    r.rssi    = WiFi.RSSI(i);
    r.channel = WiFi.channel(i);
    r.enc     = WiFi.encryptionType(i);
    r.isHidden = (r.ssid.length() == 0);
    lastScan.push_back(r);
  }

  WiFi.scanDelete();
  lastScanTime = millis();
  scanInProgress = false;
  addLog("Scan finished: %d networks found", n);
}

// ===================== WEB UI (embedded) =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Diag Hub · Grok</title>
<style>
  :root {
    --bg: #0f1115;
    --card: #1a1d24;
    --border: #2a2e38;
    --text: #e6e9ef;
    --muted: #8b93a7;
    --accent: #6c9eff;
    --green: #3dd68c;
    --orange: #ffb020;
    --red: #ff6b6b;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Segoe UI', system-ui, sans-serif;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
    line-height: 1.45;
  }
  header {
    background: var(--card);
    border-bottom: 1px solid var(--border);
    padding: 14px 20px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    position: sticky;
    top: 0;
    z-index: 10;
  }
  header h1 { font-size: 1.15rem; font-weight: 600; }
  header .badge {
    background: #243049;
    color: var(--accent);
    font-size: 0.7rem;
    padding: 3px 8px;
    border-radius: 999px;
  }
  nav {
    display: flex;
    gap: 6px;
    padding: 12px 16px;
    overflow-x: auto;
    border-bottom: 1px solid var(--border);
    background: #12151b;
  }
  nav button {
    background: transparent;
    border: 1px solid var(--border);
    color: var(--muted);
    padding: 7px 14px;
    border-radius: 8px;
    cursor: pointer;
    white-space: nowrap;
    font-size: 0.9rem;
  }
  nav button.active {
    background: var(--accent);
    color: #0b0d11;
    border-color: var(--accent);
    font-weight: 600;
  }
  main { padding: 16px; max-width: 960px; margin: 0 auto; }
  .card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 16px;
    margin-bottom: 14px;
  }
  .card h2 {
    font-size: 0.95rem;
    color: var(--muted);
    margin-bottom: 12px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
    gap: 10px;
  }
  .stat {
    background: #12151b;
    border-radius: 10px;
    padding: 12px;
    text-align: center;
  }
  .stat .val {
    font-size: 1.4rem;
    font-weight: 700;
    color: var(--accent);
  }
  .stat .lbl {
    font-size: 0.75rem;
    color: var(--muted);
    margin-top: 2px;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.88rem;
  }
  th, td {
    padding: 8px 10px;
    text-align: left;
    border-bottom: 1px solid var(--border);
  }
  th { color: var(--muted); font-weight: 500; }
  .rssi-good { color: var(--green); }
  .rssi-mid  { color: var(--orange); }
  .rssi-bad  { color: var(--red); }
  button.action {
    background: var(--accent);
    color: #0b0d11;
    border: none;
    padding: 10px 18px;
    border-radius: 8px;
    font-weight: 600;
    cursor: pointer;
    font-size: 0.95rem;
  }
  button.action:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
  .log-list {
    max-height: 320px;
    overflow-y: auto;
    font-family: ui-monospace, monospace;
    font-size: 0.8rem;
  }
  .log-item {
    padding: 4px 0;
    border-bottom: 1px solid #22262f;
    color: var(--muted);
  }
  .log-item .time { color: #5a6478; margin-right: 8px; }
  #statusDot {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--red);
    display: inline-block;
    margin-right: 6px;
  }
  #statusDot.ok { background: var(--green); }
  .hidden { display: none; }
  input[type=file] { color: var(--muted); }
  .info-row {
    display: flex;
    justify-content: space-between;
    padding: 6px 0;
    border-bottom: 1px solid #22262f;
    font-size: 0.9rem;
  }
  .info-row span:first-child { color: var(--muted); }
</style>
</head>
<body>
<header>
  <h1>ESP32 Diagnostic Hub</h1>
  <div>
    <span id="statusDot"></span>
    <span class="badge">by Grok</span>
  </div>
</header>

<nav>
  <button class="active" data-tab="dash">Dashboard</button>
  <button data-tab="scan">Scanner</button>
  <button data-tab="sys">System</button>
  <button data-tab="logs">Logs</button>
  <button data-tab="ota">OTA</button>
</nav>

<main>
  <!-- DASHBOARD -->
  <section id="tab-dash">
    <div class="card">
      <h2>Live Telemetry</h2>
      <div class="grid">
        <div class="stat"><div class="val" id="s-heap">—</div><div class="lbl">Free Heap</div></div>
        <div class="stat"><div class="val" id="s-uptime">—</div><div class="lbl">Uptime</div></div>
        <div class="stat"><div class="val" id="s-clients">—</div><div class="lbl">AP Clients</div></div>
        <div class="stat"><div class="val" id="s-ch">—</div><div class="lbl">Channel</div></div>
        <div class="stat"><div class="val" id="s-temp">—</div><div class="lbl">Temp °C</div></div>
        <div class="stat"><div class="val" id="s-minheap">—</div><div class="lbl">Min Free</div></div>
      </div>
    </div>
    <div class="card">
      <h2>Access Point</h2>
      <div class="info-row"><span>SSID</span><span>ESP32-Diag-Hub</span></div>
      <div class="info-row"><span>IP</span><span>192.168.4.1</span></div>
      <div class="info-row"><span>Password</span><span>diagnostic123</span></div>
    </div>
  </section>

  <!-- SCANNER -->
  <section id="tab-scan" class="hidden">
    <div class="card">
      <h2>Wi-Fi Scanner</h2>
      <button class="action" id="btn-scan" onclick="startScan()">Start Scan</button>
      <p style="margin-top:10px;color:var(--muted);font-size:0.85rem" id="scan-status">Ready</p>
    </div>
    <div class="card">
      <h2>Results</h2>
      <div style="overflow-x:auto">
        <table>
          <thead>
            <tr><th>SSID</th><th>RSSI</th><th>Ch</th><th>Auth</th></tr>
          </thead>
          <tbody id="scan-body"></tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- SYSTEM -->
  <section id="tab-sys" class="hidden">
    <div class="card">
      <h2>Chip & Firmware</h2>
      <div id="sys-info">Loading...</div>
    </div>
  </section>

  <!-- LOGS -->
  <section id="tab-logs" class="hidden">
    <div class="card">
      <h2>Event Log</h2>
      <div class="log-list" id="log-list">—</div>
    </div>
  </section>

  <!-- OTA -->
  <section id="tab-ota" class="hidden">
    <div class="card">
      <h2>Firmware Update (OTA)</h2>
      <p style="color:var(--muted);margin-bottom:12px;font-size:0.9rem">
        Требуется авторизация (admin / grok2026)
      </p>
      <form method="POST" action="/update" enctype="multipart/form-data" id="ota-form">
        <input type="file" name="firmware" accept=".bin" required>
        <br><br>
        <button class="action" type="submit">Upload & Update</button>
      </form>
      <p id="ota-status" style="margin-top:12px;color:var(--muted)"></p>
    </div>
  </section>
</main>

<script>
const tabs = document.querySelectorAll('nav button');
const sections = {
  dash: document.getElementById('tab-dash'),
  scan: document.getElementById('tab-scan'),
  sys:  document.getElementById('tab-sys'),
  logs: document.getElementById('tab-logs'),
  ota:  document.getElementById('tab-ota')
};

tabs.forEach(btn => {
  btn.addEventListener('click', () => {
    tabs.forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    Object.values(sections).forEach(s => s.classList.add('hidden'));
    sections[btn.dataset.tab].classList.remove('hidden');
    if (btn.dataset.tab === 'sys') loadSys();
    if (btn.dataset.tab === 'logs') loadLogs();
  });
});

function fmtUptime(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h + 'h ' + m + 'm ' + sec + 's';
}

function rssiClass(r) {
  if (r >= -60) return 'rssi-good';
  if (r >= -75) return 'rssi-mid';
  return 'rssi-bad';
}

// WebSocket
let ws;
function connectWS() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen = () => {
    document.getElementById('statusDot').classList.add('ok');
  };
  ws.onclose = () => {
    document.getElementById('statusDot').classList.remove('ok');
    setTimeout(connectWS, 2000);
  };
  ws.onmessage = (e) => {
    const d = JSON.parse(e.data);
    if (d.type === 'stats') {
      document.getElementById('s-heap').textContent = (d.freeHeap/1024).toFixed(1) + ' KB';
      document.getElementById('s-minheap').textContent = (d.minFreeHeap/1024).toFixed(1) + ' KB';
      document.getElementById('s-uptime').textContent = fmtUptime(d.uptime);
      document.getElementById('s-clients').textContent = d.apClients;
      document.getElementById('s-ch').textContent = d.channel;
      document.getElementById('s-temp').textContent = d.temp.toFixed(1);
      if (d.scanInProgress) {
        document.getElementById('scan-status').textContent = 'Scanning...';
        document.getElementById('btn-scan').disabled = true;
      } else {
        document.getElementById('btn-scan').disabled = false;
      }
    }
  };
}
connectWS();

function startScan() {
  document.getElementById('btn-scan').disabled = true;
  document.getElementById('scan-status').textContent = 'Scanning...';
  fetch('/api/scan', { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      const tbody = document.getElementById('scan-body');
      tbody.innerHTML = '';
      data.networks.forEach(n => {
        const tr = document.createElement('tr');
        tr.innerHTML = `<td>${n.ssid || '<i>hidden</i>'}</td>
                        <td class="${rssiClass(n.rssi)}">${n.rssi} dBm</td>
                        <td>${n.channel}</td>
                        <td>${n.auth}</td>`;
        tbody.appendChild(tr);
      });
      document.getElementById('scan-status').textContent = `Found ${data.networks.length} networks`;
      document.getElementById('btn-scan').disabled = false;
    })
    .catch(err => {
      document.getElementById('scan-status').textContent = 'Error: ' + err;
      document.getElementById('btn-scan').disabled = false;
    });
}

function loadSys() {
  fetch('/api/system')
    .then(r => r.json())
    .then(d => {
      const el = document.getElementById('sys-info');
      el.innerHTML = `
        <div class="info-row"><span>Chip Model</span><span>${d.model}</span></div>
        <div class="info-row"><span>Cores</span><span>${d.cores}</span></div>
        <div class="info-row"><span>Revision</span><span>${d.revision}</span></div>
        <div class="info-row"><span>CPU Freq</span><span>${d.cpuMHz} MHz</span></div>
        <div class="info-row"><span>Flash</span><span>${d.flashMB} MB</span></div>
        <div class="info-row"><span>SDK</span><span>${d.sdk}</span></div>
        <div class="info-row"><span>Arduino Core</span><span>${d.arduino}</span></div>
        <div class="info-row"><span>MAC STA</span><span>${d.macSTA}</span></div>
        <div class="info-row"><span>MAC AP</span><span>${d.macAP}</span></div>
        <div class="info-row"><span>Sketch Size</span><span>${(d.sketchSize/1024).toFixed(1)} KB</span></div>
        <div class="info-row"><span>Free Sketch</span><span>${(d.freeSketch/1024).toFixed(1)} KB</span></div>
      `;
    });
}

function loadLogs() {
  fetch('/api/logs')
    .then(r => r.json())
    .then(d => {
      const el = document.getElementById('log-list');
      el.innerHTML = d.logs.map(l =>
        `<div class="log-item"><span class="time">${l.ts}</span>${l.msg}</div>`
      ).join('') || '— empty —';
    });
}

// OTA form progress (basic)
document.getElementById('ota-form').addEventListener('submit', function(e) {
  document.getElementById('ota-status').textContent = 'Uploading... do not power off';
});
</script>
</body>
</html>
)rawliteral";

// ===================== HTTP HANDLERS =====================
void setupWeb() {
  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  // API: system info
  server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    doc["model"] = ESP.getChipModel();
    doc["cores"] = chip.cores;
    doc["revision"] = chip.revision;
    doc["cpuMHz"] = ESP.getCpuFreqMHz();
    doc["flashMB"] = ESP.getFlashChipSize() / (1024 * 1024);
    doc["sdk"] = ESP.getSdkVersion();
    doc["arduino"] = ESP.getCoreVersion();
    doc["macSTA"] = WiFi.macAddress();
    doc["macAP"] = WiFi.softAPmacAddress();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["freeSketch"] = ESP.getFreeSketchSpace();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // API: logs
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    // oldest → newest
    int start = (logCount == LOG_CAPACITY) ? logHead : 0;
    for (int i = 0; i < logCount; i++) {
      int idx = (start + i) % LOG_CAPACITY;
      JsonObject o = arr.add<JsonObject>();
      unsigned long sec = logBuf[idx].ts / 1000;
      char tbuf[16];
      snprintf(tbuf, sizeof(tbuf), "%02lu:%02lu:%02lu", (sec/3600)%24, (sec/60)%60, sec%60);
      o["ts"] = tbuf;
      o["msg"] = logBuf[idx].msg;
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // API: start scan
  server.on("/api/scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (scanInProgress) {
      request->send(409, "application/json", "{\"error\":\"scan in progress\"}");
      return;
    }
    doScan();

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (auto& r : lastScan) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = r.ssid;
      o["rssi"] = r.rssi;
      o["channel"] = r.channel;
      o["auth"] = authModeToStr(r.enc);
      o["hidden"] = r.isHidden;
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // OTA update
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!checkAuth(request)) return;
      bool ok = !Update.hasError();
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain",
        ok ? "Update successful! Rebooting..." : "Update failed");
      response->addHeader("Connection", "close");
      request->send(response);
      if (ok) {
        delay(500);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!request->authenticate(AUTH_USER, AUTH_PASS)) return;

      if (index == 0) {
        addLog("OTA start: %s", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      if (!Update.hasError()) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }
      if (final) {
        if (Update.end(true)) {
          addLog("OTA success, size %u", index + len);
        } else {
          Update.printError(Serial);
          addLog("OTA failed");
        }
      }
    }
  );

  // WebSocket events
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      addLog("WS client #%u connected", client->id());
      broadcastStats(); // immediate
    } else if (type == WS_EVT_DISCONNECT) {
      addLog("WS client #%u disconnected", client->id());
    }
  });

  server.addHandler(&ws);
  server.begin();
  addLog("Web server started on 192.168.4.1");
}

// ===================== SETUP & LOOP =====================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== ESP32 Wi-Fi Diagnostic Hub (Grok) ===");

  // SoftAP only (no STA needed)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(100);

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP SSID: %s\n", AP_SSID);
  Serial.printf("AP IP  : %s\n", ip.toString().c_str());

  addLog("Boot complete, SoftAP up");
  addLog("Free heap at boot: %u", ESP.getFreeHeap());

  setupWeb();

  // Optional: create a FreeRTOS task for stats (demonstrates multi-tasking)
  xTaskCreatePinnedToCore(
    [](void* param) {
      for (;;) {
        broadcastStats();
        vTaskDelay(pdMS_TO_TICKS(WS_INTERVAL_MS));
      }
    },
    "statsTask",
    4096,
    nullptr,
    1,
    nullptr,
    0   // core 0
  );
}

void loop() {
  // Clean up disconnected WS clients
  ws.cleanupClients();
  delay(50);
}
