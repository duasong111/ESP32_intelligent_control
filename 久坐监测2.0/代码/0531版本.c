#include <Wire.h>
#include <U8g2lib.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ================== WiFi 配置 ==================
const char* WIFI_SSID     = "raspberry";
const char* WIFI_PASSWORD = "duasong111";

// ================== 服务器配置 ==================
const char* SERVER_URL    = "http://192.168.18.149:5001/api/sedentary_report";
const char* DEVICE_ID     = "esp32_001";
const unsigned long UPLOAD_INTERVAL = 60000;

// ================== MQTT 配置 ==================
const char* MQTT_SERVER   = "";
const int   MQTT_PORT     = ;
const char* MQTT_USER     = "";
const char* MQTT_PASSWORD = "";
const char* MQTT_TOPIC = "";

// ================== OLED 配置 ==================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 9, 8);

// ================== LD2402 配置 ==================
#define RADAR_RX_PIN 20
#define RADAR_TX_PIN 21
#define RADAR_IO_PIN 5
#define LED_PIN      18
#define VIBRATE_PIN  19

// ================== 74HC595 引脚配置 ==================
#define SER_PIN  11
#define RCK_PIN  12
#define SCK_PIN  13

HardwareSerial radarSerial(2);

// ================== 参数配置 ==================
const int PRESENCE_THRESHOLD = 200;
const unsigned long WINDOW_MS = 10000;
unsigned long lastMqttStatusPrint = 0;
// ================== LED 模式定义 ==================
enum LedMode {
  LED_PROGRESSIVE,  // 渐进（随久坐时间）
  LED_BLINK,        // 全闪
  LED_CHASE,        // 追逐
  LED_ON,           // 全亮
  LED_OFF,          // 全灭
  LED_BREATHE       // 呼吸
};

// ================== 可被 MQTT 控制的参数 ==================
struct LedConfig {
  LedMode mode        = LED_PROGRESSIVE;
  uint8_t byteValue   = 0;       // on模式时直接写入的字节
  int     brightness  = 2;       // 1=暗 2=中 3=亮（控制闪烁占空比）
  unsigned long intervalMs = 1000;
};

struct VibrateConfig {
  bool enabled          = true;
  unsigned long durationMs  = 500;
  unsigned long intervalSec = 300;
};

LedConfig     ledCfg;
VibrateConfig vibCfg;

// ================== 状态变量 ==================
String uartBuffer        = "";
int currentDistance      = 0;
int displayDistance      = 0;
unsigned long lastDisplayTime  = 0;
unsigned long sitStartTime     = 0;
unsigned long lastVibrateTime  = 0;
unsigned long lastUploadTime   = 0;
int animFrame = 0;

// 统计变量
long  distanceSum   = 0;
int   distanceCount = 0;
int   distanceMax   = 0;
int   distanceMin   = 9999;

// LED 动画状态
unsigned long lastLedUpdateTime = 0;
int  chasePos      = 0;   // 追逐模式当前位置
int  breatheStep   = 0;   // 呼吸模式当前步数
bool breatheDir    = true; // 呼吸方向 true=增 false=减
bool blinkState    = false;

#define MAX_EVENTS 300
unsigned long detectEvents[MAX_EVENTS];
int eventHead = 0;

// ================== WiFi / MQTT ==================
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ================== UUID 生成 ==================
String generateUUID() {
  char uuid[37];
  snprintf(uuid, sizeof(uuid),
    "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
    (unsigned)random(0xFFFF), (unsigned)random(0xFFFF),
    (unsigned)random(0xFFFF),
    (unsigned)(random(0x0FFF) | 0x4000),
    (unsigned)(random(0x3FFF) | 0x8000),
    (unsigned)random(0xFFFF), (unsigned)random(0xFFFF),
    (unsigned)random(0xFFFF));
  return String(uuid);
}

// ================== 74HC595 底层写入 ==================
void write595(uint8_t val) {
  digitalWrite(RCK_PIN, LOW);
  shiftOut(SER_PIN, SCK_PIN, MSBFIRST, val);
  digitalWrite(RCK_PIN, HIGH);
}

// ================== brightness → 占空比时间 ==================
// brightness: 1=暗(短亮长灭) 2=中 3=亮(长亮短灭)
unsigned long brightOnTime() {
  switch (ledCfg.brightness) {
    case 1: return ledCfg.intervalMs / 4;
    case 3: return ledCfg.intervalMs * 3 / 4;
    default: return ledCfg.intervalMs / 2;
  }
}

// ================== LED 模式更新（loop中调用）==================
void updateLEDs(bool someone, unsigned long sitDuration) {
  unsigned long now = millis();

  switch (ledCfg.mode) {

    // ---------- 渐进（随久坐时间，原逻辑）----------
    case LED_PROGRESSIVE: {
      int ledsToLight = (int)(sitDuration / 60000);
      if (!someone) ledsToLight = 0;
      if (ledsToLight > 8) ledsToLight = 8;
      uint8_t mask = 0;
      for (int i = 0; i < ledsToLight; i++) mask |= (1 << (7 - i));
      write595(mask);
      break;
    }

    // ---------- 全闪 ----------
    case LED_BLINK: {
      if (now - lastLedUpdateTime >= ledCfg.intervalMs) {
        blinkState = !blinkState;
        lastLedUpdateTime = now;
      }
      // brightness 控制亮的占比
      unsigned long phase = (now - lastLedUpdateTime);
      bool lit = phase < brightOnTime();
      write595(lit ? 0xFF : 0x00);
      break;
    }

    // ---------- 追逐 ----------
    case LED_CHASE: {
      if (now - lastLedUpdateTime >= ledCfg.intervalMs) {
        chasePos = (chasePos + 1) % 8;
        lastLedUpdateTime = now;
      }
      write595(1 << chasePos);
      break;
    }

    // ---------- 全亮 ----------
    case LED_ON: {
      // byteValue 为 0 时全亮，否则用指定字节
      write595(ledCfg.byteValue == 0 ? 0xFF : ledCfg.byteValue);
      break;
    }

    // ---------- 全灭 ----------
    case LED_OFF: {
      write595(0x00);
      break;
    }

    // ---------- 呼吸（由少到多再到少）----------
    case LED_BREATHE: {
      if (now - lastLedUpdateTime >= ledCfg.intervalMs) {
        lastLedUpdateTime = now;
        if (breatheDir) {
          breatheStep++;
          if (breatheStep >= 8) { breatheStep = 8; breatheDir = false; }
        } else {
          breatheStep--;
          if (breatheStep <= 0) { breatheStep = 0; breatheDir = true; }
        }
      }
      uint8_t mask = 0;
      for (int i = 0; i < breatheStep; i++) mask |= (1 << (7 - i));
      write595(mask);
      break;
    }
  }
}

// ================== 事件窗口 ==================
bool hasPresenceInWindow() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_EVENTS; i++)
    if (detectEvents[i] > 0 && (now - detectEvents[i]) <= WINDOW_MS) return true;
  return false;
}
void recordPresenceEvent() {
  detectEvents[eventHead] = millis();
  eventHead = (eventHead + 1) % MAX_EVENTS;
}
void pruneOldEvents() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_EVENTS; i++)
    if (detectEvents[i] > 0 && (now - detectEvents[i]) > WINDOW_MS) detectEvents[i] = 0;
}

// ================== 距离平滑 ==================
void smoothDistance(int target) {
  if (target <= 0) { displayDistance = 0; return; }
  int diff = target - displayDistance;
  if (diff == 0) return;
  int step = diff / 4;
  if (step == 0) step = (diff > 0) ? 1 : -1;
  displayDistance += step;
}

// ================== WiFi 连接 ==================
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("连接 WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  else
    Serial.println("\nWiFi 失败，离线运行");
}

// ================== MQTT 消息解析 ==================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.println("\n=== MQTT 消息接收 ===");
  Serial.println("Topic: " + String(topic));
  Serial.println("Payload: " + msg);
  Serial.println("Payload Length: " + String(length));

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, msg);
  
  if (err) {
    Serial.print("JSON 解析失败: ");
    Serial.println(err.c_str());
    return;
  }

  // 打印解析后的 JSON（美化格式）
  Serial.println("Parsed JSON:");
  serializeJsonPretty(doc, Serial);
  Serial.println();

  // 类型校验
  String type = doc["type"] | "";
  if (type != "device_control") {
    Serial.println("⚠️ 类型不匹配，跳过处理。收到 type: " + type);
    return;
  }

  Serial.println(" 类型校验通过，开始处理控制命令...");

  // ---- 震动配置 ----
  if (doc.containsKey("vibration")) {
    vibCfg.enabled     = doc["vibration"]["enabled"] | true;
    vibCfg.durationMs  = doc["vibration"]["duration_ms"] | 500;
    vibCfg.intervalSec = doc["vibration"]["interval_sec"] | 300;
    
    Serial.printf("震动配置已更新: enabled=%d, duration=%lu ms, interval=%lu sec\n",
      vibCfg.enabled, vibCfg.durationMs, vibCfg.intervalSec);
  }

  // ---- LED 配置 ----
  if (doc.containsKey("led")) {
    String mode = doc["led"]["mode"] | "progressive";
    Serial.println("收到 LED 模式: " + mode);

    if      (mode == "progressive") ledCfg.mode = LED_PROGRESSIVE;
    else if (mode == "blink")       ledCfg.mode = LED_BLINK;
    else if (mode == "chase")       ledCfg.mode = LED_CHASE;
    else if (mode == "on")          ledCfg.mode = LED_ON;
    else if (mode == "off")         ledCfg.mode = LED_OFF;
    else if (mode == "breathe")     ledCfg.mode = LED_BREATHE;
    else {
      Serial.println("⚠️ 未知 LED 模式，使用默认 progressive");
      ledCfg.mode = LED_PROGRESSIVE;
    }

    ledCfg.byteValue  = doc["led"]["byte_value"]  | 0;
    ledCfg.brightness = doc["led"]["brightness"]  | 2;
    ledCfg.intervalMs = doc["led"]["interval_ms"] | 1000;

    // 重置动画状态
    chasePos = 0;
    breatheStep = 0;
    breatheDir = true;
    blinkState = false;
    lastLedUpdateTime = millis();

    Serial.printf("LED 配置更新完成: mode=%s, brightness=%d, interval=%lu ms\n",
      mode.c_str(), ledCfg.brightness, ledCfg.intervalMs);
  }
}

// ================== MQTT 连接 ==================
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 未连接，无法连接 MQTT");
    return;
  }

  if (mqttClient.connected()) return;

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  Serial.println(" 正在连接 MQTT 服务器...");

  String clientId = "esp32_" + String(random(0xFFFF), HEX);

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println(" MQTT 连接成功！ClientID: " + clientId);
    mqttClient.subscribe(MQTT_TOPIC);
    Serial.println("已订阅 Topic: " + String(MQTT_TOPIC));
  } else {
    Serial.print(" MQTT 连接失败，错误码: ");
    Serial.println(mqttClient.state());
    // 常见错误码说明
    switch (mqttClient.state()) {
      case -4: Serial.println("   → 连接超时"); break;
      case -3: Serial.println("   → 连接丢失"); break;
      case -2: Serial.println("   → 网络连接失败"); break;
      case 1:  Serial.println("   → 错误协议版本"); break;
      case 2:  Serial.println("   → 客户端标识符无效"); break;
      case 3:  Serial.println("   → 服务器不可用"); break;
      case 4:  Serial.println("   → 用户名/密码错误"); break;
      case 5:  Serial.println("   → 未授权"); break;
      default: Serial.println("   → 未知错误"); break;
    }
  }
}

// ================== 数据上传 ==================
void uploadData() {
  if (WiFi.status() != WL_CONNECTED) return;
  int avgDist = (distanceCount > 0) ? (int)(distanceSum / distanceCount) : 0;
  int maxDist = (distanceMax > 0)    ? distanceMax : 0;
  int minDist = (distanceMin < 9999) ? distanceMin : 0;
  String state = (avgDist > 90 || avgDist == 0) ? "无人" : "有人";

  String json = "{";
  json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  json += "\"uuid\":\"" + generateUUID() + "\",";
  json += "\"state\":\"" + state + "\",";
  json += "\"avg_distance_cm\":" + String(avgDist) + ",";
  json += "\"max_distance_cm\":" + String(maxDist) + ",";
  json += "\"min_distance_cm\":" + String(minDist) + ",";
  json += "\"timestamp\":" + String(millis() / 1000);
  json += "}";

  Serial.println("上传: " + json);
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  Serial.println("HTTP: " + String(code));
  http.end();

  distanceSum = 0; distanceCount = 0;
  distanceMax = 0; distanceMin = 9999;
}

// ================== 绘制函数 ==================
void drawRadarIcon(int cx, int cy) {
  u8g2.drawDisc(cx, cy, 1);
  int radii[] = {3, 5, 7};
  for (int ri = 0; ri < 3; ri++) {
    int r = radii[ri];
    float startAngle = -0.785f, endAngle = 0.785f;
    float px = cx + r * cos(startAngle), py = cy + r * sin(startAngle);
    for (int s = 1; s <= 6; s++) {
      float angle = startAngle + (endAngle - startAngle) * s / 6;
      float nx = cx + r * cos(angle), ny = cy + r * sin(angle);
      u8g2.drawLine((int)px, (int)py, (int)nx, (int)ny);
      px = nx; py = ny;
    }
  }
}

void drawVibrateIcon(int x, int y, bool someone) {
  if (!someone) return;
  int w = (animFrame / 6) % 4;
  u8g2.drawLine(x, y-4, x, y+4);
  if (w>=1){u8g2.drawLine(x-2,y-3,x-2,y+3);u8g2.drawLine(x+2,y-3,x+2,y+3);}
  if (w>=2){u8g2.drawLine(x-4,y-4,x-4,y+4);u8g2.drawLine(x+4,y-4,x+4,y+4);}
  if (w>=3){u8g2.drawLine(x-6,y-3,x-6,y+3);u8g2.drawLine(x+6,y-3,x+6,y+3);}
}

void drawTitleBar() {
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  drawRadarIcon(7, 7);
  u8g2.setFont(u8g2_font_6x10_tr);
  int tw = u8g2.getStrWidth("SIT MONITOR");
  u8g2.setCursor((128 - tw) / 2, 11);
  u8g2.print("SIT MONITOR");
  // WiFi 状态点
  if (WiFi.status() == WL_CONNECTED) u8g2.drawDisc(123, 4, 2);
  else                                u8g2.drawCircle(123, 4, 2);
  u8g2.setDrawColor(1);
}

void drawDistanceBig(bool someone) {
  char buf[10];
  snprintf(buf, sizeof(buf), someone && displayDistance > 0 ? "%d" : "--",
           displayDistance);
  u8g2.setFont(u8g2_font_logisoso24_tr);
  int numW = u8g2.getStrWidth(buf);
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int unitW = u8g2.getStrWidth("cm");
  int startX = (128 - numW - 4 - unitW) / 2;
  u8g2.setFont(u8g2_font_logisoso24_tr);
  u8g2.setCursor(startX, 44);
  u8g2.print(buf);
  u8g2.setFont(u8g2_font_logisoso16_tr);
  u8g2.setCursor(startX + numW + 4, 44);
  u8g2.print("cm");
}

void drawSitTimeSmall(unsigned long sitSeconds) {
  char buf[10];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", sitSeconds/60, sitSeconds%60);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.setCursor(128 - u8g2.getStrWidth(buf) - 2, 53);
  u8g2.print(buf);
}

void drawDistanceBar(bool someone) {
  const int BY = 54, BH = 10;
  u8g2.drawFrame(0, BY, 128, BH);
  if (someone && displayDistance > 0) {
    int c = constrain(displayDistance, 20, 100);
    int fw = (int)((float)(c - 20) / 80.0f * 126);
    if (fw > 0) u8g2.drawBox(1, BY+1, fw, BH-2);
    char label[10];
    snprintf(label, sizeof(label), "%d cm", displayDistance);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setDrawColor(2);
    u8g2.setCursor((128 - u8g2.getStrWidth(label)) / 2, BY+8);
    u8g2.print(label);
    u8g2.setDrawColor(1);
  } else {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setCursor((128 - u8g2.getStrWidth("-- cm")) / 2, BY+8);
    u8g2.print("-- cm");
  }
}

void updateOLED(bool someone, unsigned long sitSeconds) {
  smoothDistance(someone ? currentDistance : 0);
  u8g2.clearBuffer();
  drawTitleBar();
  drawVibrateIcon(120, 22, someone);
  drawDistanceBig(someone);
  drawSitTimeSmall(sitSeconds);
  drawDistanceBar(someone);
  u8g2.sendBuffer();
  animFrame++;
}

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(0));

  pinMode(SER_PIN, OUTPUT);
  pinMode(RCK_PIN, OUTPUT);
  pinMode(SCK_PIN, OUTPUT);
  write595(0x00);

  u8g2.begin();
  u8g2.enableUTF8Print();

  pinMode(RADAR_IO_PIN, INPUT);
  pinMode(LED_PIN,      OUTPUT);
  pinMode(VIBRATE_PIN,  OUTPUT);
  digitalWrite(LED_PIN,     LOW);
  digitalWrite(VIBRATE_PIN, LOW);

  memset(detectEvents, 0, sizeof(detectEvents));
  radarSerial.begin(115200, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  // 开机画面
  u8g2.clearBuffer();
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor((128 - u8g2.getStrWidth("SIT MONITOR")) / 2, 11);
  u8g2.print("SIT MONITOR");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(20, 35);
  u8g2.print("Connecting WiFi..");
  u8g2.sendBuffer();

  connectWiFi();

  u8g2.clearBuffer();
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor((128 - u8g2.getStrWidth("SIT MONITOR")) / 2, 11);
  u8g2.print("SIT MONITOR");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.setCursor(10, 30);
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.print("WiFi OK");
    u8g2.setCursor(10, 42);
    u8g2.print(WiFi.localIP().toString().c_str());
  } else {
    u8g2.print("WiFi FAILED - Offline");
  }
  u8g2.sendBuffer();
  delay(1000);

  connectMQTT();

  // LED 自检
  for (int i = 1; i <= 8; i++) {
    uint8_t mask = 0;
    for (int j = 0; j < i; j++) mask |= (1 << (7 - j));
    write595(mask);
    delay(100);
  }
  delay(200);
  write595(0x00);

  // 进度条开机动画
  for (int p = 20; p <= 100; p += 2) {
    displayDistance = p;
    u8g2.clearBuffer();
    u8g2.drawBox(0, 0, 128, 14);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor((128 - u8g2.getStrWidth("SIT MONITOR")) / 2, 11);
    u8g2.print("SIT MONITOR");
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(22, 35);
    u8g2.print("Initializing...");
    drawDistanceBar(true);
    u8g2.sendBuffer();
    delay(20);
  }
  displayDistance = 0;
  delay(300);

  lastUploadTime    = millis();
  lastLedUpdateTime = millis();
  Serial.println("\n=== 久坐监测系统启动 ===");
}

// ================== Loop ==================
void loop() {
  // MQTT 保活
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      static unsigned long lastReconnectAttempt = 0;
      if (millis() - lastReconnectAttempt > 5000) {  // 每5秒尝试重连一次
        lastReconnectAttempt = millis();
        connectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }
  // 定期打印 MQTT 连接状态（每8秒）
  // if (millis() - lastMqttStatusPrint > 8000) {
  //   lastMqttStatusPrint = millis();
  // }

  // 读取雷达
  while (radarSerial.available()) {
    char c = radarSerial.read();
    uartBuffer += c;
    if (c == '\n' && uartBuffer.indexOf("distance:") != -1) {
      int colonPos = uartBuffer.indexOf(':');
      if (colonPos > 0) {
        String distStr = uartBuffer.substring(colonPos + 1);
        distStr.trim();
        currentDistance = distStr.toInt() - 45;
        if (currentDistance < 0) currentDistance = 0;
        if (currentDistance > 0) {
          distanceSum += currentDistance;
          distanceCount++;
          if (currentDistance > distanceMax) distanceMax = currentDistance;
          if (currentDistance < distanceMin) distanceMin = currentDistance;
        }
        if (currentDistance > 0 && currentDistance <= PRESENCE_THRESHOLD)
          recordPresenceEvent();
      }
      uartBuffer = "";
    }
  }

  pruneOldEvents();
  bool windowSomeone = hasPresenceInWindow();
  digitalWrite(LED_PIN, windowSomeone ? HIGH : LOW);

  unsigned long sitDuration = 0;
  if (windowSomeone) {
    if (sitStartTime == 0) {
      sitStartTime    = millis();
      lastVibrateTime = millis();
    }
    sitDuration = millis() - sitStartTime;

    // 震动控制（受 MQTT 参数影响）
    if (vibCfg.enabled &&
        millis() - lastVibrateTime >= vibCfg.intervalSec * 1000UL) {
      digitalWrite(VIBRATE_PIN, HIGH);
      delay(vibCfg.durationMs);
      digitalWrite(VIBRATE_PIN, LOW);
      lastVibrateTime = millis();
      Serial.println("⚠️ 震动触发");
    }
  } else {
    sitStartTime    = 0;
    lastVibrateTime = 0;
  }

  // LED 模式更新
  updateLEDs(windowSomeone, sitDuration);

  // 每60秒上传
  if (millis() - lastUploadTime >= UPLOAD_INTERVAL) {
    uploadData();
    lastUploadTime = millis();
  }

  // OLED 刷新
  if (millis() - lastDisplayTime > 80) {
    unsigned long sitSeconds = (sitStartTime > 0) ? sitDuration / 1000 : 0;
    updateOLED(windowSomeone, sitSeconds);
    lastDisplayTime = millis();
  }

  delay(10);
}
// 嘉立创开源链接 https://oshwhub.com/duasong/project_bexjzztk