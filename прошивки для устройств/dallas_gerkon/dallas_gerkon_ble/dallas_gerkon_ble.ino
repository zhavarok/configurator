#include <WiFi.h>
#include <queue>
#include <HTTPClient.h>
#include <time.h>
#include <EEPROM.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// Пины
#define REED_SWITCH_PIN 27
#define LED_PIN 2

// EEPROM
#define EEPROM_SIZE 128
#define EEPROM_DEVICE_ID 0
#define EEPROM_CLIENT_ID 4
#define EEPROM_SSID 16
#define EEPROM_PASSWORD 32
#define EEPROM_BLE_MAC 48

// Параметры по умолчанию
int device_id = 4;
char client_id[12] = "2";
char ssid[16] = "TVCom";
char password[16] = "taGENeTa";
char ble_mac_address[18] = "";

// Время
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

// BLE
BLEScan* pBLEScan;
float currentTemperature = 0.0;
unsigned long lastTemperatureUpdate = 0;
#define TEMPERATURE_TIMEOUT 60000
#define BLE_SCAN_INTERVAL 5000
#define BLE_SCAN_DURATION 2000

// Сервер
#define SERVER_URL "http://80.80.101.123:503/data"
#define SAMPLE_INTERVAL 15000
#define RESEND_INTERVAL 5000
#define LOG_INTERVAL 30000
#define WIFI_RECONNECT_INTERVAL 20000
#define HTTP_TIMEOUT 10000
#define MAX_RETRIES 3
#define MAX_BUFFER_SIZE 500
#define MIN_RSSI -120

struct CardData {
  char deviceType[16];
  int deviceId;
  char clientId[12];
  float temperature;
  int status;
  int reed_switch;
  char timestamp[20];
};

std::queue<CardData> sendBuffer;

unsigned long lastSampleTime = 0;
unsigned long lastSendAttempt = 0;
unsigned long lastLogTime = 0;
unsigned long lastWifiLogTime = 0;
unsigned long lastBLEScanTime = 0;
bool wifiConnected = false;
bool isScanning = false;

// Watchdog для BLE
#define BLE_SCAN_TIMEOUT 10000

// === BLE CALLBACKS ===
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (strlen(ble_mac_address) == 0) return;
    if (strcasecmp(advertisedDevice.getAddress().toString().c_str(), ble_mac_address) != 0) return;

    Serial.printf("Найден BLE датчик: %s\n", advertisedDevice.toString().c_str());

    if (!advertisedDevice.haveManufacturerData()) {
      Serial.println("Нет manufacturer data");
      return;
    }

    std::string mfd = advertisedDevice.getManufacturerData();
    const uint8_t* data = (const uint8_t*)mfd.data();
    size_t len = mfd.length();
    if (len < 6) {
      Serial.printf("Мало данных: %d байт\n", len);
      return;
    }

    int16_t voltage_raw = (data[3] << 8) | data[2];
    int16_t temp_raw = (data[5] << 8) | data[4];
    float voltage = voltage_raw / 1000.0;
    float temperature = temp_raw / 10.0;

    Serial.printf("Voltage: %.3fV, Temp: %.1f°C\n", voltage, temperature);

    currentTemperature = temperature;
    lastTemperatureUpdate = millis();
  }
};

// === ФУНКЦИИ ===
void wifiTask(void* parameter);
void ensureTimeSynced();
void getCurrentTime(char* buffer);
void addDataToBuffer(int deviceId, const char* clientId, float temperature, int reed_switch, int status);
bool sendDataToServerWiFi();
void sendBufferData();
void loadConfigFromEEPROM();
void saveConfigToEEPROM();
void setupBLE();
void startBLEScan();

void setup() {
  Serial.begin(9600);
  Serial.println("Temperature Sensor (BLE + Reed Switch) Test");
  pinMode(LED_PIN, OUTPUT);
  pinMode(REED_SWITCH_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);
  EEPROM.begin(EEPROM_SIZE);
  loadConfigFromEEPROM();
  Serial.printf("Загружено из EEPROM: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s; BLE_MAC=%s\n",
                device_id, client_id, ssid, password, ble_mac_address);
  setupBLE();
  Serial.println("Запуск Wi-Fi задачи...");
  xTaskCreatePinnedToCore(wifiTask, "WiFiTask", 10000, NULL, 1, NULL, 0);
  ensureTimeSynced();
}

void setupBLE() {
  BLEDevice::init("ESP32_Temp_Receiver");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

// УПРОЩЕННОЕ И НАДЕЖНОЕ СКАНИРОВАНИЕ BLE
void startBLEScan() {
  if (isScanning || strlen(ble_mac_address) == 0) return;

  unsigned long currentMillis = millis();
  
  // Защита от зависания BLE
  if (isScanning && (currentMillis - lastBLEScanTime > BLE_SCAN_TIMEOUT)) {
    Serial.println("BLE ЗАВИС! Принудительный сброс.");
    pBLEScan->stop();
    pBLEScan->clearResults();
    isScanning = false;
    delay(100);
  }

  if (!isScanning && (currentMillis - lastBLEScanTime >= BLE_SCAN_INTERVAL)) {
    isScanning = true;
    Serial.println("Запуск BLE сканирования...");
    
    // Синхронное сканирование - более надежное
    BLEScanResults results = pBLEScan->start(BLE_SCAN_DURATION / 1000, false);
    Serial.printf("BLE сканирование завершено. Найдено устройств: %d\n", results.getCount());
    pBLEScan->clearResults();
    
    lastBLEScanTime = millis();
    isScanning = false;
  }
}

void wifiTask(void* parameter) {
  while (true) {
    unsigned long currentMillis = millis();
    if (wifiConnected && WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      digitalWrite(LED_PIN, LOW);
      Serial.println("WiFi соединение потеряно! Сбрасываем статус...");
    }
    if (!wifiConnected) {
      if (currentMillis - lastWifiLogTime >= WIFI_RECONNECT_INTERVAL) {
        Serial.println("Попытка подключения к WiFi...");
        lastWifiLogTime = currentMillis;
      }
      WiFi.begin(ssid, password);
      int attempt = 0;
      while (WiFi.status() != WL_CONNECTED && attempt < 20) {
        delay(500);
        attempt++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\nWi-Fi подключен! IP: " + WiFi.localIP().toString());
        Serial.println("Уровень сигнала WiFi: " + String(WiFi.RSSI()) + " dBm");
        digitalWrite(LED_PIN, HIGH);
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        ensureTimeSynced();
      }
    }
    delay(500);
  }
}

void ensureTimeSynced() {
  struct tm timeinfo;
  if (wifiConnected) {
    Serial.println("Синхронизация времени через Wi-Fi...");
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 3) {
      Serial.println("Время не синхронизировано. Повторная попытка...");
      digitalWrite(LED_PIN, LOW);
      delay(250);
      digitalWrite(LED_PIN, HIGH);
      delay(250);
      attempts++;
    }
    if (getLocalTime(&timeinfo)) {
      Serial.println("Время синхронизировано успешно!");
      char timeBuffer[20];
      getCurrentTime(timeBuffer);
      Serial.printf("Текущее время: %s\n", timeBuffer);
    } else {
      Serial.println("Ошибка синхронизации времени! Перезагрузка...");
      delay(2000);
      ESP.restart();
    }
  }
}

void getCurrentTime(char* buffer) {
  if (wifiConnected) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      Serial.println("Не удалось получить время через Wi-Fi!");
      strcpy(buffer, "0000-00-00 00:00:00");
      return;
    }
    strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    strcpy(buffer, "0000-00-00 00:00:00");
  }
}

void addDataToBuffer(int deviceId, const char* clientId, float temperature, int reed_switch, int status) {
  char currentTime[20];
  getCurrentTime(currentTime);
  CardData newData;
  strcpy(newData.deviceType, "freezer");
  newData.deviceId = deviceId;
  strncpy(newData.clientId, clientId, sizeof(newData.clientId) - 1);
  newData.clientId[sizeof(newData.clientId) - 1] = '\0';
  newData.temperature = temperature;
  newData.status = status;
  newData.reed_switch = reed_switch;
  strcpy(newData.timestamp, currentTime);
 
  Serial.printf("Добавлено в буфер: Device ID: %d - Client ID: %s - Temperature: %.2f°C - Reed Switch: %d - Status: %d - Timestamp: %s - Buffer Size: %d/%d\n",
                newData.deviceId, newData.clientId, newData.temperature, newData.reed_switch, newData.status, newData.timestamp, sendBuffer.size() + 1, MAX_BUFFER_SIZE);
 
  if (sendBuffer.size() >= MAX_BUFFER_SIZE) {
    Serial.println("Буфер переполнен, удаляем старую запись");
    sendBuffer.pop();
  }
  sendBuffer.push(newData);
}

bool sendDataToServerWiFi() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastLogTime >= LOG_INTERVAL) {
      Serial.println("WiFi не подключен!");
      lastLogTime = currentMillis;
    }
    return false;
  }
  if (sendBuffer.empty()) return false;
  int rssi = WiFi.RSSI();
  if (rssi < MIN_RSSI) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastLogTime >= LOG_INTERVAL) {
      Serial.printf("Слабый сигнал WiFi (%d dBm), ждем улучшения...\n", rssi);
      lastLogTime = currentMillis;
    }
    return false;
  }
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT);
  http.setReuse(true);
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  CardData data = sendBuffer.front();
  char jsonPayload[256];
  snprintf(jsonPayload, sizeof(jsonPayload),
           "{\"client_id\": \"%s\", \"data\": [{\"device_id\": %d, \"temperature\": %.2f, \"reed_switch\": %d, \"status\": %d, \"timestamp\": \"%s\"}]}",
           data.clientId, data.deviceId, data.temperature, data.reed_switch, data.status, data.timestamp);
  Serial.printf("Отправка JSON: %s\n", jsonPayload);
 
  int retryCount = 0;
  int httpResponseCode = -1;
  while (retryCount < MAX_RETRIES && httpResponseCode <= 0) {
    httpResponseCode = http.POST(jsonPayload);
    retryCount++;
    if (httpResponseCode <= 0) {
      unsigned long currentMillis = millis();
      if (currentMillis - lastLogTime >= LOG_INTERVAL) {
        Serial.printf("Ошибка отправки, код: %d, попытка %d/%d\n", httpResponseCode, retryCount, MAX_RETRIES);
        lastLogTime = currentMillis;
      }
      if (retryCount < MAX_RETRIES) delay(1000);
    }
  }
  if (httpResponseCode > 0) {
    Serial.printf("Данные отправлены, код: %d\n", httpResponseCode);
    if (httpResponseCode == 200) sendBuffer.pop();
    http.end();
    return httpResponseCode == 200;
  } else {
    unsigned long currentMillis = millis();
    if (currentMillis - lastLogTime >= LOG_INTERVAL) {
      Serial.printf("Ошибка отправки после %d попыток, код: %d\n", MAX_RETRIES, httpResponseCode);
      lastLogTime = currentMillis;
    }
    http.end();
    return false;
  }
}

void sendBufferData() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastSendAttempt >= RESEND_INTERVAL && !sendBuffer.empty()) {
    if (wifiConnected && WiFi.status() == WL_CONNECTED) {
      if (currentMillis - lastLogTime >= LOG_INTERVAL) {
        Serial.printf("Уровень сигнала WiFi: %d dBm\n", WiFi.RSSI());
        lastLogTime = currentMillis;
      }
      while (!sendBuffer.empty()) {
        bool success = sendDataToServerWiFi();
        if (!success) break;
        delay(100);
      }
    }
    lastSendAttempt = currentMillis;
  }
}

void loadConfigFromEEPROM() {
  device_id = EEPROM.readInt(EEPROM_DEVICE_ID);
  int client_id_len = EEPROM.read(EEPROM_CLIENT_ID);
  if (client_id_len > 0 && client_id_len < 12) {
    for (int i = 0; i < client_id_len; i++) client_id[i] = EEPROM.read(EEPROM_CLIENT_ID + 1 + i);
    client_id[client_id_len] = '\0';
  }
  for (int i = 0; i < 15; i++) ssid[i] = EEPROM.read(EEPROM_SSID + i);
  ssid[15] = '\0';
  for (int i = 0; i < 15; i++) password[i] = EEPROM.read(EEPROM_PASSWORD + i);
  password[15] = '\0';
  int mac_len = EEPROM.read(EEPROM_BLE_MAC);
  if (mac_len > 0 && mac_len < 18) {
    for (int i = 0; i < mac_len; i++) ble_mac_address[i] = EEPROM.read(EEPROM_BLE_MAC + 1 + i);
    ble_mac_address[mac_len] = '\0';
  }
}

void saveConfigToEEPROM() {
  EEPROM.writeInt(EEPROM_DEVICE_ID, device_id);
  int client_id_len = strlen(client_id);
  EEPROM.write(EEPROM_CLIENT_ID, client_id_len);
  for (int i = 0; i < client_id_len; i++) EEPROM.write(EEPROM_CLIENT_ID + 1 + i, client_id[i]);
  for (int i = 0; i < 15; i++) EEPROM.write(EEPROM_SSID + i, ssid[i]);
  for (int i = 0; i < 15; i++) EEPROM.write(EEPROM_PASSWORD + i, password[i]);
  int mac_len = strlen(ble_mac_address);
  EEPROM.write(EEPROM_BLE_MAC, mac_len);
  for (int i = 0; i < mac_len; i++) EEPROM.write(EEPROM_BLE_MAC + 1 + i, ble_mac_address[i]);
  EEPROM.commit();
}

// === ГЛАВНЫЙ ЦИКЛ ===
void loop() {
  unsigned long currentMillis = millis();

  // === Serial команды ===
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.startsWith("setdeviceid ")) {
      device_id = command.substring(12).toInt();
      saveConfigToEEPROM();
      Serial.printf("Device ID set to: %d\n", device_id);
    } else if (command.startsWith("setclientid ")) {
      String newClientId = command.substring(12);
      strncpy(client_id, newClientId.c_str(), sizeof(client_id) - 1);
      client_id[sizeof(client_id) - 1] = '\0';
      saveConfigToEEPROM();
      Serial.printf("Client ID set to: %s\n", client_id);
    } else if (command.startsWith("setssid ")) {
      String newSsid = command.substring(8);
      strncpy(ssid, newSsid.c_str(), sizeof(ssid) - 1);
      ssid[sizeof(ssid) - 1] = '\0';
      saveConfigToEEPROM();
      Serial.printf("SSID set to: %s\n", ssid);
      if (wifiConnected) {
        WiFi.disconnect();
        wifiConnected = false;
        digitalWrite(LED_PIN, LOW);
        WiFi.begin(ssid, password);
      }
    } else if (command.startsWith("setpassword ")) {
      String newPassword = command.substring(12);
      strncpy(password, newPassword.c_str(), sizeof(password) - 1);
      password[sizeof(password) - 1] = '\0';
      saveConfigToEEPROM();
      Serial.printf("Password set to: %s\n", password);
      if (wifiConnected) {
        WiFi.disconnect();
        wifiConnected = false;
        digitalWrite(LED_PIN, LOW);
        WiFi.begin(ssid, password);
      }
    } else if (command.startsWith("setblemac ")) {
      String newBleMac = command.substring(10);
      strncpy(ble_mac_address, newBleMac.c_str(), sizeof(ble_mac_address) - 1);
      ble_mac_address[sizeof(ble_mac_address) - 1] = '\0';
      saveConfigToEEPROM();
      Serial.printf("BLE MAC set to: %s\n", ble_mac_address);
    } else if (command == "getsettings") {
      Serial.printf("settings: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s; BLE_MAC=%s;\n",
                    device_id, client_id, ssid, password, ble_mac_address);
    }
  }

  // === BLE сканирование ===
  startBLEScan();

  // === Запись в буфер ===
  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    int reed_switch = !digitalRead(REED_SWITCH_PIN);

    // Определяем, есть ли свежие данные (в течение 60 сек)
    bool isTempValid = (lastTemperatureUpdate != 0) && 
                       (currentMillis - lastTemperatureUpdate <= TEMPERATURE_TIMEOUT);

    float tempToSend;
    int status;

    if (isTempValid) {
      tempToSend = currentTemperature;
      status = 1;
      Serial.printf("Температура актуальна: %.1f°C (%.1f сек назад)\n", 
                    tempToSend, (currentMillis - lastTemperatureUpdate) / 1000.0);
    } else {
      tempToSend = 0.0;
      status = 0;

      if (strlen(ble_mac_address) == 0) {
        Serial.println("BLE MAC адрес не настроен!");
      } else if (lastTemperatureUpdate == 0) {
        Serial.println("BLE датчик ещё не найден.");
      } else {
        Serial.printf("ТАЙМАУТ: 60+ сек без данных → температура = 0.0°C (%.1f сек назад)\n", 
                      (currentMillis - lastTemperatureUpdate) / 1000.0);
      }
    }

    addDataToBuffer(device_id, client_id, tempToSend, reed_switch, status);
    lastSampleTime = currentMillis;
  }

  sendBufferData();
  delay(100);
}
