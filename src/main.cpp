#include <Arduino.h>
#include <Preferences.h>
#include <WiFiS3.h>

constexpr uint16_t WEB_PORT = 80;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long DOUBLE_RESET_WINDOW_MS = 5000;
constexpr uint32_t DOUBLE_RESET_MAGIC = 0x52505355UL; // "RPSU"

constexpr char PREF_NAMESPACE[] = "remotepsu";
constexpr char PREF_SSID[] = "ssid";
constexpr char PREF_PASS[] = "pass";
constexpr char PREF_PINS[] = "pins";
constexpr char PREF_POLL_MS[] = "poll";

constexpr char SETUP_AP_SSID[] = "RemotePSU-Setup";

constexpr uint8_t MAX_PIN_NAME_LEN = 24;
constexpr uint32_t DEFAULT_POLL_INTERVAL_MS = 2000;
constexpr uint32_t MIN_POLL_INTERVAL_MS = 500;
constexpr uint32_t MAX_POLL_INTERVAL_MS = 60000;

uint32_t doubleResetMarker __attribute__((section(".noinit")));

struct ManagedPin {
  const char *label;
  uint8_t pin;
};

const ManagedPin AVAILABLE_PINS[] = {
    {"D0", D0},   {"D1", D1},   {"D2", D2},   {"D3", D3},   {"D4", D4},
    {"D5", D5},   {"D6", D6},   {"D7", D7},   {"D8", D8},   {"D9", D9},
    {"D10", D10}, {"D11", D11}, {"D12", D12}, {"D13", D13}, {"A0", A0},
    {"A1", A1},   {"A2", A2},   {"A3", A3},   {"A4", A4},   {"A5", A5},
};

constexpr uint8_t PIN_COUNT = sizeof(AVAILABLE_PINS) / sizeof(AVAILABLE_PINS[0]);

struct PinConfig {
  char name[MAX_PIN_NAME_LEN + 1];
};

PinConfig pinConfigs[PIN_COUNT];
bool pinDrivenToGround[PIN_COUNT];
Preferences preferences;
WiFiServer server(WEB_PORT);

bool setupMode = false;
bool rebootRequested = false;
bool doubleResetPending = false;
unsigned long bootMarkerSetAt = 0;
unsigned long rebootRequestedAt = 0;

String savedSsid;
String savedPass;
uint32_t pollingIntervalMs = DEFAULT_POLL_INTERVAL_MS;

void loadSettings();
void savePinSettings();
void savePollingInterval(uint32_t intervalMs);
void clearAllSettings();
void startSetupAccessPoint();
bool connectToStoredWifi();
void handleHttpClient(WiFiClient &client);
void sendDashboard(WiFiClient &client, const String &notice);
void sendPinState(WiFiClient &client, uint8_t idx);
void sendJson(WiFiClient &client, int statusCode, const String &body);
void sendText(WiFiClient &client, int statusCode, const String &body);
void sendRedirect(WiFiClient &client, const String &location);
void sendNotFound(WiFiClient &client);
void sendBadRequest(WiFiClient &client, const String &message);
String statusText(int code);
String htmlEscape(const String &value);
String urlDecode(const String &value);
String queryParam(const String &query, const String &key);
bool isValidName(const String &name);
int findPinIndexByName(const String &name);
int findPinIndexByLabel(const String &label);
void configurePins();
bool isPinShortedToGround(uint8_t idx);
void shortPinToGround(uint8_t idx);
void disconnectPin(uint8_t idx);
String boardIpAddress();
void waitForDoubleResetWindow();
void maybeClearDoubleResetMarker();
void printNetworkStatus();

void setup() {
  Serial.begin(115200);
  delay(250);

  bool doubleResetDetected = (doubleResetMarker == DOUBLE_RESET_MAGIC);
  doubleResetMarker = 0;

  if (doubleResetDetected) {
    Serial.println("Double reset detected. Clearing saved configuration.");
    if (preferences.begin(PREF_NAMESPACE)) {
      preferences.clear();
      preferences.end();
    }
  } else {
    doubleResetMarker = DOUBLE_RESET_MAGIC;
    bootMarkerSetAt = millis();
    doubleResetPending = true;
    waitForDoubleResetWindow();
  }

  loadSettings();
  configurePins();

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed.");
    while (true) {
      delay(1000);
    }
  }

  String firmwareVersion = WiFi.firmwareVersion();
  if (firmwareVersion < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.print("WiFi firmware can be updated. Current version: ");
    Serial.println(firmwareVersion);
  }

  if (!connectToStoredWifi()) {
    startSetupAccessPoint();
  }

  server.begin();
  printNetworkStatus();
}

void loop() {
  maybeClearDoubleResetMarker();

  WiFiClient client = server.available();
  if (client) {
    handleHttpClient(client);
    client.stop();
  }

  if (rebootRequested && millis() - rebootRequestedAt > 1200) {
    NVIC_SystemReset();
  }
}

void waitForDoubleResetWindow() {
  Serial.print("Press RESET again within ");
  Serial.print(DOUBLE_RESET_WINDOW_MS / 1000);
  Serial.println(" seconds to clear saved configuration.");

  while (doubleResetPending) {
    maybeClearDoubleResetMarker();
    delay(25);
  }
}

void maybeClearDoubleResetMarker() {
  if (doubleResetPending && millis() - bootMarkerSetAt > DOUBLE_RESET_WINDOW_MS) {
    doubleResetMarker = 0;
    doubleResetPending = false;
    Serial.println("Double reset window closed.");
  }
}

void loadSettings() {
  memset(pinConfigs, 0, sizeof(pinConfigs));
  memset(pinDrivenToGround, 0, sizeof(pinDrivenToGround));

  if (!preferences.begin(PREF_NAMESPACE)) {
    Serial.println("Preferences unavailable. Settings will not persist.");
    return;
  }

  savedSsid = preferences.getString(PREF_SSID, "");
  savedPass = preferences.getString(PREF_PASS, "");
  pollingIntervalMs = preferences.getUInt(PREF_POLL_MS, DEFAULT_POLL_INTERVAL_MS);
  if (pollingIntervalMs < MIN_POLL_INTERVAL_MS || pollingIntervalMs > MAX_POLL_INTERVAL_MS) {
    pollingIntervalMs = DEFAULT_POLL_INTERVAL_MS;
  }

  size_t storedSize = preferences.getBytesLength(PREF_PINS);
  if (storedSize == sizeof(pinConfigs)) {
    preferences.getBytes(PREF_PINS, pinConfigs, sizeof(pinConfigs));
  } else if (storedSize > 0) {
    preferences.remove(PREF_PINS);
  }

  preferences.end();
}

void saveWifiSettings(const String &ssid, const String &pass) {
  if (!preferences.begin(PREF_NAMESPACE)) {
    return;
  }

  if (ssid.length() == 0) {
    preferences.remove(PREF_SSID);
    preferences.remove(PREF_PASS);
  } else {
    preferences.putString(PREF_SSID, ssid);
    if (pass.length() == 0) {
      preferences.remove(PREF_PASS);
    } else {
      preferences.putString(PREF_PASS, pass);
    }
  }

  preferences.end();
}

void savePinSettings() {
  if (!preferences.begin(PREF_NAMESPACE)) {
    return;
  }

  preferences.putBytes(PREF_PINS, pinConfigs, sizeof(pinConfigs));
  preferences.end();
}

void savePollingInterval(uint32_t intervalMs) {
  if (intervalMs < MIN_POLL_INTERVAL_MS) {
    intervalMs = MIN_POLL_INTERVAL_MS;
  } else if (intervalMs > MAX_POLL_INTERVAL_MS) {
    intervalMs = MAX_POLL_INTERVAL_MS;
  }

  pollingIntervalMs = intervalMs;

  if (!preferences.begin(PREF_NAMESPACE)) {
    return;
  }

  preferences.putUInt(PREF_POLL_MS, pollingIntervalMs);
  preferences.end();
}

void clearAllSettings() {
  if (!preferences.begin(PREF_NAMESPACE)) {
    return;
  }

  preferences.clear();
  preferences.end();

  savedSsid = "";
  savedPass = "";
  pollingIntervalMs = DEFAULT_POLL_INTERVAL_MS;
  memset(pinConfigs, 0, sizeof(pinConfigs));
  memset(pinDrivenToGround, 0, sizeof(pinDrivenToGround));
  configurePins();
}

void configurePins() {
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    disconnectPin(i);
  }
}

bool connectToStoredWifi() {
  if (savedSsid.length() == 0) {
    Serial.println("No saved WiFi credentials. Starting setup AP.");
    return false;
  }

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(savedSsid);

  unsigned long startedAt = millis();
  uint8_t status = WL_IDLE_STATUS;
  while (millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    if (savedPass.length() > 0) {
      status = WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    } else {
      status = WiFi.begin(savedSsid.c_str());
    }

    if (status == WL_CONNECTED || WiFi.status() == WL_CONNECTED) {
      setupMode = false;
      return true;
    }

    delay(2500);
  }

  Serial.println("Could not connect to saved WiFi. Starting setup AP.");
  WiFi.disconnect();
  return false;
}

void startSetupAccessPoint() {
  setupMode = true;
  WiFi.end();
  delay(500);
  WiFi.config(IPAddress(192, 168, 4, 1));

  Serial.print("Creating setup access point: ");
  Serial.println(SETUP_AP_SSID);

  uint8_t status = WiFi.beginAP(SETUP_AP_SSID);
  if (status != WL_AP_LISTENING) {
    Serial.println("Failed to create setup access point.");
    while (true) {
      delay(1000);
    }
  }

  delay(3000);
}

bool isPinShortedToGround(uint8_t idx) {
  if (pinDrivenToGround[idx]) {
    return digitalRead(AVAILABLE_PINS[idx].pin) == LOW;
  }

  pinMode(AVAILABLE_PINS[idx].pin, INPUT_PULLUP);
  delayMicroseconds(50);
  bool shorted = digitalRead(AVAILABLE_PINS[idx].pin) == LOW;
  disconnectPin(idx);
  return shorted;
}

void shortPinToGround(uint8_t idx) {
  digitalWrite(AVAILABLE_PINS[idx].pin, LOW);
  pinMode(AVAILABLE_PINS[idx].pin, OUTPUT);
  pinDrivenToGround[idx] = true;
}

void disconnectPin(uint8_t idx) {
  pinMode(AVAILABLE_PINS[idx].pin, INPUT);
  pinDrivenToGround[idx] = false;
}

String boardIpAddress() {
  IPAddress ip = WiFi.localIP();
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

void handleHttpClient(WiFiClient &client) {
  client.setTimeout(1000);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();

  while (client.connected()) {
    String headerLine = client.readStringUntil('\n');
    headerLine.trim();
    if (headerLine.length() == 0) {
      break;
    }
  }

  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (firstSpace < 0 || secondSpace < 0) {
    sendBadRequest(client, "Malformed HTTP request.");
    return;
  }

  String method = requestLine.substring(0, firstSpace);
  String target = requestLine.substring(firstSpace + 1, secondSpace);
  int queryStart = target.indexOf('?');
  String path = queryStart >= 0 ? target.substring(0, queryStart) : target;
  String query = queryStart >= 0 ? target.substring(queryStart + 1) : "";
  path = urlDecode(path);

  if (method != "GET") {
    sendText(client, 405, "Only GET is supported.");
    return;
  }

  if (path == "/") {
    sendDashboard(client, queryParam(query, "notice"));
    return;
  }

  if (path == "/wifi") {
    String ssid = queryParam(query, "ssid");
    String pass = queryParam(query, "pass");
    ssid.trim();
    if (ssid.length() == 0) {
      sendRedirect(client, "/?notice=WiFi+SSID+cannot+be+empty");
      return;
    }

    saveWifiSettings(ssid, pass);
    sendRedirect(client, "/?notice=WiFi+saved.+Rebooting+to+connect");
    rebootRequested = true;
    rebootRequestedAt = millis();
    return;
  }

  if (path == "/polling") {
    uint32_t intervalMs = queryParam(query, "interval").toInt();
    savePollingInterval(intervalMs);
    sendRedirect(client, "/?notice=Polling+interval+saved");
    return;
  }

  if (path == "/pins") {
    String action = queryParam(query, "action");

    if (action == "delete") {
      int idx = queryParam(query, "idx").toInt();
      if (idx < 0 || idx >= PIN_COUNT) {
        sendRedirect(client, "/?notice=Invalid+pin+entry");
        return;
      }

      pinConfigs[idx].name[0] = '\0';
      disconnectPin(idx);
      savePinSettings();
      sendRedirect(client, "/?notice=Pin+removed");
      return;
    }

    String pinLabel = queryParam(query, "pin");
    String name = queryParam(query, "name");
    name.trim();

    int idx = findPinIndexByLabel(pinLabel);
    if (idx < 0) {
      sendRedirect(client, "/?notice=Choose+a+valid+pin");
      return;
    }

    if (!isValidName(name)) {
      sendRedirect(client, "/?notice=Use+1-24+letters,+numbers,+dash,+or+underscore");
      return;
    }

    for (uint8_t i = 0; i < PIN_COUNT; i++) {
      if (i == idx) {
        continue;
      }

      if (name.equalsIgnoreCase(pinConfigs[i].name) || pinConfigs[i].name[0] != '\0' &&
          AVAILABLE_PINS[i].pin == AVAILABLE_PINS[idx].pin) {
        pinConfigs[i].name[0] = '\0';
        disconnectPin(i);
      }
    }

    name.toCharArray(pinConfigs[idx].name, sizeof(pinConfigs[idx].name));
    savePinSettings();
    sendRedirect(client, "/?notice=Pin+saved");
    return;
  }

  if (path == "/factory-reset") {
    clearAllSettings();
    sendRedirect(client, "/?notice=Settings+cleared.+Rebooting");
    rebootRequested = true;
    rebootRequestedAt = millis();
    return;
  }

  int slash = path.indexOf('/', 1);
  if (slash > 1) {
    String pinName = path.substring(1, slash);
    String action = path.substring(slash + 1);
    int idx = findPinIndexByName(pinName);

    if (idx < 0) {
      sendNotFound(client);
      return;
    }

    if (action == "on") {
      if (!isPinShortedToGround(idx)) {
        shortPinToGround(idx);
      }
      sendPinState(client, idx);
      return;
    }

    if (action == "off") {
      if (isPinShortedToGround(idx)) {
        disconnectPin(idx);
      }
      sendPinState(client, idx);
      return;
    }

    if (action == "state") {
      sendPinState(client, idx);
      return;
    }
  }

  sendNotFound(client);
}

void sendDashboard(WiFiClient &client, const String &notice) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.println("<!doctype html><html><head><meta charset='utf-8'>");
  client.println("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  client.println("<title>Remote PSU Switch</title>");
  client.println("<style>");
  client.println("body{margin:0;font-family:Arial,sans-serif;background:#f5f7fa;color:#111827}");
  client.println("header{background:#111827;color:white;padding:18px 20px}main{max-width:920px;margin:0 auto;padding:20px}");
  client.println("section{background:white;border:1px solid #d8dee9;border-radius:8px;margin:0 0 18px;padding:16px}");
  client.println("h1{font-size:24px;margin:0}h2{font-size:18px;margin:0 0 12px}label{display:block;font-weight:700;margin:10px 0 4px}");
  client.println("input,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #aab4c0;border-radius:6px;font-size:16px}");
  client.println("button,.button{display:inline-block;margin:10px 8px 0 0;padding:10px 14px;border:0;border-radius:6px;background:#2563eb;color:white;text-decoration:none;font-weight:700}");
  client.println("button:disabled{cursor:wait;opacity:.65}");
  client.println(".danger{background:#b91c1c}.off{background:#4b5563}.muted{color:#526070}.notice{background:#e0f2fe;border-color:#7dd3fc}");
  client.println("table{width:100%;border-collapse:collapse}th,td{text-align:left;border-bottom:1px solid #e5e7eb;padding:10px}th{font-size:13px;color:#526070}");
  client.println(".state{display:inline-block;min-width:44px;padding:4px 8px;border-radius:999px;text-align:center;color:white;font-weight:700}.on{background:#15803d}.state.off{background:#6b7280}");
  client.println("</style></head><body>");
  client.println("<header><h1>Remote PSU Switch</h1></header><main>");

  if (notice.length() > 0) {
    client.print("<section class='notice'>");
    client.print(htmlEscape(notice));
    client.println("</section>");
  }

  client.println("<section><h2>Network</h2>");
  client.print("<p class='muted'>Mode: ");
  client.print(setupMode ? "Setup access point" : "Connected WiFi");
  client.print(" | Current IP address: ");
  client.print(boardIpAddress());
  client.println("</p>");
  if (setupMode) {
    client.print("<p class='muted'>Connect to AP ");
    client.print(SETUP_AP_SSID);
    client.println(" and open http://192.168.4.1/.</p>");
  }
  client.println("<form action='/wifi' method='get'>");
  client.print("<label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' maxlength='32' value='");
  client.print(htmlEscape(savedSsid));
  client.println("' required>");
  client.println("<label for='pass'>WiFi password</label><input id='pass' name='pass' type='password' maxlength='63'>");
  client.println("<button type='submit'>Save WiFi</button>");
  client.println("</form></section>");

  client.println("<section><h2>Pin Configuration</h2>");
  client.println("<form action='/pins' method='get'>");
  client.println("<input type='hidden' name='action' value='save'>");
  client.println("<label for='pin'>Board pin</label><select id='pin' name='pin'>");
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    client.print("<option value='");
    client.print(AVAILABLE_PINS[i].label);
    client.print("'>");
    client.print(AVAILABLE_PINS[i].label);
    if (pinConfigs[i].name[0] != '\0') {
      client.print(" - ");
      client.print(htmlEscape(pinConfigs[i].name));
    }
    client.println("</option>");
  }
  client.println("</select>");
  client.println("<label for='name'>Route name</label><input id='name' name='name' maxlength='24' pattern='[A-Za-z0-9_-]+' required>");
  client.println("<button type='submit'>Save Pin Name</button>");
  client.println("</form></section>");

  client.println("<section><h2>Dashboard Refresh</h2>");
  client.println("<form action='/polling' method='get'>");
  client.print("<label for='interval'>Polling interval (milliseconds)</label><input id='interval' name='interval' type='number' min='");
  client.print(MIN_POLL_INTERVAL_MS);
  client.print("' max='");
  client.print(MAX_POLL_INTERVAL_MS);
  client.print("' step='100' value='");
  client.print(pollingIntervalMs);
  client.println("' required>");
  client.println("<button type='submit'>Save Polling Interval</button>");
  client.println("</form></section>");

  client.println("<section><h2>Named Pins</h2>");
  client.println("<table><thead><tr><th>Name</th><th>Pin</th><th>State</th><th>Controls</th></tr></thead><tbody>");
  bool hasNamedPins = false;
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    if (pinConfigs[i].name[0] == '\0') {
      continue;
    }

    hasNamedPins = true;
    String routeName = htmlEscape(pinConfigs[i].name);
    client.print("<tr data-route='");
    client.print(routeName);
    client.println("'><td>");
    client.print(routeName);
    client.print("</td><td>");
    client.print(AVAILABLE_PINS[i].label);
    client.print("</td><td><span class='state ");
    client.print(isPinShortedToGround(i) ? "on'>ON" : "off'>OFF");
    client.print("</span></td><td>");
    client.print("<button type='button' class='button' data-route='");
    client.print(routeName);
    client.print("' data-action='on' onclick='controlPin(this)'>On</button>");
    client.print("<button type='button' class='button off' data-route='");
    client.print(routeName);
    client.print("' data-action='off' onclick='controlPin(this)'>Off</button>");
    client.print("<a class='button danger' href='/pins?action=delete&idx=");
    client.print(i);
    client.print("'>Remove</a>");
    client.println("</td></tr>");
  }
  if (!hasNamedPins) {
    client.println("<tr><td colspan='4' class='muted'>No pins are named yet.</td></tr>");
  }
  client.println("</tbody></table></section>");

  client.println("<section><h2>Factory Reset</h2>");
  client.print("<p class='muted'>To reset config from the board, press RESET once, then press it again within ");
  client.print(DOUBLE_RESET_WINDOW_MS / 1000);
  client.println(" seconds. You can also use this button while the dashboard is reachable.</p>");
  client.println("<a class='button danger' href='/factory-reset'>Clear all settings</a>");
  client.println("</section>");

  client.println("<script>");
  client.print("const pollIntervalMs=");
  client.print(pollingIntervalMs);
  client.println(";");
  client.println("function updateBadge(row,state){");
  client.println("  const badge=row.querySelector('.state');");
  client.println("  const on=state==='on';");
  client.println("  badge.textContent=on?'ON':'OFF';");
  client.println("  badge.className='state '+(on?'on':'off');");
  client.println("}");
  client.println("async function fetchPinState(row){");
  client.println("  const route=row.dataset.route;");
  client.println("  const res=await fetch('/'+encodeURIComponent(route)+'/state',{cache:'no-store'});");
  client.println("  if(!res.ok) throw new Error('state failed');");
  client.println("  const data=await res.json();");
  client.println("  updateBadge(row,data.state);");
  client.println("}");
  client.println("async function pollPins(){");
  client.println("  const rows=document.querySelectorAll('tr[data-route]');");
  client.println("  for(const row of rows){try{await fetchPinState(row);}catch(e){}}");
  client.println("}");
  client.println("async function controlPin(btn){");
  client.println("  const route=btn.dataset.route, action=btn.dataset.action;");
  client.println("  btn.disabled=true;");
  client.println("  try{");
  client.println("    const res=await fetch('/'+encodeURIComponent(route)+'/'+action,{cache:'no-store'});");
  client.println("    if(!res.ok) throw new Error('request failed');");
  client.println("    const data=await res.json();");
  client.println("    updateBadge(btn.closest('tr'),data.state);");
  client.println("  }catch(e){alert('Pin command failed');}");
  client.println("  finally{btn.disabled=false;}");
  client.println("}");
  client.println("if(pollIntervalMs>0){setInterval(pollPins,pollIntervalMs);}");
  client.println("</script>");
  client.println("</main></body></html>");
}

void sendPinState(WiFiClient &client, uint8_t idx) {
  bool shorted = isPinShortedToGround(idx);
  String body = "{\"name\":\"";
  body += pinConfigs[idx].name;
  body += "\",\"pin\":\"";
  body += AVAILABLE_PINS[idx].label;
  body += "\",\"state\":\"";
  body += shorted ? "on" : "off";
  body += "\"}";
  sendJson(client, 200, body);
}

void sendJson(WiFiClient &client, int statusCode, const String &body) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(" ");
  client.println(statusText(statusCode));
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.print(body);
}

void sendText(WiFiClient &client, int statusCode, const String &body) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(" ");
  client.println(statusText(statusCode));
  client.println("Content-Type: text/plain; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(body);
}

void sendRedirect(WiFiClient &client, const String &location) {
  client.println("HTTP/1.1 303 See Other");
  client.print("Location: ");
  client.println(location);
  client.println("Connection: close");
  client.println();
}

void sendNotFound(WiFiClient &client) {
  sendText(client, 404, "Not found.");
}

void sendBadRequest(WiFiClient &client, const String &message) {
  sendText(client, 400, message);
}

String statusText(int code) {
  switch (code) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    default:
      return "OK";
  }
}

String htmlEscape(const String &value) {
  String escaped;
  for (uint16_t i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&#39;";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

String urlDecode(const String &value) {
  String decoded;
  for (uint16_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < value.length()) {
      char hex[3] = {value[i + 1], value[i + 2], '\0'};
      decoded += static_cast<char>(strtoul(hex, nullptr, 16));
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

String queryParam(const String &query, const String &key) {
  uint16_t pos = 0;
  while (pos < query.length()) {
    int next = query.indexOf('&', pos);
    if (next < 0) {
      next = query.length();
    }

    String pair = query.substring(pos, next);
    int equals = pair.indexOf('=');
    String pairKey = equals >= 0 ? pair.substring(0, equals) : pair;
    String pairValue = equals >= 0 ? pair.substring(equals + 1) : "";

    if (urlDecode(pairKey) == key) {
      return urlDecode(pairValue);
    }

    pos = next + 1;
  }

  return "";
}

bool isValidName(const String &name) {
  if (name.length() == 0 || name.length() > MAX_PIN_NAME_LEN) {
    return false;
  }

  for (uint8_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = isAlphaNumeric(c) || c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }

  return true;
}

int findPinIndexByName(const String &name) {
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    if (pinConfigs[i].name[0] != '\0' && name.equalsIgnoreCase(pinConfigs[i].name)) {
      return i;
    }
  }

  return -1;
}

int findPinIndexByLabel(const String &label) {
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    if (label.equalsIgnoreCase(AVAILABLE_PINS[i].label)) {
      return i;
    }
  }

  return -1;
}

void printNetworkStatus() {
  Serial.print("Mode: ");
  Serial.println(setupMode ? "setup AP" : "WiFi client");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (setupMode) {
    Serial.print("Setup AP SSID: ");
    Serial.println(SETUP_AP_SSID);
  } else {
    Serial.print("Connected SSID: ");
    Serial.println(WiFi.SSID());
  }
}
