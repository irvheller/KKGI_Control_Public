#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "PCF8575.h"
#include <Update.h>

const char *ssid = "KKG_and_I";
const char *password = "choochookchoo";

IPAddress local_IP(192, 168, 7, 1);
IPAddress gateway(192, 168, 7, 1);
IPAddress subnet(255, 255, 255, 0);

PCF8575 pcfTrack(0x20);
PCF8575 pcfYard(0x24);
PCF8575 pcfTriple(0x22);

WebServer server(80);

// Keep a stable 16-bit output image for the yard PCF.
static uint16_t yardShadow = 0xFFFF;

// Helpers: PCF pins are active-low drive (0 drives low, 1 releases/high)
static inline void yardPinWrite(uint8_t pin, bool levelHigh)
{
  if (levelHigh) yardShadow |=  (1u << pin);
  else           yardShadow &= ~(1u << pin);
  pcfYard.write16(yardShadow);
}

// ----- Yard PCF mapping (per provided PDF) -----
// UI yard switches are numbered 0..2, which map to Switch 1..3 in the PDF:
// sw=0 => Switch 1, sw=1 => Switch 2, sw=2 => Switch 3
static const uint8_t YARD_IN1_PINS[4]  = { 7, 5, 3, 13 };   // Switch 1/2/3 Input 1
static const uint8_t YARD_IN2_PINS[4]  = { 6, 4, 2, 14 };   // Switch 1/2/3 Input 2
static const uint8_t YARD_OUT_PINS[4]  = { 11, 10, 9, 12 }; // Switch 1/2/3 Output
static const uint8_t YARD_AUX_RELAY_PIN = 8;            // Aux. Relay

// ----- Triple PCF mapping (per provided PDF) -----
// UI triple switches are numbered 0..1, which map to Triple Switch 1..2 in the PDF.
static const uint8_t TRIPLE_IN1_PINS[2]  = { 0, 2 };    // Triple 1/2 In 1
static const uint8_t TRIPLE_IN2_PINS[2]  = { 1, 3 };    // Triple 1/2 In 2
static const uint8_t TRIPLE_OUT1_PINS[2] = { 11, 9 };   // Triple 1 Out 1, Triple 2 Out 1
static const uint8_t TRIPLE_OUT2_PINS[2] = { 10, 8 };   // Triple 1 Out 2, Triple 2 Out 2

// PWM and direction pins
const int pwmPin = 14;
const int dirPin = 27;
const int pwmChannel = 0;
const int pwmFreq = 5000;
const int pwmResolution = 8;

// Track actual motor state so the UI slider can resync to the real setting
static volatile int  motorPWM = 0;        // 0..255
static volatile bool motorDirHigh = true; // true=HIGH, false=LOW

// ---------------- OTA gating (web-based) ----------------
static bool otaEnabled = false;
static uint32_t otaEnabledUntilMs = 0;
static bool shouldReboot = false;

// Exclusive OTA mode (blocks non-OTA endpoints during upload)
static volatile bool otaInProgress = false;

// Track LittleFS mount state so we can safely unmount/remount for FS OTA.
static bool littlefsMounted = false;

static bool otaIsActive()
{
  if (!otaEnabled) return false;
  // Handle millis() rollover safely with signed math
  int32_t remaining = (int32_t)(otaEnabledUntilMs - millis());
  if (remaining <= 0) {
    otaEnabled = false;
    return false;
  }
  return true;
}

static uint32_t otaSecondsLeft()
{
  if (!otaIsActive()) return 0;
  uint32_t msLeft = (uint32_t)(otaEnabledUntilMs - millis());
  return (msLeft + 999) / 1000;
}

static bool rejectIfOtaInProgress()
{
  if (otaInProgress) {
    server.send(503, "text/plain", "OTA update in progress. Please wait.");
    return true;
  }
  return false;
}

// Used by update endpoints
static bool updateOk = true;
static String updateError;

static void ensureLittleFSMounted()
{
  if (!littlefsMounted) {
    littlefsMounted = LittleFS.begin();
    if (!littlefsMounted) {
      Serial.println("LittleFS.begin() failed");
    }
  }
}

static void ensureLittleFSUnmounted()
{
  if (littlefsMounted) {
    LittleFS.end();
    littlefsMounted = false;
  }
}

// Parse Content-Length header if available (requires server.collectHeaders(...))
static size_t getRequestContentLength()
{
  String lenStr = server.header("Content-Length");
  if (lenStr.length() == 0) return 0;
  long v = lenStr.toInt();
  if (v <= 0) return 0;
  return (size_t)v;
}

// --------------------------------------------------------

void serveFile(const String &path, const String &contentType) {
  if (rejectIfOtaInProgress()) return;

  ensureLittleFSMounted();
  File file = LittleFS.open(path, "r");
  if (!file) {
    server.send(404, "text/plain", "File Not Found");
    return;
  }
  server.streamFile(file, contentType);
  file.close();
}

void handleRoot() { serveFile("/index.html", "text/html"); }
void handleInsideHTML() { serveFile("/inside.html", "text/html"); }
void handleImage() { serveFile("/inside.png", "image/png"); }

// Renamed from handlePin() for clarity: this handles track power click requests.
void handleTrackClick() {
  if (rejectIfOtaInProgress()) return;

  if (!server.hasArg("num") || !server.hasArg("state")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }

  int pin = server.arg("num").toInt();
  String state = server.arg("state");
  if (pin < 0 || pin > 15) {
    server.send(400, "text/plain", "Invalid pin number");
    return;
  }

  uint8_t value = (state == "on") ? 0 : 1;
  pcfTrack.write(pin, value);
  server.send(200, "text/plain", "Pin " + String(pin) + " set to " + state);
}

void handleTrackStatus() {
  if (rejectIfOtaInProgress()) return;

  // Return output states for track power pins 8..15
  String json = "[";
  for (int pin = 8; pin <= 15; ++pin) {
    uint8_t v = pcfTrack.read(pin);      // 0 = ON (driven LOW), 1 = OFF (HIGH)
    String state = (v == 0) ? "on" : "off";
    json += "\"" + state + "\"";
    if (pin < 15) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleYardClick() {
  if (rejectIfOtaInProgress()) return;

  if (!server.hasArg("num")) {
    server.send(400, "text/plain", "Missing switch number");
    return;
  }

  int sw = server.arg("num").toInt();
  if (sw < 0 || sw > 3) {
    server.send(400, "text/plain", "Invalid switch number");
    return;
  }

  int outputPin = YARD_OUT_PINS[sw];
  int input2    = YARD_IN2_PINS[sw];

  bool isRed = !pcfYard.read(input2);

  if (isRed) {
    yardPinWrite(YARD_AUX_RELAY_PIN, false); // LOW
    delay(10);
  }

  yardPinWrite(outputPin, false); // LOW
  delay(2000);
  yardPinWrite(outputPin, true);  // HIGH

  if (isRed) {
    yardPinWrite(YARD_AUX_RELAY_PIN, true); // HIGH
  }

  server.send(200, "application/json",
    "{\"status\":\"success\",\"message\":\"Yard switch " + String(sw) + " activated\"}");
}

void handleYardStatus() {
  if (rejectIfOtaInProgress()) return;

  String json = "[";
  for (int i = 0; i < 4; ++i) {
    int input1 = YARD_IN1_PINS[i];
    int input2 = YARD_IN2_PINS[i];

    bool green = !pcfYard.read(input1);
    bool red   = !pcfYard.read(input2);

    String color = "blue";
    if (green) color = "green";
    else if (red) color = "red";

    json += "\"" + color + "\"";
    if (i < 3) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleTripleStatus() {
  if (rejectIfOtaInProgress()) return;

  String json = "[";
  for (int i = 0; i < 2; ++i) {
    bool in1 = !pcfTriple.read(TRIPLE_IN1_PINS[i]);
    bool in2 = !pcfTriple.read(TRIPLE_IN2_PINS[i]);

    String color = "orange";
    if (in1 && in2) color = "blue";
    else if (in1 && !in2) color = "green";
    else if (!in1 && in2) color = "red";

    json += "\"" + color + "\"";
    if (i == 0) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleTripleClick() {
  if (rejectIfOtaInProgress()) return;

  if (!server.hasArg("num")) {
    server.send(400, "text/plain", "Missing switch number");
    return;
  }

  int sw = server.arg("num").toInt();
  if (sw != 0 && sw != 1) {
    server.send(400, "text/plain", "Invalid switch number");
    return;
  }

  bool in1 = !pcfTriple.read(TRIPLE_IN1_PINS[sw]);
  bool in2 = !pcfTriple.read(TRIPLE_IN2_PINS[sw]);

  String color = "orange";
  if (in1 && in2) color = "blue";
  else if (in1 && !in2) color = "green";
  else if (!in1 && in2) color = "red";

  uint8_t out1 = TRIPLE_OUT1_PINS[sw];
  uint8_t out2 = TRIPLE_OUT2_PINS[sw];

  if (color == "blue") {
    yardPinWrite(YARD_AUX_RELAY_PIN, false);
    pcfTriple.write(out1, 0);
    delay(1000);
    pcfTriple.write(out1, 1);
    yardPinWrite(YARD_AUX_RELAY_PIN, true);
  } else if (color == "green") {
    yardPinWrite(YARD_AUX_RELAY_PIN, false);
    pcfTriple.write(out2, 0);
    delay(1000);
    pcfTriple.write(out2, 1);
    yardPinWrite(YARD_AUX_RELAY_PIN, true);
  } else if (color == "red") {
    pcfTriple.write(out1, 0);
    pcfTriple.write(out2, 0);
    delay(1000);
    pcfTriple.write(out1, 1);
    pcfTriple.write(out2, 1);
  }

  server.send(200, "application/json",
    "{\"status\":\"success\",\"message\":\"Triple switch " + String(sw) + " activated (" + color + ")\"}");
}

void handleMotorSpeed() {
  if (rejectIfOtaInProgress()) return;

  if (!server.hasArg("val")) {
    server.send(400, "text/plain", "Missing val");
    return;
  }
  int val = server.arg("val").toInt();
  val = constrain(val, 0, 255);
  ledcWrite(pwmChannel, val);
  motorPWM = val;
  Serial.println("PWM set to " + String(val));
  server.send(200, "text/plain", "PWM set to " + String(val));
}

void handleMotorDirection() {
  if (rejectIfOtaInProgress()) return;

  if (!server.hasArg("state")) {
    server.send(400, "text/plain", "Missing state");
    return;
  }
  String state = server.arg("state");
  if (state == "high") {
    digitalWrite(dirPin, HIGH);
    motorDirHigh = true;
  } else {
    digitalWrite(dirPin, LOW);
    motorDirHigh = false;
  }
  server.send(200, "text/plain", "Direction set to " + state);
}

// NEW: Report actual motor PWM + direction so the UI slider can resync safely
void handleMotorStatus() {
  if (rejectIfOtaInProgress()) return;

  String json = "{";
  json += "\"pwm\":" + String((int)motorPWM) + ",";
  json += "\"dir\":\"" + String(motorDirHigh ? "high" : "low") + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// ---------------- OTA endpoints ----------------

void handleOTAEnable() {
  otaEnabled = true;
  otaEnabledUntilMs = millis() + 60000UL;

  String json = "{";
  json += "\"enabled\":true,";
  json += "\"seconds_left\":" + String(otaSecondsLeft()) + ",";
  json += "\"in_progress\":" + String(otaInProgress ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleOTACancel() {
  otaEnabled = false;

  String json = "{";
  json += "\"enabled\":false,";
  json += "\"seconds_left\":0,";
  json += "\"in_progress\":" + String(otaInProgress ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleOTAStatus() {
  bool active = otaIsActive();
  String json = "{";
  json += "\"enabled\":" + String(active ? "true" : "false") + ",";
  json += "\"seconds_left\":" + String(active ? otaSecondsLeft() : 0) + ",";
  json += "\"in_progress\":" + String(otaInProgress ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleOTAPage() {
  if (!otaIsActive()) {
    server.send(403, "text/plain", "OTA is disabled. Enable it from the index page for 60 seconds.");
    return;
  }

  String html;
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>OTA Update</title>";
  html += "<style>body{font-family:Arial;text-align:center;padding:20px;} ";
  html += "form{margin:16px auto; padding:16px; border:1px solid #ccc; border-radius:8px; max-width:520px;} ";
  html += "button{font-size:18px;padding:10px 18px;border:0;border-radius:6px;cursor:pointer;} ";
  html += ".btn{background:#2f6fab;color:#fff;} .btn:hover{background:#285f93;} ";
  html += ".danger{background:#b33a3a;color:#fff;} .danger:hover{background:#9e3232;} ";
  html += "</style></head><body>";
  html += "<h1>OTA Update</h1>";
  html += "<p><b>Enabled:</b> " + String(otaSecondsLeft()) + "s remaining</p>";
  html += "<p><b>Status:</b> " + String(otaInProgress ? "UPLOAD IN PROGRESS" : "Idle") + "</p>";

  html += "<form method='POST' action='/update_fw' enctype='multipart/form-data'>";
  html += "<h2>Firmware (.bin)</h2>";
  html += "<input type='file' name='update' accept='.bin' required><br><br>";
  html += "<button class='btn' type='submit'>Upload Firmware</button>";
  html += "</form>";

  html += "<form method='POST' action='/update_fs' enctype='multipart/form-data'>";
  html += "<h2>Filesystem image (LittleFS)</h2>";
  html += "<p style='margin-top:-6px;'>Upload <b>littlefs.bin</b> built by PlatformIO.</p>";
  html += "<input type='file' name='update' required><br><br>";
  html += "<button class='btn' type='submit'>Upload Filesystem</button>";
  html += "</form>";

  html += "<form method='POST' action='/ota_cancel'>";
  html += "<button class='danger' type='submit'>Cancel OTA Now</button>";
  html += "</form>";

  html += "<p><a href='/'>Back to index</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

static void otaRequireEnabledOr403() {
  if (!otaIsActive()) {
    server.send(403, "text/plain", "OTA is disabled. Enable it from the index page for 60 seconds.");
  }
}

// Firmware upload handler
void handleUpdateFirmwareUpload() {
  if (!otaIsActive()) return;

  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    updateOk = true;
    updateError = "";
    Serial.printf("OTA FW: Upload start: %s\n", upload.filename.c_str());

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      updateOk = false;
      updateError = Update.errorString();
      Serial.printf("OTA FW: Update.begin failed: %s\n", updateError.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (updateOk) {
      size_t written = Update.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        updateOk = false;
        updateError = Update.errorString();
        Serial.printf("OTA FW: Update.write failed: %s\n", updateError.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (updateOk) {
      if (!Update.end(true)) {
        updateOk = false;
        updateError = Update.errorString();
        Serial.printf("OTA FW: Update.end failed: %s\n", updateError.c_str());
      }
    }
    Serial.printf("OTA FW: Upload end. Size: %u\n", (unsigned)upload.totalSize);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    updateOk = false;
    updateError = "Upload aborted";
    Serial.println("OTA FW: Upload aborted");
    otaInProgress = false;
  }
}

// Filesystem upload handler (writes to FS partition; constant name is U_SPIFFS in Arduino-ESP32)
void handleUpdateFSUpload() {
  if (!otaIsActive()) return;

  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    updateOk = true;
    updateError = "";

    Serial.printf("OTA FS: Upload start: %s\n", upload.filename.c_str());

    // CRITICAL: unmount LittleFS before writing the filesystem partition
    ensureLittleFSUnmounted();

    // Log header for debugging, but DO NOT trust it for multipart uploads
    size_t headerLen = getRequestContentLength();
    Serial.printf("OTA FS: Content-Length (header) = %u (not trusted)\n", (unsigned)headerLen);

    // IMPORTANT: use unknown size, because multipart Content-Length includes extra bytes
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
      updateOk = false;
      updateError = Update.errorString();
      Serial.printf("OTA FS: Update.begin failed: %s\n", updateError.c_str());
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (updateOk) {
      size_t written = Update.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        updateOk = false;
        updateError = Update.errorString();
        Serial.printf("OTA FS: Update.write failed: %s\n", updateError.c_str());
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (updateOk) {
      if (!Update.end(true)) {
        updateOk = false;
        updateError = Update.errorString();
        Serial.printf("OTA FS: Update.end failed: %s\n", updateError.c_str());
      }
    }
    Serial.printf("OTA FS: Upload end. Size: %u\n", (unsigned)upload.totalSize);
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    updateOk = false;
    updateError = "Upload aborted";
    Serial.println("OTA FS: Upload aborted");

    // Try to remount so the UI can recover without a reboot.
    ensureLittleFSMounted();

    otaInProgress = false;
  }
}

// ---------------- end OTA endpoints ----------------

void setup() {
  Serial.begin(115200);
  Wire.begin();
  pcfTrack.begin();
  pcfYard.begin();
  pcfTriple.begin();

  for (uint8_t i = 0; i < 16; ++i) pcfTrack.write(i, 1);

  yardShadow = 0xFFFF;
  pcfYard.write16(yardShadow);

  for (uint8_t i = 0; i <= 11; ++i) {
    pcfTriple.write(i, 1);
  }

  ledcSetup(pwmChannel, pwmFreq, pwmResolution);
  ledcAttachPin(pwmPin, pwmChannel);
  ledcWrite(pwmChannel, 0);
  motorPWM = 0;

  pinMode(dirPin, OUTPUT);
  digitalWrite(dirPin, HIGH);
  motorDirHigh = true;

  littlefsMounted = LittleFS.begin();
  if (!littlefsMounted) {
    Serial.println("LittleFS.begin() failed");
  }

  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssid, password);

  // Collect Content-Length so server.header("Content-Length") works
  const char* headerKeys[] = {"Content-Length"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", handleRoot);
  server.on("/inside.html", handleInsideHTML);
  server.on("/inside.png", handleImage);

  server.on("/track_click", handleTrackClick);
  server.on("/track_status", handleTrackStatus);
  server.on("/yard_click", handleYardClick);
  server.on("/yard_status", handleYardStatus);
  server.on("/triple_status", handleTripleStatus);
  server.on("/triple_click", handleTripleClick);
  server.on("/motor_speed", handleMotorSpeed);
  server.on("/motor_dir", handleMotorDirection);
  server.on("/motor_status", handleMotorStatus); // NEW

  server.on("/ota_enable", HTTP_POST, handleOTAEnable);
  server.on("/ota_cancel", HTTP_POST, handleOTACancel);
  server.on("/ota_status", HTTP_GET, handleOTAStatus);
  server.on("/ota", HTTP_GET, handleOTAPage);

  server.on("/update_fw", HTTP_POST,
    []() {
      if (!otaIsActive()) { otaRequireEnabledOr403(); return; }
      if (!updateOk) {
        otaInProgress = false;
        server.send(500, "text/plain", "Firmware update failed: " + updateError);
        return;
      }
      server.send(200, "text/plain", "Firmware update OK. Rebooting...");
      shouldReboot = true;
    },
    handleUpdateFirmwareUpload
  );

  server.on("/update_fs", HTTP_POST,
    []() {
      if (!otaIsActive()) { otaRequireEnabledOr403(); return; }
      if (!updateOk) {
        ensureLittleFSMounted();
        otaInProgress = false;
        server.send(500, "text/plain", "Filesystem update failed: " + updateError);
        return;
      }
      server.send(200, "text/plain", "Filesystem update OK. Rebooting...");
      shouldReboot = true;
    },
    handleUpdateFSUpload
  );

  server.onNotFound([]() {
    if (otaInProgress) {
      server.send(503, "text/plain", "OTA update in progress. Please wait.");
      return;
    }
    server.send(404, "text/plain", "404 Not Found");
  });

  server.begin();
  Serial.println("Server started");
}

void loop() {
  otaIsActive();
  server.handleClient();

  if (shouldReboot) {
    delay(250);
    ESP.restart();
  }
}
