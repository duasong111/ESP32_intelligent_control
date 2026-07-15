#include <WiFi.h>
#include "Audio.h"

const char* ssid     = "raspberry";
const char* password = "duasong111";

Audio audio;

// I2S 引脚
#define I2S_DOUT 11
#define I2S_BCLK 9
#define I2S_LRC  10

void setup() {

  Serial.begin(115200);

  WiFi.begin(ssid, password);

  Serial.print("正在连接 WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi 连接成功");

  // I2S 配置
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

  // 音量 0~21
  audio.setVolume(20);

  // 网络电台
  audio.connecttohost("http://stream.live.vc.bbcmedia.co.uk/bbc_world_service");
}

void loop() {
  audio.loop();
}