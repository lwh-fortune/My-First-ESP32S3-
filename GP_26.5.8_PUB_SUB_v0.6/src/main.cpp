#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <driver/i2s.h>
#include <OneButton.h>
#include <ESP32Servo.h>
#include <NTPClient.h>
#include <SPIFFS.h>           
#include <esp32-hal-psram.h>  

// ====================== 硬件引脚定义 ======================
#define TFT_BL     2
#define TFT_CS     42
#define TFT_DC     41
#define TFT_RST    40
#define TFT_MOSI   20
#define TFT_SCLK   21

#define SPK_LRC    14
#define SPK_BCLK   13
#define SPK_DIN    12

#define LED_PIN    19
#define FAN_PIN    48  
#define HUM_PIN    35  
#define SERVO_PIN  47  

#define BTN_LED    10
#define BTN_FAN    9
#define BTN_HUM    11

#define DHT20_SDA  7
#define DHT20_SCL  15
#define DHT20_ADDR 0x38

// ====================== 音频配置 ======================
#define BUFFER_SAMPLES 4096        
int16_t* wavBuffer = nullptr;      
volatile bool isPlayingMusic = false; 
TaskHandle_t musicTaskHandle = NULL; 
bool musicAvailable = false;   // 音频资源是否就绪

// ====================== 颜色 ======================
#define TFT_BLACK     0x0000
#define TFT_WHITE     0xFFFF
#define TFT_RED       0xF800
#define TFT_GREEN     0x07E0
#define TFT_BLUE      0x001F
#define TFT_CYAN      0x07FF
#define TFT_YELLOW    0xFFE0
#define TFT_DARKGREY  0x2945

// ====================== 全局对象 ======================
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600, 60000); 
Servo curtainServo;

OneButton btnLED(BTN_LED, true, true);
OneButton btnFan(BTN_FAN, true, true);
OneButton btnHum(BTN_HUM, true, true);

// ====================== 系统状态 ======================
float temperature = 0.0;
float humidity = 0.0;
bool ledState = false;
bool fanState = false;
bool humState = false;
bool curtainOpen = false; 
String currentTimeStr = "00:00";
bool wifiConnected = false;

// OneNET MQTT 配置
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* MQTT_SERVER = "mqtts.heclouds.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "YOUR_DEVICE_ID";
const char* MQTT_USERNAME = "YOUR_PRODUCT_ID";
const char* MQTT_PASSWORD = "YOUR_TOKEN_OR_SECURITY_KEY";

const char* MQTT_PUB_TOPIC = "$sys/YOUR_PRODUCT_ID/YOUR_DEVICE_ID/thing/property/post";
const char* MQTT_SUB_TOPIC  = "$sys/YOUR_PRODUCT_ID/YOUR_DEVICE_ID/thing/property/set";
const char* MQTT_REPLY_TOPIC = "$sys/YOUR_PRODUCT_ID/YOUR_DEVICE_ID/thing/property/set_reply";

// ====================== 前置声明 ======================
void drawUI();
void uploadToOneNET(); 
void playFeedbackSound();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void handleMusicToggle();

// ====================== 音乐播放任务 ======================
void playMusicTask(void *parameter) {
    if (!musicAvailable || !wavBuffer) {
        Serial.println("音乐播放中止：资源未就绪");
        isPlayingMusic = false;
        musicTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    File file = SPIFFS.open("/test.wav");
    if (!file) {
        Serial.println("打开 /test.wav 失败！");
        isPlayingMusic = false;
        musicTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    Serial.println("开始播放音乐...");
    file.seek(44);                       // 跳过 WAV 头
    i2s_start(I2S_NUM_0);

    size_t bytesRead, bytesWritten;
    while (file.available() && isPlayingMusic) {
        bytesRead = file.read((uint8_t*)wavBuffer, BUFFER_SAMPLES * sizeof(int16_t));
        if (bytesRead == 0) break;
        i2s_write(I2S_NUM_0, wavBuffer, bytesRead, &bytesWritten, portMAX_DELAY);
    }

    // 停止播放，清理资源
    Serial.println("音乐停止");
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_stop(I2S_NUM_0);
    file.close();
    isPlayingMusic = false;
    musicTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ====================== 提示音 ======================
void playFeedbackSound() {
    if (isPlayingMusic) return;
    if (!musicAvailable) return;
    if (musicTaskHandle != NULL) {   // 音乐任务还在运行，I2S 可能被占用
        Serial.println("提示音跳过：音乐任务尚未完全退出");
        return;
    }

    size_t bytes_written;
    int16_t sample = 0;
    i2s_start(I2S_NUM_0);
    for (int i = 0; i < 1200; i++) {
        sample = (i % 16 < 8) ? 4000 : -4000;
        i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_stop(I2S_NUM_0);
}

// ====================== MQTT 上传 ======================
void uploadToOneNET() {
    if (!mqttClient.connected()) return;

    String jsonData = "{\"id\":\"123\",\"version\":\"1.0\",\"params\":{";
    jsonData += "\"Temperature\":{\"value\":" + String(temperature, 1) + "},";
    jsonData += "\"Humidity\":{\"value\":" + String(humidity, 1) + "},";
    jsonData += "\"Led_switch\":{\"value\":" + String(ledState ? "true" : "false") + "},";
    jsonData += "\"Fan_switch\":{\"value\":" + String(fanState ? "true" : "false") + "},";
    jsonData += "\"Hum_switch\":{\"value\":" + String(humState ? "true" : "false") + "},";
    jsonData += "\"Servo_switch\":{\"value\":" + String(curtainOpen ? "true" : "false") + "}";
    jsonData += "},\"method\":\"thing.property.post\"}";

    Serial.println("正在上传: " + jsonData);
    mqttClient.publish(MQTT_PUB_TOPIC, jsonData.c_str());
}

// ====================== MQTT 回调 ======================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[CALLBACK] 触发！Topic: %s\n", topic);

    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    Serial.println("[CALLBACK] 原始消息: " + message);

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        Serial.print("[DEBUG] 解析失败: ");
        Serial.println(error.c_str());
        return;
    }
    Serial.println("[DEBUG] 解析成功");

    JsonObject params = doc["params"].as<JsonObject>();
    if (params.isNull()) {
        Serial.println("[DEBUG] params 为空");
        return;
    }

    bool changed = false;

    if (params.containsKey("Led_switch")) {
        bool val = params["Led_switch"].as<bool>();
        Serial.printf("[DEBUG] 找到 Led_switch = %d\n", val);
        ledState = val;
        digitalWrite(LED_PIN, ledState);
        changed = true;
    }
    if (params.containsKey("Fan_switch")) {
        bool val = params["Fan_switch"].as<bool>();
        Serial.printf("[DEBUG] 找到 Fan_switch = %d\n", val);
        fanState = val;
        digitalWrite(FAN_PIN, fanState);
        changed = true;
    }
    if (params.containsKey("Hum_switch")) {
        bool val = params["Hum_switch"].as<bool>();
        Serial.printf("[DEBUG] 找到 Hum_switch = %d\n", val);
        humState = val;
        digitalWrite(HUM_PIN, humState);
        changed = true;
    }
    if (params.containsKey("Servo_switch")) {
        bool val = params["Servo_switch"].as<bool>();
        Serial.printf("[DEBUG] 找到 Servo_switch = %d\n", val);
        curtainOpen = val;
        curtainServo.write(curtainOpen ? 90 : 0);
        changed = true;
    }

    // 在 mqttCallback 的 if (changed) 之前加入
    if (params.containsKey("Music_switch")) {
         bool val = params["Music_switch"].as<bool>();
         Serial.printf("[DEBUG] 找到 Music_switch = %d\n", val);
     // 确保状态与云端同步：云端开则播放，关则停止
         if (val != isPlayingMusic) {
             handleMusicToggle();   // 内部切换音乐并刷新UI
            }
              changed = true;
        }

    if (changed) {
        playFeedbackSound();
        drawUI();
        uploadToOneNET();

        const char* msgId = doc["id"] | "unknown";
        String rPayload = "{\"id\":\"" + String(msgId) + "\",\"code\":200,\"msg\":\"success\"}";
        Serial.println("[CALLBACK] 回复: " + rPayload);
        mqttClient.publish(MQTT_REPLY_TOPIC, rPayload.c_str());
    } else {
        Serial.println("[CALLBACK] 没有匹配到任何属性，不回复");
    }
}

// ====================== UI 与 传感器 ======================
void drawUI() {
    tft.fillRect(0, 35, 240, 205, TFT_BLACK);
    tft.drawLine(0, 35, 240, 35, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW); tft.setCursor(10, 50); tft.print("Temp:"); 
    tft.setTextColor(TFT_WHITE); tft.print(temperature, 1); tft.print("C");
    tft.setTextColor(TFT_CYAN); tft.setCursor(10, 80); tft.print("Hum :"); 
    tft.setTextColor(TFT_WHITE); tft.print(humidity, 1); tft.print("%");
    
    tft.setTextColor(TFT_WHITE); tft.setCursor(10, 130); tft.print("LED:");
    tft.setTextColor(ledState ? TFT_GREEN : TFT_DARKGREY); tft.print(ledState ? "ON " : "OFF");
    tft.setTextColor(TFT_WHITE); tft.setCursor(120, 130); tft.print("Cur:");
    tft.setTextColor(curtainOpen ? TFT_GREEN : TFT_DARKGREY); tft.print(curtainOpen ? "ON " : "OFF");
    tft.setTextColor(TFT_WHITE); tft.setCursor(10, 170); tft.print("Fan:");
    tft.setTextColor(fanState ? TFT_GREEN : TFT_DARKGREY); tft.print(fanState ? "ON " : "OFF");
    tft.setTextColor(TFT_WHITE); tft.setCursor(120, 170); tft.print("Hum:");
    tft.setTextColor(humState ? TFT_GREEN : TFT_DARKGREY); tft.print(humState ? "ON " : "OFF");

    // 音乐状态：标签 Mus，颜色与其它设备一致，状态用 ON/OFF
    tft.setTextColor(TFT_WHITE); tft.setCursor(10, 210); tft.print("Mus:");
    tft.setTextColor(isPlayingMusic ? TFT_GREEN : TFT_DARKGREY);
    tft.print(isPlayingMusic ? "ON " : "OFF");
}

void updateTimeUI() {
    tft.fillRect(0, 0, 240, 34, TFT_BLACK);
    tft.setTextSize(2); tft.setTextColor(TFT_WHITE);
    tft.setCursor(5, 10); tft.print(currentTimeStr);
    tft.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED);
    tft.setCursor(140, 10); tft.print(wifiConnected ? "ONLINE" : "OFFLINE");
}

bool readDHT20() {
    uint8_t data[6] = {0};
    Wire.beginTransmission(DHT20_ADDR);
    Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
    if (Wire.endTransmission() == 0) {
        delay(100);
        Wire.requestFrom(DHT20_ADDR, 6);
        if (Wire.available() == 6) {
            for (int i = 0; i < 6; i++) data[i] = Wire.read();
            uint32_t hum_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
            uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
            humidity = (hum_raw / (float)0x100000) * 100.0;
            temperature = (temp_raw / (float)0x100000) * 200.0 - 50.0;
            return true;
        }
    }
    return false;
}

// ====================== 按键逻辑 ======================
void toggleLED() { ledState = !ledState; digitalWrite(LED_PIN, ledState); playFeedbackSound(); drawUI(); uploadToOneNET(); }
void toggleFan() { fanState = !fanState; digitalWrite(FAN_PIN, fanState); playFeedbackSound(); drawUI(); uploadToOneNET(); }
void toggleHum() { humState = !humState; digitalWrite(HUM_PIN, humState); playFeedbackSound(); drawUI(); uploadToOneNET(); }
void toggleCurtain() { curtainOpen = !curtainOpen; curtainServo.write(curtainOpen ? 90 : 0); playFeedbackSound(); drawUI(); uploadToOneNET(); }

void handleMusicToggle() {
    if (!musicAvailable) {
        Serial.println("音频资源不可用，忽略音乐切换");
        return;
    }

    if (!isPlayingMusic) {
        Serial.println("长按/云端：开启音乐");
        isPlayingMusic = true;
        xTaskCreatePinnedToCore(playMusicTask, "MusicTask", 10240, NULL, 1, &musicTaskHandle, 0);
    } else {
        Serial.println("长按/云端：关闭音乐");
        isPlayingMusic = false;   // 通知任务退出

        // 等待音乐任务自行结束（最多 3 秒）
        if (musicTaskHandle != NULL) {
            uint32_t startWait = millis();
            while (musicTaskHandle != NULL && (millis() - startWait) < 3000) {
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            // 超时强制清理（理论上不会发生）
            if (musicTaskHandle != NULL) {
                vTaskDelete(musicTaskHandle);
                musicTaskHandle = NULL;
                // 确保 I2S 停止
                i2s_zero_dma_buffer(I2S_NUM_0);
                i2s_stop(I2S_NUM_0);
            }
        }
    }
    drawUI();
}

// ====================== Setup ======================
void setup() {
    Serial.begin(115200);
    mqttClient.setBufferSize(1024); 

    pinMode(LED_PIN, OUTPUT); pinMode(FAN_PIN, OUTPUT); pinMode(HUM_PIN, OUTPUT);
    curtainServo.attach(SERVO_PIN, 500, 2500); curtainServo.write(0);
    
    Wire.begin(DHT20_SDA, DHT20_SCL);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.init(240, 240); tft.setRotation(2); tft.fillScreen(TFT_BLACK);

    // ---- 初始化 SPIFFS 并分配音频缓冲区 ----
    if (SPIFFS.begin(true)) {
        Serial.println("SPIFFS 挂载成功");
        // 列出文件，确认 test.wav 存在
        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        while (file) {
            Serial.printf("SPIFFS 文件: %s (%u 字节)\n", file.name(), file.size());
            file = root.openNextFile();
        }
        
        wavBuffer = (int16_t*)ps_malloc(BUFFER_SAMPLES * sizeof(int16_t));
        if (!wavBuffer) {
            wavBuffer = (int16_t*)malloc(BUFFER_SAMPLES * sizeof(int16_t));
        }
        if (wavBuffer) {
            Serial.printf("音频缓冲区分配成功: %u 字节\n", BUFFER_SAMPLES * sizeof(int16_t));
            musicAvailable = true;
        } else {
            Serial.println("音频缓冲区分配失败！无法播放音乐");
        }
    } else {
        Serial.println("SPIFFS 挂载失败，音乐功能不可用");
    }

    // ---- I2S 配置（使用 I2S_NUM_0 更稳定） ----
    i2s_config_t spk_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t spk_pins = {
        .bck_io_num = SPK_BCLK,
        .ws_io_num = SPK_LRC,
        .data_out_num = SPK_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_0, &spk_cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &spk_pins);
    i2s_stop(I2S_NUM_0);

    // 按键绑定
    btnLED.attachClick(toggleLED);
    btnLED.attachLongPressStart(toggleCurtain);
    btnFan.attachClick(toggleFan);
    btnHum.attachClick(toggleHum);
    btnHum.attachLongPressStart(handleMusicToggle);  // IO11 长按切换音乐

    WiFi.begin(ssid, password);
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    drawUI();
}

void loop() {
    btnLED.tick(); btnFan.tick(); btnHum.tick();
    
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 5000) {
        lastUpdate = millis();
        wifiConnected = (WiFi.status() == WL_CONNECTED);
        if (wifiConnected) {
            if (!mqttClient.connected()) {
                if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
                    mqttClient.subscribe(MQTT_SUB_TOPIC);
                    Serial.println("MQTT 已连接并订阅主题");
                }
            }
            timeClient.update();
            currentTimeStr = timeClient.getFormattedTime().substring(0, 5);
        }
        readDHT20();
        drawUI();
        updateTimeUI();
        uploadToOneNET(); 
    }
    mqttClient.loop();
}
