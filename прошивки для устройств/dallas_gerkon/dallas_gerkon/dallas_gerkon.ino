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
#define EEPROM_SIZE 128  // Увеличиваем размер EEPROM для URL
#define EEPROM_DEVICE_ID 0
#define EEPROM_CLIENT_ID 4
#define EEPROM_SSID 16
#define EEPROM_PASSWORD 32
#define EEPROM_URL 48  // Новый адрес для URL (максимум 32 байта)

char server_url[32] = "http://80.80.101.123:503/data"; // По умолчанию

// Параметры по умолчанию
int device_id = 4;
char client_id[12] = "2"; // Фиксированный массив вместо String
char ssid[16] = "TVCom";
char password[16] = "taGENeTa";

// Параметры
#define SAMPLE_INTERVAL 15000  // Интервал записи данных (15 секунд)
#define RESEND_INTERVAL 5000   // Интервал проверки отправки (5 секунд)
#define LOG_INTERVAL 30000     // Интервал для логов ошибок (30 секунд)
#define WIFI_RECONNECT_INTERVAL 20000 // Интервал для логов попытки подключения WiFi (20 секунд)
#define HTTP_TIMEOUT 10000     // Таймаут HTTP-запроса (10 секунд)
#define MAX_RETRIES 3          // Максимальное количество попыток отправки
#define MAX_BUFFER_SIZE 500    // Увеличенный размер буфера
#define MIN_RSSI -90           // Минимальный уровень сигнала WiFi (dBm)

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

// Переменные для отслеживания состояния двери
int lastReedState = -1; // Последнее известное состояние двери
unsigned long lastDoorChangeTime = 0; // Время последнего изменения состояния двери
#define DOOR_DEBOUNCE_DELAY 50 // Задержка для дребезга контактов (50 мс)

void wifiTask(void* parameter);
void ensureTimeSynced();
void getCurrentTime(char* buffer);
void addDataToBuffer(int deviceId, const char* clientId, float temperature, int reed_switch, int status);
bool sendDataToServerWiFi();
void sendBufferData();
void loadConfigFromEEPROM();
void saveConfigToEEPROM();
void checkDoorState(); // Новая функция для проверки состояния двери

void setup() {
  Serial.begin(9600);
  Serial.println("Temperature Sensor (DS18B20 + Reed Switch) Test");

  pinMode(LED_PIN, OUTPUT);
  pinMode(REED_SWITCH_PIN, INPUT_PULLUP);  // Геркон с подтягивающим резистором
  digitalWrite(LED_PIN, LOW);

  sensors.begin();

  EEPROM.begin(EEPROM_SIZE);
  loadConfigFromEEPROM();
  Serial.printf("Загружено из EEPROM: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s; URL=%s\n",
                device_id, client_id, ssid, password, server_url);

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

  // Инициализация состояния двери
  lastReedState = digitalRead(REED_SWITCH_PIN);
  Serial.printf("Начальное состояние двери: %s\n", lastReedState == HIGH ? "ЗАКРЫТО" : "ОТКРЫТО");

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
  http.begin(server_url);  // Используем динамический URL
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
  for (int i = 0; i < 32; i++) {  // Чтение URL (до 32 байт)
    server_url[i] = EEPROM.read(EEPROM_URL + i);
    if (server_url[i] == '\0') break;  // Прерываем при достижении конца строки
  }
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
  for (int i = 0; i < strlen(server_url); i++) {
    EEPROM.write(EEPROM_URL + i, server_url[i]);
  }
  EEPROM.write(EEPROM_URL + strlen(server_url), '\0');  // Завершающий нуль
  EEPROM.commit();
}

void checkDoorState() {
  unsigned long currentMillis = millis();
  
  // Проверяем состояние двери
  int currentReedState = digitalRead(REED_SWITCH_PIN);
  
  // Используем антидребезг
  if (currentMillis - lastDoorChangeTime > DOOR_DEBOUNCE_DELAY) {
    if (currentReedState != lastReedState) {
      // Состояние двери изменилось
      lastReedState = currentReedState;
      lastDoorChangeTime = currentMillis;
      
      // Запрашиваем температуру
      sensors.requestTemperatures();
      float temperature = sensors.getTempCByIndex(0);
      
      if (temperature != DEVICE_DISCONNECTED_C) {
        // Формируем пакет при изменении состояния двери
        addDataToBuffer(device_id, client_id, temperature, currentReedState, 1);
        Serial.printf("Изменение состояния двери: %s, температура: %.2f°C\n", 
                     currentReedState == HIGH ? "ЗАКРЫТО" : "ОТКРЫТО", temperature);
      } else {
        Serial.printf("Изменение состояния двери: %s, температура: ОШИБКА ДАТЧИКА\n", 
                     currentReedState == HIGH ? "ЗАКРЫТО" : "ОТКРЫТО");
        addDataToBuffer(device_id, client_id, 0.0, currentReedState, 0);
      }
    }
  }
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
    } else if (command.startsWith("seturl ")) {
      String newUrl = command.substring(7);  // Учитываем "seturl "
      strncpy(server_url, newUrl.c_str(), sizeof(server_url) - 1);
      server_url[sizeof(server_url) - 1] = '\0';
      saveConfigToEEPROM();
      Serial.printf("URL set to: %s\n", server_url);
    } else if (command == "getsettings") {
      Serial.printf("settings: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s; URL=%s;\n",
                    device_id, client_id, ssid, password, server_url);
    }
  }

  // Проверяем изменение состояния двери
  checkDoorState();

  // Регулярный опрос температуры по расписанию
  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    sensors.requestTemperatures();
    float temperature = sensors.getTempCByIndex(0);
    int reed_switch = digitalRead(REED_SWITCH_PIN);  // 0=открыто, 1=закрыто

    if (temperature != DEVICE_DISCONNECTED_C) {
      addDataToBuffer(device_id, client_id, temperature, reed_switch, 1);
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
