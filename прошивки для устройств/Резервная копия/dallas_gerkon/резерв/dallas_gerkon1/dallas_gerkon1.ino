#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <queue>
#include <HTTPClient.h>
#include <time.h>
#include <EEPROM.h>

// Пины
#define ONE_WIRE_BUS 32  // Пин для DS18B20
#define REED_SWITCH_PIN 27  // Пин для геркона
#define LED_PIN 2       // Пин для индикации

// Определяем адреса в EEPROM
#define EEPROM_SIZE 64
#define EEPROM_DEVICE_ID 0
#define EEPROM_CLIENT_ID 4
#define EEPROM_SSID 16
#define EEPROM_PASSWORD 32

// Параметры по умолчанию
int device_id = 4;
char client_id[12] = "2"; // Фиксированный массив вместо String
char ssid[16] = "TVCom";
char password[16] = "taGENeTa";

// Параметры
#define SERVER_URL "http://80.80.101.123:503/data"
#define SAMPLE_INTERVAL 15000  // Интервал записи данных (15 секунд)
#define RESEND_INTERVAL 5000   // Интервал проверки отправки (5 секунд)
#define LOG_INTERVAL 30000     // Интервал для логов ошибок (30 секунд)
#define WIFI_RECONNECT_INTERVAL 20000 // Интервал для логов попытки подключения WiFi (20 секунд)
#define HTTP_TIMEOUT 10000     // Таймаут HTTP-запроса (10 секунд)
#define MAX_RETRIES 3          // Максимальное количество попыток отправки
#define MAX_BUFFER_SIZE 500    // Увеличенный размер буфера
#define MIN_RSSI -90            // Минимальный уровень сигнала WiFi (dBm)

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;  // UTC+3 (Москва)
const int daylightOffset_sec = 0;

// Структура для буфера данных
struct CardData {
  char deviceType[16];  // "freezer"
  int deviceId;         // 4 байта
  char clientId[12];    // Например, "2"
  float temperature;    // 4 байта
  int status;           // 4 байта
  int reed_switch;      // 4 байта
  char timestamp[20];   // "2025-07-25 10:37:00"
};

std::queue<CardData> sendBuffer;
unsigned long lastSampleTime = 0;
unsigned long lastSendAttempt = 0;
unsigned long lastLogTime = 0; // Для ограничения частоты логов
unsigned long lastWifiLogTime = 0; // Для логов попытки подключения WiFi
bool wifiConnected = false;

void wifiTask(void* parameter);
void ensureTimeSynced();
void getCurrentTime(char* buffer);
void addDataToBuffer(int deviceId, const char* clientId, float temperature, int reed_switch, int status);
bool sendDataToServerWiFi();
void sendBufferData();
void loadConfigFromEEPROM();
void saveConfigToEEPROM();

void setup() {
  Serial.begin(9600);
  Serial.println("Temperature Sensor (DS18B20 + Reed Switch) Test");

  pinMode(LED_PIN, OUTPUT);
  pinMode(REED_SWITCH_PIN, INPUT_PULLUP);  // Геркон с подтягивающим резистором
  digitalWrite(LED_PIN, LOW);

  sensors.begin();

  EEPROM.begin(EEPROM_SIZE);
  loadConfigFromEEPROM();
  Serial.printf("Загружено из EEPROM: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s\n",
                device_id, client_id, ssid, password);

  Serial.println("Запуск Wi-Fi задачи...");
  xTaskCreatePinnedToCore(
    wifiTask,
    "WiFiTask",
    10000,
    NULL,
    1,
    NULL,
    0
  );

  ensureTimeSynced();
}

void wifiTask(void* parameter) {
  while (true) {
    unsigned long currentMillis = millis();
    
    // Проверяем, потеряно ли соединение
    if (wifiConnected && WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      digitalWrite(LED_PIN, LOW);
      Serial.println("WiFi соединение потеряно! Сбрасываем статус...");
    }

    // Пытаемся подключиться, если не подключены
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
    while (!getLocalTime(&timeinfo) && attempts < 5) {
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
      Serial.println("Ошибка синхронизации времени!");
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

  if (sendBuffer.empty()) {
    return false;
  }

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
      if (retryCount < MAX_RETRIES) {
        delay(1000);
      }
    }
  }

  if (httpResponseCode > 0) {
    Serial.printf("Данные отправлены, код: %d\n", httpResponseCode);
    if (httpResponseCode == 200) {
      sendBuffer.pop();
    }
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
      // Отправляем все записи из буфера
      while (!sendBuffer.empty()) {
        bool success = sendDataToServerWiFi();
        if (!success) {
          break; // Прерываем, если отправка не удалась
        }
        delay(100); // Небольшая задержка между отправками
      }
    }
    lastSendAttempt = currentMillis;
  }
}

void loadConfigFromEEPROM() {
  device_id = EEPROM.readInt(EEPROM_DEVICE_ID);
  int client_id_len = EEPROM.read(EEPROM_CLIENT_ID);
  if (client_id_len > 0 && client_id_len < 12) {
    for (int i = 0; i < client_id_len; i++) {
      client_id[i] = EEPROM.read(EEPROM_CLIENT_ID + 1 + i);
    }
    client_id[client_id_len] = '\0';
  }
  for (int i = 0; i < 15; i++) {
    ssid[i] = EEPROM.read(EEPROM_SSID + i);
  }
  ssid[15] = '\0';
  for (int i = 0; i < 15; i++) {
    password[i] = EEPROM.read(EEPROM_PASSWORD + i);
  }
  password[15] = '\0';
}

void saveConfigToEEPROM() {
  EEPROM.writeInt(EEPROM_DEVICE_ID, device_id);
  int client_id_len = strlen(client_id);
  EEPROM.write(EEPROM_CLIENT_ID, client_id_len);
  for (int i = 0; i < client_id_len; i++) {
    EEPROM.write(EEPROM_CLIENT_ID + 1 + i, client_id[i]);
  }
  for (int i = 0; i < 15; i++) {
    EEPROM.write(EEPROM_SSID + i, ssid[i]);
  }
  for (int i = 0; i < 15; i++) {
    EEPROM.write(EEPROM_PASSWORD + i, password[i]);
  }
  EEPROM.commit();
}

void loop() {
  unsigned long currentMillis = millis();

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
    } else if (command == "getsettings") {
      Serial.printf("settings: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s;\n",
                    device_id, client_id, ssid, password);
    }
  }

  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    sensors.requestTemperatures();
    float temperature = sensors.getTempCByIndex(0);
    int reed_switch = !digitalRead(REED_SWITCH_PIN);  // 0=открыто, 1=закрыто (инвертировано для корректной логики)

    if (temperature != DEVICE_DISCONNECTED_C) {
      // Игнорируем значения выше 84°C
      if (temperature <= 84.0) {
        addDataToBuffer(device_id, client_id, temperature, reed_switch, 1);
      } else {
        Serial.print("Игнорирую температуру > 84°C: ");
        Serial.println(temperature); // Корректный вывод значения температуры
      }
    } else {
      if (currentMillis - lastLogTime >= LOG_INTERVAL) {
        Serial.println("Ошибка: датчик температуры не подключен! Отправляем нулевые данные.");
        lastLogTime = currentMillis;
      }
      addDataToBuffer(device_id, client_id, 0.0, reed_switch, 0);
    }

    lastSampleTime = currentMillis;
  }

  sendBufferData(); 

  delay(100);
}
