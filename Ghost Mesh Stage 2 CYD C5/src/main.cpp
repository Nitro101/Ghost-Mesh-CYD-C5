/***************************************************************
 * GHOST L2 - STAGE 2 CYD CONSOLE
 * Full PlatformIO firmware with automatic touch calibration
 * Modern security console theme
 ***************************************************************/




#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include <LovyanGFX.hpp>




static uint16_t cydRGB(uint8_t r, uint8_t g, uint8_t b);




#define GHOST_L2_VERSION 3
#define GHOST_MAGIC 0x324C4747UL
#define GHOST_UDP_PORT 4210




#define MAX_MESHES 5
#define MAX_NODES 32
#define MAX_SENSORS 16




#define DEFAULT_MESH_CHANNEL 1
#define DEFAULT_LOSS_TIMEOUT 10
#define DEFAULT_AP_PASSWORD "Stage1Config123"
#define DEFAULT_OTA_PASSWORD "Stage1Config123"
#define DEFAULT_MESH_KEY "CHANGE-MESH-KEY"




#define NODE_TYPE_UNKNOWN 0
#define NODE_TYPE_RADAR 1
#define NODE_TYPE_SENSOR 2
#define NODE_TYPE_COMBINED 3




#define PKT_HELLO 1
#define PKT_SENSOR 2
#define PKT_RADAR 3
#define PKT_TAMPER 4
#define PKT_HEARTBEAT 5
#define PKT_ALARM 6




#define ALARM_RADAR 0x01
#define ALARM_TAMPER 0x02
#define ALARM_SENSOR_FAULT 0x04
#define ALARM_LOW_BATTERY 0x08




#define SENSOR_TEMP 0
#define SENSOR_HUMIDITY 1
#define SENSOR_PRESSURE 2
#define SENSOR_TDS 3
#define SENSOR_EC 4
#define SENSOR_PH 5
#define SENSOR_SOIL 6
#define SENSOR_BATTERY 7
#define SENSOR_LIGHT 8
#define SENSOR_WATER_LEVEL 9
#define SENSOR_FLOW 10
#define SENSOR_CO2 11
#define SENSOR_ORP 12
#define SENSOR_RAIN 13
#define SENSOR_WIND 14
#define SENSOR_CUSTOM 15




#define TFT_SCLK 6
#define TFT_MISO 2
#define TFT_MOSI 7
#define TFT_CS 23
#define TFT_DC 24
#define TFT_BL 25
#define BUZZER_PIN 28




class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_SPI bus;
  lgfx::Panel_ST7789 panel;
  lgfx::Touch_XPT2046 touch;




  LGFX(void) {
    {
      auto cfg = bus.config();




      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = TFT_SCLK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_dc = TFT_DC;




      bus.config(cfg);
      panel.setBus(&bus);
    }




    {
      auto cfg = panel.config();




      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;




      panel.config(cfg);
    }




    {
      auto cfg = touch.config();




      cfg.x_min = 200;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3900;
      cfg.pin_int = -1;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = TFT_SCLK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_cs = 1;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;




      touch.config(cfg);
    }




    setPanel(&panel);
    panel.setTouch(&touch);
  }
};




LGFX tft;
static lgfx::LGFX_Sprite cydCanvas(&tft);
bool cydCanvasReady = false;




WebServer web(80);
WiFiUDP udp;
Preferences prefs;




struct __attribute__((packed)) RadarTarget {
  int16_t x;
  int16_t y;
  int16_t speed;
  uint16_t distanceCm;
  int16_t angle10;
};




struct __attribute__((packed)) GhostPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t packetType;
  uint8_t nodeType;
  uint8_t meshId;
  uint8_t nodeId;
  uint8_t expectedNodes;
  uint32_t sequence;
  uint32_t uptime;
  uint32_t auth;
  uint32_t sensorMask;
  uint8_t alarmFlags;
  uint8_t targetCount;
  uint8_t links;
  uint8_t reserved;
  float sensor[MAX_SENSORS];
  RadarTarget target[2];
};




static_assert(sizeof(GhostPacket) <= 250, "GhostPacket too large for ESP-NOW");




struct __attribute__((packed)) LegacyRadarPacket {
  uint8_t magic;
  uint8_t version;
  uint8_t mesh;
  uint8_t node;
  uint32_t msg;
  uint32_t seq;
  uint8_t type;
  uint8_t ttl;
  uint8_t target;
  int16_t x;
  int16_t y;
  int16_t speed;
  uint16_t distanceCm;
  int16_t angle10;
  uint8_t links;
  uint32_t auth;
};




static_assert(sizeof(LegacyRadarPacket) == 30, "Legacy packet must be 30 bytes");




struct MeshConfig {
  char name[21];
  char key[33];
  bool enabled;
  bool alarmEnabled;
  char expectedIds[100];
  uint8_t expectedCount;
  uint16_t lossTimeout;
};




struct WifiConfig {
  char ssid[33];
  char pass[65];
};




MeshConfig meshes[MAX_MESHES];
WifiConfig wifiProfiles[5];




char consoleName[25] = "Ghost L2 Stage 2";
char apPassword[33] = DEFAULT_AP_PASSWORD;
char otaPassword[33] = DEFAULT_OTA_PASSWORD;
uint8_t meshChannel = DEFAULT_MESH_CHANNEL;




uint32_t armDelaySeconds = 30;




struct NodeState {
  bool seen;
  bool lossAlarm;
  bool radarAlarm;
  bool tamperAlarm;
  bool sensorFault;
  bool lowBattery;
  uint8_t nodeType;
  uint8_t targetCount;
  uint8_t alarmFlags;
  uint32_t lastSequence;
  uint32_t lastSeenMs;
  uint32_t packets;
  uint32_t sensorMask;
  float sensor[MAX_SENSORS];
  RadarTarget target[2];
};




NodeState nodes[MAX_MESHES][MAX_NODES + 1];




#define RX_QUEUE_SIZE 12
GhostPacket rxQueue[RX_QUEUE_SIZE];
volatile uint8_t rxHead = 0;
volatile uint8_t rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;




volatile uint32_t espNowRxCount = 0;
volatile uint32_t espNowBadLength = 0;
volatile int espNowLastLength = 0;




bool espNowOK = false;
bool wifiOK = false;
bool udpOK = false;
bool otaOK = false;
uint8_t currentWifi = 0;




bool globalAlarm = false;
String globalAlarmText = "SYSTEM OK";




uint32_t lastAlarmBeep = 0;
uint32_t lastDisplayUpdate = 0;
uint32_t lastWifiAttempt = 0;




uint8_t cydPage = 0;
bool cydArmed = false;




bool cydArming = false;
uint32_t cydArmingUntil = 0;




bool cydPinModal = false;
String cydPinEntry = "";
char cydControlPin[13] = "2468";
uint8_t cydPinAction = 0;
String cydPinMessage = "";
uint32_t cydPinMessageUntil = 0;




bool cydTouchDown = false;
uint32_t cydLastTouchMs = 0;




bool cydInverted = false;




// ============================================================
// ARM MODES
// ============================================================




uint8_t cydArmMode = 0;
bool cydModeSelect = false;




// ============================================================
// PERMANENT SOFT ALARMS - 5 SECTIONS
// ============================================================




struct ConstantSoftSection {
  uint8_t meshId;
  char nodeIds[100];
  bool enabled;
};




ConstantSoftSection constantSoftSections[MAX_MESHES];




// ============================================================
// STAGE 2 BRIDGE
// ============================================================




uint8_t bridgeMac[6] = {0, 0, 0, 0, 0, 0};
bool bridgeConfigured = false;




// ============================================================
// STAGE 3 TEST FLAGS
// ============================================================




bool testLoudAlarm = false;
bool testSoftAlarm = false;
bool testTransmissionAlarm = false;
bool testReactionAlarm = false;




String webControlMessage = "";
uint32_t webControlMessageUntil = 0;




int cydSelectedMesh = -1;
int cydSelectedNode = -1;




struct TouchCalibration {
  uint32_t magic;
  uint16_t xMin;
  uint16_t xMax;
  uint16_t yMin;
  uint16_t yMax;
  uint8_t mapMode;
};




TouchCalibration touchCal;
bool touchCalValid = false;
bool forceTouchCalibration = false;
const uint32_t TOUCH_CAL_MAGIC = 0x54434831UL;




uint32_t fnv1a(const uint8_t *data, size_t len) {
  uint32_t h = 2166136261UL;




  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619UL;
  }




  return h;
}




uint32_t packetAuth(GhostPacket p, const char *key) {
  p.auth = 0;
  uint32_t h = fnv1a((uint8_t *)&p, sizeof(p));
  h ^= fnv1a((uint8_t *)key, strlen(key));
  return h;
}




uint32_t legacyAuth(LegacyRadarPacket p, const char *key) {
  p.auth = 0;
  uint32_t h = fnv1a((uint8_t *)&p, sizeof(p));
  h ^= fnv1a((uint8_t *)key, strlen(key));
  return h;
}




void copyString(char *dest, size_t destSize, const String src) {
  if (destSize == 0) return;
  strncpy(dest, src.c_str(), destSize - 1);
  dest[destSize - 1] = 0;
}




String htmlEscape(const String s) {
  String out;




  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];




    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '\"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }




  return out;
}




String compiledDisplayName() {
  return "NM-CYD-C5 240x320 TFT";
}




void defaults() {
  memset(meshes, 0, sizeof(meshes));
  memset(wifiProfiles, 0, sizeof(wifiProfiles));
  memset(nodes, 0, sizeof(nodes));




  for (int i = 0; i < MAX_MESHES; i++) {
    snprintf(meshes[i].name, sizeof(meshes[i].name), "MESH %d", i + 1);
    copyString(meshes[i].key, sizeof(meshes[i].key), DEFAULT_MESH_KEY);
    meshes[i].enabled = (i == 0);
    meshes[i].alarmEnabled = true;
    meshes[i].expectedIds[0] = 0;
    meshes[i].expectedCount = 0;
    meshes[i].lossTimeout = DEFAULT_LOSS_TIMEOUT;
  }




  copyString(apPassword, sizeof(apPassword), DEFAULT_AP_PASSWORD);
  copyString(otaPassword, sizeof(otaPassword), DEFAULT_OTA_PASSWORD);
  copyString(consoleName, sizeof(consoleName), "Ghost L2 Stage 2");




  meshChannel = DEFAULT_MESH_CHANNEL;
  armDelaySeconds = 30;




  for (int i = 0; i < MAX_MESHES; i++) {
    constantSoftSections[i].meshId = 0;
    constantSoftSections[i].nodeIds[0] = 0;
    constantSoftSections[i].enabled = false;
  }
}




bool expectedNode(uint8_t meshIndex, uint8_t nodeId) {
  if (meshIndex >= MAX_MESHES || nodeId == 0 || nodeId > MAX_NODES) {
    return false;
  }




  String q = String(meshes[meshIndex].expectedIds);




  if (q.length() == 0) {
    return false;
  }




  int start = 0;




  while (start < int(q.length())) {
    int comma = q.indexOf(',', start);




    if (comma < 0) {
      comma = q.length();
    }




    String item = q.substring(start, comma);
    item.trim();




    if (item.toInt() == nodeId) {
      return true;
    }




    start = comma + 1;
  }




  return false;
}




uint8_t countExpected(uint8_t meshIndex) {
  if (meshIndex >= MAX_MESHES) {
    return 0;
  }




  uint8_t count = 0;
  String q = String(meshes[meshIndex].expectedIds);
  int start = 0;




  while (start < int(q.length()) && count < MAX_NODES) {
    int comma = q.indexOf(',', start);




    if (comma < 0) {
      comma = q.length();
    }




    String item = q.substring(start, comma);
    item.trim();




    int id = item.toInt();




    if (id >= 1 && id <= MAX_NODES) {
      count++;
    }




    start = comma + 1;
  }




  return count;
}




// ============================================================
// LOAD / SAVE CONFIG
// ============================================================




void loadConfig() {
  defaults();




  prefs.begin("ghost2", true);




  String s = prefs.getString("console", consoleName);
  copyString(consoleName, sizeof(consoleName), s);




  s = prefs.getString("appass", apPassword);
  copyString(apPassword, sizeof(apPassword), s);




  s = prefs.getString("otapass", otaPassword);
  copyString(otaPassword, sizeof(otaPassword), s);




  meshChannel = prefs.getUChar("channel", DEFAULT_MESH_CHANNEL);




  if (meshChannel < 1 || meshChannel > 13) {
    meshChannel = DEFAULT_MESH_CHANNEL;
  }




  armDelaySeconds = prefs.getUInt("armdelay", 30);




  if (armDelaySeconds > 300) {
    armDelaySeconds = 300;
  }




  for (int i = 0; i < MAX_MESHES; i++) {
    String meshKey = "softmesh" + String(i);
    String nodeKey = "softnodes" + String(i);
    String enabledKey = "softenabled" + String(i);




    constantSoftSections[i].meshId = prefs.getUChar(
      meshKey.c_str(),
      0
    );




    if (constantSoftSections[i].meshId > MAX_MESHES) {
      constantSoftSections[i].meshId = 0;
    }




    String savedNodes = prefs.getString(nodeKey.c_str(), "");




    savedNodes.toCharArray(
      constantSoftSections[i].nodeIds,
      sizeof(constantSoftSections[i].nodeIds)
    );




    constantSoftSections[i].enabled = prefs.getBool(
      enabledKey.c_str(),
      false
    );
  }




  String savedBridgeMac = prefs.getString("bridgemac", "");




  if (savedBridgeMac.length() == 17) {
    int v[6];




    if (
      sscanf(
        savedBridgeMac.c_str(),
        "%x:%x:%x:%x:%x:%x",
        &v[0], &v[1], &v[2],
        &v[3], &v[4], &v[5]
      ) == 6
    ) {
      for (int i = 0; i < 6; i++) {
        bridgeMac[i] = v[i];
      }




      bridgeConfigured = true;
    }
  }




  for (int i = 0; i < MAX_MESHES; i++) {
    String k = "m" + String(i);




    s = prefs.getString((k + "n").c_str(), meshes[i].name);
    copyString(meshes[i].name, sizeof(meshes[i].name), s);




    s = prefs.getString((k + "k").c_str(), meshes[i].key);
    copyString(meshes[i].key, sizeof(meshes[i].key), s);




    meshes[i].enabled = prefs.getBool((k + "e").c_str(), meshes[i].enabled);
    meshes[i].alarmEnabled = prefs.getBool((k + "a").c_str(), meshes[i].alarmEnabled);




    s = prefs.getString((k + "ids").c_str(), "");
    copyString(meshes[i].expectedIds, sizeof(meshes[i].expectedIds), s);




    meshes[i].expectedCount = countExpected(i);




    meshes[i].lossTimeout = prefs.getUShort(
      (k + "to").c_str(),
      DEFAULT_LOSS_TIMEOUT
    );




    if (meshes[i].lossTimeout < 2 || meshes[i].lossTimeout > 3600) {
      meshes[i].lossTimeout = DEFAULT_LOSS_TIMEOUT;
    }
  }




  for (int i = 0; i < 5; i++) {
    String k = "w" + String(i);




    s = prefs.getString((k + "s").c_str(), "");
    copyString(wifiProfiles[i].ssid, sizeof(wifiProfiles[i].ssid), s);




    s = prefs.getString((k + "p").c_str(), "");
    copyString(wifiProfiles[i].pass, sizeof(wifiProfiles[i].pass), s);
  }




  prefs.end();
}




void saveConfig() {
  prefs.begin("ghost2", false);




  prefs.putString("console", consoleName);
  prefs.putString("appass", apPassword);
  prefs.putString("otapass", otaPassword);
  prefs.putUChar("channel", meshChannel);
  prefs.putUInt("armdelay", armDelaySeconds);




  for (int i = 0; i < MAX_MESHES; i++) {
    String meshKey = "softmesh" + String(i);
    String nodeKey = "softnodes" + String(i);
    String enabledKey = "softenabled" + String(i);




    prefs.putUChar(
      meshKey.c_str(),
      constantSoftSections[i].meshId
    );




    prefs.putString(
      nodeKey.c_str(),
      String(constantSoftSections[i].nodeIds)
    );




    prefs.putBool(
      enabledKey.c_str(),
      constantSoftSections[i].enabled
    );
  }




  for (int i = 0; i < MAX_MESHES; i++) {
    String k = "m" + String(i);




    prefs.putString((k + "n").c_str(), meshes[i].name);
    prefs.putString((k + "k").c_str(), meshes[i].key);
    prefs.putBool((k + "e").c_str(), meshes[i].enabled);
    prefs.putBool((k + "a").c_str(), meshes[i].alarmEnabled);
    prefs.putString((k + "ids").c_str(), meshes[i].expectedIds);
    prefs.putUShort((k + "to").c_str(), meshes[i].lossTimeout);
  }




  for (int i = 0; i < 5; i++) {
    String k = "w" + String(i);




    prefs.putString((k + "s").c_str(), wifiProfiles[i].ssid);
    prefs.putString((k + "p").c_str(), wifiProfiles[i].pass);
  }




  prefs.end();
}




// ============================================================
// ALARM
// ============================================================




void beepShort() {
  tone(BUZZER_PIN, 2600, 120);
}




void beepCritical() {
  tone(BUZZER_PIN, 3200, 220);
  delay(50);
  tone(BUZZER_PIN, 3200, 220);
}




void setAlarm(const String text, bool critical) {
  globalAlarm = true;
  globalAlarmText = text;




  if (critical) {
    beepCritical();
  } else {
    beepShort();
  }
}




void clearAlarms() {
  globalAlarm = false;
  globalAlarmText = "SYSTEM OK";




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      nodes[m][n].radarAlarm = false;
      nodes[m][n].tamperAlarm = false;
      nodes[m][n].lossAlarm = false;
      nodes[m][n].sensorFault = false;
      nodes[m][n].lowBattery = false;
      nodes[m][n].alarmFlags = 0;
    }
  }
}




// ============================================================
// AUTOMATIC TOUCH CALIBRATION
// ============================================================




bool touchCalibrated() {
  return touchCalValid;
}




void drawCalibrationScreen(
  const char *line1,
  const char *line2,
  int targetX,
  int targetY
) {
  cydCanvas.fillScreen(cydRGB(3, 4, 6));
  cydCanvas.fillRoundRect(20, 45, 280, 80, 10, cydRGB(13, 15, 18));
  cydCanvas.drawRoundRect(20, 45, 280, 80, 10, cydRGB(34, 211, 238));




  cydCanvas.setTextSize(2);
  cydCanvas.setTextColor(cydRGB(34, 211, 238), cydRGB(18, 27, 44));
  cydCanvas.setCursor(35, 62);
  cydCanvas.print("TOUCH CALIBRATION");




  cydCanvas.setTextSize(1);
  cydCanvas.setTextColor(cydRGB(229, 231, 235), cydRGB(18, 27, 44));
  cydCanvas.setCursor(35, 90);
  cydCanvas.print(line1);




  cydCanvas.setTextColor(cydRGB(148, 163, 184), cydRGB(18, 27, 44));
  cydCanvas.setCursor(35, 105);
  cydCanvas.print(line2);




  if (targetX >= 0) {
    cydCanvas.fillCircle(targetX, targetY, 10, cydRGB(248, 113, 113));
    cydCanvas.drawCircle(targetX, targetY, 16, cydRGB(34, 211, 238));
  }




  if (cydCanvasReady) {
    cydCanvas.pushSprite(0, 0);
  }
}




bool waitStableRawTouch(uint16_t &rawX, uint16_t &rawY) {
  uint32_t firstSeen = 0;
  uint16_t lastX = 0;
  uint16_t lastY = 0;




  while (true) {
    uint16_t x = 0;
    uint16_t y = 0;




    if (!tft.getTouchRaw(&x, &y)) {
      firstSeen = 0;
      delay(10);
      continue;
    }




    if (x < 100 || x > 4000 || y < 100 || y > 4000) {
      firstSeen = 0;
      delay(10);
      continue;
    }




    if (firstSeen == 0) {
      firstSeen = millis();
      lastX = x;
      lastY = y;
      delay(10);
      continue;
    }




    if (abs(int(x) - int(lastX)) > 40 || abs(int(y) - int(lastY)) > 40) {
      firstSeen = millis();
      lastX = x;
      lastY = y;
      delay(10);
      continue;
    }




    if (millis() - firstSeen >= 250) {
      rawX = x;
      rawY = y;




      while (tft.getTouchRaw(&x, &y)) {
        delay(10);
      }




      return true;
    }




    delay(10);
  }
}




bool calibrateTouchCorner(
  const char *line1,
  const char *line2,
  int targetX,
  int targetY,
  uint16_t &rawX,
  uint16_t &rawY
) {
  drawCalibrationScreen(line1, line2, targetX, targetY);




  uint16_t x = 0;
  uint16_t y = 0;




  if (!waitStableRawTouch(x, y)) {
    return false;
  }




  rawX = x;
  rawY = y;




  Serial.printf(
    "CALIBRATION: %s raw=%u,%u\n",
    line1,
    x,
    y
  );




  return true;
}




void saveTouchCalibration() {
  prefs.begin("ghost2", false);
  prefs.putBytes("touchcal", &touchCal, sizeof(touchCal));
  prefs.end();




  touchCalValid = true;




  Serial.printf(
    "TOUCH CALIBRATION SAVED X:%u-%u Y:%u-%u mode:%u\n",
    touchCal.xMin,
    touchCal.xMax,
    touchCal.yMin,
    touchCal.yMax,
    touchCal.mapMode
  );
}




void loadTouchCalibration() {
  touchCalValid = false;
  memset(&touchCal, 0, sizeof(touchCal));




  prefs.begin("ghost2", true);




  if (prefs.getBytesLength("touchcal") == sizeof(TouchCalibration)) {
    prefs.getBytes("touchcal", &touchCal, sizeof(TouchCalibration));
  }




  prefs.end();




  touchCalValid = (
    touchCal.magic == TOUCH_CAL_MAGIC &&
    touchCal.xMax - touchCal.xMin > 500 &&
    touchCal.yMax - touchCal.yMin > 500
  );




  Serial.printf(
    "TOUCH CALIBRATION %s\n",
    touchCalValid ? "LOADED" : "NOT FOUND"
  );
}




bool runTouchCalibration() {
  uint16_t rawTLx = 0, rawTLy = 0;
  uint16_t rawTRx = 0, rawTRy = 0;
  uint16_t rawBLx = 0, rawBLy = 0;
  uint16_t rawBRx = 0, rawBRy = 0;




  drawCalibrationScreen(
    "Touch each red target",
    "Use a stylus if possible",
    -1,
    -1
  );




  delay(1200);




  if (!calibrateTouchCorner(
    "TOUCH TOP-LEFT CORNER",
    "Hold until accepted",
    25,
    45,
    rawTLx,
    rawTLy
  )) {
    return false;
  }




  if (!calibrateTouchCorner(
    "TOUCH TOP-RIGHT CORNER",
    "Hold until accepted",
    294,
    45,
    rawTRx,
    rawTRy
  )) {
    return false;
  }




  if (!calibrateTouchCorner(
    "TOUCH BOTTOM-LEFT CORNER",
    "Hold until accepted",
    25,
    214,
    rawBLx,
    rawBLy
  )) {
    return false;
  }




  if (!calibrateTouchCorner(
    "TOUCH BOTTOM-RIGHT CORNER",
    "Hold until accepted",
    294,
    214,
    rawBRx,
    rawBRy
  )) {
    return false;
  }




  touchCal.magic = TOUCH_CAL_MAGIC;
  touchCal.xMin = min(rawTLx, rawBLx);
  touchCal.xMax = max(rawTRx, rawBRx);
  touchCal.yMin = min(rawTLy, rawTRy);
  touchCal.yMax = max(rawBLx, rawBRy);
  touchCal.mapMode = 0;




  if (
    touchCal.xMax - touchCal.xMin < 500 ||
    touchCal.yMax - touchCal.yMin < 500
  ) {
    Serial.println("CALIBRATION FAILED: touch range too small");
    return false;
  }




  saveTouchCalibration();




  drawCalibrationScreen(
    "CALIBRATION SAVED",
    "Starting Ghost L2...",
    -1,
    -1
  );




  delay(1200);




  return true;
}




bool getCalibratedTouch(int16_t &tx, int16_t &ty) {
  if (!touchCalibrated()) {
    return false;
  }




  uint16_t rx = 0;
  uint16_t ry = 0;




  if (!tft.getTouchRaw(&rx, &ry)) {
    return false;
  }




  if (touchCal.mapMode == 0) {
    tx = map(ry, touchCal.yMin, touchCal.yMax, 0, 319);
    ty = map(rx, touchCal.xMin, touchCal.xMax, 0, 239);
  } else if (touchCal.mapMode == 1) {
    tx = map(rx, touchCal.xMin, touchCal.xMax, 0, 319);
    ty = map(ry, touchCal.yMin, touchCal.yMax, 0, 239);
  } else {
    tx = map(ry, touchCal.yMax, touchCal.yMin, 0, 319);
    ty = map(rx, touchCal.xMax, touchCal.xMin, 0, 239);
  }




  tx = constrain(tx, 0, 319);
  ty = constrain(ty, 0, 239);




  return true;
}
// ============================================================
// PACKET VALIDATION
// ============================================================



bool verifyGhostPacket(GhostPacket p) {
  if (p.magic != GHOST_MAGIC) {
    return false;
  }



  if (p.version != GHOST_L2_VERSION) {
    return false;
  }



  if (p.meshId < 1 || p.meshId > MAX_MESHES) {
    return false;
  }



  if (p.nodeId < 1 || p.nodeId > MAX_NODES) {
    return false;
  }



  if (!meshes[p.meshId - 1].enabled) {
    return false;
  }



  uint32_t received = p.auth;
  p.auth = 0;



  uint32_t calculated = packetAuth(p, meshes[p.meshId - 1].key);



  p.auth = received;



  return received == calculated;
}



bool verifyLegacyPacket(LegacyRadarPacket p) {
  if (p.magic != 0xA7) {
    return false;
  }



  if (p.version != 1) {
    return false;
  }



  if (p.mesh < 1 || p.mesh > MAX_MESHES) {
    return false;
  }



  if (p.node < 1 || p.node > MAX_NODES) {
    return false;
  }



  if (!meshes[p.mesh - 1].enabled) {
    return false;
  }



  uint32_t received = p.auth;
  p.auth = 0;



  uint32_t calculated = legacyAuth(p, meshes[p.mesh - 1].key);



  p.auth = received;



  return received == calculated;
}



// ============================================================
// PROCESS UNIFIED PACKET
// ============================================================



void processGhostPacket(GhostPacket p) {
  if (!verifyGhostPacket(p)) {
    return;
  }



  uint8_t m = p.meshId - 1;
  uint8_t n = p.nodeId;
  NodeState &node = nodes[m][n];



  if (node.seen && node.lastSequence == p.sequence) {
    return;
  }



  node.seen = true;
  node.lossAlarm = false;
  node.lastSequence = p.sequence;
  node.lastSeenMs = millis();
  node.packets++;
  node.nodeType = p.nodeType;
  node.targetCount = p.targetCount;
  node.alarmFlags = p.alarmFlags;
  node.sensorMask = p.sensorMask;



  for (int i = 0; i < MAX_SENSORS; i++) {
    node.sensor[i] = p.sensor[i];
  }



  for (int i = 0; i < 2; i++) {
    node.target[i] = p.target[i];
  }



  node.radarAlarm = (p.alarmFlags & ALARM_RADAR) != 0;
  node.tamperAlarm = (p.alarmFlags & ALARM_TAMPER) != 0;
  node.sensorFault = (p.alarmFlags & ALARM_SENSOR_FAULT) != 0;
  node.lowBattery = (p.alarmFlags & ALARM_LOW_BATTERY) != 0;



  if (node.tamperAlarm) {
    setAlarm(String(meshes[m].name) + " NODE " + String(n) + " TAMPER", true);
  }



  if (node.radarAlarm && meshes[m].alarmEnabled) {
    setAlarm(
      String(meshes[m].name) + " NODE " + String(n) + " RADAR ALARM",
      false
    );
  }



  if (node.sensorFault) {
    setAlarm(
      String(meshes[m].name) + " NODE " + String(n) + " SENSOR FAULT",
      false
    );
  }



  if (node.lowBattery) {
    setAlarm(
      String(meshes[m].name) + " NODE " + String(n) + " LOW BATTERY",
      false
    );
  }
}



// ============================================================
// PROCESS LEGACY STAGE 1 RADAR PACKET
// ============================================================



void processLegacyRadar(LegacyRadarPacket p) {
  if (!verifyLegacyPacket(p)) {
    return;
  }



  uint8_t m = p.mesh - 1;
  uint8_t n = p.node;
  NodeState &node = nodes[m][n];




  if (node.seen && node.lastSequence == p.seq) {
    return;
  }




  node.seen = true;
  node.lossAlarm = false;
  node.lastSequence = p.seq;
  node.lastSeenMs = millis();
  node.packets++;
  node.nodeType = NODE_TYPE_RADAR;
  node.targetCount = 1;




  node.target[0].x = p.x;
  node.target[0].y = p.y;
  node.target[0].speed = p.speed;
  node.target[0].distanceCm = p.distanceCm;
  node.target[0].angle10 = p.angle10;




  if (p.type == 2) {
    node.radarAlarm = true;
    node.alarmFlags |= ALARM_RADAR;




    if (meshes[m].alarmEnabled) {
      setAlarm(
        String(meshes[m].name) + " NODE " + String(n) + " RADAR ALARM",
        false
      );
    }
  } else if (p.type == 3) {
    node.radarAlarm = false;
    node.alarmFlags &= ~ALARM_RADAR;
  }
}



// ============================================================
// ESP-NOW RECEIVE
// ============================================================



void onEspNowReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len
) {
  espNowRxCount = espNowRxCount + 1;
  espNowLastLength = len;




  if (!data || len != sizeof(GhostPacket)) {
    espNowBadLength = espNowBadLength + 1;
    return;
  }




  GhostPacket p;
  memcpy(&p, data, sizeof(p));




  portENTER_CRITICAL_ISR(&rxMux);




  uint8_t nextHead = (rxHead + 1) % RX_QUEUE_SIZE;




  if (nextHead != rxTail) {
    rxQueue[rxHead] = p;
    rxHead = nextHead;
  }




  portEXIT_CRITICAL_ISR(&rxMux);
}



void processEspNowQueue() {
  while (true) {
    GhostPacket p;
    bool havePacket = false;




    portENTER_CRITICAL(&rxMux);




    if (rxTail != rxHead) {
      p = rxQueue[rxTail];
      rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
      havePacket = true;
    }




    portEXIT_CRITICAL(&rxMux);




    if (!havePacket) {
      break;
    }




    processGhostPacket(p);
  }
}



bool startEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    return false;
  }




  esp_now_register_recv_cb(onEspNowReceive);




  Serial.println("ESP-NOW initialized.");




  return true;
}



// ============================================================
// WIFI
// ============================================================



void startAccessPoint() {
  Serial.println("Starting WiFi subsystem...");




  Serial.printf("Free heap before WiFi: %u\n", ESP.getFreeHeap());
  Serial.printf("Largest free block: %u\n", ESP.getMaxAllocHeap());




  WiFi.mode(WIFI_OFF);
  delay(300);




  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);




  WiFi.mode(WIFI_AP_STA);
  delay(300);




  Serial.printf("Free heap after WIFI_AP_STA: %u\n", ESP.getFreeHeap());




  if (strlen(apPassword) < 8 || strlen(apPassword) > 63) {
    copyString(apPassword, sizeof(apPassword), DEFAULT_AP_PASSWORD);
    Serial.println("AP password invalid; using default.");
  }




  bool apStarted = false;




  for (int attempt = 1; attempt <= 3 && !apStarted; attempt++) {
    Serial.printf("AP start attempt %u...\n", attempt);




    apStarted = WiFi.softAP(
      "Ghost L2 Stage2",
      apPassword,
      meshChannel,
      0,
      4
    );




    if (!apStarted) {
      Serial.println("AP start failed.");




      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(500);




      WiFi.mode(WIFI_AP_STA);
      delay(300);
    }
  }




  if (apStarted) {
    Serial.println("Configuration AP: STARTED");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.printf("AP channel: %u\n", WiFi.channel());
  } else {
    Serial.println("Configuration AP: FAILED");




    Serial.printf("Free heap after failure: %u\n", ESP.getFreeHeap());
    Serial.printf("Largest free block after failure: %u\n", ESP.getMaxAllocHeap());
  }
}



// ============================================================
// OTA
// ============================================================



void setupOTA() {
  if (!wifiOK) {
    otaOK = false;
    return;
  }




  ArduinoOTA.setHostname("Ghost-L2-Stage2");
  ArduinoOTA.setPassword(otaPassword);




  ArduinoOTA
    .onStart([]() {
      Serial.println("OTA START");
    })
    .onEnd([]() {
      Serial.println("OTA END");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("OTA: %u%%\n", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("OTA ERROR [%u]\n", error);
    });




  ArduinoOTA.begin();




  otaOK = true;




  Serial.println("OTA ready.");
}



// ============================================================
// UDP RECEIVE
// ============================================================



void receiveUDP() {
  if (!udpOK) {
    return;
  }




  int packetSize = udp.parsePacket();




  while (packetSize > 0) {
    if (packetSize == sizeof(GhostPacket)) {
      GhostPacket p;
      int n = udp.read((uint8_t *)&p, sizeof(p));




      if (n == sizeof(p)) {
        processGhostPacket(p);
      }
    } else if (packetSize == sizeof(LegacyRadarPacket)) {
      LegacyRadarPacket p;
      int n = udp.read((uint8_t *)&p, sizeof(p));




      if (n == sizeof(p)) {
        processLegacyRadar(p);
      }
    } else {
      uint8_t dump[256];
      int toRead = packetSize;




      if (toRead > 256) {
        toRead = 256;
      }




      udp.read(dump, toRead);
    }




    packetSize = udp.parsePacket();
  }
}



// ============================================================
// NODE LOSS CHECK
// ============================================================



void checkNodeLoss() {
  static uint32_t lastCheck = 0;
  static uint32_t monitorStart = 0;




  if (monitorStart == 0) {
    monitorStart = millis();
  }




  if (millis() - lastCheck < 1000) {
    return;
  }




  lastCheck = millis();




  for (int m = 0; m < MAX_MESHES; m++) {
    if (!meshes[m].enabled) {
      continue;
    }




    uint32_t timeout = (uint32_t)meshes[m].lossTimeout * 1000UL;




    for (int n = 1; n <= MAX_NODES; n++) {
      if (!expectedNode(m, n)) {
        continue;
      }




      NodeState &node = nodes[m][n];




      if (!node.seen) {
        if (millis() - monitorStart >= timeout) {
          if (!node.lossAlarm) {
            node.lossAlarm = true;
            setAlarm(
              String(meshes[m].name) + " NODE " + String(n) + " NODE LOSS",
              true
            );
          }
        }




        continue;
      }




      if (!node.lossAlarm && millis() - node.lastSeenMs >= timeout) {
        node.lossAlarm = true;
        setAlarm(
          String(meshes[m].name) + " NODE " + String(n) + " NODE LOSS",
          true
        );
      }
    }
  }
}



// ============================================================
// STATUS COUNTERS
// ============================================================



uint16_t onlineCount(uint8_t meshIndex) {
  if (meshIndex >= MAX_MESHES) {
    return 0;
  }




  uint16_t count = 0;




  for (int n = 1; n <= MAX_NODES; n++) {
    if (
      expectedNode(meshIndex, n) &&
      nodes[meshIndex][n].seen &&
      !nodes[meshIndex][n].lossAlarm
    ) {
      count++;
    }
  }




  return count;
}



uint16_t radarAlarmCount() {
  uint16_t count = 0;




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      if (nodes[m][n].radarAlarm) {
        count++;
      }
    }
  }




  return count;
}



uint16_t tamperCount() {
  uint16_t count = 0;




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      if (nodes[m][n].tamperAlarm) {
        count++;
      }
    }
  }




  return count;
}



uint16_t lossCount() {
  uint16_t count = 0;




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      if (nodes[m][n].lossAlarm) {
        count++;
      }
    }
  }




  return count;
}



uint16_t sensorNodeCount() {
  uint16_t count = 0;




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      if (nodes[m][n].seen && nodes[m][n].nodeType == NODE_TYPE_SENSOR) {
        count++;
      }
    }
  }




  return count;
}



uint16_t radarNodeCount() {
  uint16_t count = 0;




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      if (
        nodes[m][n].seen &&
        (
          nodes[m][n].nodeType == NODE_TYPE_RADAR ||
          nodes[m][n].nodeType == NODE_TYPE_COMBINED
        )
      ) {
        count++;
      }
    }
  }




  return count;
}



// ============================================================
// SENSOR NAMES AND VALUES
// ============================================================



const char *sensorName(uint8_t index) {
  switch (index) {
    case SENSOR_TEMP: return "Temperature";
    case SENSOR_HUMIDITY: return "Humidity";
    case SENSOR_PRESSURE: return "Pressure";
    case SENSOR_TDS: return "TDS";
    case SENSOR_EC: return "EC";
    case SENSOR_PH: return "pH";
    case SENSOR_SOIL: return "Soil";
    case SENSOR_BATTERY: return "Battery";
    case SENSOR_LIGHT: return "Light";
    case SENSOR_WATER_LEVEL: return "Water Level";
    case SENSOR_FLOW: return "Flow";
    case SENSOR_CO2: return "CO2";
    case SENSOR_ORP: return "ORP";
    case SENSOR_RAIN: return "Rain";
    case SENSOR_WIND: return "Wind";
    case SENSOR_CUSTOM: return "Custom";
  }



  return "Sensor";
}



String sensorValueString(uint8_t index, float value) {
  if (!isfinite(value)) {
    return "--";
  }




  char b[32];




  switch (index) {
    case SENSOR_TEMP:
      snprintf(b, sizeof(b), "%.1f C", value);
      break;




    case SENSOR_HUMIDITY:
      snprintf(b, sizeof(b), "%.1f %%", value);
      break;




    case SENSOR_PRESSURE:
      snprintf(b, sizeof(b), "%.1f hPa", value);
      break;




    case SENSOR_TDS:
      snprintf(b, sizeof(b), "%.0f ppm", value);
      break;




    case SENSOR_EC:
      snprintf(b, sizeof(b), "%.3f mS", value);
      break;




    case SENSOR_PH:
      snprintf(b, sizeof(b), "%.2f", value);
      break;




    case SENSOR_SOIL:
      snprintf(b, sizeof(b), "%.1f %%", value);
      break;




    case SENSOR_BATTERY:
      snprintf(b, sizeof(b), "%.2f V", value);
      break;




    case SENSOR_LIGHT:
      snprintf(b, sizeof(b), "%.0f lux", value);
      break;




    case SENSOR_WATER_LEVEL:
      snprintf(b, sizeof(b), "%.1f %%", value);
      break;




    case SENSOR_FLOW:
      snprintf(b, sizeof(b), "%.2f L/m", value);
      break;




    case SENSOR_CO2:
      snprintf(b, sizeof(b), "%.0f ppm", value);
      break;




    case SENSOR_ORP:
      snprintf(b, sizeof(b), "%.0f mV", value);
      break;




    case SENSOR_RAIN:
      snprintf(b, sizeof(b), "%.1f mm", value);
      break;




    case SENSOR_WIND:
      snprintf(b, sizeof(b), "%.1f m/s", value);
      break;




    default:
      snprintf(b, sizeof(b), "%.2f", value);
      break;
  }




  return String(b);
}



// ============================================================
// WEB PAGE STYLE
// ============================================================



String css() {
  String x;




  x += "<style>";
  x += "*{box-sizing:border-box}";
  x += "body{font-family:Arial;background:#0b1220;color:#e5e7eb;margin:0;padding:20px}";
  x += ".box{background:#121b2c;padding:20px;margin-bottom:20px;border-radius:12px;border:1px solid #23324a}";
  x += "h1,h2,h3{color:#22d3ee}";
  x += "h1{margin-top:0}";
  x += "label{display:block;margin-top:12px}";
  x += "input,select{width:100%;padding:12px;margin-top:5px;box-sizing:border-box;background:#1a2438;color:white;border:1px solid #2f425e;border-radius:8px}";
  x += "button{padding:12px 20px;margin-top:15px;border:0;border-radius:8px;background:#22d3ee;color:#06202a;font-weight:bold}";
  x += ".danger{background:#ef4444;color:white}";
  x += ".calibrate{background:#fbbf24;color:#272005}";
  x += ".good{color:#34d399}";
  x += ".bad{color:#f87171}";
  x += ".warn{color:#fbbf24}";
  x += "table{width:100%;border-collapse:collapse}";
  x += "td,th{border:1px solid #2f425e;padding:8px;text-align:left}";
  x += "a{color:#22d3ee}";
  x += ".alarm{background:#3b1111;border:2px solid #ef4444;padding:15px;border-radius:10px;color:#fca5a5;font-weight:bold}";
  x += ".ok{background:#0f2b22;border:2px solid #34d399;padding:15px;border-radius:10px;color:#6ee7b7;font-weight:bold}";
  x += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:20px}";
  x += ".small{font-size:13px;color:#94a3b8}";
  x += "</style>";




  return x;
}



// ============================================================
// DASHBOARD PAGE
// ============================================================



String dashboardPage() {
  String x;




  x += "<!DOCTYPE html><html><head>";
  x += "<meta charset='UTF-8'>";
  x += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  x += "<title>Ghost L2 Stage 2</title>";
  x += css();
  x += "</head><body>";




  x += "<h1>Ghost L2 Security Mesh</h1>";




  x += "<div class='box'>";
  x += "<h2>Stage 2 Console</h2>";
  x += "<p><b>" + htmlEscape(String(consoleName)) + "</b></p>";
  x += "<p>Display hardware: <b>" + htmlEscape(compiledDisplayName()) + "</b></p>";




  x += "<p>Wi-Fi: ";
  x += wifiOK ? "<span class='good'>CONNECTED</span>" : "<span class='bad'>OFFLINE</span>";
  x += "</p>";




  if (wifiOK) {
    x += "<p>Router IP: <b>" + WiFi.localIP().toString() + "</b></p>";
    x += "<p>Router channel: <b>" + String(WiFi.channel()) + "</b></p>";
  }




  x += "<p>ESP-NOW: ";
  x += espNowOK ? "<span class='good'>READY</span>" : "<span class='bad'>OFFLINE</span>";
  x += "</p>";




  x += "<p>OTA: ";
  x += otaOK ? "<span class='good'>READY</span>" : "<span class='bad'>NOT AVAILABLE</span>";
  x += "</p>";




  x += "<p>Touch calibration: ";
  x += touchCalibrated()
    ? "<span class='good'>CALIBRATED</span>"
    : "<span class='warn'>NOT CALIBRATED</span>";
  x += "</p>";




  x += "<p>Exit delay: <b>" + String(armDelaySeconds) + " seconds</b></p>";
  x += "</div>";




  if (globalAlarm) {
    x += "<div class='alarm'>!!! " + htmlEscape(globalAlarmText) + " !!!</div>";
  } else {
    x += "<div class='ok'>SYSTEM NORMAL</div>";
  }




  x += "<div class='grid'>";




  x += "<div class='box'>";
  x += "<h2>Security</h2>";
  x += "<p>Radar nodes: <b>" + String(radarNodeCount()) + "</b></p>";
  x += "<p>Radar alarms: <b class='bad'>" + String(radarAlarmCount()) + "</b></p>";
  x += "<p>Tamper alarms: <b class='bad'>" + String(tamperCount()) + "</b></p>";
  x += "<p>Node loss: <b class='bad'>" + String(lossCount()) + "</b></p>";
  x += "</div>";




  int totalOnline = 0;




  for (int m = 0; m < MAX_MESHES; m++) {
    totalOnline += onlineCount(m);
  }




  x += "<div class='box'>";
  x += "<h2>Sensors</h2>";
  x += "<p>Sensor nodes: <b>" + String(sensorNodeCount()) + "</b></p>";
  x += "<p>Total online: <b>" + String(totalOnline) + "</b></p>";
  x += "</div>";




  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Mesh Status</h2>";
  x += "<table>";
  x += "<tr><th>Mesh</th><th>Enabled</th><th>Expected</th><th>Online</th><th>Alarm</th></tr>";




  for (int m = 0; m < MAX_MESHES; m++) {
    if (!meshes[m].enabled) {
      continue;
    }




    x += "<tr>";
    x += "<td>" + htmlEscape(String(meshes[m].name)) + "</td>";
    x += "<td class='good'>YES</td>";
    x += "<td>" + String(countExpected(m)) + "</td>";
    x += "<td>" + String(onlineCount(m)) + "</td>";




    bool meshAlarm = false;




    for (int n = 1; n <= MAX_NODES; n++) {
      if (
        nodes[m][n].radarAlarm ||
        nodes[m][n].tamperAlarm ||
        nodes[m][n].lossAlarm ||
        nodes[m][n].sensorFault ||
        nodes[m][n].lowBattery
      ) {
        meshAlarm = true;
        break;
      }
    }




    x += meshAlarm
      ? "<td class='bad'>ALARM</td>"
      : "<td class='good'>CLEAR</td>";




    x += "</tr>";
  }




  x += "</table>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Stage 1 Radar Nodes</h2>";
  x += "<table>";
  x += "<tr><th>Mesh</th><th>Node</th><th>Status</th><th>Radar</th><th>Targets</th><th>Distance</th></tr>";




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      NodeState node = nodes[m][n];




      if (!node.seen) {
        continue;
      }




      if (node.nodeType != NODE_TYPE_RADAR && node.nodeType != NODE_TYPE_COMBINED) {
        continue;
      }




      x += "<tr>";
      x += "<td>" + htmlEscape(String(meshes[m].name)) + "</td>";
      x += "<td>" + String(n) + "</td>";




      x += node.lossAlarm
        ? "<td class='bad'>LOST</td>"
        : "<td class='good'>ONLINE</td>";




      x += node.radarAlarm
        ? "<td class='bad'>ALARM</td>"
        : "<td class='good'>CLEAR</td>";




      x += "<td>" + String(node.targetCount) + "</td>";




      if (node.targetCount > 0) {
        x += "<td>" + String(node.target[0].distanceCm / 100.0f, 2) + " m</td>";
      } else {
        x += "<td>--</td>";
      }




      x += "</tr>";
    }
  }




  x += "</table>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Sensor Nodes</h2>";




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      NodeState node = nodes[m][n];




      if (!node.seen || node.nodeType != NODE_TYPE_SENSOR) {
        continue;
      }




      x += "<div class='box'>";
      x += "<h3>" + htmlEscape(String(meshes[m].name)) + " Node " + String(n) + "</h3>";




      x += "<p>Status: ";
      x += node.lossAlarm
        ? "<span class='bad'>OFFLINE</span>"
        : "<span class='good'>ONLINE</span>";
      x += "</p>";




      x += "<table>";
      x += "<tr><th>Sensor</th><th>Value</th></tr>";




      for (int s = 0; s < MAX_SENSORS; s++) {
        if (!(node.sensorMask & (1UL << s))) {
          continue;
        }




        x += "<tr>";
        x += "<td>" + String(sensorName(s)) + "</td>";
        x += "<td>" + htmlEscape(sensorValueString(s, node.sensor[s])) + "</td>";
        x += "</tr>";
      }




      x += "</table>";
      x += "</div>";
    }
  }




  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>System Control</h2>";




  if (webControlMessage.length() && millis() < webControlMessageUntil) {
    x += "<p><b>" + htmlEscape(webControlMessage) + "</b></p>";
  }




  x += "<p>System status: ";
  x += cydArming ? "<span class='warn'>ARMING</span>" :
       cydArmed ? "<span class='good'>ARMED</span>" :
       "<span class='bad'>DISARMED</span>";
  x += "</p>";




  x += "<p>Arm mode: ";




  if (cydArmMode == 1) {
    x += "<b>MODE 1: FULL ALARM</b>";
  } else if (cydArmMode == 2) {
    x += "<b>MODE 2: PERIMETER</b>";
  } else if (cydArmMode == 3) {
    x += "<b>MODE 3: SOFT ALARMS</b>";
  } else {
    x += "<b>NOT SELECTED</b>";
  }




  x += "</p>";




  x += "<form method='POST' action='/control'>";
  x += "<label>Control PIN</label>";
  x += "<input name='pin' type='password' placeholder='Enter control PIN'>";
  x += "<button type='submit' name='action' value='arm'>ARM SYSTEM</button>";
  x += "<button type='submit' name='action' value='disarm' class='danger'>DISARM SYSTEM</button>";
  x += "</form>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Permanent Soft Alarms</h2>";
  x += "<p>Set a mesh and node range for each section. Leave a section disabled if it is not used.</p>";




  x += "<form method='POST' action='/softalarm/save'>";




  for (int section = 0; section < MAX_MESHES; section++) {
    x += "<div style='border:1px solid #2f425e;border-radius:8px;padding:15px;margin:15px 0'>";
    x += "<h3>Section " + String(section + 1) + "</h3>";




    x += "<label>ENABLED</label>";
    x += "<select name='softenabled" + String(section) + "'>";
    x += "<option value='0'";




    if (!constantSoftSections[section].enabled) {
      x += " selected";
    }




    x += ">OFF</option>";
    x += "<option value='1'";




    if (constantSoftSections[section].enabled) {
      x += " selected";
    }




    x += ">ON</option>";
    x += "</select>";




    x += "<label>MESH</label>";
    x += "<select name='softmesh" + String(section) + "'>";




    x += "<option value='0'";




    if (constantSoftSections[section].meshId == 0) {
      x += " selected";
    }




    x += ">ANY MESH</option>";




    for (int m = 0; m < MAX_MESHES; m++) {
      x += "<option value='" + String(m + 1) + "'";




      if (constantSoftSections[section].meshId == m + 1) {
        x += " selected";
      }




      x += ">" + htmlEscape(String(meshes[m].name)) + "</option>";
    }




    x += "</select>";




    x += "<label>NODE IDS</label>";
    x += "<input name='softnodes" + String(section) + "' placeholder='Example: 1,2,3,5-32' value='" +
         htmlEscape(String(constantSoftSections[section].nodeIds)) + "'>";




    x += "</div>";
  }




  x += "<button>SAVE PERMANENT SOFT ALARMS</button>";
  x += "</form>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Stage 2 Bridge</h2>";




  char bridgeMacStr[18];




  sprintf(
    bridgeMacStr,
    "%02X:%02X:%02X:%02X:%02X:%02X",
    bridgeMac[0], bridgeMac[1], bridgeMac[2],
    bridgeMac[3], bridgeMac[4], bridgeMac[5]
  );




  x += "<form method='POST' action='/bridge/save'>";
  x += "<label>BRIDGE MAC ADDRESS</label>";
  x += "<input name='bridgemac' value='" + String(bridgeMacStr) + "'>";
  x += "<button>SAVE BRIDGE MAC</button>";
  x += "</form>";




  x += "<form method='POST' action='/bridge/pair'>";
  x += "<button>PAIR WITH BRIDGE</button>";
  x += "</form>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Configuration</h2>";
  x += "<p><a href='/config'>Open Ghost L2 Configuration</a></p>";
  x += "<p><a href='/api/status'>Open JSON Status API</a></p>";




  x += "<form method='POST' action='/clear'>";
  x += "<button class='danger'>Clear Active Alarms</button>";
  x += "</form>";
  x += "</div>";




  x += "</body></html>";




  return x;
}



// ============================================================
// CONFIGURATION PAGE
// ============================================================



String configPage() {
  String x;




  x += "<!DOCTYPE html><html><head>";
  x += "<meta charset='UTF-8'>";
  x += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  x += "<title>Ghost L2 Configuration</title>";
  x += css();
  x += "</head><body>";




  x += "<h1>Ghost L2 Security Mesh</h1>";




  x += "<div class='box'>";
  x += "<h2>Stage 2 Console</h2>";
  x += "<form method='POST' action='/save'>";




  x += "<label>Console Name</label>";
  x += "<input name='console' value='" + htmlEscape(String(consoleName)) + "'>";




  x += "<label>Exit Delay Before Arming (seconds)</label>";
  x += "<input name='armdelay' type='number' min='0' max='300' value='" + String(armDelaySeconds) + "'>";




  x += "<p class='small'>";
  x += "After entering the correct PIN to activate the system, the console waits this many seconds before arming.";
  x += "</p>";




  x += "<p class='small'>";
  x += "ESP-NOW channel: <b>" + String(meshChannel) + "</b>. ";
  x += "When connected to a router, the console automatically uses the router's Wi-Fi channel for ESP-NOW.";
  x += "</p>";




  for (int m = 0; m < MAX_MESHES; m++) {
    x += "<div class='box'>";
    x += "<h2>Mesh " + String(m + 1) + "</h2>";




    x += "<label>Mesh Name</label>";
    x += "<input name='mn" + String(m) + "' value='" + htmlEscape(String(meshes[m].name)) + "'>";




    x += "<label>Mesh Security Key</label>";
    x += "<input name='mk" + String(m) + "' value='" + htmlEscape(String(meshes[m].key)) + "'>";




    x += "<label>Enabled</label>";
    x += "<input type='checkbox' name='me" + String(m) + "'" +
         (meshes[m].enabled ? " checked" : "") + ">";




    x += "<label>Alarm Monitoring Enabled</label>";
    x += "<input type='checkbox' name='ma" + String(m) + "'" +
         (meshes[m].alarmEnabled ? " checked" : "") + ">";




    x += "<label>Expected Node IDs</label>";
    x += "<input name='ids" + String(m) + "' placeholder='Example: 1,2,3,4' value='" +
         htmlEscape(String(meshes[m].expectedIds)) + "'>";




    x += "<p class='small'>";
    x += "Enter the exact node IDs expected on this mesh. Example: 1,2,3,4,7. ";
    x += "These IDs are used for node-loss alarms.";
    x += "</p>";




    x += "<label>Node Loss Timeout (seconds)</label>";
    x += "<input name='to" + String(m) + "' type='number' min='2' max='3600' value='" +
         String(meshes[m].lossTimeout) + "'>";




    x += "<p>Expected nodes: <b>" + String(countExpected(m)) + "</b></p>";
    x += "</div>";
  }




  x += "<div class='box'>";
  x += "<h2>Configuration AP Password</h2>";




  x += "<label>Control PIN</label>";
  x += "<input name='controlpin' value='" + htmlEscape(String(cydControlPin)) + "'>";
  x += "<p class='small'>PIN used to arm or disarm the system from the touchscreen and web dashboard.</p>";




  x += "<label>AP Password</label>";
  x += "<input name='ap' value='" + htmlEscape(String(apPassword)) + "'>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>OTA</h2>";




  x += "<label>OTA Password</label>";
  x += "<input name='ota' value='" + htmlEscape(String(otaPassword)) + "'>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Wi-Fi Networks</h2>";




  for (int i = 0; i < 5; i++) {
    x += "<h3>Wi-Fi " + String(i + 1) + "</h3>";




    x += "<label>SSID</label>";
    x += "<input name='ws" + String(i) + "' value='" +
         htmlEscape(String(wifiProfiles[i].ssid)) + "'>";




    x += "<label>Password</label>";
    x += "<input name='wp" + String(i) + "' value='" +
         htmlEscape(String(wifiProfiles[i].pass)) + "'>";
  }




  x += "</div>";




  x += "<div class='box'>";
  x += "<button>Save and Reboot</button>";
  x += "</form>";




  x += "<p><a href='/'>Return to Dashboard</a></p>";
  x += "</div>";




  x += "<div class='box'>";
  x += "<h2>Touchscreen</h2>";
  x += "<p>Current status: ";
  x += touchCalibrated()
    ? "<span class='good'>CALIBRATED</span>"
    : "<span class='warn'>NOT CALIBRATED</span>";
  x += "</p>";




  x += "<form method='POST' action='/recalibrate'>";
  x += "<button class='calibrate'>CALIBRATE TOUCHSCREEN</button>";
  x += "</form>";




  x += "<p class='small'>";
  x += "This restarts the console into the guided four-corner touchscreen calibration.";
  x += "</p>";
  x += "</div>";




  x += "</body></html>";




  return x;
}



// ============================================================
// SAVE WEB CONFIG
// ============================================================



void saveWeb() {
  String s = web.arg("console");




  if (s.length() > 0) {
    copyString(consoleName, sizeof(consoleName), s);
  }




  int delaySeconds = web.arg("armdelay").toInt();




  if (delaySeconds < 0) {
    delaySeconds = 0;
  }




  if (delaySeconds > 300) {
    delaySeconds = 300;
  }




  armDelaySeconds = delaySeconds;




  s = web.arg("controlpin");




  if (s.length() >= 4 && s.length() <= 12) {
    copyString(cydControlPin, sizeof(cydControlPin), s);
  }




  s = web.arg("ap");




  if (s.length() >= 8) {
    copyString(apPassword, sizeof(apPassword), s);
  }




  s = web.arg("ota");




  if (s.length() >= 8) {
    copyString(otaPassword, sizeof(otaPassword), s);
  }




  for (int m = 0; m < MAX_MESHES; m++) {
    s = web.arg("mn" + String(m));
    copyString(meshes[m].name, sizeof(meshes[m].name), s);




    s = web.arg("mk" + String(m));




    if (s.length() > 0) {
      copyString(meshes[m].key, sizeof(meshes[m].key), s);
    }




    meshes[m].enabled = web.hasArg("me" + String(m));
    meshes[m].alarmEnabled = web.hasArg("ma" + String(m));




    s = web.arg("ids" + String(m));
    copyString(meshes[m].expectedIds, sizeof(meshes[m].expectedIds), s);
    meshes[m].expectedCount = countExpected(m);




    int timeout = web.arg("to" + String(m)).toInt();




    if (timeout < 2) {
      timeout = DEFAULT_LOSS_TIMEOUT;
    }




    if (timeout > 3600) {
      timeout = 3600;
    }




    meshes[m].lossTimeout = timeout;
  }




  for (int i = 0; i < 5; i++) {
    s = web.arg("ws" + String(i));
    copyString(wifiProfiles[i].ssid, sizeof(wifiProfiles[i].ssid), s);




    s = web.arg("wp" + String(i));
    copyString(wifiProfiles[i].pass, sizeof(wifiProfiles[i].pass), s);
  }




  saveConfig();




  web.send(
    200,
    "text/html",
    "<html><body style='background:#0b1220;color:#e5e7eb;font-family:Arial;padding:20px'>"
    "<h2 style='color:#22d3ee'>Saved</h2>"
    "<p>Ghost L2 Stage 2 is rebooting.</p>"
    "</body></html>"
  );




  delay(800);
  ESP.restart();
}



// ============================================================
// JSON API
// ============================================================



String jsonFloat(float v) {
  if (!isfinite(v)) {
    return "null";
  }




  return String(v, 3);
}



String jsonStatus() {
  String x;




  x += "{";
  x += "\"console\":\"" + htmlEscape(String(consoleName)) + "\",";
  x += "\"wifi\":" + String(wifiOK ? "true" : "false") + ",";
  x += "\"ip\":\"" + (wifiOK ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  x += "\"channel\":\"" + String(wifiOK ? WiFi.channel() : meshChannel) + "\",";
  x += "\"espnow\":" + String(espNowOK ? "true" : "false") + ",";
  x += "\"alarm\":" + String(globalAlarm ? "true" : "false") + ",";
  x += "\"alarmText\":\"" + htmlEscape(globalAlarmText) + "\",";
  x += "\"touchCalibrated\":" + String(touchCalValid ? "true" : "false") + ",";
  x += "\"armed\":" + String(cydArmed ? "true" : "false") + ",";
  x += "\"arming\":" + String(cydArming ? "true" : "false") + ",";
  x += "\"armDelaySeconds\":" + String(armDelaySeconds) + ",";
  x += "\"armMode\":" + String(cydArmMode) + ",";
  x += "\"radarAlarms\":" + String(radarAlarmCount()) + ",";
  x += "\"tamperAlarms\":" + String(tamperCount()) + ",";
  x += "\"nodeLoss\":" + String(lossCount()) + ",";
  x += "\"sensorNodes\":" + String(sensorNodeCount()) + ",";




  x += "\"meshes\":[";




  for (int m = 0; m < MAX_MESHES; m++) {
    if (m > 0) {
      x += ",";
    }




    x += "{";
    x += "\"id\":" + String(m + 1) + ",";
    x += "\"name\":\"" + htmlEscape(String(meshes[m].name)) + "\",";
    x += "\"enabled\":" + String(meshes[m].enabled ? "true" : "false") + ",";
    x += "\"expected\":" + String(countExpected(m)) + ",";
    x += "\"online\":" + String(onlineCount(m));
    x += "}";
  }




  x += "],";




  x += "\"nodes\":[";




  bool first = true;




  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      NodeState node = nodes[m][n];




      if (!node.seen) {
        continue;
      }




      if (!first) {
        x += ",";
      }




      first = false;




      x += "{";
      x += "\"mesh\":" + String(m + 1) + ",";
      x += "\"node\":" + String(n) + ",";
      x += "\"type\":" + String(node.nodeType) + ",";
      x += "\"online\":" + String(node.lossAlarm ? "false" : "true") + ",";
      x += "\"radarAlarm\":" + String(node.radarAlarm ? "true" : "false") + ",";
      x += "\"tamper\":" + String(node.tamperAlarm ? "true" : "false") + ",";
      x += "\"sensorFault\":" + String(node.sensorFault ? "true" : "false") + ",";
      x += "\"lowBattery\":" + String(node.lowBattery ? "true" : "false") + ",";
      x += "\"targetCount\":" + String(node.targetCount) + ",";
      x += "\"sensorMask\":" + String(node.sensorMask) + ",";




      x += "\"sensors\":{";




      bool firstSensor = true;




      for (int s = 0; s < MAX_SENSORS; s++) {
        if (!(node.sensorMask & (1UL << s))) {
          continue;
        }




        if (!firstSensor) {
          x += ",";
        }




        firstSensor = false;




        x += "\"" + String(sensorName(s)) + "\":" + jsonFloat(node.sensor[s]);
      }




      x += "}}";
    }
  }




  x += "]}";




  return x;
}



// ============================================================
// WEB ARM / DISARM / STAGE 3 TESTS
// ============================================================



void handleWebArmDisarm() {
  String pin = web.arg("pin");
  String action = web.arg("action");




  if (pin != String(cydControlPin)) {
    webControlMessage = "INCORRECT PIN";
    webControlMessageUntil = millis() + 3000;




    Serial.println("WEB CONTROL: incorrect PIN");




    web.sendHeader("Location", "/");
    web.send(303);
    return;
  }




  if (action == "arm") {
    if (cydArmMode == 0) {
      webControlMessage = "SELECT ARM MODE FIRST";
      webControlMessageUntil = millis() + 3000;




      web.sendHeader("Location", "/");
      web.send(303);
      return;
    }




    if (armDelaySeconds > 0) {
      cydArming = true;
      cydArmingUntil = millis() + (armDelaySeconds * 1000UL);




      webControlMessage = "ARMING - MODE " + String(cydArmMode) +
                          " - EXIT DELAY " + String(armDelaySeconds) + "s";




      Serial.printf(
        "WEB CONTROL: arm mode %u, exit delay started, %u seconds\n",
        cydArmMode,
        armDelaySeconds
      );
    } else {
      cydArmed = true;




      webControlMessage = "SYSTEM ARMED - MODE " + String(cydArmMode);




      Serial.printf(
        "WEB CONTROL: system armed in mode %u\n",
        cydArmMode
      );
    }
  } else if (action == "disarm") {
    cydArmed = false;
    cydArming = false;
    cydArmingUntil = 0;
    cydArmMode = 0;




    webControlMessage = "SYSTEM DISARMED";




    Serial.println("WEB CONTROL: system disarmed");
  }




  beepShort();




  webControlMessageUntil = millis() + 3000;




  web.sendHeader("Location", "/");
  web.send(303);
}



void handleTestLoudAlarm() {
  testLoudAlarm = true;




  Serial.println("WEB TEST: LOUD ALARM requested");




  beepCritical();




  webControlMessage = "LOUD ALARM TEST SENT";
  webControlMessageUntil = millis() + 3000;




  web.sendHeader("Location", "/");
  web.send(303);
}



void handleTestSoftAlarm() {
  testSoftAlarm = true;




  Serial.println("WEB TEST: SOFT ALARM requested");




  beepShort();




  webControlMessage = "SOFT ALARM TEST SENT";
  webControlMessageUntil = millis() + 3000;




  web.sendHeader("Location", "/");
  web.send(303);
}



void handleTestTransmissionAlarm() {
  testTransmissionAlarm = true;




  Serial.println("WEB TEST: TRANSMISSION ALARM requested");
  Serial.println("WEB TEST: Stage 3 LoRa/NRF24 transmission pending");




  webControlMessage = "TRANSMISSION ALARM TEST SENT";
  webControlMessageUntil = millis() + 3000;




  web.sendHeader("Location", "/");
  web.send(303);
}



void handleTestReactionAlarm() {
  testReactionAlarm = true;




  Serial.println("WEB TEST: REACTION ALARM requested");
  Serial.println("WEB TEST: Stage 3 reaction nodes pending");




  webControlMessage = "REACTION ALARM TEST SENT";
  webControlMessageUntil = millis() + 3000;




  web.sendHeader("Location", "/");
  web.send(303);
}



// ============================================================
// PERMANENT SOFT ALARM SAVE - 5 SECTIONS
// ============================================================



void handleSoftAlarmSave() {
  for (int section = 0; section < MAX_MESHES; section++) {
    String meshArg = "softmesh" + String(section);
    String nodeArg = "softnodes" + String(section);
    String enabledArg = "softenabled" + String(section);




    int selectedMesh = web.arg(meshArg).toInt();




    if (selectedMesh < 0 || selectedMesh > MAX_MESHES) {
      selectedMesh = 0;
    }




    constantSoftSections[section].meshId = selectedMesh;




    String nodes = web.arg(nodeArg);




    nodes.toCharArray(
      constantSoftSections[section].nodeIds,
      sizeof(constantSoftSections[section].nodeIds)
    );




    constantSoftSections[section].enabled =
      web.arg(enabledArg) == "1";
  }




  prefs.begin("ghost2", false);




  for (int section = 0; section < MAX_MESHES; section++) {
    String meshKey = "softmesh" + String(section);
    String nodeKey = "softnodes" + String(section);
    String enabledKey = "softenabled" + String(section);




    prefs.putUChar(
      meshKey.c_str(),
      constantSoftSections[section].meshId
    );




    prefs.putString(
      nodeKey.c_str(),
      String(constantSoftSections[section].nodeIds)
    );




    prefs.putBool(
      enabledKey.c_str(),
      constantSoftSections[section].enabled
    );
  }




  prefs.end();




  webControlMessage = "PERMANENT SOFT ALARMS SAVED";
  webControlMessageUntil = millis() + 3000;




  web.sendHeader("Location", "/");
  web.send(303);
}



// ============================================================
// BRIDGE SAVE / PAIR
// ============================================================



void handleBridgeSave() {
  String mac = web.arg("bridgemac");
  mac.trim();



  int v[6];



  if (
    sscanf(
      mac.c_str(),
      "%x:%x:%x:%x:%x:%x",
      &v[0], &v[1], &v[2],
      &v[3], &v[4], &v[5]
    ) != 6
  ) {
    webControlMessage = "INVALID BRIDGE MAC";
    webControlMessageUntil = millis() + 3000;



    web.sendHeader("Location", "/");
    web.send(303);
    return;
  }



  for (int i = 0; i < 6; i++) {
    bridgeMac[i] = v[i];
  }



  bridgeConfigured = true;



  prefs.begin("ghost2", false);
  prefs.putString("bridgemac", mac);
  prefs.end();



  webControlMessage = "BRIDGE MAC SAVED";
  webControlMessageUntil = millis() + 3000;



  web.sendHeader("Location", "/");
  web.send(303);
}



void handleBridgePair() {
  if (!bridgeConfigured) {
    webControlMessage = "BRIDGE MAC NOT SET";
    webControlMessageUntil = millis() + 3000;



    web.sendHeader("Location", "/");
    web.send(303);
    return;
  }



  struct __attribute__((packed)) BridgePairingRequest {
    char cydName[24];
  };



  BridgePairingRequest request;



  copyString(
    request.cydName,
    sizeof(request.cydName),
    String(consoleName)
  );



  esp_now_peer_info_t peer;



  memset(&peer, 0, sizeof(peer));



  memcpy(peer.peer_addr, bridgeMac, 6);
  peer.channel = 0;
  peer.encrypt = false;



  esp_now_add_peer(&peer);



  esp_now_send(
    bridgeMac,
    (uint8_t *)&request,
    sizeof(request)
  );



  webControlMessage = "PAIRING REQUEST SENT";
  webControlMessageUntil = millis() + 3000;



  web.sendHeader("Location", "/");
  web.send(303);
}



// ============================================================
// WEB ROUTES
// ============================================================



void setupWeb() {
  web.on("/", HTTP_GET, []() {
    web.send(200, "text/html; charset=utf-8", dashboardPage());
  });



  web.on("/config", HTTP_GET, []() {
    web.send(200, "text/html; charset=utf-8", configPage());
  });



  web.on("/save", HTTP_POST, saveWeb);



  web.on("/clear", HTTP_POST, []() {
    clearAlarms();



    web.send(
      200,
      "text/html; charset=utf-8",
      "<html><body style='background:#0b1220;color:#e5e7eb;font-family:Arial;padding:20px'>"
      "<h2 style='color:#22d3ee'>Alarms cleared</h2>"
      "<p><a style='color:#22d3ee' href='/'>Return to dashboard</a></p>"
      "</body></html>"
    );
  });



  web.on("/recalibrate", HTTP_POST, []() {
    prefs.begin("ghost2", false);
    prefs.putBool("calreq", true);
    prefs.end();



    web.send(
      200,
      "text/html",
      "<html><body style='background:#0b1220;color:#e5e7eb;font-family:Arial;padding:20px'>"
      "<h2 style='color:#22d3ee'>Recalibration requested</h2>"
      "<p>The console will restart into touchscreen calibration.</p>"
      "</body></html>"
    );



    delay(800);
    ESP.restart();
  });



  web.on("/api/status", HTTP_GET, []() {
    web.send(200, "application/json", jsonStatus());
  });



  web.on("/api/clear", HTTP_POST, []() {
    clearAlarms();
    web.send(200, "application/json", "{\"ok\":true}");
  });



  web.on("/control", HTTP_POST, handleWebArmDisarm);



  web.on("/softalarm/save", HTTP_POST, handleSoftAlarmSave);



  web.on("/bridge/save", HTTP_POST, handleBridgeSave);
  web.on("/bridge/pair", HTTP_POST, handleBridgePair);



  web.on("/test/loud", HTTP_POST, handleTestLoudAlarm);
  web.on("/test/soft", HTTP_POST, handleTestSoftAlarm);
  web.on("/test/transmission", HTTP_POST, handleTestTransmissionAlarm);
  web.on("/test/reaction", HTTP_POST, handleTestReactionAlarm);



  web.begin();



  Serial.println("Web server started.");
}
// ============================================================
// CYD UI HELPERS
// ============================================================



static uint16_t cydRGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}



static void cydText(
  const String text,
  int x,
  int y,
  int size,
  uint16_t fg,
  uint16_t bg
) {
  cydCanvas.setTextSize(size);
  cydCanvas.setTextColor(fg, bg);
  cydCanvas.setCursor(x, y);
  cydCanvas.print(text);
}



static void cydPanel(
  int x,
  int y,
  int w,
  int h,
  uint16_t fill,
  uint16_t border
) {
  cydCanvas.fillRoundRect(x, y, w, h, 8, fill);
  cydCanvas.drawRoundRect(x, y, w, h, 8, border);
}



static void cydButton(
  int x,
  int y,
  int w,
  int h,
  const String label,
  uint16_t fill,
  uint16_t fg,
  uint16_t border
) {
  cydCanvas.fillRoundRect(x, y, w, h, 8, fill);
  cydCanvas.drawRoundRect(x, y, w, h, 8, border);



  int tw = label.length() * 6;
  int tx = x + (w - tw) / 2;



  if (tx < x + 3) {
    tx = x + 3;
  }



  cydText(label, tx, y + (h - 8) / 2, 1, fg, fill);
}



static void cydStatusDot(
  int x,
  int y,
  bool ok,
  uint16_t green,
  uint16_t red
) {
  cydCanvas.fillCircle(x, y, 4, ok ? green : red);
}



static String cydNodeType(const NodeState n) {
  if (n.nodeType == NODE_TYPE_SENSOR) {
    return "SENSOR";
  }



  if (n.nodeType == NODE_TYPE_COMBINED) {
    return "RADAR + SENSOR";
  }



  return "RADAR";
}



static NodeState *cydLatestNode(
  int &meshOut,
  int &nodeOut,
  uint32_t &ageOut
) {
  NodeState *latest = nullptr;
  uint32_t newest = UINT32_MAX;



  meshOut = -1;
  nodeOut = -1;
  ageOut = UINT32_MAX;



  for (int m = 0; m < MAX_MESHES; m++) {
    for (int n = 1; n <= MAX_NODES; n++) {
      NodeState *candidate = &nodes[m][n];



      if (!candidate->seen) {
        continue;
      }



      uint32_t age = millis() - candidate->lastSeenMs;



      if (!latest || age < newest) {
        latest = candidate;
        newest = age;
        meshOut = m;
        nodeOut = n;
        ageOut = age;
      }
    }
  }



  return latest;
}



// ============================================================
// CYD HEADER / TABS / BOTTOM BAR
// ============================================================



static void cydHeader(
  const String section,
  uint16_t bg,
  uint16_t accent,
  uint16_t white,
  uint16_t muted
) {
  const int W = cydCanvas.width();



  cydCanvas.fillRect(0, 0, W, 34, cydRGB(8, 9, 11));
  cydCanvas.fillRect(0, 0, 6, 34, accent);



  cydText("GHOST L2", 16, 8, 2, white, cydRGB(13, 20, 36));
  cydText(section, 122, 11, 1, accent, cydRGB(13, 20, 36));



  cydButton(
    W - 66,
    5,
    66,
    22,
    "INV",
    cydRGB(18, 20, 24),
    accent,
    cydRGB(35, 50, 74)
  );



  cydText("STAGE 2", W - 130, 11, 1, muted, cydRGB(13, 20, 36));



  cydCanvas.drawFastHLine(0, 33, W, cydRGB(35, 50, 74));
}



static void cydTabs(
  uint16_t bg,
  uint16_t panel,
  uint16_t accent,
  uint16_t white,
  uint16_t muted
) {
  const char *tabs[] = {"OVERVIEW", "NODES", "SENSORS"};
  const int xs[] = {6, 110, 218};
  const int ws[] = {98, 102, 96};



  for (int i = 0; i < 3; i++) {
    bool active = (cydPage == i);



    uint16_t fill = active ? cydRGB(20, 42, 58) : cydRGB(18, 27, 44);
    uint16_t border = active ? accent : cydRGB(35, 50, 74);
    uint16_t fg = active ? accent : muted;



    cydButton(xs[i], 40, ws[i], 26, tabs[i], fill, fg, border);



    if (active) {
      cydCanvas.fillRect(xs[i] + 8, 63, ws[i] - 16, 2, accent);
    }
  }
}



static void cydBottomBar(
  uint16_t bg,
  uint16_t panel,
  uint16_t accent,
  uint16_t green,
  uint16_t red,
  uint16_t white,
  uint16_t muted
) {
  const int H = cydCanvas.height();



  cydCanvas.fillRect(0, H - 40, cydCanvas.width(), 40, cydRGB(13, 20, 36));
  cydCanvas.drawFastHLine(0, H - 41, cydCanvas.width(), cydRGB(35, 50, 74));



  bool armed = cydArmed;
  bool arming = cydArming;



  cydButton(
    8,
    H - 33,
    98,
    26,
    "ACTIVATE",
    armed ? cydRGB(18, 52, 42) : cydRGB(20, 42, 58),
    armed ? green : accent,
    armed ? green : accent
  );



  cydButton(
    112,
    H - 33,
    98,
    26,
    "DEACTIVATE",
    (!armed && !arming) ? cydRGB(18, 27, 44) : cydRGB(58, 26, 32),
    (!armed && !arming) ? muted : red,
    (!armed && !arming) ? cydRGB(35, 50, 74) : red
  );



  cydButton(
    216,
    H - 33,
    96,
    26,
    "ALARM",
    globalAlarm ? cydRGB(58, 26, 32) : cydRGB(18, 27, 44),
    globalAlarm ? red : muted,
    globalAlarm ? red : cydRGB(35, 50, 74)
  );



  if (arming) {
    int secondsLeft = (cydArmingUntil - millis()) / 1000;



    if (secondsLeft < 0) {
      secondsLeft = 0;
    }



    cydText(
      "EXIT DELAY " + String(secondsLeft) + "s",
      112,
      H - 38,
      1,
      accent,
      cydRGB(13, 20, 36)
    );
  }
}



// ============================================================
// CYD OVERVIEW PAGE
// ============================================================



static void cydDrawOverview(
  uint16_t bg,
  uint16_t panel,
  uint16_t border,
  uint16_t accent,
  uint16_t green,
  uint16_t red,
  uint16_t white,
  uint16_t muted
) {
  int onlineTotal = 0;
  int expectedTotal = 0;



  for (int m = 0; m < MAX_MESHES; m++) {
    if (!meshes[m].enabled) {
      continue;
    }



    onlineTotal += onlineCount(m);
    expectedTotal += countExpected(m);
  }



  cydPanel(6, 72, 153, 62, panel, border);
  cydCanvas.fillRect(
    6,
    72,
    4,
    62,
    cydArming ? accent : (cydArmed ? green : cydRGB(94, 106, 126))
  );



  cydText("SYSTEM", 18, 79, 1, muted, panel);



  if (cydArming) {
    int secondsLeft = (cydArmingUntil - millis()) / 1000;



    if (secondsLeft < 0) {
      secondsLeft = 0;
    }



    cydText("ARMING " + String(secondsLeft) + "s", 18, 92, 2, accent, panel);
    cydText("Exit delay active", 18, 113, 1, muted, panel);
  } else {
    cydText(
      cydArmed ? "ARMED" : "DISARMED",
      18,
      92,
      2,
      cydArmed ? green : cydRGB(148, 163, 184),
      panel
    );



    cydText(
      cydArmed ? "Monitoring active" : "Console ready",
      18,
      113,
      1,
      muted,
      panel
    );
  }



  cydStatusDot(142, 81, !globalAlarm, green, red);



  cydPanel(165, 72, 149, 62, panel, border);
  cydCanvas.fillRect(165, 72, 4, 62, accent);



  cydText("COMMS", 177, 79, 1, muted, panel);



  cydText(
    String("ESP-NOW ") + (espNowOK ? "ONLINE" : "OFFLINE"),
    177,
    92,
    1,
    espNowOK ? green : red,
    panel
  );



  cydText(
    String("WI-FI ") + (wifiOK ? "ONLINE" : "LOCAL AP"),
    177,
    106,
    1,
    wifiOK ? green : cydRGB(148, 163, 184),
    panel
  );



  cydText(
    String("NODES ") + String(onlineTotal) + "/" + String(expectedTotal),
    177,
    120,
    1,
    onlineTotal == expectedTotal ? green : accent,
    panel
  );



  cydPanel(6, 140, 98, 32, panel, border);
  cydCanvas.fillRect(6, 140, 4, 32, radarAlarmCount() ? red : green);



  cydText("RADAR", 18, 146, 1, muted, panel);
  cydText(
    String(radarAlarmCount()),
    18,
    156,
    2,
    radarAlarmCount() ? red : green,
    panel
  );



  cydPanel(110, 140, 98, 32, panel, border);
  cydCanvas.fillRect(110, 140, 4, 32, tamperCount() ? red : green);



  cydText("TAMPER", 122, 146, 1, muted, panel);
  cydText(
    String(tamperCount()),
    122,
    156,
    2,
    tamperCount() ? red : green,
    panel
  );



  cydPanel(214, 140, 100, 32, panel, border);
  cydCanvas.fillRect(214, 140, 4, 32, lossCount() ? accent : green);



  cydText("LOSS", 226, 146, 1, muted, panel);
  cydText(
    String(lossCount()),
    226,
    156,
    2,
    lossCount() ? accent : green,
    panel
  );



  cydPanel(
    6,
    178,
    308,
    18,
    globalAlarm ? cydRGB(58, 26, 32) : panel,
    globalAlarm ? red : border
  );



  cydCanvas.fillRect(6, 178, 4, 18, globalAlarm ? red : green);



  String statusText = "SYSTEM NORMAL";



  if (cydArmMode == 1) {
    statusText = "ARMED - MODE 1: FULL ALARM";
  } else if (cydArmMode == 2) {
    statusText = "ARMED - MODE 2: PERIMETER";
  } else if (cydArmMode == 3) {
    statusText = "ARMED - MODE 3: SOFT ALARMS";
  }



  cydText(
    globalAlarm
      ? "!! " + globalAlarmText + " !!"
      : statusText,
    18,
    183,
    1,
    globalAlarm ? white : muted,
    globalAlarm ? cydRGB(58, 26, 32) : panel
  );
}



// ============================================================
// CYD NODES PAGE
// ============================================================



static void cydDrawNodes(
  uint16_t panel,
  uint16_t border,
  uint16_t accent,
  uint16_t green,
  uint16_t red,
  uint16_t white,
  uint16_t muted
) {
  int visibleMesh[32];
  int visibleNode[32];
  uint32_t visibleAge[32];
  int visibleCount = 0;



  for (int m = 0; m < MAX_MESHES && visibleCount < 32; m++) {
    for (int n = 1; n <= MAX_NODES && visibleCount < 32; n++) {
      NodeState &candidate = nodes[m][n];



      if (!candidate.seen) {
        continue;
      }



      if (
        candidate.nodeType != NODE_TYPE_RADAR &&
        candidate.nodeType != NODE_TYPE_COMBINED
      ) {
        continue;
      }



      visibleMesh[visibleCount] = m;
      visibleNode[visibleCount] = n;
      visibleAge[visibleCount] = millis() - candidate.lastSeenMs;
      visibleCount++;
    }
  }



  int selMesh = cydSelectedMesh;
  int selNode = cydSelectedNode;
  bool selectedVisible = false;



  for (int i = 0; i < visibleCount; i++) {
    if (visibleMesh[i] == selMesh && visibleNode[i] == selNode) {
      selectedVisible = true;
      break;
    }
  }



  if (!selectedVisible) {
    int best = -1;
    uint32_t newest = UINT32_MAX;



    for (int i = 0; i < visibleCount; i++) {
      if (visibleAge[i] < newest) {
        newest = visibleAge[i];
        best = i;
      }
    }



    if (best >= 0) {
      selMesh = visibleMesh[best];
      selNode = visibleNode[best];
      cydSelectedMesh = selMesh;
      cydSelectedNode = selNode;
    } else {
      selMesh = -1;
      selNode = -1;
    }
  }



  cydPanel(6, 72, 178, 132, panel, border);
  cydCanvas.fillRect(6, 72, 4, 132, accent);



  cydText("NODE STATUS", 18, 79, 1, accent, panel);



  if (selMesh < 0) {
    cydText("NO RADAR NODES", 18, 100, 2, accent, panel);
    cydText("Waiting for Stage 1 packets", 18, 126, 1, muted, panel);
  } else {
    NodeState &node = nodes[selMesh][selNode];
    uint32_t age = millis() - node.lastSeenMs;



    cydText(
      "M" + String(selMesh + 1) + " / N" + String(selNode),
      18,
      94,
      2,
      white,
      panel
    );



    cydText(cydNodeType(node), 18, 114, 1, green, panel);



    cydText("HEARTBEAT", 18, 132, 1, muted, panel);
    cydText(
      String(age / 1000.0f, 1) + " s",
      96,
      132,
      1,
      age < 5000 ? green : red,
      panel
    );



    cydText("PACKETS", 18, 146, 1, muted, panel);
    cydText(String(node.packets), 96, 146, 1, white, panel);



    cydText("SEQ", 18, 160, 1, muted, panel);
    cydText(String(node.lastSequence), 96, 160, 1, white, panel);



    const char *alarmNames[] = {"RADAR", "TAMPER", "LOSS", "FAULT", "BATT"};
    bool alarmActive[] = {
      node.radarAlarm,
      node.tamperAlarm,
      node.lossAlarm,
      node.sensorFault,
      node.lowBattery
    };



    int chipX = 18;



    for (int i = 0; i < 5; i++) {
      if (!alarmActive[i]) {
        continue;
      }



      int chipW = 8 + strlen(alarmNames[i]) * 6;



      if (chipX + chipW > 176) {
        break;
      }



      uint16_t chipBg = (i == 0)
        ? cydRGB(58, 26, 32)
        : cydRGB(45, 38, 18);



      uint16_t chipFg = (i == 0) ? red : cydRGB(251, 191, 36);



      cydCanvas.fillRoundRect(chipX, 174, chipW, 16, 6, chipBg);
      cydText(alarmNames[i], chipX + 4, 178, 1, chipFg, chipBg);



      chipX += chipW + 5;
    }



    bool anyAlarm = (
      node.radarAlarm ||
      node.tamperAlarm ||
      node.lossAlarm ||
      node.sensorFault ||
      node.lowBattery
    );



    if (!anyAlarm) {
      cydText("NO ACTIVE ALARMS", 18, 178, 1, green, panel);
    }
  }



  cydPanel(190, 72, 124, 132, panel, border);
  cydText("NODES", 200, 78, 1, accent, panel);



  if (visibleCount == 0) {
    cydText("NONE", 200, 100, 2, muted, panel);
    cydText("Connect a radar node", 200, 122, 1, muted, panel);
  } else {
    int rowY = 92;



    for (int i = 0; i < visibleCount && i < 6; i++) {
      int m = visibleMesh[i];
      int n = visibleNode[i];
      NodeState &node = nodes[m][n];



      bool selected = (m == selMesh && n == selNode);
      bool online = visibleAge[i] < 5000;



      uint16_t rowBg = selected ? cydRGB(20, 42, 58) : panel;



      cydCanvas.fillRoundRect(194, rowY, 116, 17, 5, rowBg);



      if (selected) {
        cydCanvas.drawRoundRect(194, rowY, 116, 17, 5, accent);
      }



      cydCanvas.fillCircle(
        202,
        rowY + 8,
        3,
        node.lossAlarm ? red : (online ? green : cydRGB(251, 191, 36))
      );



      cydText(
        "M" + String(m + 1) + "/N" + String(n),
        210,
        rowY + 4,
        1,
        selected ? accent : white,
        rowBg
      );



      String right = node.lossAlarm
        ? "LOSS"
        : (online ? String(visibleAge[i] / 1000) + "s" : "STALE");



      cydText(
        right,
        272,
        rowY + 4,
        1,
        node.lossAlarm ? red : (online ? green : cydRGB(251, 191, 36)),
        rowBg
      );



      rowY += 18;
    }



    if (visibleCount > 6) {
      cydText(
        "+" + String(visibleCount - 6) + " MORE",
        200,
        186,
        1,
        muted,
        panel
      );
    } else {
      cydText(
        String(visibleCount) + " RADAR NODE" + (visibleCount == 1 ? "" : "S"),
        200,
        186,
        1,
        muted,
        panel
      );
    }
  }
}



// ============================================================
// CYD SENSOR PAGE
// ============================================================



static void cydDrawSensors(
  uint16_t panel,
  uint16_t border,
  uint16_t accent,
  uint16_t green,
  uint16_t red,
  uint16_t white,
  uint16_t muted
) {
  cydPanel(6, 72, 308, 132, panel, border);
  cydCanvas.fillRect(6, 72, 4, 132, accent);



  cydText("STAGE 1 SENSOR STATUS", 18, 79, 1, accent, panel);



  int row = 98;
  int shownNodes = 0;



  for (int m = 0; m < MAX_MESHES && shownNodes < 4; m++) {
    for (int n = 1; n <= MAX_NODES && shownNodes < 4; n++) {
      NodeState &node = nodes[m][n];



      if (!node.seen) {
        continue;
      }



      if (node.nodeType != NODE_TYPE_SENSOR && node.nodeType != NODE_TYPE_COMBINED) {
        continue;
      }



      String left = "M" + String(m + 1) + " N" + String(n);
      cydText(left, 18, row, 1, white, panel);



      bool online = (millis() - node.lastSeenMs) < 5000;



      cydText(
        online ? "ONLINE" : "LOSS",
        60,
        row,
        1,
        online ? green : red,
        panel
      );



      int sensorShown = 0;



      for (int s = 0; s < MAX_SENSORS && sensorShown < 2; s++) {
        if (!(node.sensorMask & (1UL << s))) {
          continue;
        }



        String val = String(sensorName(s)) + ": " +
                     sensorValueString(s, node.sensor[s]);



        if (val.length() > 26) {
          val = val.substring(0, 26);
        }



        cydText(
          val,
          110 + (sensorShown * 102),
          row,
          1,
          sensorShown == 0 ? white : muted,
          panel
        );



        sensorShown++;
      }



      row += 25;
      shownNodes++;
    }
  }



  if (!shownNodes) {
    cydText("NO SENSOR NODES REPORTING", 24, 122, 1, muted, panel);
    cydText("Waiting for Stage 1 sensor packets", 24, 142, 1, muted, panel);
  }



  cydPanel(6, 188, 308, 18, panel, border);
  cydCanvas.fillRect(6, 188, 4, 18, accent);
  cydText(
    "HEARTBEAT: TIME SINCE LAST STAGE 1 PACKET",
    18,
    193,
    1,
    muted,
    panel
  );
}



// ============================================================
// CYD MODE SELECT SCREEN
// ============================================================



static void cydDrawModeSelect(
  uint16_t bg,
  uint16_t panel,
  uint16_t border,
  uint16_t accent,
  uint16_t white,
  uint16_t muted
) {
  cydCanvas.fillScreen(bg);



  cydCanvas.fillRect(0, 0, cydCanvas.width(), 42, cydRGB(13, 20, 36));
  cydCanvas.fillRect(0, 0, 6, 42, accent);



  cydText("SELECT ARM MODE", 16, 13, 2, accent, cydRGB(13, 20, 36));



  cydButton(20, 70, 280, 44, "MODE 1: FULL ALARM", panel, white, border);
  cydButton(20, 124, 280, 44, "MODE 2: PERIMETER", panel, white, border);
  cydButton(20, 178, 280, 44, "MODE 3: SOFT ALARMS", panel, white, border);



  cydText(
    "CHOOSE A MODE - THEN ENTER PIN",
    30,
    232,
    1,
    muted,
    bg
  );
}



// ============================================================
// CYD PIN KEYPAD
// ============================================================



static void cydDrawKeypad(
  uint16_t bg,
  uint16_t panel,
  uint16_t border,
  uint16_t accent,
  uint16_t red,
  uint16_t white,
  uint16_t muted
) {
  cydCanvas.fillScreen(cydRGB(11, 18, 32));
  cydCanvas.fillRect(0, 0, cydCanvas.width(), 42, cydRGB(13, 20, 36));
  cydCanvas.fillRect(0, 0, 6, 42, accent);
  cydCanvas.drawFastHLine(0, 41, cydCanvas.width(), cydRGB(35, 50, 74));



  const char *heading = (cydPinAction == 2)
    ? "DEACTIVATE SYSTEM"
    : "ACTIVATE SYSTEM";



  cydText(heading, 16, 13, 2, accent, cydRGB(13, 20, 36));



  String shown = cydPinEntry;



  while (shown.length() < 8) {
    shown += "_";
  }



  cydText("PIN", 16, 52, 2, muted, cydRGB(11, 18, 32));
  cydText(shown, 66, 52, 2, white, cydRGB(11, 18, 32));



  if (cydPinMessage.length() && millis() < cydPinMessageUntil) {
    cydText(cydPinMessage, 16, 76, 1, red, cydRGB(11, 18, 32));
  }



  const char *keys[] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    "CLEAR", "0", "ENTER"
  };



  int k = 0;



  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      const int x = 12 + (c * 102);
      const int y = 92 + (r * 36);



      const bool enter = (k == 11);
      const bool clear = (k == 9);



      uint16_t fill = enter
        ? cydRGB(20, 42, 58)
        : (clear ? cydRGB(58, 26, 32) : cydRGB(24, 34, 52));



      uint16_t fg = enter
        ? accent
        : (clear ? red : white);



      cydButton(x, y, 94, 30, keys[k], fill, fg, border);



      k++;
    }
  }



  cydText(
    "CLEAR ERASES - ENTER CONFIRMS",
    66,
    234,
    1,
    muted,
    cydRGB(11, 18, 32)
  );
}



// ============================================================
// CYD TOUCH HANDLING
// ============================================================



static void openCYDPinModal(uint8_t action) {
  cydPinAction = action;
  cydPinModal = true;
  cydPinEntry = "";
  cydPinMessage = "";
  cydPinMessageUntil = 0;
}



static void handleCYDTouch() {
  int16_t x = 0;
  int16_t y = 0;



  if (!getCalibratedTouch(x, y)) {
    cydTouchDown = false;
    return;
  }



  if (cydTouchDown) {
    return;
  }



  cydTouchDown = true;
  cydLastTouchMs = millis();



  uint16_t rawX = 0;
  uint16_t rawY = 0;
  tft.getTouchRaw(&rawX, &rawY);



  Serial.printf(
    "CYD TOUCH mapped=%d,%d raw=%u,%u page=%u modal=%u\n",
    x,
    y,
    rawX,
    rawY,
    cydPage,
    cydPinModal
  );



  if (cydModeSelect) {
    if (x >= 20 && x <= 300 && y >= 70 && y <= 114) {
      cydArmMode = 1;
    } else if (x >= 20 && x <= 300 && y >= 124 && y <= 168) {
      cydArmMode = 2;
    } else if (x >= 20 && x <= 300 && y >= 178 && y <= 222) {
      cydArmMode = 3;
    } else {
      return;
    }



    cydModeSelect = false;
    openCYDPinModal(1);



    Serial.printf("CYD UI: MODE %d selected\n", cydArmMode);
    return;
  }



  if (cydPinModal) {
    if (x >= 270 && y >= 0 && y <= 42) {
      cydPinModal = false;
      cydPinEntry = "";
      cydPinMessage = "";
      cydPinAction = 0;



      Serial.println("CYD PIN cancelled");
      return;
    }



    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 3; c++) {
        const int bx = 12 + (c * 102);
        const int by = 92 + (r * 36);



        if (!(x >= bx - 2 && x <= bx + 96 && y >= by - 2 && y <= by + 32)) {
          continue;
        }



        const int key = (r * 3) + c;



        if (key < 9) {
          if (cydPinEntry.length() < 12) {
            cydPinEntry += char('1' + key);
          }



          Serial.printf("CYD PIN digit %d entered\n", key + 1);
        } else if (key == 9) {
          cydPinEntry = "";
          Serial.println("CYD PIN cleared");
        } else if (key == 10) {
          if (cydPinEntry.length() < 12) {
            cydPinEntry += "0";
          }



          Serial.println("CYD PIN zero entered");
        } else if (key == 11) {
          if (cydPinEntry == String(cydControlPin)) {
            if (cydPinAction == 1) {
              if (armDelaySeconds > 0) {
                cydArming = true;
                cydArmingUntil = millis() + (armDelaySeconds * 1000UL);



                Serial.printf(
                  "CYD CONTROL: MODE %d arm exit delay started, %u seconds\n",
                  cydArmMode,
                  armDelaySeconds
                );
              } else {
                cydArmed = true;



                Serial.printf(
                  "CYD CONTROL: system armed in MODE %d\n",
                  cydArmMode
                );
              }
            } else if (cydPinAction == 2) {
              cydArmed = false;
              cydArming = false;
              cydArmingUntil = 0;
              cydArmMode = 0;



              Serial.println("CYD CONTROL: system disarmed");
            }



            cydPinModal = false;
            cydPinEntry = "";
            cydPinMessage = "";
            cydPinAction = 0;
            beepShort();
          } else {
            cydPinEntry = "";
            cydPinMessage = "INCORRECT PIN";
            cydPinMessageUntil = millis() + 1800;



            Serial.println("CYD PIN: incorrect PIN");
            beepCritical();
          }
        }



        return;
      }
    }



    return;
  }



  if (x >= 286 && x <= 320 && y >= 5 && y <= 27) {
    cydInverted = !cydInverted;
    tft.invertDisplay(cydInverted);



    Serial.printf("CYD UI: screen invert %s\n", cydInverted ? "ON" : "OFF");
    return;
  }



  if (y >= 36 && y <= 68) {
    if (x >= 0 && x <= 108) {
      cydPage = 0;
      Serial.println("CYD UI: OVERVIEW tab");
      return;
    }



    if (x > 108 && x <= 214) {
      cydPage = 1;
      Serial.println("CYD UI: NODES tab");
      return;
    }



    if (x > 214 && x <= 320) {
      cydPage = 2;
      Serial.println("CYD UI: SENSORS tab");
      return;
    }



    return;
  }



  if (
    cydPage == 1 &&
    x >= 190 &&
    x < 314 &&
    y >= 92 &&
    y < 200
  ) {
    int row = (y - 92) / 18;



    if (row >= 0 && row < 6) {
      int index = 0;



      for (int m = 0; m < MAX_MESHES; m++) {
        for (int n = 1; n <= MAX_NODES; n++) {
          NodeState &candidate = nodes[m][n];



          if (!candidate.seen) {
            continue;
          }



          if (
            candidate.nodeType != NODE_TYPE_RADAR &&
            candidate.nodeType != NODE_TYPE_COMBINED
          ) {
            continue;
          }



          if (index == row) {
            cydSelectedMesh = m;
            cydSelectedNode = n;



            Serial.printf("CYD UI: selected M%d/N%d\n", m + 1, n);
            return;
          }



          index++;
        }
      }
    }



    return;
  }



  const int bottomY = cydCanvas.height() - 33;



  if (y >= bottomY - 4 && y <= bottomY + 29) {
    if (x >= 4 && x <= 110) {
      cydModeSelect = true;
      Serial.println("CYD UI: mode selection requested");
      return;
    }



    if (x >= 108 && x <= 214) {
      openCYDPinModal(2);
      Serial.println("CYD UI: deactivate PIN requested");
      return;
    }



    if (x >= 212 && x <= 316) {
      if (globalAlarm) {
        clearAlarms();
        beepShort();
        Serial.println("CYD UI: alarm acknowledged");
      } else {
        Serial.println("CYD UI: no active alarm to acknowledge");
      }



      return;
    }
  }
}



// ============================================================
// CYD MAIN DRAW
// ============================================================



static void drawCYD() {
  if (millis() - lastDisplayUpdate < 250) {
    return;
  }



  lastDisplayUpdate = millis();



  const uint16_t BG = cydRGB(11, 18, 32);
  const uint16_t PANEL = cydRGB(18, 27, 44);
  const uint16_t BORDER = cydRGB(35, 50, 74);
  const uint16_t ACCENT = cydRGB(34, 211, 238);
  const uint16_t GREEN = cydRGB(52, 211, 153);
  const uint16_t RED = cydRGB(248, 113, 113);
  const uint16_t WHITE = cydRGB(229, 231, 235);
  const uint16_t MUTED = cydRGB(148, 163, 184);



  cydCanvas.fillScreen(BG);



  cydHeader(
    cydPage == 0 ? "SYSTEM OVERVIEW" :
    cydPage == 1 ? "RADAR NODES" :
    "SENSORS",
    BG,
    ACCENT,
    WHITE,
    MUTED
  );



  cydTabs(BG, PANEL, ACCENT, WHITE, MUTED);



  if (globalAlarm) {
    cydCanvas.fillRect(0, 68, cydCanvas.width(), 3, RED);
  }



  if (cydModeSelect) {
    cydDrawModeSelect(BG, PANEL, BORDER, ACCENT, WHITE, MUTED);



    if (cydCanvasReady) {
      cydCanvas.pushSprite(0, 0);
    }



    return;
  }



  if (cydPinModal) {
    cydDrawKeypad(BG, PANEL, BORDER, ACCENT, RED, WHITE, MUTED);



    if (cydCanvasReady) {
      cydCanvas.pushSprite(0, 0);
    }



    return;
  }



  if (cydPage == 0) {
    cydDrawOverview(BG, PANEL, BORDER, ACCENT, GREEN, RED, WHITE, MUTED);
  } else if (cydPage == 1) {
    cydDrawNodes(PANEL, BORDER, ACCENT, GREEN, RED, WHITE, MUTED);
  } else {
    cydDrawSensors(PANEL, BORDER, ACCENT, GREEN, RED, WHITE, MUTED);
  }



  cydBottomBar(BG, PANEL, ACCENT, GREEN, RED, WHITE, MUTED);



  if (cydCanvasReady) {
    cydCanvas.pushSprite(0, 0);
  }
}



// ============================================================
// DISPLAY SETUP
// ============================================================



void setupDisplay() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);



  tft.init();
  tft.setRotation(1);



  cydInverted = true;
  tft.invertDisplay(true);



  tft.fillScreen(0x0000);



  cydCanvas.setColorDepth(8);
  cydCanvasReady = cydCanvas.createSprite(tft.width(), tft.height());



  if (!cydCanvasReady) {
    Serial.println("CYD framebuffer allocation failed");



    tft.setTextColor(0x07FF);
    tft.setTextSize(3);
    tft.setCursor(10, 10);
    tft.println("GHOST L2");



    tft.setTextSize(2);
    tft.setCursor(10, 55);
    tft.println("STAGE 2");



    delay(1000);
  }



  pinMode(BUZZER_PIN, OUTPUT);
}



// ============================================================
// SERIAL STATUS
// ============================================================



void printStatus() {
  Serial.println();
  Serial.println();
  Serial.println("GHOST L2 STAGE 2");
  Serial.println();



  Serial.println("Display hardware: NM-CYD-C5");
  Serial.println("Touch system: automatic XPT2046 calibration");



  Serial.print("Touch calibration: ");
  Serial.println(touchCalibrated() ? "VALID" : "NOT CALIBRATED");



  Serial.print("ESP-NOW: ");
  Serial.println(espNowOK ? "OK" : "FAILED");



  Serial.print("WiFi: ");
  Serial.println(wifiOK ? "CONNECTED" : "OFFLINE");



  if (wifiOK) {
    Serial.print("PC dashboard IP: ");
    Serial.println(WiFi.localIP());



    Serial.print("Router channel: ");
    Serial.println(WiFi.channel());
  }



  Serial.print("Mesh channel setting: ");
  Serial.println(meshChannel);



  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());



  Serial.print("Exit delay: ");
  Serial.print(armDelaySeconds);
  Serial.println(" seconds");



  Serial.println("Expected node monitoring:");



  for (int m = 0; m < MAX_MESHES; m++) {
    if (!meshes[m].enabled) {
      continue;
    }



    Serial.print(" - ");
    Serial.print(meshes[m].name);
    Serial.print(": ");
    Serial.println(meshes[m].expectedIds);
  }



  Serial.println();
}



void ensureAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);



  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );



  bool apStarted = WiFi.softAP(
    "GHOST-L2-STAGE2",
    apPassword,
    meshChannel
  );



  Serial.print("AP: ");
  Serial.println(apStarted ? "STARTED" : "FAILED");



  Serial.println("AP SSID: GHOST-L2-STAGE2");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Configured AP channel: ");
  Serial.println(meshChannel);
}



// ============================================================
// WIFI CONNECTION
// ============================================================



bool connectWifi() {
  wifiOK = false;
  udpOK = false;



  Serial.println("Attempting WiFi connection...");



  WiFi.mode(WIFI_AP_STA);
  delay(200);



  ensureAccessPoint();



  for (int i = 0; i < 5; i++) {
    if (strlen(wifiProfiles[i].ssid) == 0) {
      continue;
    }



    Serial.print("Trying WiFi profile ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(wifiProfiles[i].ssid);



    WiFi.begin(wifiProfiles[i].ssid, wifiProfiles[i].pass);



    uint32_t started = millis();



    while (
      WiFi.status() != WL_CONNECTED &&
      millis() - started < 7000
    ) {
      delay(100);
    }



    if (WiFi.status() == WL_CONNECTED) {
      wifiOK = true;



      udpOK = udp.begin(GHOST_UDP_PORT);



      Serial.println("WiFi connected.");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      Serial.print("Router channel: ");
      Serial.println(WiFi.channel());



      ensureAccessPoint();



      return true;
    }



    Serial.println("WiFi profile failed.");



    WiFi.disconnect(false, false);
    delay(300);



    ensureAccessPoint();
  }



  Serial.println("No configured WiFi connected.");
  ensureAccessPoint();



  return false;
}



// ============================================================
// SETUP
// ============================================================



void setup() {
  Serial.begin(115200);
  delay(500);



  Serial.println();
  Serial.println("GHOST L2 STAGE 2 BOOT");



  loadConfig();



  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);



  setupDisplay();
  Serial.println("Display started.");



  ensureAccessPoint();
  Serial.println("Access point started.");



  loadTouchCalibration();



  prefs.begin("ghost2", true);
  forceTouchCalibration = prefs.getBool("calreq", false);
  prefs.end();



  if (!touchCalibrated || forceTouchCalibration) {
    Serial.println("Starting touchscreen calibration...");



    while (!runTouchCalibration()) {
      drawCalibrationScreen(
        "CALIBRATION FAILED",
        "Touch screen to retry",
        -1,
        -1
      );
    }



    uint16_t x = 0;
    uint16_t y = 0;



    while (!tft.getTouchRaw(&x, &y)) {
      delay(10);
    }



    while (tft.getTouchRaw(&x, &y)) {
      delay(10);
    }



    prefs.begin("ghost2", false);
    prefs.putBool("calreq", false);
    prefs.end();
  }



  connectWifi();



  espNowOK = startEspNow();



  setupOTA();
  setupWeb();



  printStatus();



  Serial.println("GHOST L2 STAGE 2 READY");
  beepShort();
}



// ============================================================
// MAIN LOOP
// ============================================================



void loop() {
  web.handleClient();
  processEspNowQueue();



  static uint32_t lastEspNowDebug = 0;



  if (millis() - lastEspNowDebug >= 2000) {
    lastEspNowDebug = millis();



    Serial.printf(
      "ESP-NOW RX:%lu badLength:%lu lastLength:%d expected:%u\n",
      (unsigned long)espNowRxCount,
      (unsigned long)espNowBadLength,
      espNowLastLength,
      (unsigned)sizeof(GhostPacket)
    );
  }



  receiveUDP();



  if (otaOK) {
    ArduinoOTA.handle();
  }



  checkNodeLoss();



  if (cydArming && millis() >= cydArmingUntil) {
    cydArming = false;
    cydArmed = true;
    cydArmingUntil = 0;



    Serial.printf(
      "CYD CONTROL: exit delay complete; system armed in MODE %u\n",
      cydArmMode
    );



    beepShort();
  }



  if (globalAlarm && millis() - lastAlarmBeep >= 4000) {
    lastAlarmBeep = millis();
    beepCritical();
  }



  handleCYDTouch();
  drawCYD();



  if (!wifiOK && millis() - lastWifiAttempt >= 15000) {
    lastWifiAttempt = millis();



    if (connectWifi()) {
      setupOTA();
    } else {
      ensureAccessPoint();
    }
  }



  delay(2);
}