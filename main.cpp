#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <RTClib.h>
#include <Adafruit_ADS1X15.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <SD.h>

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

const char* WIFI_SSID = "1johnny";
const char* WIFI_PASSWORD = "buyservice";

const char* SERVER_URL = "http://10.24.21.100:5000/api/esp/data";
const char* API_KEY = "school-lab-esp32-key-12345";

const unsigned long SEND_INTERVAL_MS = 10000;
unsigned long lastSendMs = 0;

const int SD_CS_PIN = 5;
const char* SD_LOG_FILE = "/SD Card Log.csv";
bool sdReady = false;

const unsigned long SD_LOG_INTERVAL_MS = 600000;
unsigned long lastSdLogMs = 0;

const unsigned long ESP_NOW_SEND_INTERVAL_MS = 500;
unsigned long lastEspNowSendMs = 0;

uint8_t receiverMacAddress[] = {0x00, 0x70, 0x07, 0x2E, 0x94, 0xFC};
uint8_t wifiChannel = 1;

LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_ADS1115 ads;
RTC_DS3231 rtc;

const int CH_I1 = 0;
const int CH_I2 = 1;
const int CH_V1 = 2;
const int CH_V2 = 3;

const int BUTTON_PIN = 4;

enum DisplayMode {
  SCREEN_LINES = 0,
  SCREEN_POWER_ENERGY = 1
};

DisplayMode currentScreen = SCREEN_LINES;

bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 60;

const int SAMPLE_COUNT = 800;
const float ADS_LSB_VOLTS = 0.000125f;

float VOLT_CAL_L1 = 362.05f;
float VOLT_CAL_L2 = 365.23f;

float CURRENT_CAL_L1 = 122.63f;
float CURRENT_CAL_L2 = 122.63f;

float V1_SIGN = 1.0f;
float V2_SIGN = 1.0f;
float I1_SIGN = 1.0f;
float I2_SIGN = 1.0f;

const float VOLTAGE_NOISE_FLOOR = 5.0f;
const float ZERO_CURRENT_THRESHOLD = 0.75f;

float displayL1V = 0.0f;
float displayL1I = 0.0f;
float displayL2V = 0.0f;
float displayL2I = 0.0f;

const float ALPHA_V = 0.12f;
const float ALPHA_I = 0.20f;

float energyWh = 0.0f;
uint32_t lastEnergyUnix = 0;

struct Metrics {
  float line1Voltage;
  float line1Current;
  float line1RealPower;
  float line1ApparentPower;
  float line1PF;

  float line2Voltage;
  float line2Current;
  float line2RealPower;
  float line2ApparentPower;
  float line2PF;

  float totalVoltage;
  float displayCurrent;
  float totalRealPower;
  float totalApparentPower;
  float powerFactor;

  bool hasLine1Voltage;
  bool hasLine1Current;
  bool hasLine2Voltage;
  bool hasLine2Current;
  bool hasAnyVoltage;
  bool hasAnyCurrent;
};

typedef struct EnergyData {
  float voltage;
  float current;
  float power;
  float energy;
  float pf;

  float phase1Voltage;
  float phase1Current;
  float phase2Voltage;
  float phase2Current;

  char rtcDate[12];
  char rtcTime[12];

  bool systemOn;
} EnergyData;

EnergyData espNowData;

String fit16(const String &s) {
  if (s.length() >= 16) return s.substring(0, 16);

  String out = s;
  while (out.length() < 16) out += ' ';
  return out;
}

float countsToVolts(float counts) {
  return counts * ADS_LSB_VOLTS;
}

float smoothValue(float oldValue, float newValue, float alpha) {
  if (newValue == 0.0f) return 0.0f;
  if (oldValue == 0.0f) return newValue;
  return ((1.0f - alpha) * oldValue) + (alpha * newValue);
}

String twoDigits(int value) {
  if (value < 10) return "0" + String(value);
  return String(value);
}

String getRtcDate() {
  DateTime now = rtc.now();
  return String(now.year()) + "-" + twoDigits(now.month()) + "-" + twoDigits(now.day());
}

String getRtcTime() {
  DateTime now = rtc.now();
  return twoDigits(now.hour()) + ":" + twoDigits(now.minute()) + ":" + twoDigits(now.second());
}

void setupButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lastButtonReading = digitalRead(BUTTON_PIN);
  stableButtonState = lastButtonReading;

  Serial.println("Button ready on GPIO4.");
}

void handleDisplayButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
    lastButtonReading = reading;
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != stableButtonState) {
      stableButtonState = reading;

      if (stableButtonState == LOW) {
        if (currentScreen == SCREEN_LINES) {
          currentScreen = SCREEN_POWER_ENERGY;
        } else {
          currentScreen = SCREEN_LINES;
        }

        lcd.clear();

        Serial.print("Button pressed. Screen changed to: ");
        Serial.println(currentScreen == SCREEN_LINES ? "LINES" : "POWER/ENERGY");
      }
    }
  }
}

void setupRTC() {
  if (!rtc.begin()) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("RTC ERROR");
    lcd.setCursor(0, 1);
    lcd.print("Check wiring");

    Serial.println("ERROR: Could not find DS3231 RTC.");
    Serial.println("SDA -> GPIO21");
    Serial.println("SCL -> GPIO22");

    while (1) {
      delay(1000);
    }
  }

  Serial.println("RTC found.");

  if (rtc.lostPower()) {
    Serial.println("RTC lost power. Setting RTC to compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  DateTime now = rtc.now();

  Serial.print("RTC Time: ");
  Serial.print(now.year());
  Serial.print("-");
  Serial.print(twoDigits(now.month()));
  Serial.print("-");
  Serial.print(twoDigits(now.day()));
  Serial.print(" ");
  Serial.print(twoDigits(now.hour()));
  Serial.print(":");
  Serial.print(twoDigits(now.minute()));
  Serial.print(":");
  Serial.println(twoDigits(now.second()));
}

void showRTCTimeOnLCD() {
  DateTime now = rtc.now();

  String dateLine = String(now.year()) + "/" + twoDigits(now.month()) + "/" + twoDigits(now.day());
  String timeLine = twoDigits(now.hour()) + ":" + twoDigits(now.minute()) + ":" + twoDigits(now.second());

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(fit16(dateLine));
  lcd.setCursor(0, 1);
  lcd.print(fit16(timeLine));
  delay(2000);
  lcd.clear();
}

String formatStorage(uint64_t bytes) {
  float mb = bytes / (1024.0 * 1024.0);

  if (mb >= 1024.0) {
    return String(mb / 1024.0, 2) + "GB";
  }

  return String(mb, 1) + "MB";
}

String getSdFreeSpace() {
  if (!sdReady) {
    return "SD Free: N/A";
  }

  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  uint64_t freeBytes = totalBytes - usedBytes;

  return "SD Free:" + formatStorage(freeBytes);
}

void showSdCapacity() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(fit16("SD Remaining"));
  lcd.setCursor(0, 1);
  lcd.print(fit16(getSdFreeSpace()));
  delay(2000);
  lcd.clear();
}

void setupSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    sdReady = false;
    Serial.println("SD card not ready. SD logging disabled.");
    return;
  }

  sdReady = true;

  if (!SD.exists(SD_LOG_FILE)) {
    File file = SD.open(SD_LOG_FILE, FILE_WRITE);

    if (file) {
      file.println("Date,Time,Voltage_V,Current_A,PowerFactor,Energy_kWh,Power_W,Line1Voltage_V,Line1Current_A,Line2Voltage_V,Line2Current_A");
      file.close();
    } else {
      Serial.println("Could not create SD log file.");
    }
  }

  showSdCapacity();
  lastSdLogMs = millis();
}

void saveToSD(const Metrics &m) {
  if (!sdReady) {
    Serial.println("SD not ready. Skipping SD log.");
    return;
  }

  File file = SD.open(SD_LOG_FILE, FILE_APPEND);

  if (!file) {
    Serial.println("Could not open SD log file.");
    return;
  }

  float energyKWh = energyWh / 1000.0f;

  String row = "";
  row += getRtcDate();
  row += ",";
  row += getRtcTime();
  row += ",";
  row += String(m.totalVoltage, 2);
  row += ",";
  row += String(m.displayCurrent, 3);
  row += ",";
  row += String(m.powerFactor, 2);
  row += ",";
  row += String(energyKWh, 4);
  row += ",";
  row += String(m.totalRealPower, 2);
  row += ",";
  row += String(m.line1Voltage, 2);
  row += ",";
  row += String(m.line1Current, 3);
  row += ",";
  row += String(m.line2Voltage, 2);
  row += ",";
  row += String(m.line2Current, 3);

  file.println(row);
  file.close();

  Serial.println("Saved to SD:");
  Serial.println(row);
}

void connectWiFiNonBlocking() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("Connecting to WiFi...");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    handleDisplayButton();
    delay(500);
    Serial.println("Still connecting...");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.print("ESP IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed");
    Serial.print("WiFi status code: ");
    Serial.println(WiFi.status());
  }
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi disconnected. Reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 3000) {
    handleDisplayButton();
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi reconnected");
    Serial.print("ESP IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi reconnect failed");
    Serial.print("WiFi status code: ");
    Serial.println(WiFi.status());
  }
}

void getAndPrintWiFiChannel() {
  uint8_t primaryChannel;
  wifi_second_chan_t secondChannel;

  esp_wifi_get_channel(&primaryChannel, &secondChannel);

  wifiChannel = primaryChannel;

  Serial.print("Main ESP WiFi Channel: ");
  Serial.println(wifiChannel);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  Serial.print("ESP-NOW send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed");
}

void setupEspNow() {
  getAndPrintWiFiChannel();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMacAddress, 6);

  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add ESP-NOW peer");
    return;
  }

  Serial.println("ESP-NOW ready.");
  Serial.println("ESP-NOW peer channel: AUTO");
}

void sendDataToOtherEsp(const Metrics &m) {
  DateTime nowRtc = rtc.now();

  String rtcDate = String(nowRtc.year()) + "-" + twoDigits(nowRtc.month()) + "-" + twoDigits(nowRtc.day());
  String rtcTime = twoDigits(nowRtc.hour()) + ":" + twoDigits(nowRtc.minute()) + ":" + twoDigits(nowRtc.second());

  espNowData.voltage = m.totalVoltage;
  espNowData.current = m.displayCurrent;
  espNowData.power = m.totalRealPower;
  espNowData.energy = energyWh / 1000.0f;
  espNowData.pf = m.powerFactor;

  espNowData.phase1Voltage = m.line1Voltage;
  espNowData.phase1Current = m.line1Current;
  espNowData.phase2Voltage = m.line2Voltage;
  espNowData.phase2Current = m.line2Current;

  rtcDate.toCharArray(espNowData.rtcDate, sizeof(espNowData.rtcDate));
  rtcTime.toCharArray(espNowData.rtcTime, sizeof(espNowData.rtcTime));

  espNowData.systemOn = m.hasAnyVoltage || m.hasAnyCurrent;

  esp_err_t result = esp_now_send(receiverMacAddress, (uint8_t *)&espNowData, sizeof(espNowData));

  if (result == ESP_OK) {
    Serial.println("Sent data to second ESP");
  } else {
    Serial.println("Error sending ESP-NOW data");
  }
}

void sendDataToServer(const Metrics &m) {
  ensureWiFiConnected();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping send: WiFi not connected");
    return;
  }

  DateTime nowRtc = rtc.now();

  String rtcDate = String(nowRtc.year()) + "-" + twoDigits(nowRtc.month()) + "-" + twoDigits(nowRtc.day());
  String rtcTime = twoDigits(nowRtc.hour()) + ":" + twoDigits(nowRtc.minute()) + ":" + twoDigits(nowRtc.second());

  HTTPClient http;

  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", API_KEY);

  float energyKWh = energyWh / 1000.0f;

  String jsonData = "{";
  jsonData += "\"device_id\":\"lab_meter_1\",";
  jsonData += "\"rtcDate\":\"" + rtcDate + "\",";
  jsonData += "\"rtcTime\":\"" + rtcTime + "\",";
  jsonData += "\"voltage\":" + String(m.totalVoltage, 2) + ",";
  jsonData += "\"current\":" + String(m.displayCurrent, 3) + ",";
  jsonData += "\"power\":" + String(m.totalRealPower, 2) + ",";
  jsonData += "\"energy\":" + String(energyKWh, 4) + ",";
  jsonData += "\"pf\":" + String(m.powerFactor, 2) + ",";

  jsonData += "\"phaseA\":{";
  jsonData += "\"voltage\":" + String(m.line1Voltage, 2) + ",";
  jsonData += "\"current\":" + String(m.line1Current, 3);
  jsonData += "},";

  jsonData += "\"phaseB\":{";
  jsonData += "\"voltage\":" + String(m.line2Voltage, 2) + ",";
  jsonData += "\"current\":" + String(m.line2Current, 3);
  jsonData += "},";

  jsonData += "\"displayMode\":" + String((int)currentScreen);
  jsonData += "}";

  int responseCode = http.POST(jsonData);

  Serial.println("----- Sending to Server -----");
  Serial.println(jsonData);
  Serial.print("HTTP Response code: ");
  Serial.println(responseCode);

  if (responseCode > 0) {
    String response = http.getString();
    Serial.println(response);
  } else {
    Serial.print("Failed to send data: ");
    Serial.println(http.errorToString(responseCode));
  }

  http.end();
}

void showStartupMessage() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Building");
  lcd.setCursor(0, 1);
  lcd.print("Monitoring Start");
  delay(2000);
  lcd.clear();
}

Metrics sampleMetrics() {
  Metrics m{};

  double sumI1 = 0.0;
  double sumI2 = 0.0;
  double sumV1 = 0.0;
  double sumV2 = 0.0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    if (i % 20 == 0) handleDisplayButton();

    int16_t rawI1 = ads.readADC_SingleEnded(CH_I1);
    int16_t rawV1 = ads.readADC_SingleEnded(CH_V1);
    int16_t rawI2 = ads.readADC_SingleEnded(CH_I2);
    int16_t rawV2 = ads.readADC_SingleEnded(CH_V2);

    sumI1 += rawI1;
    sumV1 += rawV1;
    sumI2 += rawI2;
    sumV2 += rawV2;

    delayMicroseconds(300);
  }

  float offsetI1 = sumI1 / SAMPLE_COUNT;
  float offsetI2 = sumI2 / SAMPLE_COUNT;
  float offsetV1 = sumV1 / SAMPLE_COUNT;
  float offsetV2 = sumV2 / SAMPLE_COUNT;

  double sumSqI1 = 0.0;
  double sumSqI2 = 0.0;
  double sumSqV1 = 0.0;
  double sumSqV2 = 0.0;
  double sumP1 = 0.0;
  double sumP2 = 0.0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    if (i % 20 == 0) handleDisplayButton();

    int16_t rawI1 = ads.readADC_SingleEnded(CH_I1);
    int16_t rawV1 = ads.readADC_SingleEnded(CH_V1);
    int16_t rawI2 = ads.readADC_SingleEnded(CH_I2);
    int16_t rawV2 = ads.readADC_SingleEnded(CH_V2);

    float ci1Counts = ((float)rawI1 - offsetI1) * I1_SIGN;
    float cv1Counts = ((float)rawV1 - offsetV1) * V1_SIGN;
    float ci2Counts = ((float)rawI2 - offsetI2) * I2_SIGN;
    float cv2Counts = ((float)rawV2 - offsetV2) * V2_SIGN;

    float i1Actual = countsToVolts(ci1Counts) * CURRENT_CAL_L1;
    float v1Actual = countsToVolts(cv1Counts) * VOLT_CAL_L1;
    float i2Actual = countsToVolts(ci2Counts) * CURRENT_CAL_L2;
    float v2Actual = countsToVolts(cv2Counts) * VOLT_CAL_L2;

    sumSqI1 += i1Actual * i1Actual;
    sumSqV1 += v1Actual * v1Actual;
    sumSqI2 += i2Actual * i2Actual;
    sumSqV2 += v2Actual * v2Actual;

    sumP1 += v1Actual * i1Actual;
    sumP2 += v2Actual * i2Actual;

    delayMicroseconds(300);
  }

  m.line1Voltage = sqrt(sumSqV1 / SAMPLE_COUNT);
  m.line1Current = sqrt(sumSqI1 / SAMPLE_COUNT);
  m.line2Voltage = sqrt(sumSqV2 / SAMPLE_COUNT);
  m.line2Current = sqrt(sumSqI2 / SAMPLE_COUNT);

  if (m.line1Voltage < VOLTAGE_NOISE_FLOOR) {
    m.line1Voltage = 0.0f;
    m.hasLine1Voltage = false;
  } else {
    m.hasLine1Voltage = true;
  }

  if (m.line2Voltage < VOLTAGE_NOISE_FLOOR) {
    m.line2Voltage = 0.0f;
    m.hasLine2Voltage = false;
  } else {
    m.hasLine2Voltage = true;
  }

  if (m.line1Current < ZERO_CURRENT_THRESHOLD) {
    m.line1Current = 0.0f;
    m.hasLine1Current = false;
  } else {
    m.hasLine1Current = true;
  }

  if (m.line2Current < ZERO_CURRENT_THRESHOLD) {
    m.line2Current = 0.0f;
    m.hasLine2Current = false;
  } else {
    m.hasLine2Current = true;
  }

  m.line1RealPower = fabs(sumP1 / SAMPLE_COUNT);
  m.line2RealPower = fabs(sumP2 / SAMPLE_COUNT);

  if (!m.hasLine1Voltage || !m.hasLine1Current) m.line1RealPower = 0.0f;
  if (!m.hasLine2Voltage || !m.hasLine2Current) m.line2RealPower = 0.0f;

  m.line1ApparentPower = m.line1Voltage * m.line1Current;
  m.line2ApparentPower = m.line2Voltage * m.line2Current;

  if (m.line1ApparentPower > 0.001f) {
    m.line1PF = m.line1RealPower / m.line1ApparentPower;
    if (m.line1PF > 1.0f) m.line1PF = 1.0f;
    if (m.line1PF < 0.0f) m.line1PF = 0.0f;
  } else {
    m.line1PF = 0.0f;
  }

  if (m.line2ApparentPower > 0.001f) {
    m.line2PF = m.line2RealPower / m.line2ApparentPower;
    if (m.line2PF > 1.0f) m.line2PF = 1.0f;
    if (m.line2PF < 0.0f) m.line2PF = 0.0f;
  } else {
    m.line2PF = 0.0f;
  }

  m.totalVoltage = m.line1Voltage + m.line2Voltage;
  m.displayCurrent = m.line1Current;
  m.totalRealPower = m.line1RealPower + m.line2RealPower;
  m.totalApparentPower = m.line1ApparentPower + m.line2ApparentPower;

  if (m.totalApparentPower > 0.001f) {
    m.powerFactor = m.totalRealPower / m.totalApparentPower;
    if (m.powerFactor > 1.0f) m.powerFactor = 1.0f;
    if (m.powerFactor < 0.0f) m.powerFactor = 0.0f;
  } else {
    m.powerFactor = 0.0f;
  }

  m.hasAnyVoltage = m.hasLine1Voltage || m.hasLine2Voltage;
  m.hasAnyCurrent = m.hasLine1Current || m.hasLine2Current;

  return m;
}

void updateEnergyUsingRTC(const Metrics &m) {
  DateTime energyNowRtc = rtc.now();
  uint32_t nowUnix = energyNowRtc.unixtime();

  if (lastEnergyUnix == 0) {
    lastEnergyUnix = nowUnix;
    return;
  }

  if (nowUnix > lastEnergyUnix) {
    float dtHours = (nowUnix - lastEnergyUnix) / 3600.0f;
    lastEnergyUnix = nowUnix;

    if (m.totalRealPower > 0.0f) {
      energyWh += m.totalRealPower * dtHours;
    }

    Serial.print("RTC energy updated. Energy Wh: ");
    Serial.println(energyWh, 4);
  } else if (nowUnix < lastEnergyUnix) {
    Serial.println("RTC time moved backward. Energy time reset.");
    lastEnergyUnix = nowUnix;
  }
}

void displayLineReadings(const Metrics &m) {
  String line1 = "L1:" + String(m.line1Voltage, 0) + "V " + String(m.line1Current, 2) + "A";
  String line2 = "L2:" + String(m.line2Voltage, 0) + "V " + String(m.line2Current, 2) + "A";

  lcd.setCursor(0, 0);
  lcd.print(fit16(line1));

  lcd.setCursor(0, 1);
  lcd.print(fit16(line2));
}

void displayPowerEnergy(const Metrics &m) {
  String line1 = "PF:" + String(m.powerFactor, 2) + " P:" + String(m.totalRealPower, 0) + "W";
  String line2 = "E:" + String(energyWh / 1000.0f, 4) + "kWh";

  lcd.setCursor(0, 0);
  lcd.print(fit16(line1));

  lcd.setCursor(0, 1);
  lcd.print(fit16(line2));
}

void updateLCD(const Metrics &m) {
  if (currentScreen == SCREEN_LINES) {
    displayLineReadings(m);
  } else {
    displayPowerEnergy(m);
  }
}

void setup() {
  Serial.begin(115200);

  setupButton();

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  setupRTC();

  if (!ads.begin(0x48)) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ADS1115 ERROR");
    lcd.setCursor(0, 1);
    lcd.print("Check wiring");

    Serial.println("Failed to initialize ADS1115.");
    Serial.println("Check SDA/SCL wiring and ADS1115 address.");

    while (1) {
      delay(100);
    }
  }

  ads.setGain(GAIN_ONE);
  ads.setDataRate(RATE_ADS1115_860SPS);

  showStartupMessage();
  showRTCTimeOnLCD();

  setupSDCard();

  connectWiFiNonBlocking();

  setupEspNow();

  lastEnergyUnix = rtc.now().unixtime();
  lastSendMs = millis();
  lastEspNowSendMs = millis();
  lastSdLogMs = millis();

  Serial.print("Energy RTC start Unix: ");
  Serial.println(lastEnergyUnix);
}

void loop() {
  handleDisplayButton();

  Metrics m = sampleMetrics();

  displayL1V = smoothValue(displayL1V, m.line1Voltage, ALPHA_V);
  displayL1I = smoothValue(displayL1I, m.line1Current, ALPHA_I);

  displayL2V = smoothValue(displayL2V, m.line2Voltage, ALPHA_V);
  displayL2I = smoothValue(displayL2I, m.line2Current, ALPHA_I);

  unsigned long now = millis();

  updateEnergyUsingRTC(m);

  handleDisplayButton();
  updateLCD(m);

  if (now - lastEspNowSendMs >= ESP_NOW_SEND_INTERVAL_MS) {
    lastEspNowSendMs = now;
    sendDataToOtherEsp(m);
  }

  if (now - lastSdLogMs >= SD_LOG_INTERVAL_MS) {
    lastSdLogMs = now;
    saveToSD(m);
    showSdCapacity();
  }

  if (WiFi.status() == WL_CONNECTED && now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendDataToServer(m);
  }

  delay(150);
}