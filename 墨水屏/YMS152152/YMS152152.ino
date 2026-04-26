/*
  YMS152152 1.54寸三色墨水屏 - 多任务卡片（增删）
  分辨率: 152x152   驱动芯片: SSD1683
  主控: ESP32
  接线: CS=5 DC=17 RST=16 BUSY=19 SCK=18 MOSI=23
*/

#define ENABLE_GxEPD2_GFX 1

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include "GxEPD2_0154_Z98c.h"
#include <U8g2_for_Adafruit_GFX.h>

// ==================== WiFi ====================
const char* ssid        = "raspberry";
const char* password    = "duasong111";

// ==================== MQTT ====================
const char *mqtt_server   = "60.205.140.163";
const int   mqtt_port     = 1883;
const char *mqtt_user     = "admin";
const char *mqtt_password = "password";
const char *mqtt_topic    = "sensor/data";
const char *control_topic = "control/esp32";

// ==================== 墨水屏 ====================
#define EPD2_COLOR  GxEPD2_3C
#define EPD2_MODE   GxEPD2_0154_Z98c

EPD2_COLOR<EPD2_MODE, EPD2_MODE::HEIGHT> display(EPD2_MODE(/*CS=*/5, /*DC=*/17, /*RST=*/16, /*BUSY=*/19));

// ==================== U8g2 中文 ====================
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// ==================== 全局对象 ====================
WiFiClient   espClient;
PubSubClient mqtt(espClient);

// ==================== 任务列表 ====================
#define MAX_TASKS 3

struct Task {
  String text;
  bool   used;  // 是否有内容
};

Task taskList[MAX_TASKS] = {{"", false}, {"", false}, {"", false}};

// 指令类型
enum CmdType { CMD_NONE, CMD_ADD, CMD_DELETE, CMD_CLEAR };

struct PendingCmd {
  CmdType type;
  String  text;   // ADD 时用
  int     id;     // DELETE 时用（1~3）
};

PendingCmd pendingCmd = {CMD_NONE, "", 0};
bool hasNewCmd = false;

// ==================== 任务操作 ====================
void addTask(String text)
{
  // 找第一个空槽
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!taskList[i].used) {
      taskList[i].text = text;
      taskList[i].used = true;
      Serial.println("[任务] 添加到槽 " + String(i + 1) + ": " + text);
      return;
    }
  }
  // 全满则顶掉最旧（槽0）
  Serial.println("[任务] 已满，顶掉槽1，向前移动");
  taskList[0] = taskList[1];
  taskList[1] = taskList[2];
  taskList[2].text = text;
  taskList[2].used = true;
}

void deleteTask(int id)
{
  // id 是 1~3
  int idx = id - 1;
  if (idx < 0 || idx >= MAX_TASKS) {
    Serial.println("[任务] 删除失败，id 超出范围: " + String(id));
    return;
  }
  if (!taskList[idx].used) {
    Serial.println("[任务] 槽 " + String(id) + " 已经是空的");
    return;
  }
  Serial.println("[任务] 删除槽 " + String(id) + ": " + taskList[idx].text);

  // 删除后向前补位
  for (int i = idx; i < MAX_TASKS - 1; i++) {
    taskList[i] = taskList[i + 1];
  }
  taskList[MAX_TASKS - 1].text = "";
  taskList[MAX_TASKS - 1].used = false;
}

void clearTasks()
{
  Serial.println("[任务] 清空所有任务");
  for (int i = 0; i < MAX_TASKS; i++) {
    taskList[i].text = "";
    taskList[i].used = false;
  }
}

int getTaskCount()
{
  int count = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (taskList[i].used) count++;
  }
  return count;
}

// ==================== 刷新屏幕 ====================
void refreshScreen()
{
  Serial.println("[屏幕] 开始刷新...");

  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);

    // -------- 顶部状态栏 --------
    display.drawLine(0, 20, 152, 20, GxEPD_BLACK);
    u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312a);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    u8g2Fonts.setCursor(4, 15);
    u8g2Fonts.print("任务栏");

    String countStr = String(getTaskCount()) + "/" + String(MAX_TASKS) + "条";
    u8g2Fonts.setCursor(100, 15);
    u8g2Fonts.print(countStr);

    // -------- 任务卡片区 --------
    int cardHeight = 42;
    int startY     = 22;

    for (int i = 0; i < MAX_TASKS; i++) {
      int cardY = startY + i * cardHeight;

      // 卡片分隔线
      display.drawLine(0, cardY + cardHeight, 152, cardY + cardHeight, GxEPD_BLACK);

      // 序号
      u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312a);
      u8g2Fonts.setForegroundColor(GxEPD_BLACK);
      u8g2Fonts.setCursor(4, cardY + 16);
      u8g2Fonts.print("#" + String(i + 1));

      if (taskList[i].used) {
        // 有任务：红色显示
        u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312a);
        u8g2Fonts.setForegroundColor(GxEPD_RED);
        u8g2Fonts.setCursor(4, cardY + 36);

        String task = taskList[i].text;
        if (task.length() > 16) {
          task = task.substring(0, 14) + "..";
        }
        char buf[task.length() + 1];
        task.toCharArray(buf, task.length() + 1);
        u8g2Fonts.print(buf);

      } else {
        // 空槽
        u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312a);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setCursor(4, cardY + 30);
        u8g2Fonts.print("( 空 )");
      }
    }

    // -------- 底部状态栏 --------
    display.drawLine(0, 148, 152, 148, GxEPD_BLACK);
    u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312a);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setCursor(4, 152);

  } while (display.nextPage());

  Serial.println("[屏幕] 刷新完成");
}

// ==================== MQTT 回调 ====================
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  String raw = "";
  for (unsigned int i = 0; i < length; i++) {
    raw += (char)payload[i];
  }
  Serial.println("----------------------------------------");
  Serial.println("[MQTT] 收到消息");
  Serial.println("[MQTT] Topic  : " + String(topic));
  Serial.println("[MQTT] Payload: " + raw);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, raw);

  if (error) {
    Serial.println("[MQTT] JSON 解析失败: " + String(error.c_str()));
    Serial.println("----------------------------------------");
    return;
  }

  if (!doc.containsKey("type")) {
    Serial.println("[MQTT] 缺少 type 字段，忽略");
    Serial.println("----------------------------------------");
    return;
  }

  String type = doc["type"].as<String>();
  Serial.println("[MQTT] 指令类型: " + type);

  if (type == "add") {
    if (doc.containsKey("text")) {
      pendingCmd.type = CMD_ADD;
      pendingCmd.text = doc["text"].as<String>();
      hasNewCmd = true;
      Serial.println("[MQTT] 添加任务: " + pendingCmd.text);
    } else {
      Serial.println("[MQTT] add 指令缺少 text 字段");
    }

  } else if (type == "delete") {
    if (doc.containsKey("id")) {
      pendingCmd.type = CMD_DELETE;
      pendingCmd.id   = doc["id"].as<int>();
      hasNewCmd = true;
      Serial.println("[MQTT] 删除任务 id: " + String(pendingCmd.id));
    } else {
      Serial.println("[MQTT] delete 指令缺少 id 字段");
    }

  } else if (type == "clear") {
    pendingCmd.type = CMD_CLEAR;
    hasNewCmd = true;
    Serial.println("[MQTT] 清空所有任务");

  } else {
    Serial.println("[MQTT] 未知 type: " + type);
  }

  Serial.println("----------------------------------------");
}

// ==================== WiFi 连接 ====================
void connectWiFi()
{
  Serial.println("----------------------------------------");
  Serial.println("[WiFi] 开始连接: " + String(ssid));
  WiFi.begin(ssid, password);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    retry++;
    if (retry % 20 == 0) {
      Serial.println();
      Serial.println("[WiFi] 仍在尝试连接...");
    }
  }

  Serial.println();
  Serial.println("[WiFi] 连接成功！");
  Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
  Serial.println("[WiFi] 信号: " + String(WiFi.RSSI()) + " dBm");
  Serial.println("----------------------------------------");
}

// ==================== MQTT 连接 ====================
void connectMQTT()
{
  while (!mqtt.connected()) {
    Serial.println("----------------------------------------");
    Serial.println("[MQTT] 连接: " + String(mqtt_server) + ":" + String(mqtt_port));

    String clientId = "ESP32-" + String(random(0xffff), HEX);
    Serial.println("[MQTT] Client ID: " + clientId);

    if (mqtt.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("[MQTT] 连接成功！");
      mqtt.subscribe(control_topic);
      Serial.println("[MQTT] 已订阅: " + String(control_topic));
      Serial.println("----------------------------------------");
    } else {
      Serial.println("[MQTT] 失败，错误码: " + String(mqtt.state()));
      Serial.println("[MQTT] 3秒后重试...");
      Serial.println("----------------------------------------");
      delay(3000);
    }
  }
}

// ==================== setup ====================
void setup()
{
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("     MQTT 任务栏墨水屏启动");
  Serial.println("========================================");

  Serial.println("[SPI]  初始化...");
  SPI.begin(18, -1, 23, 5);
  Serial.println("[SPI]  完成");

  Serial.println("[屏幕] 初始化...");
  display.init();
  display.setRotation(1);
  u8g2Fonts.begin(display);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  Serial.println("[屏幕] 完成");

  refreshScreen();

  connectWiFi();

  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  refreshScreen();

  Serial.println("========================================");
  Serial.println("     系统就绪，等待指令...");
  Serial.println("========================================");
}

// ==================== loop ====================
void loop()
{
  if (!mqtt.connected()) {
    Serial.println("[MQTT] 断开，重连中...");
    connectMQTT();
  }
  mqtt.loop();

  if (hasNewCmd) {
    hasNewCmd = false;
    switch (pendingCmd.type) {
      case CMD_ADD:
        addTask(pendingCmd.text);
        break;
      case CMD_DELETE:
        deleteTask(pendingCmd.id);
        break;
      case CMD_CLEAR:
        clearTasks();
        break;
      default:
        break;
    }
    refreshScreen();
  }
}