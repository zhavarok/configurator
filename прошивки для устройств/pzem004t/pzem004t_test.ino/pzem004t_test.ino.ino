#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <queue>
#include <HTTPClient.h>
#include <time.h>
#include <EEPROM.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>  // Для watchdog

// Пины для PZEM-004T
const int RXPin_PZEM = 14; // Пин RX для PZEM
const int TXPin_PZEM = 13; // Пин TX для PZEM

// Определяем адреса в EEPROM
#define EEPROM_SIZE 128  // Увеличиваем размер для хранения URL
#define EEPROM_DEVICE_ID 0
#define EEPROM_CLIENT_ID 4
#define EEPROM_SSID 16
#define EEPROM_PASSWORD 32
#define EEPROM_URL 48  // Новый адрес для URL (максимум 32 байта)

char server_url[32] = "http://80.80.101.123:503/data"; // По умолчанию

// Параметры по умолчанию
int device_id = 3;
char client_id[12] = "2";
char ssid[16] = "Zhavar";
char password[16] = "00005555";

// Параметры
#define SAMPLE_INTERVAL 15000  // Интервал записи данных (15 секунд)
#define RESEND_INTERVAL 5000   // Интервал проверки отправки (5 секунд)
#define LOG_INTERVAL 30000     // Интервал для логов ошибок (30 секунд)
#define WIFI_RECONNECT_INTERVAL 20000 // Интервал для логов попытки подключения WiFi (20 секунд)
#define HTTP_TIMEOUT 5000      // Таймаут HTTP-запроса (5 секунд, снижено для избежания зависаний)
#define HTTP_CONNECT_TIMEOUT 5000  // Таймаут подключения (новое, для fix hang)
#define MAX_RETRIES 3          // Максимальное количество попыток отправки
#define RETRY_DELAY 2000       // Задержка между попытками (2 секунды)
#define MAX_BUFFER_SIZE 500    // Увеличенный размер буфера
#define MIN_RSSI -75           // Минимальный уровень сигнала WiFi (dBm)
#define WDT_TIMEOUT 5          // Watchdog timeout в секундах

// Инициализация PZEM на Serial1
PZEM004Tv30 pzem(Serial1, RXPin_PZEM, TXPin_PZEM);

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;  // UTC+3 (Москва)
const int daylightOffset_sec = 0;

// Пин для индикации
#define LED_PIN 2

// Структура для буфера данных
struct CardData {
  char deviceType[16];  // "split_system"
  int deviceId;         // 4 байта
  char clientId[12];    // Например, "2"
  float voltage;        // 4 байта
  float current;        // 4 байта
  float power;          // 4 байта
  float energy;         // 4 байта
  float frequency;      // 4 байта
  int status;           // 4 байта
  char timestamp[20];   // "2025-07-25 10:37:00"
};

std::queue<CardData> sendBuffer;
unsigned long lastSampleTime = 0;
unsigned long lastSendAttempt = 0;
unsigned long lastLogTime = 0;
unsigned long lastWifiLogTime = 0;
volatile bool wifiConnected = false;  // Volatile для безопасного доступа из тасков

void wifiTask(void* parameter);
void ensureTimeSynced();
void getCurrentTime(char* buffer);
void addDataToBuffer(int deviceId, const char* clientId, float voltage, float current, float power, float energy, float frequency, int status);
bool sendDataToServerWiFi();
void sendBufferData();
void loadConfigFromEEPROM();
void saveConfigToEEPROM();

void setup() {
  Serial.begin(9600);
  Serial.println("PZEM-004T Current Sensor Test");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial1.begin(9600, SERIAL_8N1, RXPin_PZEM, TXPin_PZEM);

  EEPROM.begin(EEPROM_SIZE);
  loadConfigFromEEPROM();
  Serial.printf("Загружено из EEPROM: DEVICE_ID=%d; CLIENT_ID=%s; SSID=%s; PASSWORD=%s; URL=%s\n",
                device_id, client_id, ssid, password, server_url);

  // Инициализация watchdog
  esp_task_wdt_init(WDT_TIMEOUT, true);  // Включить WDT с ресетом
  esp_task_wdt_add(NULL);  // Добавить main task (loop)

  Serial.println("Запуск Wi-Fi задачи...");
  xTaskCreatePinnedToCore(
    wifiTask,
    "WiFiTask",
    10000,
    NULL,
    1,
    NULL,
    1
  );

  ensureTimeSynced();
}

void wifiTask(void* parameter) {
  esp_task_wdt_add(NULL);  // Добавить этот таск в WDT

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
    esp_task_wdt_reset();  // Сброс WDT
    delay(500);
  }
}

void ensureTimeSynced() {
  struct tm timeinfo;
  if (wifiConnected) {
    Serial.println("Синхронизация времени через Wi-Fi...");
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 10) {
      Serial.println("Время не синхронизировано. Повторная попытка...");
      digitalWrite(LED_PIN, LOW);
      delay(500);
      digitalWrite(LED_PIN, HIGH);
      delay(500);
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
      Serial.println("Ошибка при получении времени через NTP!");
      strcpy(buffer, "0000-00-00 00:00:00");
      return;
    }
    strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    strcpy(buffer, "0000-00-00 00:00:00");
  }
}

void addDataToBuffer(int deviceId, const char* clientId, float voltage, float current, float power, float energy, float frequency, int status) {
  char timestamp[20];
  getCurrentTime(timestamp);
  CardData newData;
  strcpy(newData.deviceType, "split_system");
  newData.deviceId = deviceId;
  strncpy(newData.clientId, clientId, sizeof(newData.clientId) - 1);
  newData.clientId[sizeof(newData.clientId) - 1] = '\0';
  newData.voltage = voltage;
  newData.current = current;
  newData.power = power;
  newData.energy = energy;
  newData.frequency = frequency;
  newData.status = status;
  strcpy(newData.timestamp, timestamp);
  
  Serial.printf("Добавлено в буфер: Device ID: %d - Client ID: %s - Voltage: %.1f В - Current: %.2f А - Power: %.1f Вт - Energy: %.3f кВт·ч - Frequency: %.1f Гц - Status: %d - Timestamp: %s - Buffer Size: %d/%d\n",
                newData.deviceId, newData.clientId, newData.voltage, newData.current, newData.power, newData.energy, newData.frequency, newData.status, newData.timestamp, sendBuffer.size() + 1, MAX_BUFFER_SIZE);
  
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
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);  // Новое: таймаут подключения для fix hang
  http.setTimeout(HTTP_TIMEOUT);  // Сниженный таймаут
  http.setReuse(false); // Отключаем повторное использование соединения
  if (!http.begin(server_url)) {
    Serial.println("Ошибка инициализации HTTP-клиента!");
    return false;
  }
  http.addHeader("Content-Type", "application/json; charset=utf-8");

  CardData data = sendBuffer.front();
  char jsonPayload[512];
  snprintf(jsonPayload, sizeof(jsonPayload),
           "{\"client_id\": \"%s\", \"data\": [{\"device_id\": %d, \"voltage\": %.1f, \"current\": %.2f, \"power\": %.1f, \"energy\": %.3f, \"frequency\": %.1f, \"status\": %d, \"timestamp\": \"%s\"}]}",
           data.clientId, data.deviceId, data.voltage, data.current, data.power, data.energy, data.frequency, data.status, data.timestamp);

  Serial.printf("Отправка JSON: %s\n", jsonPayload);
  
  int retryCount = 0;
  int httpResponseCode = -1;
  while (retryCount < MAX_RETRIES && httpResponseCode <= 0) {
    httpResponseCode = http.POST(jsonPayload);
    retryCount++;
    if (httpResponseCode <= 0) {
      unsigned long currentMillis = millis();
      if (currentMillis - lastLogTime >= LOG_INTERVAL) {
        Serial.printf("Ошибка отправки, код: %d, попытка %d/%d, свободная память: %d байт\n",
                      httpResponseCode, retryCount, MAX_RETRIES, heap_caps_get_free_size(MALLOC_CAP_8BIT));
        lastLogTime = currentMillis;
      }
      if (retryCount < MAX_RETRIES) {
        delay(RETRY_DELAY);
        http.end(); // Закрываем соединение
        http.begin(server_url); // Повторная инициализация
        http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);  // Переустановка
        http.setTimeout(HTTP_TIMEOUT);
        http.addHeader("Content-Type", "application/json; charset=utf-8");
      }
    }
  }

  if (httpResponseCode > 0) {
    Serial.printf("Данные отправлены, код: %d, свободная память: %d байт\n",
                  httpResponseCode, heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (httpResponseCode == 200) {
      sendBuffer.pop();
    }
    http.end();
    return httpResponseCode == 200;
  } else {
    unsigned long currentMillis = millis();
    if (currentMillis - lastLogTime >= LOG_INTERVAL) {
      Serial.printf("Ошибка отправки после %d попыток, код: %d, свободная память: %d байт\n",
                    MAX_RETRIES, httpResponseCode, heap_caps_get_free_size(MALLOC_CAP_8BIT));
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
        Serial.printf("Уровень сигнала WiFi: %d dBm, свободная память: %d байт\n",
                      WiFi.RSSI(), heap_caps_get_free_size(MALLOC_CAP_8BIT));
        lastLogTime = currentMillis;
      }
      while (!sendBuffer.empty()) {
        bool success = sendDataToServerWiFi();
        if (!success) {
          break;
        }
        delay(500); // Увеличенная задержка между отправками для стабильности
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

  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power = pzem.power();
    float energy = pzem.energy();
    float frequency = pzem.frequency();

    if (!isnan(voltage) && !isnan(current) && !isnan(power) && !isnan(energy) && !isnan(frequency)) {
      addDataToBuffer(device_id, client_id, voltage, current, power, energy, frequency, 1);
    } else {
      if (currentMillis - lastLogTime >= LOG_INTERVAL) {
        Serial.println("Ошибка: данные с PZEM-004T не получены! Отправляем нулевые данные.");
        lastLogTime = currentMillis;
      }
      addDataToBuffer(device_id, client_id, 0.0, 0.0, 0.0, 0.0, 0.0, 0);
    }

    lastSampleTime = currentMillis;
  }

  sendBufferData();

  esp_task_wdt_reset();  // Сброс WDT в main loop
  delay(100);
}
