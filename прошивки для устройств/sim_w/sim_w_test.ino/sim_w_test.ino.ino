#include <WiFi.h>
#include <OneWire.h>
#include <queue>
#include <time.h>
#include <HTTPClient.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <freertos/semphr.h>

#define ONE_WIRE_IN 19
#define ONE_WIRE_OUT 23
#define LED_PIN 2

OneWire oneWireIn(ONE_WIRE_IN);
OneWire oneWireOut(ONE_WIRE_OUT);

const int ledIN = 5;
const int ledOUT = 18;

// Определяем адреса в EEPROM
#define EEPROM_SIZE 64
#define EEPROM_DEVICE_ID 0
#define EEPROM_SSID 1
#define EEPROM_PASSWORD 16
#define EEPROM_URL 32

char server_url[32] = "http://80.80.101.123:502/data";

int device_id;
char ssid[16] = {0};
char password[16] = {0};

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

struct CardData {
    String reader;
    unsigned long cardID;
    String timestamp;
};

struct LastCardInfo {
    unsigned long cardID;
    unsigned long lastTime;
};

struct LedState {
    bool isOn;
    unsigned long onTime;
};

std::queue<CardData> sendBuffer;

unsigned long card_in = 0;
unsigned long card_out = 0;
unsigned long prev_card_in = 0;
unsigned long prev_card_out = 0;

unsigned long lastCardInTime = 0;
unsigned long lastCardOutTime = 0;
const unsigned long debounceTime = 5000;
const unsigned long cardRepeatInterval = 60000;
const unsigned long ledPulseDuration = 1000;
const unsigned long maxValidCardID = 4000000000UL;

LastCardInfo lastCardIn = {0, 0};
LastCardInfo lastCardOut = {0, 0};
LedState ledInState = {false, 0};
LedState ledOutState = {false, 0};

unsigned long lastSendAttempt = 0;
const unsigned long resendInterval = 5000;

bool wifiConnected = false;

SemaphoreHandle_t queueMutex;

void processCardOnCore0(void* parameter);
void wifiTask(void* parameter);
void setup();
void ensureTimeSynced();
String getCurrentTime();
void addDataToBuffer(const String& reader, unsigned long cardID);
bool sendDataToServerWiFi(const CardData& data);
void sendBufferData();
void processCard(OneWire& reader, unsigned long& cardVar, unsigned long& prevCardVar, const String& readerName, int ledPin, unsigned long& lastCardTime);
void loop();
unsigned long readCard(OneWire &oneWire);
void loadConfigFromEEPROM();
void saveConfigToEEPROM();

void processCardOnCore0(void* parameter) {
    while (true) {
        processCard(oneWireIn, card_in, prev_card_in, "Entrance", ledIN, lastCardInTime);
        processCard(oneWireOut, card_out, prev_card_out, "Exit", ledOUT, lastCardOutTime);
        delay(100);
    }
}

void wifiTask(void* parameter) {
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            Serial.print(".");
            WiFi.begin(ssid, password);
            int attempt = 0;
            while (WiFi.status() != WL_CONNECTED && attempt < 20) {
                delay(1000);
                Serial.print(".");
                attempt++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                wifiConnected = true;
                Serial.println("\nWi-Fi подключен!");
                digitalWrite(LED_PIN, HIGH);
                configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
                Serial.println("Синхронизация времени через NTP...");
                ensureTimeSynced();
            }
        }
        delay(1000);
    }
}

void setup() {
    Serial.begin(9600);
    
    pinMode(LED_PIN, OUTPUT);
    pinMode(ledIN, OUTPUT);
    pinMode(ledOUT, OUTPUT);
    
    digitalWrite(ledIN, LOW);
    digitalWrite(ledOUT, LOW);

    EEPROM.begin(EEPROM_SIZE);
    loadConfigFromEEPROM();
    Serial.println("Загружено из EEPROM: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password) + "; URL=" + String(server_url));

    queueMutex = xSemaphoreCreateMutex();

    Serial.println("Попытка подключения к Wi-Fi в фоновом режиме...");
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

    xTaskCreatePinnedToCore(
        processCardOnCore0,
        "ProcessCards",
        10000,
        NULL,
        1,
        NULL,
        0
    );
}

void ensureTimeSynced() {
    struct tm timeinfo;
    if (wifiConnected) {
        Serial.println("Режим Wi-Fi: Синхронизация времени через Wi-Fi...");
        int attempts = 0;
        while (!getLocalTime(&timeinfo) && attempts < 10) {
            Serial.println("Время не синхронизировано. Повторная попытка...");
            digitalWrite(LED_PIN, LOW);
            delay(500);
            digitalWrite(LED_PIN, HIGH);
            delay(500);
            attempts++;
        }
        if (attempts < 10) {
            Serial.println("Время синхронизировано успешно!");
            Serial.print("Текущее время: ");
            Serial.println(getCurrentTime());
        } else {
            Serial.println("Не удалось синхронизировать время после 10 попыток.");
        }
    }
}

String getCurrentTime() {
    if (wifiConnected) {
        struct tm timeinfo;
        vTaskDelay(10);
        if (!getLocalTime(&timeinfo)) {
            Serial.println("Не удалось получить время через Wi-Fi!");
            return "00:00:00";
        }
        char buffer[20];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        return String(buffer);
    }
    return "00:00:00";
}

void addDataToBuffer(const String& reader, unsigned long cardID) {
    String currentTime = getCurrentTime();
    CardData newData = {reader, cardID, currentTime};
    
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        sendBuffer.push(newData);
        xSemaphoreGive(queueMutex);
    }
    
    Serial.print("Добавлено в буфер: ");
    Serial.print(reader);
    Serial.print(" - Card ID: ");
    Serial.print(cardID);
    Serial.print(" - Timestamp: ");
    Serial.println(currentTime);
}

bool sendDataToServerWiFi(const CardData& data) {
    if (!wifiConnected) return false;
    HTTPClient http;
    http.begin(server_url);
    http.addHeader("Content-Type", "application/json");
    
    String jsonData = "{\"device_id\": " + String(device_id) + ", \"reader\": \"" + data.reader + "\", \"card_id\": " + String(data.cardID) + ", \"timestamp\": \"" + data.timestamp + "\"}";
    
    int httpResponseCode = http.POST(jsonData);
    
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Ответ сервера: " + response);
        return true;
    } else {
        Serial.println("Ошибка отправки данных: " + String(httpResponseCode));
        return false;
    }
    
    http.end();
}

void sendBufferData() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastSendAttempt >= resendInterval) {
        CardData data;
        bool hasData = false;
        
        if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
            if (!sendBuffer.empty()) {
                data = sendBuffer.front();
                hasData = true;
            }
            xSemaphoreGive(queueMutex);
        }
        
        if (hasData) {
            bool success = sendDataToServerWiFi(data);

            if (success) {
                if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
                    sendBuffer.pop();
                    xSemaphoreGive(queueMutex);
                }
            }
            
            lastSendAttempt = currentMillis;
        }
    }
}

void processCard(OneWire& reader, unsigned long& cardVar, unsigned long& prevCardVar, 
                const String& readerName, int ledPin, unsigned long& lastCardTime) {
    unsigned long new_card = readCard(reader);
    unsigned long currentMillis = millis();
    LastCardInfo& lastCard = (readerName == "Entrance") ? lastCardIn : lastCardOut;
    LedState& ledState = (readerName == "Entrance") ? ledInState : ledOutState;

    if (new_card != 0 && new_card < maxValidCardID && (currentMillis - lastCardTime) >= debounceTime) {
        if (!ledState.isOn) {
            digitalWrite(ledPin, HIGH);
            ledState.isOn = true;
            ledState.onTime = currentMillis;
            lastCardTime = currentMillis;
            Serial.println(readerName + ": Светодиод включен");
        }

        if (new_card != lastCard.cardID || (currentMillis - lastCard.lastTime) >= cardRepeatInterval) {
            prevCardVar = new_card;
            cardVar = new_card;
            
            lastCard.cardID = new_card;
            lastCard.lastTime = currentMillis;
            
            Serial.print(readerName + " (Decimal): ");
            Serial.println(cardVar);
            addDataToBuffer(readerName, cardVar);
        }
    } 
    else if (new_card >= maxValidCardID) {
        Serial.println(readerName + ": Ошибочный номер карты: " + String(new_card) + " (игнорируется)");
    }
    else if (new_card == 0 && cardVar != 0) {
        cardVar = 0;
        prevCardVar = 0;
        Serial.println(readerName + " (Decimal): 0");
    }

    if (ledState.isOn && (currentMillis - ledState.onTime >= ledPulseDuration)) {
        digitalWrite(ledPin, LOW);
        ledState.isOn = false;
        Serial.println(readerName + ": Светодиод выключен");
    }
}

void loop() {
    sendBufferData();
    
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command.startsWith("setdeviceid ")) {
            device_id = command.substring(12).toInt();
            saveConfigToEEPROM();
            Serial.println("Device ID set to: " + String(device_id));
        } else if (command.startsWith("setssid ")) {
            String newSsid = command.substring(8);
            newSsid.toCharArray(ssid, 16);
            saveConfigToEEPROM();
            Serial.println("SSID set to: " + String(ssid));
            if (wifiConnected) {
                WiFi.begin(ssid, password);
            }
        } else if (command.startsWith("setpassword ")) {
            String newPassword = command.substring(12);
            newPassword.toCharArray(password, 16);
            saveConfigToEEPROM();
            Serial.println("Password set to: " + String(password));
            if (wifiConnected) {
                WiFi.begin(ssid, password);
            }
        } else if (command.startsWith("seturl ")) {
            String newUrl = command.substring(7);
            newUrl.toCharArray(server_url, 32);
            saveConfigToEEPROM();
            Serial.println("URL set to: " + String(server_url));
        } else if (command == "gettimesettings") {
            Serial.println("settings: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password) + "; URL=" + String(server_url) + ";");
        }
    }
    
    delay(100);
}

unsigned long readCard(OneWire &oneWire) {
    static unsigned long lastCard = 0;
    static unsigned long lastReadTime = 0;
    const unsigned long readDebounce = 500;

    if (!oneWire.reset()) {
        lastCard = 0;
        return 0;
    }

    unsigned long currentMillis = millis();
    if (currentMillis - lastReadTime < readDebounce) {
        return lastCard;
    }

    oneWire.write(0x33);
    byte cardData[8];
    for (int i = 0; i < 8; i++) {
        cardData[i] = oneWire.read();
    }
    unsigned long cardID = 0;
    for (int i = 6; i >= 1; i--) {
        cardID = (cardID << 8) | cardData[i];
    }

    if (cardID != 0) {
        lastCard = cardID;
        lastReadTime = currentMillis;
    }
    return cardID;
}

void loadConfigFromEEPROM() {
    device_id = EEPROM.read(EEPROM_DEVICE_ID);
    for (int i = 0; i < 15; i++) {
        ssid[i] = EEPROM.read(EEPROM_SSID + i);
    }
    ssid[15] = '\0';
    for (int i = 0; i < 15; i++) {
        password[i] = EEPROM.read(EEPROM_PASSWORD + i);
    }
    password[15] = '\0';
    for (int i = 0; i < 32; i++) {
        server_url[i] = EEPROM.read(EEPROM_URL + i);
        if (server_url[i] == '\0') break;
    }

    Serial.println("Загружено из EEPROM: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password) + "; URL=" + String(server_url));
}

void saveConfigToEEPROM() {
    EEPROM.write(EEPROM_DEVICE_ID, device_id);
    for (int i = 0; i < 15; i++) {
        EEPROM.write(EEPROM_SSID + i, ssid[i]);
    }
    for (int i = 0; i < 15; i++) {
        EEPROM.write(EEPROM_PASSWORD + i, password[i]);
    }
    for (int i = 0; i < strlen(server_url); i++) {
        EEPROM.write(EEPROM_URL + i, server_url[i]);
    }
    EEPROM.write(EEPROM_URL + strlen(server_url), '\0');
    EEPROM.commit();

    Serial.println("Настройки сохранены в EEPROM: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password) + "; URL=" + String(server_url));
}
