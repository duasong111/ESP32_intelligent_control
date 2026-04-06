
#include <TFT_eSPI.h>
#include <DHT.h>
#include <WiFi.h>
#include "time.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// ========= WiFi 配置 =========
const char *ssid     = "raspberry";
const char *password = "duasong111";

// ========= MQTT 配置 =========
const char *mqtt_server = "";
const int mqtt_port = 1883;
const char *mqtt_user = "";
const char *mqtt_password = "";

const char *mqtt_topic = "sensor/data";
const char *control_topic = "control/esp32";

// ========= 设备专属控制 Topic =========
String deviceControlTopic;

// ========= DHT11 配置 =========
#define DHTPIN 27
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ========= HC-SR04 配置 =========
#define TRIG_PIN 13
#define ECHO_PIN 12

// ========= RGB 配置 =========
#define RED_PIN 14
#define GREEN_PIN 15
#define BLUE_PIN 16

// ========= 蜂鸣器配置 =========
#define BUZZER_PIN 25

// ========= TFT =========
TFT_eSPI tft = TFT_eSPI();

// ========= NTP =========
const char* ntpServer = "ntp.aliyun.com";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

// ========= MQTT =========
WiFiClient espClient;
PubSubClient client(espClient);

// ========= 传感器数据上报 =========
const String alertUrl   = "http://192.168.18.210:8000/api/device/sensor_data";
const String deviceId   = "esp32_001";          // ← 已改为你测试脚本里的设备ID（可自行修改）
const String secretKey  = "hqcgk2JZQYhmJr67puec";   
unsigned long lastAlertTime = 0;
const unsigned long alertInterval = 60000UL;   // 1分钟（原注释写错，已修正；如需4分钟请改成240000UL）

// 临时文本显示
bool showingText = false;
unsigned long textStartTime = 0;
unsigned long textDuration = 10000;
String textContent = "";
int textFontSize = 2;
bool textScroll = false;
int textX = 10;
int textY = 100;
uint16_t textColor = TFT_WHITE;
uint16_t bgColor = TFT_BLACK;
unsigned long lastScrollUpdate = 0;
const unsigned long scrollInterval = 50;

// 蜂鸣器控制
bool buzzerActive = false;
int buzzerFrequency = 2000;
int buzzerDuration = 500;
int buzzerInterval = 200;
int buzzerCycles = 1;
int buzzerCycleCount = 0;
bool buzzerIsOn = false;
unsigned long buzzerPhaseStart = 0;

// 刷新计时
unsigned long lastScreenUpdate = 0;
unsigned long lastMQTTUpdate = 0;
const unsigned long screenInterval = 250;   // 推荐值，减少闪烁
const unsigned long mqttInterval = 5000;

// 全局传感器缓存（提高稳定性）
float currentTemp = NAN;
float currentHum = NAN;
float currentDistance = -1.0;

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);        // 如方向不对，可改成 1、2 或 3
  tft.fillScreen(TFT_BLACK);

  dht.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  analogWrite(RED_PIN, 0);
  analogWrite(GREEN_PIN, 0);
  analogWrite(BLUE_PIN, 0);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // WiFi 连接提示
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(40, 110);
  tft.println("WiFi Connecting");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi 已连接");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  deviceControlTopic = "devices/" + deviceId + "/control";
  Serial.println("设备专属控制 Topic: " + deviceControlTopic);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  tft.fillScreen(TFT_BLACK);
  delay(500);

  tone(BUZZER_PIN, 1200, 200);
  delay(250);
  noTone(BUZZER_PIN);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("【收到控制指令】 Topic: ");
  Serial.print(topic);
  Serial.print(" | Payload: ");
  Serial.println(message);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.println("JSON 解析失败");
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "buzzer") == 0) {
    const char* state = doc["state"] | "off";
    if (strcmp(state, "off") == 0) {
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
      buzzerActive = false;
      buzzerIsOn = false;
      Serial.println("→ 蜂鸣器已关闭");
      return;
    }
    if (strcmp(state, "on") == 0) {
      buzzerFrequency = doc["frequency"] | 2000;
      buzzerDuration = doc["duration"] | 500;
      buzzerInterval = doc["interval"] | 200;
      buzzerCycles = doc["cycles"] | 1;
      buzzerActive = true;
      buzzerCycleCount = 0;
      buzzerIsOn = false;
      buzzerPhaseStart = millis();
      Serial.println("→ 蜂鸣器开启");
      return;
    }
  }

  if (strcmp(type, "text") == 0) {
    textContent = doc["text"] | doc["content"] | "无内容";
    textDuration = (doc["duration"] | 10UL) * 1000UL;
    textScroll = doc["scroll"] | false;
    textFontSize = doc["font_size"] | 2;
    if (textFontSize < 1) textFontSize = 1;
    if (textFontSize > 5) textFontSize = 5;
    textX = 10;
    showingText = true;
    textStartTime = millis();
    lastScrollUpdate = millis();
    return;
  }

  // RGB 控制
  const char* state = doc["state"] | "on";
  const char* colorStr = doc["color"] | "white";
  int brightness = doc["brightness"] | 255;
  if (brightness < 0) brightness = 0;
  if (brightness > 255) brightness = 255;

  if (strcmp(state, "off") == 0) {
    analogWrite(RED_PIN, 0);
    analogWrite(GREEN_PIN, 0);
    analogWrite(BLUE_PIN, 0);
    Serial.println("→ 执行：关灯");
    return;
  }

  int r = 255, g = 255, b = 255;
  if (strcmp(colorStr, "red") == 0)      { r=255; g=0; b=0; }
  else if (strcmp(colorStr, "green") == 0) { r=0; g=255; b=0; }
  else if (strcmp(colorStr, "blue") == 0)  { r=0; g=0; b=255; }
  else if (strcmp(colorStr, "yellow") == 0){ r=255; g=255; b=0; }
  else if (strcmp(colorStr, "purple") == 0){ r=255; g=0; b=255; }
  else if (strcmp(colorStr, "cyan") == 0)  { r=0; g=255; b=255; }

  analogWrite(RED_PIN, map(r, 0, 255, 0, brightness));
  analogWrite(GREEN_PIN, map(g, 0, 255, 0, brightness));
  analogWrite(BLUE_PIN, map(b, 0, 255, 0, brightness));
  Serial.println("→ RGB 小灯已更新");
}

void reconnectMQTT() {
  int retry = 0;
  while (!client.connected() && retry < 5) {
    Serial.print("MQTT 连接中...");
    String clientId = "ESP32Sensor-" + String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("已连接");
      client.subscribe(control_topic);
      client.subscribe(deviceControlTopic.c_str());
      Serial.println("已订阅通用控制: " + String(control_topic));
      Serial.println("已订阅设备专属控制: " + deviceControlTopic);
    } else {
      Serial.print("失败 rc=");
      Serial.println(client.state());
      delay(5000);
    }
    retry++;
  }
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01 00:00:00";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1.0;
  float distance = duration * 0.0343 / 2.0;
  if (distance < 2.0 || distance > 450.0) return -1.0;
  return distance;
}

void sendToMQTT(float temp, float hum, float distance) {
  if (!client.connected()) reconnectMQTT();
  if (client.connected()) {
    String payload = "{";
    payload += "\"time\":\"" + getFormattedTime() + "\",";
    payload += "\"temperature\":" + (isnan(temp) ? "\"--\"" : String(temp, 1)) + ",";
    payload += "\"humidity\":" + (isnan(hum) ? "\"--\"" : String(hum, 1)) + ",";
    payload += "\"distance\":" + (distance < 0 ? "\"--\"" : String(distance, 1));
    payload += "}";
    client.publish(mqtt_topic, payload.c_str());
  }
}

// 发送传感器数据到本地服务器 + 打印发送内容（方便调试）
void sendSensorData(float temp, float hum, float distance) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 未连接，无法发送数据");
    return;
  }

  HTTPClient http;
  http.begin(alertUrl);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["device_id"] = deviceId;
  doc["secret_key"] = secretKey;

  if (isnan(temp)) {
    doc["temperature"] = nullptr;
  } else {
    doc["temperature"] = temp;
  }

  if (isnan(hum)) {
    doc["humidity"] = nullptr;
  } else {
    doc["humidity"] = hum;
  }

  doc["distance"] = (distance < 0) ? -1.0f : distance;

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);
  if (httpResponseCode > 0) {
    Serial.printf("数据发送成功，响应码: %d\n", httpResponseCode);
  } else {
    Serial.printf("发送失败，错误: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  http.end();
}

void handleBuzzer(unsigned long currentMillis) {
  if (!buzzerActive) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerIsOn = false;
    return;
  }

  if (buzzerCycles == -1 || buzzerCycleCount < buzzerCycles) {
    if (buzzerIsOn) {
      if (currentMillis - buzzerPhaseStart >= buzzerDuration) {
        noTone(BUZZER_PIN);
        digitalWrite(BUZZER_PIN, LOW);
        buzzerIsOn = false;
        buzzerPhaseStart = currentMillis;
        buzzerCycleCount++;
      }
    } else {
      if (currentMillis - buzzerPhaseStart >= buzzerInterval) {
        tone(BUZZER_PIN, buzzerFrequency);
        buzzerIsOn = true;
        buzzerPhaseStart = currentMillis;
      }
    }
  } else {
    buzzerActive = false;
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void loop() {
  unsigned long currentMillis = millis();
  client.loop();
  handleBuzzer(currentMillis);

  // ==================== 统一读取传感器 ====================
  if (currentMillis - lastMQTTUpdate >= mqttInterval) {
    lastMQTTUpdate = currentMillis;
    currentTemp = dht.readTemperature();
    currentHum = dht.readHumidity();
    currentDistance = getDistance();

    sendToMQTT(currentTemp, currentHum, currentDistance);
  }

  // 每1分钟上报到本地服务
  if (currentMillis - lastAlertTime >= alertInterval) {
    lastAlertTime = currentMillis;
    sendSensorData(currentTemp, currentHum, currentDistance);
  }

  // ==================== 屏幕刷新（已去掉红色圈圈 + 去重） ====================
  if (currentMillis - lastScreenUpdate >= screenInterval) {
    lastScreenUpdate = currentMillis;

    if (showingText && (currentMillis - textStartTime < textDuration)) {
      if (textScroll && (currentMillis - lastScrollUpdate >= scrollInterval)) {
        lastScrollUpdate = currentMillis;
        textX -= 3;
        int textWidth = tft.textWidth(textContent);
        if (textX < -textWidth - 20) {
          textX = tft.width() + 20;
        }
      }
      tft.fillScreen(bgColor);
      tft.setTextSize(textFontSize);
      tft.setTextColor(textColor, bgColor);
      tft.setCursor(textX, textY);
      tft.print(textContent);
    } 
    else {
      if (showingText) {
        showingText = false;
        bgColor = TFT_BLACK;
        tft.fillScreen(TFT_BLACK);
        Serial.println("→ 恢复正常界面");
      }

      static bool firstNormalDraw = true;
      if (firstNormalDraw) {
        tft.fillScreen(TFT_BLACK);
        firstNormalDraw = false;
      }

      // 只保留一个浅灰色外圈（已去掉红色圈圈）
      tft.drawCircle(120, 120, 118, TFT_DARKGREY);

      // 时间与日期
      char timeStr[10] = "--:--:--";
      char dateStr[16] = "--/--";
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        strftime(dateStr, sizeof(dateStr), "%m-%d", &timeinfo);
      }

      tft.setTextSize(3);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.setCursor(30, 40);
      tft.print(timeStr);

      tft.setTextSize(2);
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(75, 80);
      tft.print(dateStr);

      // 传感器显示（使用全局缓存）
      tft.setTextSize(2);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(20, 130);
      if (isnan(currentTemp))
        tft.print("Temp: --.- C");
      else
        tft.printf("Temp: %.1f C", currentTemp);

      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setCursor(20, 160);
      if (isnan(currentHum))
        tft.print("Hum : --.- %");
      else
        tft.printf("Hum : %.1f %%", currentHum);

      tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
      tft.setCursor(20, 190);
      if (currentDistance < 0)
        tft.print("Dist: -- cm");
      else
        tft.printf("Dist: %.1f cm", currentDistance);
    }
  }
}