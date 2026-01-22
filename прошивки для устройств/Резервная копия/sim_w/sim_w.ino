#include <WiFi.h>
#include <OneWire.h>
#include <queue>
#include <time.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <RTClib.h>
#include <EEPROM.h>

#define ONE_WIRE_IN 19
#define ONE_WIRE_OUT 22
#define LED_PIN 2
 
OneWire oneWireIn(ONE_WIRE_IN);
OneWire oneWireOut(ONE_WIRE_OUT);

HardwareSerial sim800(1);

#define RXD1 14
#define TXD1 12

const int ledIN = 5;
const int ledOUT = 18;

// Определяем адреса в EEPROM
#define EEPROM_SIZE 32
#define EEPROM_DEVICE_ID 0
#define EEPROM_SSID 1
#define EEPROM_PASSWORD 16

int device_id;
char ssid[16] = {0}; // 15 символов + завершающий \0
char password[16] = {0}; // 15 символов + завершающий \0

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

int modee = 2;

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

void processCardOnCore0(void* parameter);
void wifiTask(void* parameter);
void setup();
void ensureTimeSynced();
void initModem();
String getCurrentTime();
void addDataToBuffer(const String& reader, unsigned long cardID);
bool sendDataToServerWiFi(const CardData& data);
bool sendDataToServerSIM(const CardData& data);
bool sendATCommand(const char* command, const char* expectedResponse1, const char* expectedResponse2, unsigned long timeout);
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
        if (!wifiConnected) {
            Serial.print(".");
            if (WiFi.status() != WL_CONNECTED) {
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
    Serial.println("Загружено из EEPROM: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password));

    if (modee == 1) {
        sim800.begin(4800, SERIAL_8N1, RXD1, TXD1);
        initModem();
    }

    if (modee == 2) {
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
    }

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
    if (modee == 2 && wifiConnected) {
        Serial.println("Режим Wi-Fi: Синхронизация времени через Wi-Fi...");
        while (!getLocalTime(&timeinfo)) {
            Serial.println("Время не синхронизировано. Повторная попытка...");
            digitalWrite(LED_PIN, LOW);
            delay(500);
            digitalWrite(LED_PIN, HIGH);
            delay(500);
        }
        Serial.println("Время синхронизировано успешно!");
        Serial.print("Текущее время: ");
        Serial.println(getCurrentTime());
    }
}

void initModem() {
    delay(10000);
}

String getCurrentTime() {
    if (modee == 2 && wifiConnected) {
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
    sendBuffer.push(newData);
    
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
    String serverUrl = "http://80.80.101.123:502/data";
    
    String jsonData = "{\"device_id\": " + String(device_id) + ", \"reader\": \"" + data.reader + "\", \"card_id\": " + String(data.cardID) + ", \"timestamp\": \"" + data.timestamp + "\"}";
    
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");
    
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

bool sendDataToServerSIM(const CardData& data) {
    int MAX_RETRIES = 3;
    int retryCount = 0;
    sendATCommand("AT+CIPCLOSE", "CLOSE OK", "CLOSE OK", 15000);
    delay(2000);

    while (retryCount < MAX_RETRIES) {
        Serial.printf("Попытка подключения #%d из %d...\n", retryCount + 1, MAX_RETRIES);
        if (sendATCommand("AT+CIPSTART=\"TCP\",\"80.80.101.123\",501", "CONNECT OK", "CONNECT OK", 10000)) {
            Serial.println("Успешное подключение к серверу или уже подключены.");
            break;
        }
        Serial.println("Ошибка подключения через TCP. Повторяем попытку...");
        retryCount++;
    }

    if (retryCount == MAX_RETRIES) {
        Serial.println("Не удалось подключиться после 3 попыток. Перезагрузка модема...");
        sendATCommand("AT+CFUN=1,1", "OK", "OK", 15000);
        return false;
    }

    String jsonData = "{\"reader\": \"" + data.reader + "\", \"card_id\": " + String(data.cardID) + ", \"timestamp\": \"" + data.timestamp + "\"}";
    
    String httpHeader = "POST /data HTTP/1.1\r\n";
    httpHeader += "Host: 80.80.101.123\r\n";
    httpHeader += "Content-Type: application/json\r\n";
    httpHeader += "Content-Length: " + String(jsonData.length()) + "\r\n";
    httpHeader += "\r\n";

    if (!sendATCommand("AT+CIPSEND", ">", ">", 15000)) {
        Serial.println("Ошибка при подготовке к отправке данных.");
        return false;
    }

    sim800.print(httpHeader);
    sim800.print(jsonData);
    sim800.write(26);
    
    delay(2000);
    return true;
}

bool sendATCommand(const char* command, const char* expectedResponse1, const char* expectedResponse2, unsigned long timeout) {
    sim800.println(command);
    delay(5000);
    unsigned long startTime = millis();
    String response = "";

    while (millis() - startTime < timeout) {
        if (sim800.available()) {
            char c = sim800.read();
            response += c;
            Serial.print(c);
            if (response.indexOf(expectedResponse1) != -1 || response.indexOf(expectedResponse2) != -1) {
                Serial.println();
                return true;
            }
        }
    }
    
    Serial.println();
    Serial.println("Ошибка или таймаут при выполнении команды: " + String(command));
    Serial.println("Ответ: " + response);
    return false;
}

void sendBufferData() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastSendAttempt >= resendInterval && !sendBuffer.empty()) {
        CardData data = sendBuffer.front();
        bool success = false;

        if (modee == 1) {
            success = sendDataToServerSIM(data);
        } else if (modee == 2) {
            success = sendDataToServerWiFi(data);
        }

        if (success) {
            sendBuffer.pop();
        }
        
        lastSendAttempt = currentMillis;
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
            newSsid.toCharArray(ssid, 16); // Ограничиваем до 15 символов + \0
            saveConfigToEEPROM();
            Serial.println("SSID set to: " + String(ssid));
            if (wifiConnected) {
                WiFi.begin(ssid, password);
            }
        } else if (command.startsWith("setpassword ")) {
            String newPassword = command.substring(12);
            newPassword.toCharArray(password, 16); // Ограничиваем до 15 символов + \0
            saveConfigToEEPROM();
            Serial.println("Password set to: " + String(password));
            if (wifiConnected) {
                WiFi.begin(ssid, password);
            }
        } else if (command == "gettimesettings") {
            Serial.println("settings: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password) + ";");
        }
    }
    
    if (sim800.available()) {
        Serial.write(sim800.read());
    }
    
    if (Serial.available()) {
        sim800.write(Serial.read());
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
    ssid[15] = '\0'; // Завершающий нулевой символ
    for (int i = 0; i < 15; i++) {
        password[i] = EEPROM.read(EEPROM_PASSWORD + i);
    }
    password[15] = '\0'; // Завершающий нулевой символ

    Serial.println("Загружено из EEPROM: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password));
}

void saveConfigToEEPROM() {
    EEPROM.write(EEPROM_DEVICE_ID, device_id);
    for (int i = 0; i < 15; i++) {
        EEPROM.write(EEPROM_SSID + i, ssid[i]);
    }
    for (int i = 0; i < 15; i++) {
        EEPROM.write(EEPROM_PASSWORD + i, password[i]);
    }
    EEPROM.commit();

    Serial.println("Настройки сохранены в EEPROM: DEVICE_ID=" + String(device_id) + "; SSID=" + String(ssid) + "; PASSWORD=" + String(password));
}
