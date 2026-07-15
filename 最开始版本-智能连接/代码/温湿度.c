#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>

const char* ssid     = "raspberry";
const char* password = "duasong111";

// AHT20对象
Adafruit_AHTX0 aht;

void setup() {
    Serial.begin(115200);
    delay(1000);

    // I2C引脚
    Wire.begin(17, 18);

    Serial.println("初始化AHT20...");

    if (!aht.begin()) {
        Serial.println("AHT20初始化失败！");
        while (1) {
            delay(1000);
        }
    }

    Serial.println("AHT20初始化成功");

    // 连接WiFi
    WiFi.begin(ssid, password);

    Serial.print("连接WiFi");

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi连接成功！");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
}

void loop() {
    sensors_event_t humidity;
    sensors_event_t temp;

    aht.getEvent(&humidity, &temp);

    Serial.println("====================");
    Serial.print("温度: ");
    Serial.print(temp.temperature);
    Serial.println(" °C");

    Serial.print("湿度: ");
    Serial.print(humidity.relative_humidity);
    Serial.println(" %");

    delay(2000);
}