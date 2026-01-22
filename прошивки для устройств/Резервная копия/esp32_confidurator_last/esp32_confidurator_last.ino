TaskHandle_t Task1;
#include "esp_task_wdt.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <EEPROM.h>  // Для работы с EEPROM

const int RXPin = 16;  // Пин RX MAX485
const int TXPin = 17;  // Пин TX MAX485
const int DEPin = 4;   // Пин управления направлением передачи

const int ledPin = 2; // Пин для светодиода (GPIO4)

int scanTime = 1; // Время сканирования в секундах

const int stroki = 29;  // Максимальное количество MAC-адресов
const int LEN_MAC = 6;
uint8_t knownMacs[stroki][LEN_MAC];

const int EEPROM_SIZE = stroki * LEN_MAC + 13 * sizeof(int);

#define NUM_POSITIONS 6 // количество позиций для MAC-адресов

int sdvig[NUM_POSITIONS];  // Массив сдвигов для каждой позиции

// Переменные для конфигурации
int updateInterval = 60; // Период обновления (по умолчанию 60)
int bestRssiTime = 30;   // Время удержания метки (по умолчанию 30)
int bestRssiCount = 3;   // Количество повторений (по умолчанию 3)
int8_t bestRssiThreshold = -90; // Уровень сигнала (по умолчанию -90)
int debugMode = 0;  // Флаг для включения/отключения отладки 485 (по умолчанию выключено!)
int modee = 0; // Переменная для хранения режима работы (0 - обнаружение меток, 1 - ретранслятор)

uint16_t combinedValues[stroki] = {0};  // Инициализируем с нулями
unsigned long lastUpdateTimeDut[stroki]; // массив для хранения времени последнего обновления для каждого knownMac

BLEScan* pBLEScan;
BLEScanResults foundDevices;
int networkAddress = 4; // Сетевой адрес (по умолчанию 4)
int bestRssi = -150;
uint8_t bestMac[LEN_MAC];
unsigned long lastUpdate = 0;
unsigned long bestMacSec = 0;
unsigned long lastUpdatenull = 0;
int bestimenull = 0;
int iter = 0;
int num = 0;
bool firstTimeTagLost = false;
bool macMatch = true;
int rssiCounters[stroki] = {0}; // Счётчики для каждого известного MAC
unsigned long lastRequestTime = 0;
const unsigned long requestTimeout = 1000; // Тайм-аут в миллисекундах для запроса

// Семафор для синхронизации
SemaphoreHandle_t xSemaphore = NULL;

void onAdvertisedDevice(BLEAdvertisedDevice advertisedDevice) {
  if (modee == 1) {
    std::string currentMac = advertisedDevice.getAddress().toString();
    
    for (int j = 0; j < NUM_POSITIONS; j++) {  // Только первые 6 позиций для ДУТ
      if (memcmp(advertisedDevice.getAddress().getNative(), knownMacs[j], LEN_MAC) == 0) {
        
        if (debugMode) {
          Serial.printf("Найдено устройство с MAC-адресом %s (позиция %d)\n", currentMac.c_str(), j+1);
        }

        uint8_t* advData = advertisedDevice.getPayload();
        size_t packetLength = advertisedDevice.getPayloadLength();

        int sdvigValue = sdvig[j];

        if (sdvigValue + 1 < packetLength) {  // Проверяем, что есть достаточно данных
          uint8_t byteFromAdv = advData[sdvigValue];
          uint8_t byteFromAdv2 = advData[sdvigValue + 1];
          uint16_t combinedValue = (static_cast<uint16_t>(byteFromAdv2) << 8) | byteFromAdv;

          // Безопасный доступ к общим данным
          if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
            combinedValues[j] = combinedValue;
            lastUpdateTimeDut[j] = millis();
            xSemaphoreGive(xSemaphore);
          }

          if (debugMode) {
            Serial.printf("Значение = %d (0x%04X) для MAC %s\n", combinedValue, combinedValue, currentMac.c_str());
          }
        } else if (debugMode) {
          Serial.printf("Ошибка: рекламный пакет устройства с MAC %s слишком короткий\n", currentMac.c_str());
        }
        break; // Нашли устройство, выходим из цикла
      }
    }
  }
}

// Реализация класса с обязательной реализацией метода onResult
class SimpleAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    onAdvertisedDevice(advertisedDevice);
  }
};

void setup() {
  Serial.begin(9600);
  Serial2.begin(19200, SERIAL_8N1, RXPin, TXPin); // Инициализация RS485
  pinMode(DEPin, OUTPUT);
  digitalWrite(DEPin, LOW);
  delay(100); // Ожидание после подачи питания
  pinMode(ledPin, OUTPUT); // Устанавливаем пин GPIO4 как выход
  digitalWrite(ledPin, HIGH); // Включаем светодиод при запуске
  delay(2000);
  digitalWrite(ledPin, LOW); // Выключаем светодиод при запуске

  // Создаем семафор для синхронизации
  xSemaphore = xSemaphoreCreateMutex();

  // Настройка Watchdog Timer с таймаутом 20 секунд
  esp_task_wdt_init(20, true);
  esp_task_wdt_add(NULL);

  // Создаем задачу, которая будет выполняться на ядре 0
  xTaskCreatePinnedToCore(
    Task1code,
    "Task1",
    10000,
    NULL,
    1,
    &Task1,
    0);
  delay(500);

  Serial.println("BLE BASA v1.0");
  Serial.println("Инициализация...");

  // Установка значений по умолчанию перед загрузкой из EEPROM
  updateInterval = 60;
  bestRssiTime = 30;
  bestRssiCount = 3;
  bestRssiThreshold = -90;
  networkAddress = 4;
  modee = 0;
  debugMode = 0; // Диагностика по умолчанию выключена!

  EEPROM.begin(EEPROM_SIZE);
  loadKnownMacsFromEEPROM();
  loadConfigFromEEPROM();
  
  // Выводим информацию о режиме
  Serial.printf("Режим работы: %s\n", modee == 0 ? "Метки" : "ДУТ");
  Serial.printf("Сетевой адрес: %d\n", networkAddress);
  Serial.printf("Диагностика: %s\n", debugMode ? "ВКЛ" : "ВЫКЛ");

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new SimpleAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  // Инициализация массивов времени
  for (int i = 0; i < stroki; i++) {
    lastUpdateTimeDut[i] = 0;
  }
}

void dut_translator() {
  pBLEScan->start(2, false); // Короткое сканирование для ДУТ режима
}

void searchTags() {
  foundDevices = pBLEScan->start(scanTime, false);

  for (int i = 0; i < foundDevices.getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices.getDevice(i);
    int currentRssi = device.getRSSI();
    std::string currentMac = device.getAddress().toString();
    iter++;

    if (currentRssi >= bestRssiThreshold) {
      for (int j = 0; j < stroki; j++) {
        if (memcmp(device.getAddress().getNative(), knownMacs[j], LEN_MAC) == 0) {
          if (debugMode) {
            Serial.printf("Известное устройство: %s, RSSI = %d\n", currentMac.c_str(), currentRssi);
          }
          firstTimeTagLost = false;
          updateRssiCounter(j, currentRssi);
          break;
        }
      }
    }
  }

  // Обработка удержания меток
  if (millis() - bestMacSec >= bestRssiTime * 1000) {
    for (int i = 0; i < stroki; i++) {
      macMatch = true;
      for (int j = 0; j < LEN_MAC; j++) {
        if (bestMac[j] != knownMacs[i][j]) {
          macMatch = false;
          break;
        }
      }
      if (macMatch) {
        num = i + 1001;
        if (debugMode) {
          Serial.printf("Прошло %d сек, лучшая метка: %d\n", bestRssiTime, num);
        }
        break;
      }
    }
  }

  for (int j = 0; j < LEN_MAC; j++) {
    if (bestMac[j] == 0x00) {
      if (!firstTimeTagLost) {
        firstTimeTagLost = true;
        bestimenull = millis();
      }
    }
  }

  if (millis() - lastUpdate >= updateInterval * 1000) {
    if (debugMode) {
      Serial.println("Сброс метки по таймауту");
    }
    bestRssi = -100;
    if (bestMac[0] == 0x00 && iter > 500 && millis() - bestimenull > 60) {
      iter = 0;
      num = 0;
    }
    memset(bestMac, 0, LEN_MAC);
    lastUpdate = millis();
  }

  if (debugMode && millis() % 5000 < 100) { // Вывод раз в 5 секунд
    Serial.printf("Текущая метка: %d\n", num);
  }
}

void Task1code(void * pvParameters) {
  Serial.print("Задача RS485 работает на ядре ");
  Serial.println(xPortGetCoreID());

  for (;;) {
    // Режим приема данных
    digitalWrite(DEPin, LOW);

    // Обрабатываем запросы от RS485
    if (Serial2.available()) {
      receiveRequest();
    }

    // Сброс watchdog
    esp_task_wdt_reset();

    // Короткая задержка
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void loop() {
  unsigned long currentTime = millis();
  
  // Проверяем устаревание данных ДУТ
  if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
    for (int j = 0; j < NUM_POSITIONS; j++) {
      if (currentTime - lastUpdateTimeDut[j] > 60000) { // 60 секунд
        combinedValues[j] = 0;
      }
    }
    xSemaphoreGive(xSemaphore);
  }

  // Проверяем запросы от конфигуратора
  if (Serial.available()) {
    receiveConfigRequest();
  }

  // Выполняем основную функцию в зависимости от режима
  if (modee == 0) {
    searchTags();
    digitalWrite(ledPin, num > 0 ? HIGH : LOW);
  } else if (modee == 1) {
    dut_translator();
  }

  // Сброс watchdog
  esp_task_wdt_reset();
}

void receiveConfigRequest() {
  String requestStr = Serial.readStringUntil('\n');
  requestStr.trim();

  if (requestStr.startsWith("newmac ")) {
    String macStr = requestStr.substring(7);
    addMacToEEPROM(macStr);
  } else if (requestStr == "deleteall") {
    deleteAllMacs();
  } else if (requestStr.startsWith("newdut ")) {
    int firstSpace = requestStr.indexOf(' ', 7);
    int secondSpace = requestStr.indexOf(' ', firstSpace + 1);
    
    if (firstSpace == -1 || secondSpace == -1) {
      Serial.println("Ошибка: неправильный формат команды newdut");
      return;
    }

    String macStr = requestStr.substring(7, firstSpace);
    int position = requestStr.substring(firstSpace + 1, secondSpace).toInt();
    int byteNumber = requestStr.substring(secondSpace + 1).toInt();
    
    WriteMacToEEPROM(macStr, position, byteNumber);
  } else if (requestStr.startsWith("deleteat ")) {
    int index = requestStr.substring(9).toInt();
    deleteMacAtIndex(index);
  } else if (requestStr == "getmacs") {
    sendKnownMacs();
  } else if (requestStr.startsWith("setnetaddr ")) {
    networkAddress = requestStr.substring(11).toInt();
    saveConfigToEEPROM();
    Serial.printf("Сетевой адрес установлен на %d\n", networkAddress);
  } else if (requestStr.startsWith("setupdate ")) {
    updateInterval = requestStr.substring(10).toInt();
    saveConfigToEEPROM();
    Serial.printf("Период обновления установлен на %d\n", updateInterval);
  } else if (requestStr.startsWith("setbtime ")) {
    bestRssiTime = requestStr.substring(9).toInt();
    saveConfigToEEPROM();
    Serial.printf("Время удержания метки установлено на %d\n", bestRssiTime);
  } else if (requestStr.startsWith("setbcount ")) {
    bestRssiCount = requestStr.substring(10).toInt();
    saveConfigToEEPROM();
    Serial.printf("Количество повторений установлено на %d\n", bestRssiCount);
  } else if (requestStr.startsWith("setbthreshold ")) {
    bestRssiThreshold = requestStr.substring(14).toInt();
    saveConfigToEEPROM();
    Serial.printf("Уровень сигнала установлен на %d\n", bestRssiThreshold);
  } else if (requestStr.startsWith("setmode ")) {
    modee = requestStr.substring(8).toInt();
    saveConfigToEEPROM();
    Serial.printf("Режим работы установлен на %d\n", modee);
  } else if (requestStr.startsWith("debugMode ")) {
    debugMode = requestStr.substring(10).toInt();
    Serial.printf("Диагностика %s\n", debugMode ? "включена" : "выключена");
  } else if (requestStr == "getsettings") {
    String response = "Сетевой адрес: " + String(networkAddress) + "\n" +
                      "Режим работы: " + String(modee) + "\n" +
                      "Период обновления: " + String(updateInterval) + "\n" +
                      "Время удержания метки: " + String(bestRssiTime) + "\n" +
                      "Количество повторений: " + String(bestRssiCount) + "\n" +
                      "Уровень сигнала: " + String(bestRssiThreshold) + "\n" +
                      "Диагностика: " + String(debugMode ? "ВКЛ" : "ВЫКЛ") + "\n";
    Serial.print(response);
  } else if (requestStr == "status") {
    Serial.printf("Режим: %s, Адрес: %d, Метка: %d\n", 
                  modee == 0 ? "Метки" : "ДУТ", networkAddress, num);
  } else if (requestStr.length() > 0) {
    Serial.println("Некорректный запрос");
  }
}

void WriteMacToEEPROM(const String& macStr, int position, int byteNumber) {
  uint8_t mac[LEN_MAC];

  if (!parseMAC(macStr, mac)) {
    Serial.println("Ошибка: неправильный формат MAC-адреса.");
    return;
  }

  if (position < 1 || position > stroki) {
    Serial.println("Ошибка: некорректная позиция.");
    return;
  }

  // Сохраняем MAC-адрес
  int eepromAddr = (position - 1) * LEN_MAC;
  memcpy(knownMacs[position - 1], mac, LEN_MAC);
  
  for (int i = 0; i < LEN_MAC; i++) {
    EEPROM.write(eepromAddr + i, mac[i]);
  }

  // Сохраняем сдвиг для ДУТ (только для первых 6 позиций)
  if (position <= NUM_POSITIONS) {
    sdvig[position - 1] = byteNumber;
    
    // Сохраняем сдвиги в EEPROM
    int startAddress = stroki * LEN_MAC + 7 * sizeof(int);
    for (int i = 0; i < NUM_POSITIONS; i++) {
      int currentAddress = startAddress + i * sizeof(int);
      EEPROM.put(currentAddress, sdvig[i]);
    }
  }

  EEPROM.commit();
  Serial.printf("MAC добавлен в позицию %d, сдвиг %d\n", position, byteNumber);
}

void addMacToEEPROM(const String& macStr) {
  uint8_t mac[LEN_MAC];
  if (!parseMAC(macStr, mac)) {
    Serial.println("Ошибка: неправильный формат MAC-адреса.");
    return;
  }

  // Проверка на существование
  for (int i = 0; i < stroki; i++) {
    if (memcmp(knownMacs[i], mac, LEN_MAC) == 0) {
      Serial.println("Этот MAC-адрес уже существует.");
      return;
    }
  }

  // Поиск свободного слота
  int freeIndex = -1;
  for (int i = 0; i < stroki; i++) {
    bool emptySlot = true;
    for (int j = 0; j < LEN_MAC; j++) {
      if (knownMacs[i][j] != 0xFF) {
        emptySlot = false;
        break;
      }
    }
    if (emptySlot) {
      freeIndex = i;
      break;
    }
  }

  if (freeIndex == -1) {
    Serial.println("Нет свободного места.");
    return;
  }

  // Сохранение
  memcpy(knownMacs[freeIndex], mac, LEN_MAC);
  int eepromAddr = freeIndex * LEN_MAC;
  for (int i = 0; i < LEN_MAC; i++) {
    EEPROM.write(eepromAddr + i, mac[i]);
  }
  EEPROM.commit();
  Serial.printf("MAC добавлен в позицию %d\n", freeIndex + 1);
}

void deleteAllMacs() {
  for (int i = 0; i < stroki; i++) {
    memset(knownMacs[i], 0xFF, LEN_MAC);
    int eepromAddr = i * LEN_MAC;
    for (int j = 0; j < LEN_MAC; j++) {
      EEPROM.write(eepromAddr + j, 0xFF);
    }
  }
  EEPROM.commit();
  Serial.println("Все MAC-адреса удалены.");
}

void deleteMacAtIndex(int index) {
  index = index - 1;

  if (index < 0 || index >= stroki) {
    Serial.println("Ошибка: индекс вне диапазона.");
    return;
  }

  memset(knownMacs[index], 0xFF, LEN_MAC);
  int eepromAddr = index * LEN_MAC;
  for (int j = 0; j < LEN_MAC; j++) {
    EEPROM.write(eepromAddr + j, 0xFF);
  }
  EEPROM.commit();
  Serial.printf("MAC в позиции %d удален.\n", index + 1);
}

void sendKnownMacs() {
  Serial.println("Список сохраненных MAC-адресов:");
  for (int i = 0; i < stroki; i++) {
    bool emptySlot = true;
    for (int j = 0; j < LEN_MAC; j++) {
      if (knownMacs[i][j] != 0xFF) {
        emptySlot = false;
        break;
      }
    }

    if (!emptySlot) {
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
              knownMacs[i][0], knownMacs[i][1], knownMacs[i][2],
              knownMacs[i][3], knownMacs[i][4], knownMacs[i][5]);
      if (i < NUM_POSITIONS) {
        Serial.printf("%d: %s (сдвиг: %d)\n", i + 1, macStr, sdvig[i]);
      } else {
        Serial.printf("%d: %s\n", i + 1, macStr);
      }
    }
  }
}

void updateRssiCounter(int knownIndex, int currentRssi) {
  rssiCounters[knownIndex]++;
  if (currentRssi > bestRssi) {
    bestRssi = currentRssi;
    memcpy(bestMac, knownMacs[knownIndex], LEN_MAC);
    bestMacSec = millis();
  }
}

bool parseMAC(const String& macStr, uint8_t* mac) {
  int values[LEN_MAC];
  if (sscanf(macStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
             &values[0], &values[1], &values[2],
             &values[3], &values[4], &values[5]) == 6) {
    for (int i = 0; i < LEN_MAC; ++i) {
      mac[i] = (uint8_t)values[i];
    }
    return true;
  } else {
    return false;
  }
}

void loadKnownMacsFromEEPROM() {
  for (int i = 0; i < stroki; i++) {
    int eepromAddr = i * LEN_MAC;
    for (int j = 0; j < LEN_MAC; j++) {
      knownMacs[i][j] = EEPROM.read(eepromAddr + j);
    }
  }
}

void loadConfigFromEEPROM() {
  int eepromBase = stroki * LEN_MAC;
  updateInterval = EEPROM.read(eepromBase);
  bestRssiTime = EEPROM.read(eepromBase + sizeof(int));
  bestRssiCount = EEPROM.read(eepromBase + 2 * sizeof(int));
  networkAddress = EEPROM.read(eepromBase + 3 * sizeof(int));
  EEPROM.get(eepromBase + 4 * sizeof(int), bestRssiThreshold);
  EEPROM.get(eepromBase + 6 * sizeof(int), modee);

  // Проверка корректности
  if (updateInterval <= 0 || updateInterval > 3600) updateInterval = 60;
  if (bestRssiTime <= 0 || bestRssiTime > 300) bestRssiTime = 30;
  if (bestRssiCount <= 0 || bestRssiCount > 10) bestRssiCount = 3;
  if (bestRssiThreshold > 0 || bestRssiThreshold < -120) bestRssiThreshold = -90;
  if (networkAddress <= 0 || networkAddress > 255) networkAddress = 4;
  if (modee < 0 || modee > 1) modee = 0;

  // Загрузка сдвигов
  int startAddress = stroki * LEN_MAC + 7 * sizeof(int);
  for (int i = 0; i < NUM_POSITIONS; i++) {
    int currentAddress = startAddress + i * sizeof(int);
    EEPROM.get(currentAddress, sdvig[i]);
    if (sdvig[i] < 0 || sdvig[i] > 255) sdvig[i] = 0;
  }
}

void saveConfigToEEPROM() {
  EEPROM.write(stroki * LEN_MAC, updateInterval);
  EEPROM.write(stroki * LEN_MAC + sizeof(int), bestRssiTime);
  EEPROM.write(stroki * LEN_MAC + 2 * sizeof(int), bestRssiCount);
  EEPROM.write(stroki * LEN_MAC + 3 * sizeof(int), networkAddress);
  EEPROM.put(stroki * LEN_MAC + 4 * sizeof(int), bestRssiThreshold);
  EEPROM.put(stroki * LEN_MAC + 6 * sizeof(int), modee);
  EEPROM.commit();
}

bool isMacConfigured(int index) {
  // Проверяем, есть ли MAC-адрес для данной позиции
  // Индекс от 0 до 5 (для позиций 1-6)
  if (index < 0 || index >= NUM_POSITIONS) {
    return false;
  }
  
  // Проверяем, не пустой ли слот (0xFF - пустой слот)
  for (int j = 0; j < LEN_MAC; j++) {
    if (knownMacs[index][j] != 0xFF) {
      return true;  // MAC-адрес настроен
    }
  }
  return false;  // Слот пустой
}

void receiveRequest() {
  byte request[4];
  int index = 0;
  
  // Читаем первый байт
  if (Serial2.available()) {
    request[0] = Serial2.read();
    
    // Проверяем префикс запроса
    if (request[0] == 0x31) {
      // Ждем остальные 3 байта
      unsigned long startTime = millis();
      while (index < 3 && millis() - startTime < 50) {
        if (Serial2.available()) {
          request[++index] = Serial2.read();
        }
      }
      
      if (index == 3) {
        int requestedAddr = request[1];
        
        if (debugMode) {
          Serial.printf("RS485 запрос: адрес %d\n", requestedAddr);
        }
        
        if (modee == 0) {
          // Режим меток - отвечаем текущим номером метки только если запрошен наш сетевой адрес
          if (requestedAddr == networkAddress) {
            sendResponse(num);
          } else if (debugMode) {
            Serial.printf("Игнорируем запрос для адреса %d (наш адрес %d)\n", requestedAddr, networkAddress);
          }
        } else if (modee == 1) {
          // Режим ДУТ - отвечаем только если есть настроенный MAC для этого адреса
          if (requestedAddr >= 1 && requestedAddr <= NUM_POSITIONS) {
            int valueIndex = requestedAddr - 1;
            
            // Проверяем, есть ли MAC-адрес для этой позиции
            if (isMacConfigured(valueIndex)) {
              uint16_t valueToSend = 0;
              
              if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
                valueToSend = combinedValues[valueIndex];
                xSemaphoreGive(xSemaphore);
              }
              
              // Сохраняем текущий адрес, устанавливаем запрошенный для ответа
              int originalAddr = networkAddress;
              networkAddress = requestedAddr;
              sendResponse(valueToSend);
              networkAddress = originalAddr;
              
              if (debugMode) {
                Serial.printf("Ответ для адреса %d: значение %d\n", requestedAddr, valueToSend);
              }
            } else {
              // MAC не настроен для этого адреса - НЕ ОТВЕЧАЕМ
              if (debugMode) {
                Serial.printf("MAC не настроен для адреса %d, игнорируем запрос\n", requestedAddr);
              }
            }
          } else {
            if (debugMode) {
              Serial.printf("Некорректный адрес запроса: %d\n", requestedAddr);
            }
          }
        }
      }
    }
  }
}

void sendResponse(uint16_t numValue) {
  byte numLowByte = numValue & 0xFF;
  byte numHighByte = (numValue >> 8) & 0xFF;

  byte response[] = {
    0x3E,           // Префикс
    (byte)networkAddress, // Сетевой адрес
    0x06,           // Код операции
    20,             // Температура
    numLowByte,     // Младший байт
    numHighByte,    // Старший байт
    0x08, 0x02,     // Значение частоты
    0x00            // Контрольная сумма (пока 0)
  };

  // Рассчитываем контрольную сумму
  response[8] = calculateChecksum(response, sizeof(response) - 1);

  // Переключаем на передачу
  digitalWrite(DEPin, HIGH);
  delayMicroseconds(100);

  // Отправляем ТОЛЬКО по RS485 (Serial2)
  Serial2.write(response, sizeof(response));
  Serial2.flush();

  // Переключаем обратно на прием
  delayMicroseconds(100);
  digitalWrite(DEPin, LOW);

  // ТОЛЬКО текстовое сообщение в Serial (без бинарных данных!)
  if (debugMode) {
    Serial.printf("RS485 ответ: адрес=%d, значение=%d\n", networkAddress, numValue);
  }
}

byte calculateChecksum(byte data[], int length) {
  byte crc = 0;

  for (int i = 0; i < length; i++) {
    byte byteToCheck = data[i];
    crc ^= byteToCheck;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x01) {
        crc = (crc >> 1) ^ 0x8C;
      } else {
        crc >>= 1;
      }
    }
  }

  return crc;
}
