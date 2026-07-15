#include <HardwareSerial.h>
#include <WiFi.h>

const char* WIFI_SSID     = "raspberry";
const char* WIFI_PASSWORD = "duasong111";

#define RADAR_RX_PIN  39
#define RADAR_TX_PIN  38
#define RADAR_IO_PIN   5

HardwareSerial radarSerial(1);

// 状态变量
String uartBuffer    = "";
int    currentDistance = 0;
bool   targetDetected  = false;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] 连接中");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] 连接成功 IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] 连接失败，离线运行");
  }
}

void readRadar() {
  while (radarSerial.available()) {
    char c = radarSerial.read();
    uartBuffer += c;

    if (c == '\n') {
      uartBuffer.trim();

      if (uartBuffer == "OFF") {
        targetDetected  = false;
        currentDistance = 0;
      } else if (uartBuffer.startsWith("distance:")) {
        currentDistance = uartBuffer.substring(9).toInt();
        targetDetected  = true;
      }

      uartBuffer = "";
    }

    if (uartBuffer.length() > 64) uartBuffer = "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== ESP32-S3 雷达 & WiFi 测试 =====");

  pinMode(RADAR_IO_PIN, INPUT);
  radarSerial.begin(115200, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  Serial.println("[Radar] 串口初始化完成 RX=29 TX=28");

  connectWiFi();
  Serial.println("===== 开始监测 =====\n");
}

unsigned long lastPrintTime = 0;

void loop() {
  readRadar();

  if (millis() - lastPrintTime >= 1000) {
    lastPrintTime = millis();
    bool ioHigh = digitalRead(RADAR_IO_PIN);
    Serial.println("-----------------------------");
    Serial.printf("[IO]    OUT引脚: %s\n",      ioHigh ? "HIGH(有人)" : "LOW(无人)");
    Serial.printf("[Radar] 目标状态: %s\n",     targetDetected ? "有人" : "无人");
    Serial.printf("[Radar] 距离: %d cm\n",      currentDistance);
    Serial.printf("[WiFi]  状态: %s  RSSI: %d dBm\n",
      WiFi.status() == WL_CONNECTED ? "已连接" : "断开", WiFi.RSSI());
  }

  delay(10);
}