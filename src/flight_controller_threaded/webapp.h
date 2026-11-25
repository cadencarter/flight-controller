#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <AsyncTCP.h>             // by ESP32Async
#include <ESPAsyncWebServer.h>    // by ESP32Async
#include <string.h>

//#include "commands.h"
#include "index_html.h"
#include "mon_struts.h"
#include "esp_heap_caps.h"

// ---- WiFi Settings ---- //
#define USE_ACCESS_POINT true 

const char *AP_SSID = "Aerial Matrix";
const char *AP_PASS = "12345678"; // at least 8 characters

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

const char *WIFI_SSID = "your_ssid"; // only used when USE_ACCESS_POINT is false
const char *WIFI_PASS = "your_password";
// ------------------

// ---- Monitoring ---- //
static char  buf[SYSMON_LINES_BUF_BYTES];
static char *lines[SYSMON_MAX_LINES];
// -------------------- //

// ---- Attachments ---- //
// Only the LIGHT module is identified by its sense-pin voltage.
// Grove AI, Portenta AI and Environmental modules have no sense pin wired,
// so they are detected through the health API instead (see "Module health").
#define LIGHT_MODULE          3100    //  2.5V
// (unused now - no sense pin on these modules)
//#define GROVE_AI_MODULE       2500    //  2.0V
//#define PORTENTA_AI_MODULE    1850    //  1.5V
//#define ENVIRONMENTAL_MODULE  1250    //  1.0V

#define ATTACHMENT_1        18
#define ATTACHMENT_1_SENSE  17
#define ATTACHMENT_2        12
#define ATTACHMENT_2_SENSE  13
uint8_t togglePin = 0;
bool ledState = false;

#define ATTACH_TOLERANCE   100   // +/- ADC counts around each signature
#define ATTACH_SCAN_MS     500   // how often the loop rescans the attachments
 
enum AttachmentType : uint8_t {
  ATT_NONE = 0,
  ATT_LIGHT,
  ATT_GROVE_AI,
  ATT_PORTENTA_AI,
  ATT_ENVIRONMENTAL
};
 
// attachments[0] = attachment 1, attachments[1] = attachment 2
// (each port holds at most one module; either port may be empty)
static AttachmentType attachments[2] = { ATT_NONE, ATT_NONE };

static const char *attachmentName(AttachmentType t) {
  switch (t) {
    case ATT_LIGHT:         return "LIGHT";
    case ATT_GROVE_AI:      return "GROVE_AI";
    case ATT_PORTENTA_AI:   return "PORTENTA_AI";
    case ATT_ENVIRONMENTAL: return "ENVIRONMENTAL";
    default:                return "NONE";
  }
}

// Sense-pin read: only the light module can be identified this way.
static AttachmentType readSensedAttachment(uint8_t sensePin) {
  int counts = analogRead(sensePin);
  if (abs(counts - LIGHT_MODULE) <= ATTACH_TOLERANCE) return ATT_LIGHT;
  return ATT_NONE;  // empty port, or a module with no sense pin
}
// --------------------- //

// ---- Module health / data ---- //
#define MODULE_TIMEOUT_MS 15000     // a module is "attached" while it has pinged within this window

// Modules that are detected via the health API (no sense pin)
enum HealthModule : uint8_t {
  HM_ENV = 0,
  HM_GROVE,
  HM_PORTENTA,
  HM_COUNT
};

struct ModuleHealth {
  volatile uint32_t lastSeen;
  volatile bool     everSeen;
  volatile int8_t   requestedPort;   // -1 = module didn't say, 0 = port 1, 1 = port 2
};

static ModuleHealth modHealth[HM_COUNT] = {
  { 0, false, -1 },   // HM_ENV
  { 0, false, -1 },   // HM_GROVE
  { 0, false, -1 },   // HM_PORTENTA
};

static const AttachmentType hmAttachType[HM_COUNT] = {
  ATT_ENVIRONMENTAL, ATT_GROVE_AI, ATT_PORTENTA_AI
};

// Which port each health module currently occupies (-1 = none). Sticky, so a
// module doesn't hop between ports as other modules come and go.
static int8_t hmSlot[HM_COUNT] = { -1, -1, -1 };

static bool moduleAlive(HealthModule m) {
  if (!modHealth[m].everSeen) return false;
  return (millis() - modHealth[m].lastSeen) <= MODULE_TIMEOUT_MS;
}

// Record a health ping. Optional ?port=1 or ?port=2 pins the module to a port.
static void markHealth(HealthModule m, AsyncWebServerRequest *request) {
  if (request->hasParam("port")) {
    int p = request->getParam("port")->value().toInt();
    modHealth[m].requestedPort = (p == 1 || p == 2) ? (int8_t)(p - 1) : (int8_t)-1;
  }
  modHealth[m].lastSeen = millis();
  modHealth[m].everSeen = true;
}

// Combine sense-pin readings (light) with health pings (env / Grove / Portenta)
static void checkAttachments() {
  AttachmentType next[2];
  next[0] = readSensedAttachment(ATTACHMENT_1_SENSE);
  next[1] = readSensedAttachment(ATTACHMENT_2_SENSE);

  // 1) Release ports held by modules that timed out
  for (uint8_t m = 0; m < HM_COUNT; m++) {
    if (!moduleAlive((HealthModule)m)) hmSlot[m] = -1;
  }

  // 2) Keep live modules where they were, unless a sensed module took the port
  for (uint8_t m = 0; m < HM_COUNT; m++) {
    if (hmSlot[m] < 0) continue;
    if (next[hmSlot[m]] == ATT_NONE) next[hmSlot[m]] = hmAttachType[m];
    else                             hmSlot[m] = -1;
  }

  // 3) Newly alive modules that asked for a specific port
  for (uint8_t m = 0; m < HM_COUNT; m++) {
    if (hmSlot[m] >= 0 || !moduleAlive((HealthModule)m)) continue;
    int8_t want = modHealth[m].requestedPort;
    if (want >= 0 && next[want] == ATT_NONE) {
      next[want] = hmAttachType[m];
      hmSlot[m] = want;
    }
  }

  // 4) Remaining alive modules take the first free port
  for (uint8_t m = 0; m < HM_COUNT; m++) {
    if (hmSlot[m] >= 0 || !moduleAlive((HealthModule)m)) continue;
    for (int8_t p = 0; p < 2; p++) {
      if (next[p] == ATT_NONE) {
        next[p] = hmAttachType[m];
        hmSlot[m] = p;
        break;
      }
    }
  }

  attachments[0] = next[0];
  attachments[1] = next[1];
}

// Latest correction from the AI module
static volatile float    aiDeltaX = 0.0f;
static volatile float    aiDeltaY = 0.0f;
static volatile uint32_t aiDataMs = 0;
 
bool getAiCorrection(float *dx, float *dy, uint32_t *ageMs) {
  if (!aiDataMs) return false;
  *dx = aiDeltaX;
  *dy = aiDeltaY;
  *ageMs = millis() - aiDataMs;
  return true;
}
 
// Formats "<label>: <status>" into out. Stack buffer, no heap (String concatenation
// every 2.5 s churned the heap and fragmented it over long sessions).
static void moduleStatus(char *out, size_t len, const char *label, HealthModule m) {
  if (!modHealth[m].everSeen) { snprintf(out, len, "%s: WAITING (no contact yet)", label); return; }
  uint32_t age = millis() - modHealth[m].lastSeen;
  snprintf(out, len, "%s: %s (%lu ms since last)", label,
           age > MODULE_TIMEOUT_MS ? "TIMEOUT" : "OK", (unsigned long)age);
}
// ------------------------------ //


AsyncWebServer server(80);
AsyncEventSource events("/events");

void handleRoot(AsyncWebServerRequest *request) { request->send_P(200, "text/html", PAGE); }

// ---- Defined in flight_controller_threaded.ino ----
#define FLIGHT_MSG_LEN 128   // must equal LOG_MSG_LEN (checked by static_assert there)
static int getFlightData(char out[][FLIGHT_MSG_LEN], int max);
extern volatile bool armed;
bool requestArm();
void requestDisarm(const char *why);
const char *lastArmRefusal();
void logEvent(const char *fmt, ...);
static int getTemp();

// new below
#define PRESS_MS 150 // try 100-300 if the lamp doesn't respond

void handleToggle(AsyncWebServerRequest *request){
  if(!togglePin){
    request->send(500, "text/plain", "No toggle pin configured");
    return;
  }
  digitalWrite(togglePin, HIGH);
  delay(PRESS_MS);
  digitalWrite(togglePin, LOW);
  ledState = !ledState;
  request->send(200, "text/plain", ledState ? "ON" : "OFF");
}

// Health endpoints. Optional ?port=1 / ?port=2 tells us which port the module is plugged into.
void envHealth(AsyncWebServerRequest *request){
  markHealth(HM_ENV, request);
  request->send(200, "text/plain", "OK");
}

void groveHealth(AsyncWebServerRequest *request){
  markHealth(HM_GROVE, request);
  request->send(200, "text/plain", "OK");
}

void portentaHealth(AsyncWebServerRequest *request){
  markHealth(HM_PORTENTA, request);
  request->send(200, "text/plain", "OK");
}


void reboot(AsyncWebServerRequest *request){
  if (armed) { request->send(409, "text/plain", "Disarm before rebooting"); return; }
  request->send(200, "text/plain", "Rebooting");
  delay(200);   // let the reply go out first
  ESP.restart();
}
// ---- Heap watch ---- //
// Every status tick, compare free internal heap against the value 60 s after boot.
// A real leak shows up here as a steady downward trend; it is logged (always, not
// only in debug mode) each time another HEAP_LEAK_STEP bytes have gone missing.
#define HEAP_BASELINE_MS  60000
#define HEAP_LEAK_STEP    8192
#define HEAP_LOW_BYTES    32768
static void heapWatch() {
  static uint32_t baseline = 0, reported = 0;
  static bool lowWarned = false;
  uint32_t freeNow  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t largest  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (!baseline) {
    if (millis() >= HEAP_BASELINE_MS) baseline = reported = freeNow;
    return;
  }
  if (reported > freeNow && reported - freeNow >= HEAP_LEAK_STEP) {
    reported = freeNow;
    logEvent("[heap] free %lu B (down %lu B since baseline), largest block %lu B",
             (unsigned long)freeNow, (unsigned long)(baseline - freeNow), (unsigned long)largest);
  }
  if (!lowWarned && freeNow < HEAP_LOW_BYTES) {
    lowWarned = true;
    logEvent("[heap] LOW: only %lu B free", (unsigned long)freeNow);
  }
}

void systemStatus(){
  heapWatch();
  if (events.count() == 0) return;   // nobody listening: don't queue anything

  char msg[96];
  snprintf(msg, sizeof msg, "att1: %s, att2: %s", attachmentName(attachments[0]), attachmentName(attachments[1]));
  events.send(msg, "attachments", millis());

  moduleStatus(msg, sizeof msg, "env", HM_ENV);
  events.send(msg, "envHealth", millis());
  moduleStatus(msg, sizeof msg, "grove", HM_GROVE);
  events.send(msg, "groveHealth", millis());
  moduleStatus(msg, sizeof msg, "portenta", HM_PORTENTA);
  events.send(msg, "portentaHealth", millis());

  snprintf(msg, sizeof msg, "temp: %d", getTemp());
  events.send(msg, "temperature", millis());

  // Format once. (Before, this call sat in the loop condition, so the whole report
  // was rebuilt for every single line sent.)
  int n = system_monitor_get_lines(buf, sizeof buf, lines, SYSMON_MAX_LINES);
  for (int j = 0; j < n; j++) events.send(lines[j], "monitoringData", millis());
}
void armESC(AsyncWebServerRequest *request){
  // Goes through the same safety checks as the serial command (IMU alive, level).
  if (requestArm()) request->send(200, "text/plain", "ARMED");
  else request->send(409, "text/plain", String("Arm refused: ") + lastArmRefusal());
}
void disarmESC(AsyncWebServerRequest *request){
  requestDisarm("web");
  request->send(200, "text/plain", "DISARMED");
}

static bool getFloatArg(AsyncWebServerRequest *request, const char *name, float &out){
  if (request->hasParam(name)) {
    out = request->getParam(name)->value().toFloat();
    return true;
  }
  if (request->hasParam(name, true)) {
    out = request->getParam(name, true)->value().toFloat();
    return true;
  }
  return false;
}
 
// AI module reports correction ~1/s:  /acc/ai/data?dx=1.5&dy=-0.8[&type=portenta][&port=1]
// type defaults to grove; pass type=portenta when the Portenta is the sender.
void aiData(AsyncWebServerRequest *request){
  float dx, dy;
  if (!getFloatArg(request, "dx", dx) || !getFloatArg(request, "dy", dy)) {
    request->send(400, "text/plain", "Missing dx or dy");
    return;
  }
  aiDeltaX = dx;
  aiDeltaY = dy;
  aiDataMs = millis();
 
  // A data report also proves the module is alive
  HealthModule m = HM_GROVE;
  if (request->hasParam("type") &&
      request->getParam("type")->value().equalsIgnoreCase("portenta")) {
    m = HM_PORTENTA;
  }
  markHealth(m, request);
 
  // TODO: hand dx/dy to the flight controller here if it should act on them immediately
  request->send(200, "text/plain", "OK");
}



// --- WEB THREAD BELOW --- //

void webApplication(void *pvParameters)
{
  // togglePin 0 means "not configured". Don't drive GPIO0 (a boot strapping pin).
  if (togglePin) {
    pinMode(togglePin, OUTPUT);
    digitalWrite(togglePin, LOW);
  }

  static char webQueue[3][FLIGHT_MSG_LEN];
  int count;
  static uint32_t lastRun = 0;
  static uint32_t lastAttachScan = 0;

#if USE_ACCESS_POINT
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Connect to Wi-Fi '");
  Serial.print(AP_SSID);
  Serial.println("', then open:");
  Serial.println(WiFi.softAPIP()); // 192.168.4.1 by default
#else
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Open in your browser: ");
  Serial.println(WiFi.localIP());
#endif

  server.on("/", handleRoot);

  // ---    Flight Control    --- //
  server.on("/reboot", reboot);
  server.on("/esc/arm", armESC);
  server.on("/esc/disarm", disarmESC);

  // ---    Attachments       --- //
  server.on("/acc/light/toggle", handleToggle);          // toggle light attachment(s)
  server.on("/acc/env/health", envHealth);               // health of environmental module(s)
  server.on("/acc/grove/health", groveHealth);           // health of Grove AI module(s)
  server.on("/acc/portenta/health", portentaHealth);     // health of Portenta AI module(s)
  server.on("/acc/ai/data", aiData);                     // accepts data from ai modules

  server.addHandler(&events);
  server.begin();

  for(;;){

    // Always drain the queue, but only hand messages to the event source when a
    // browser is connected and keeping up. Each SSE client buffers unsent messages
    // on the heap; a stalled client (phone screen off, out of WiFi range) would
    // otherwise sit on a full buffer until its connection times out.
    count = getFlightData(webQueue, 3);
    if (events.count() > 0 && events.avgPacketsWaiting() < 8) {
      for(int i = 0; i < count; i++) {
        events.send(webQueue[i], "flightData", millis());
      }
    }

    if (millis() - lastAttachScan >= ATTACH_SCAN_MS) {
      lastAttachScan = millis();
      checkAttachments();
    }

    if (millis() - lastRun >= 2500) {
      lastRun = millis();
      systemStatus();
    }
    vTaskDelay(10);
  }
}