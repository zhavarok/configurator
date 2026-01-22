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

const int stroki = 29;  // Увеличение максимального количества MAC-адресов до 20
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
int debugMode = 1;  // Флаг для включения/отключения отладки 485
int modee = 0; // Переменная для хранения режима работы (0 - обнаружение меток, 1 - ретранслятор) (по умолчанию 0)

// Глобальные переменные для синхронизации между ядрами
volatile uint16_t combinedValues[stroki] = {0};  // Изменено на volatile
volatile unsigned long lastUpdateTimeDut[stroki]; // volatile для корректного доступа из разных ядер

const int timeout = 2000;
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
int previousNum = 0;
int newNum2 = 0;
int rssiCounters[stroki] = {0}; // Счётчики для каждого известного MAC
unsigned long lastRequestTime = 0;
const unsigned long requestTimeout = 1000; // Тайм-аут в миллисекундах для запроса

// Семафор для синхронизации доступа к combinedValues
SemaphoreHandle_t xSemaphore = NULL;

void onAdvertisedDevice(BLEAdvertisedDevice advertisedDevice) {
  if (modee == 1) {
    std::string currentMac = advertisedDevice.getAddress().toString();
    bool isKnownDevice = false;
    int deviceIndex = -1;

    for (int j = 0; j < stroki; j++) {
      if (memcmp(advertisedDevice.getAddress().getNative(), knownMacs[j], 6) == 0) {
        isKnownDevice = true;
        deviceIndex = j;
        break;
      }
    }

    if (isKnownDevice && deviceIndex >= 0 && deviceIndex < NUM_POSITIONS) {
      if (debugMode) {
        Serial.printf("Найдено устройство с MAC-адресом %s\n", currentMac.c_str());
      }

      uint8_t* advData = advertisedDevice.getPayload();
      size_t packetLength = advertisedDevice.getPayloadLength();

      if (debugMode > 1) {
        Serial.print("Рекламный пакет: ");
        for (size_t i = 0; i < packetLength; i++) {
          Serial.printf("%02X ", advData[i]);
        }
        Serial.println();
      }

      int sdvigValue = sdvig[deviceIndex];

      if (sdvigValue + 1 < packetLength) {  // Проверяем, что есть достаточно данных
        uint8_t byteFromAdv = advData[sdvigValue];
        uint8_t byteFromAdv2 = advData[sdvigValue + 1];
        uint16_t combinedValue = (static_cast<uint16_t>(byteFromAdv2) << 8) | byteFromAdv;

        // Захватываем семафор для безопасного доступа к общим данным
        if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
          combinedValues[deviceIndex] = combinedValue;
          lastUpdateTimeDut[deviceIndex] = millis();
          xSemaphoreGive(xSemaphore);
        }

        if (debugMode) {
          Serial.printf("combinedValue = %d (0x%04X) для MAC %s, позиция %d\n", 
                       combinedValue, combinedValue, currentMac.c_str(), deviceIndex + 1);
        }
      } else {
        Serial.printf("Ошибка: рекламный пакет устройства с MAC %s слишком короткий для смещения %d\n", 
                     currentMac.c_str(), sdvigValue);
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
  esp_task_wdt_init(20, true); // Таймаут 20 секунд, автоматический сброс
  esp_task_wdt_add(NULL); // Добавляем текущую задачу в WDT

  // Создаем задачу, которая будет выполняться на ядре 0 с максимальным приоритетом (1)
  xTaskCreatePinnedToCore(
    Task1code,   /* Функция задачи. */
    "Task1",     /* Ее имя. */
    10000,       /* Размер стека функции */
    NULL,        /* Параметры */
    1,           /* Приоритет */
    &Task1,      /* Дескриптор задачи для отслеживания */
    0);          /* Указываем пин для данного ядра */
  delay(500);

  Serial.println("Scanning...");

  // Установка значений по умолчанию перед загрузкой из EEPROM
  updateInterval = 60;  // Период обновления
  bestRssiTime = 30;   // Время удержания метки
  bestRssiCount = 3;   // Количество повторений
  bestRssiThreshold = -90; // Уровень сигнала
  networkAddress = 4;  // Сетевой адрес
  modee = 0;           // Режим работы (метки)

  EEPROM.begin(EEPROM_SIZE); // Инициализация EEPROM
  loadKnownMacsFromEEPROM(); // Загрузка MAC-адресов из EEPROM
  loadConfigFromEEPROM(); // Загрузка конфигурации из EEPROM
  // Отправка списка MAC-адресов
  sendKnownMacs();

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  // Устанавливаем созданный класс с реализованным onResult
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
  pBLEScan->start(5, false); // Уменьшено время сканирования для более частых обновлений
  esp_task_wdt_reset();
}

// Функция для поиска и обработки меток
void searchTags() {
  // Сканирование BLE-устройств
  foundDevices = pBLEScan->start(scanTime, false);

  for (int i = 0; i < foundDevices.getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices.getDevice(i);
    int currentRssi = device.getRSSI();
    std::string currentMac = device.getAddress().toString();
    iter++;

    if (currentRssi >= bestRssiThreshold) {
      bool isKnown = false;
      int knownIndex = -1;
      for (int j = 0; j < stroki; j++) {
        if (memcmp(device.getAddress().getNative(), knownMacs[j], LEN_MAC) == 0) {
          isKnown = true;
          knownIndex = j;
          break;
        }
      }

      if (isKnown) {
        if (debugMode) {
          Serial.printf("Найдено известное устройство MAC: %s\n", currentMac.c_str());
          Serial.printf("RSSI = %d\n", currentRssi);
        }
        firstTimeTagLost = false;

        updateRssiCounter(knownIndex, currentRssi);
      }
    }
  }

  // Обработка удержания меток
  if (millis() - bestMacSec >= bestRssiTime * 1000) { // Если прошло время удержания метки
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
          Serial.printf("Прошло %d сек как нашли лучший номер %d\n", bestRssiTime, num);
        }
        break;
      }
    }
  }

  for (int j = 0; j < LEN_MAC; j++) {
    if (bestMac[j] == 0x00) {
      if (!firstTimeTagLost) { // Проверяем, была ли метка ранее обнаружена
        firstTimeTagLost = true;
        bestimenull = millis(); // Запоминаем время только при первой потере метки
      }
    }
  }

  if (millis() - lastUpdate >= updateInterval * 1000) {
    if (debugMode) {
      Serial.println("Псевдо сброс метки");
    }
    bestRssi = -100;
    if (bestMac[0] == 0x00 && iter > 500 && millis() - bestimenull > 60) {
      iter = 0;
      num = 0;
    }
    memset(bestMac, 0, LEN_MAC);
    lastUpdate = millis();
  }

  if (debugMode) {
    Serial.printf("Метка = %d\n", num);  // Вывод значения num в той же строке
  }
  esp_task_wdt_reset();
}

void Task1code(void * pvParameters) {
  Serial.print("Task1 running on core ");
  Serial.println(xPortGetCoreID());

  for (;;) {
    // Переключаем на прием данных
    digitalWrite(DEPin, LOW);

    // Обрабатываем запросы от 485
    if (Serial2.available()) {
      receiveRequest();
    }

    // Сбрасываем сторожевой таймер
    esp_task_wdt_reset();

    // Небольшая задержка
    vTaskDelay(1 / portTICK_PERIOD_MS);  // Уменьшена задержка для более быстрого ответа
  }
}

void loop() {
  unsigned long currentTime = millis();
  
  // Проверяем, не устарели ли данные BLE
  if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
    for (int j = 0; j < NUM_POSITIONS; j++) {  // Только для первых 6 позиций
      if (currentTime - lastUpdateTimeDut[j] > 30000) { // 30000 мс = 30 секунд
        combinedValues[j] = 0; // обнуляем значение, если прошло более 30 секунд
      }
    }
    xSemaphoreGive(xSemaphore);
  }

  // Проверяем запросы от конфигуратора через Serial
  if (Serial.available()) {
    receiveConfigRequest();
    esp_task_wdt_reset(); // Сброс WDT при обработке конфигурации
  }

  // Проверяем режим и вызываем соответствующую функцию
  if (modee == 0) {
    searchTags();  // Обычный режим поиска меток
    if (num > 0) {
      digitalWrite(ledPin, HIGH);
    } else {
      digitalWrite(ledPin, LOW);
    }
  } else if (modee == 1) {
    dut_translator();  // Режим dut_translator
  }

  esp_task_wdt_reset(); // Сброс WDT в основном цикле
}

void receiveConfigRequest() {
  String requestStr = Serial.readStringUntil('\n'); // Чтение строки запроса до новой строки

  if (requestStr.startsWith("newmac")) {
    String macStr = requestStr.substring(7); // Извлечение строки MAC-адреса
    addMacToEEPROM(macStr);
  } else if (requestStr.startsWith("deleteall")) {
    deleteAllMacs();
  } else if (requestStr.startsWith("newdut")) {
    // Разделяем строку на части по пробелам
    int startIndex = 7; // Пропускаем "newdut"
    int spaceIndex1 = 24;
    int spaceIndex2 = requestStr.indexOf(' ', spaceIndex1 + 1);

    // Проверяем, найдены ли пробелы
    if (spaceIndex1 == -1 || spaceIndex2 == -1) {
      Serial.println("Ошибка: неправильный формат команды.");
      return;
    }

    String macStr = requestStr.substring(startIndex, spaceIndex1);
    Serial.println(requestStr);  // Вывести всю строку для отладки

    Serial.printf("Извлеченный MAC-адрес: %s\n", macStr.c_str());

    int position = requestStr.substring(spaceIndex1 + 1, spaceIndex2).toInt();  // Позиция для записи
    int byteNumber = requestStr.substring(spaceIndex2 + 1).toInt();  // Номер байта
    WriteMacToEEPROM(macStr, position, byteNumber);
  } else if (requestStr.startsWith("deleteat")) {
    String indexStr = requestStr.substring(8); // Извлечение индекса для удаления
    int index = indexStr.toInt();
    deleteMacAtIndex(index);
  } else if (requestStr.startsWith("getmacs")) {  // Команда для получения списка MAC-адресов
    sendKnownMacs();
  } else if (requestStr.startsWith("setnetaddr")) {  // Команда для установки сетевого адреса
    String addrStr = requestStr.substring(11); // Извлечение значения сетевого адреса
    networkAddress = addrStr.toInt();
    saveConfigToEEPROM(); // Сохранение в EEPROM
    Serial.printf("Сетевой адрес установлен на %d\n", networkAddress);
  } else if (requestStr.startsWith("setupdate")) {  // Установка периода обновления
    String intervalStr = requestStr.substring(10); // Извлечение значения
    updateInterval = intervalStr.toInt();
    saveConfigToEEPROM(); // Сохранение в EEPROM
    Serial.printf("Период обновления установлен на %d\n", updateInterval);
  } else if (requestStr.startsWith("setbtime")) {  // Установка времени удержания метки
    String timeStr = requestStr.substring(9); // Извлечение значения
    bestRssiTime = timeStr.toInt();
    saveConfigToEEPROM(); // Сохранение в EEPROM
    Serial.printf("Время удержания метки установлено на %d\n", bestRssiTime);
  } else if (requestStr.startsWith("setbcount")) {  // Установка количества повторений
    String countStr = requestStr.substring(10); // Извлечение значения
    bestRssiCount = countStr.toInt();
    saveConfigToEEPROM(); // Сохранение в EEPROM
    Serial.printf("Количество повторений установлено на %d\n", bestRssiCount);
  } else if (requestStr.startsWith("setbthreshold")) {  // Установка уровня сигнала
    String thresholdStr = requestStr.substring(14); // Извлечение значения
    bestRssiThreshold = thresholdStr.toInt();
    saveConfigToEEPROM(); // Сохранение в EEPROM
  } else if (requestStr.startsWith("setmode")) {  // Установка режима
    String modeStr = requestStr.substring(8); // Извлечение значения режима
    modee = modeStr.toInt();
    saveConfigToEEPROM(); // Сохранение в EEPROM
    Serial.printf("Режим работы установлен на %d\n", modee);
  } else if (requestStr.startsWith("debugMode")) {  // включение диагностики 485
    String debugStr = requestStr.substring(10); // Извлечение значения
    debugMode = debugStr.toInt();
    Serial.printf("Диагностика установлена на %d\n", debugMode);
  } else if (requestStr.startsWith("getsettings")) {  // Команда для получения настроек
    String response = "Сетевой адрес: " + String(networkAddress) + "\n" +
                      "Режим работы: " + String(modee) + "\n" +
                      "Период обновления: " + String(updateInterval) + "\n" +
                      "Время удержания метки: " + String(bestRssiTime) + "\n" +
                      "Количество повторений: " + String(bestRssiCount) + "\n" +
                      "Уровень сигнала: " + String(bestRssiThreshold) + "\n";

    // Отправляем ответ через Serial
    Serial.println(response);
  } else {
    Serial.println("Некорректный запрос");
  }
}

void WriteMacToEEPROM(const String& macStr, int position, int byteNumber) {
  uint8_t mac[LEN_MAC];

  // Проверка правильности MAC-адреса
  if (!parseMAC(macStr, mac)) {
    Serial.println("Ошибка: неправильный формат MAC-адреса.");
    return;
  }

  // Проверка корректности переданной позиции
  if (position < 1 || position > stroki) {
    Serial.println("Ошибка: некорректная позиция.");
    return;
  }

  // Сохранение MAC-адреса в массиве и EEPROM
  int eepromAddr = (position - 1) * LEN_MAC;  // Рассчитываем адрес для сохранения
  memcpy(knownMacs[position - 1], mac, LEN_MAC);  // Сохраняем MAC-адрес в массив knownMacs

  // Запись MAC-адреса в EEPROM
  for (int i = 0; i < LEN_MAC; i++) {
    EEPROM.write(eepromAddr + i, mac[i]);
  }

  // Запись дополнительного байта сразу после MAC-адреса
  EEPROM.write(eepromAddr + LEN_MAC, byteNumber);

  // Обновляем переменную сдвига для указанной позиции
  if (position - 1 < NUM_POSITIONS) {
    sdvig[position - 1] = byteNumber;
  }

  // Сохраняем сдвиги в EEPROM (после всех MAC-адресов)
  int startAddress = stroki * LEN_MAC + 7 * sizeof(int);  // Начало свободной области для записи сдвигов
  for (int i = 0; i < NUM_POSITIONS; i++) {
    int currentAddress = startAddress + i * sizeof(int);  // Адрес для текущего сдвига
    EEPROM.put(currentAddress, sdvig[i]);  // Сохраняем сдвиг для каждой позиции
  }

  EEPROM.commit();  // Подтверждаем запись в EEPROM

  Serial.printf("Новый MAC-адрес добавлен в позицию %d с байтом %d.\n", position, byteNumber);
}

void addMacToEEPROM(const String& macStr) {
  uint8_t mac[LEN_MAC];
  if (!parseMAC(macStr, mac)) {
    Serial.println("Ошибка: неправильный формат MAC-адреса.");
    return;
  }

  // Проверка на существование MAC-адреса
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
      if (knownMacs[i][j] != 0xFF) {  // Проверка на "пустое" значение
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
    Serial.println("Нет свободного места для нового MAC-адреса.");
    return;
  }

  // Сохранение MAC-адреса в массиве и EEPROM
  memcpy(knownMacs[freeIndex], mac, LEN_MAC);
  int eepromAddr = freeIndex * LEN_MAC;
  for (int i = 0; i < LEN_MAC; i++) {
    EEPROM.write(eepromAddr + i, mac[i]);
  }
  EEPROM.commit();

  Serial.printf("Новый MAC-адрес добавлен в позицию %d.\n", freeIndex + 1);
}

void deleteAllMacs() {
  // Удаление всех MAC-адресов, не затрагивая другие данные в EEPROM
  for (int i = 0; i < stroki; i++) {
    memset(knownMacs[i], 0xFF, LEN_MAC); // Заполнение значений 0xFF
    int eepromAddr = i * LEN_MAC;
    for (int j = 0; j < LEN_MAC; j++) {
      EEPROM.write(eepromAddr + j, 0xFF); // Установка значений в 0xFF
    }
  }
  EEPROM.commit();
  Serial.println("Все MAC-адреса удалены.");
}

void deleteMacAtIndex(int index) {
  // Корректировка позиции, чтобы соответствовать нумерации массива
  index = index - 1;

  if (index < 0 || index >= stroki) {
    Serial.println("Ошибка: индекс вне диапазона.");
    return;
  }

  // Удаление конкретного MAC-адреса
  memset(knownMacs[index], 0xFF, LEN_MAC);
  int eepromAddr = index * LEN_MAC;
  for (int j = 0; j < LEN_MAC; j++) {
    EEPROM.write(eepromAddr + j, 0xFF); // Установка значений в 0xFF
  }
  EEPROM.commit();

  Serial.printf("MAC-адрес в позиции %d удален.\n", index + 1);
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
      Serial.printf("%d: %s", i + 1, macStr);
      
      // Выводим сдвиг, если это одна из первых 6 позиций
      if (i < NUM_POSITIONS) {
        Serial.printf(" (сдвиг: %d)", sdvig[i]);
      }
      Serial.println();
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
  // Загрузка MAC-адресов из EEPROM в массив
  for (int i = 0; i < stroki; i++) {
    int eepromAddr = i * LEN_MAC;
    for (int j = 0; j < LEN_MAC; j++) {
      knownMacs[i][j] = EEPROM.read(eepromAddr + j);
    }
  }
}

void loadConfigFromEEPROM() {
  // Чтение конфигурации с проверкой на корректность
  int eepromBase = stroki * LEN_MAC;
  updateInterval = EEPROM.read(eepromBase);
  bestRssiTime = EEPROM.read(eepromBase + sizeof(int));
  bestRssiCount = EEPROM.read(eepromBase + 2 * sizeof(int));
  networkAddress = EEPROM.read(eepromBase + 3 * sizeof(int));
  EEPROM.get(eepromBase + 4 * sizeof(int), bestRssiThreshold);
  EEPROM.get(eepromBase + 6 * sizeof(int), modee);

  // Проверка на корректность (если данные некорректны, используем значения по умолчанию)
  if (updateInterval <= 0 || updateInterval > 3600) updateInterval = 60;
  if (bestRssiTime <= 0 || bestRssiTime > 300) bestRssiTime = 30;
  if (bestRssiCount <= 0 || bestRssiCount > 10) bestRssiCount = 3;
  if (bestRssiThreshold > 0 || bestRssiThreshold < -120) bestRssiThreshold = -90;
  if (networkAddress <= 0 || networkAddress > 255) networkAddress = 4;
  if (modee < 0 || modee > 1) modee = 0;

  int startAddress = stroki * LEN_MAC + 7 * sizeof(int);  // Начало области сдвигов
  for (int i = 0; i < NUM_POSITIONS; i++) {
    int currentAddress = startAddress + i * sizeof(int);  // Адрес для текущего сдвига
    EEPROM.get(currentAddress, sdvig[i]);  // Чтение сдвига из EEPROM
    if (sdvig[i] < 0 || sdvig[i] > 255) sdvig[i] = 0; // Значение по умолчанию для сдвига
  }
}

void saveConfigToEEPROM() {
  // Сохранение конфигурации в EEPROM
  EEPROM.write(stroki * LEN_MAC, updateInterval);
  EEPROM.write(stroki * LEN_MAC + sizeof(int), bestRssiTime);
  EEPROM.write(stroki * LEN_MAC + 2 * sizeof(int), bestRssiCount);
  EEPROM.write(stroki * LEN_MAC + 3 * sizeof(int), networkAddress);
  EEPROM.put(stroki * LEN_MAC + 4 * sizeof(int), bestRssiThreshold);
  EEPROM.put(stroki * LEN_MAC + 6 * sizeof(int), modee);
  EEPROM.commit(); // Сохранить
}

void receiveRequest() {
  byte request[4];  // Массив для хранения принятого сообщения
  int index = 0;    // Индекс текущего байта в сообщении
  
  // Читаем первый байт
  if (Serial2.available()) {
    request[0] = Serial2.read();
    
    // Если первый байт соответствует ожидаемому
    if (request[0] == 0x31) {
      // Ждем остальные 3 байта с таймаутом
      unsigned long startTime = millis();
      while (index < 3 && millis() - startTime < 50) {
        if (Serial2.available()) {
          request[++index] = Serial2.read();
        }
      }
      
      // Если получили все 4 байта
      if (index == 3) {
        if (debugMode) {
          Serial.print("Получен запрос: ");
          for (int i = 0; i < 4; i++) {
            Serial.printf("%02X ", request[i]);
          }
          Serial.println();
        }
        
        // Обработка запроса в зависимости от режима
        if (modee == 0) {
          // Режим обнаружения меток
          if (request[0] == 0x31 && request[1] == networkAddress) {
            sendResponse(num);
          } else if (debugMode) {
            Serial.println("Некорректный сетевой адрес в запросе");
          }
        } else if (modee == 1) {
          // Режим ретранслятора - ищем соответствующий сетевой адрес
          uint16_t valueToSend = 0;
          
          // Определяем, какой сетевой адрес запрашивается
          if (request[0] == 0x31) {
            int requestedAddr = request[1];
            int valueIndex = -1;
            
            // Преобразуем сетевой адрес в индекс (1-6)
            if (requestedAddr >= 0x01 && requestedAddr <= 0x06) {
              valueIndex = requestedAddr - 1;
            }
            
            if (valueIndex >= 0 && valueIndex < NUM_POSITIONS) {
              // Безопасно читаем значение
              if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
                valueToSend = combinedValues[valueIndex];
                xSemaphoreGive(xSemaphore);
              }
              
              if (debugMode) {
                Serial.printf("Запрос для адреса %d, значение %d\n", requestedAddr, valueToSend);
              }
              
              // Временно устанавливаем сетевой адрес для ответа
              int originalAddr = networkAddress;
              networkAddress = requestedAddr;
              sendResponse(valueToSend);
              networkAddress = originalAddr; // Восстанавливаем оригинальный адрес
            } else if (debugMode) {
              Serial.printf("Некорректный сетевой адрес: %d\n", requestedAddr);
            }
          }
        }
      } else if (debugMode) {
        Serial.println("Неполный запрос");
      }
    } else if (debugMode) {
      Serial.printf("Некорректный первый байт: %02X\n", request[0]);
    }
  }
}

void sendResponse(uint16_t numValue) {
  byte numLowByte = numValue & 0xFF; // Младший байт
  byte numHighByte = (numValue >> 8) & 0xFF; // Старший байт

  // Формируем ответ
  byte response[] = {
    0x3E,           // Префикс
    networkAddress, // Сетевой адрес отправителя
    0x06,           // Код операции
    20,             // Температура в градусах Цельсия
    numLowByte,     // Младший байт переменной num
    numHighByte,    // Старший байт переменной num
    0x08, 0x02,     // Значение частоты
    0x00            // Контрольная сумма (будет рассчитана)
  };

  // Рассчитываем контрольную сумму
  response[8] = calculateChecksum(response, sizeof(response) - 1);

  // Переключаем на передачу данных
  digitalWrite(DEPin, HIGH);
  delayMicroseconds(100); // Короткая задержка перед передачей

  // Отправляем ответ
  Serial2.write(response, sizeof(response));
  Serial2.flush(); // Ждем завершения передачи

  // Переключаем обратно на прием данных
  delayMicroseconds(100); // Короткая задержка перед переключением
  digitalWrite(DEPin, LOW);

  if (debugMode) {
    Serial.print("Отправлен ответ: ");
    for (int i = 0; i < sizeof(response); i++) {
      Serial.printf("%02X ", response[i]);
    }
    Serial.printf("(значение: %d)\n", numValue);
  }
}

byte calculateChecksum(byte data[], int length) {
  byte crc = 0;

  for (int i = 0; i < length; i++) {
    byte byteToCheck = data[i];
    crc ^= byteToCheck;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x01) {
        crc = (crc >> 1) ^ 0x8C;  // Полином a^8 + a^5 + a^4 + 1
      } else {
        crc >>= 1;
      }
    }
  }

  return crc;
}
