#include <EEPROM.h>

float impuls_na_litr = 7.184;  // 71,84 мл = 0.07184 литра
int dataPin = 6;    // Пин подключен к DS входу 74HC595
int latchPin = 8;   // Пин подключен к ST_CP входу 74HC595
int clockPin = 10;  // Пин подключен к SH_CP входу 74HC595
int galileo = 5;    // Пин к которому подключен output0 galileo
int bypas = 7;      // Пин байпаса
const int inputPin = 2;  // Пин подключения импульсов
const int buttonPin = 3; // Пин подключения кнопки переключения режимов

volatile unsigned long pulseCount = 0;
volatile bool lastButtonState = LOW;
volatile bool buttonState = 0;
volatile uint32_t debounce = 0; // Инициализация debounce
double obshee = 0.0;   // Общий объем жидкости
double lastPulseCount = 0.0;  // Последний объем жидкости перед сбросом
volatile unsigned long lastPulseTime = 0;
unsigned long resetTime = 30000; // 30 секунд для автосброса
unsigned long currentTime = 0;
int mode = -1; // Начальное значение -1 для режима змейки
unsigned long lastButtonPressTime = 0;
const unsigned long modeSwitchDelay = 10000; // 10 секунд
volatile bool ignorePulses = false; // Флаг для игнорирования импульсов после сброса по карточке
const float minLitrageThreshold = 0.5; // Порог для игнорирования маленьких заправок

byte numbers[] = {
  B00000011,  // 0
  B10011111,  // 1
  B00100101,  // 2
  B00001101,  // 3
  B10011001,  // 4
  B01001001,  // 5
  B01000001,  // 6
  B00011111,  // 7
  B00000001,  // 8
  B00001001   // 9
};

void setup() {
  Serial.begin(9600);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(inputPin, INPUT_PULLUP);
  pinMode(galileo, INPUT_PULLUP);
  pinMode(bypas, INPUT_PULLUP);
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);
  
  attachInterrupt(digitalPinToInterrupt(inputPin), countPulse, CHANGE);
  attachInterrupt(digitalPinToInterrupt(buttonPin), button, CHANGE);

  // Чтение из EEPROM
  float eepromValue;
  EEPROM.get(0, eepromValue);
  if (isnan(eepromValue)) eepromValue = 0.0;
  obshee = eepromValue;
  EEPROM.get(4, impuls_na_litr);
  if (isnan(impuls_na_litr) || impuls_na_litr <= 0) impuls_na_litr = 7.184;
  EEPROM.get(8, eepromValue);
  if (isnan(eepromValue)) eepromValue = 0.0;
  lastPulseCount = eepromValue;

  Serial.print("Общий литраж = ");
  Serial.println(obshee);
  Serial.print("Импульс на литр = ");
  Serial.println(impuls_na_litr);
  Serial.print("Последняя заправка = ");
  Serial.println(lastPulseCount);
  Serial.println("Введите 'set <value>' для установки импульсов на литр.");
  Serial.println("Введите 'reset' для сброса текущего количества или 'reset <value>' для установки общего литража.");
}

void loop() {
  currentTime = millis();
  int galileobut = digitalRead(galileo);
  int bypass = digitalRead(bypas);

  // Отладка состояния buttonPin и lastPulseTime
  static unsigned long lastPrintTime = 0;
  // if (currentTime - lastPrintTime >= 500) {
  //   Serial.print("buttonPin state: ");
  //   Serial.println(digitalRead(buttonPin));
  //   Serial.print("lastPulseTime: ");
  //   Serial.print(lastPulseTime);
  //   Serial.print(", time since last pulse: ");
  //   Serial.println(currentTime - lastPulseTime);
  //   lastPrintTime = currentTime;
  //   Serial.print("currentTime: ");
  //   Serial.print(currentTime);
  // }

  // Сброс флага ignorePulses при galileobut == LOW или bypass == 1
  if (galileobut == LOW || bypass == 1) {
    ignorePulses = false;
  }

  // Обработка Serial-команд
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input == "HELLO") {
      Serial.println("WORLD");
      return;
    }
    if (input == "GET_INFO") {
      Serial.print("Общий литраж = ");
      Serial.println(obshee);
      Serial.print("Импульс на литр = ");
      Serial.println(impuls_na_litr);
      Serial.print("Текущий литраж = ");
      Serial.println(pulseCount / impuls_na_litr);
      Serial.print("Последняя заправка = ");
      Serial.println(lastPulseCount);
      return;
    }
    if (input.startsWith("set ")) {
      String valueStr = input.substring(4);
      float newValue = valueStr.toFloat();
      if (newValue > 0) {
        impuls_na_litr = newValue;
        EEPROM.put(4, impuls_na_litr);
        Serial.print("Новый импульс на литр = ");
        Serial.println(impuls_na_litr);
      } else {
        Serial.println("Некорректный ввод. Введите положительное число.");
      }
    } 
    else if (input.startsWith("reset")) {
      if (input.length() > 5) {
        String valueStr = input.substring(6);
        float newValue = valueStr.toFloat();
        if (newValue >= 0) {
          obshee = newValue;
          EEPROM.put(0, obshee);
          Serial.print("Общий литраж установлен на: ");
          Serial.println(obshee);
        } else {
          Serial.println("Некорректный ввод. Введите положительное число.");
        }
      } else {
        obshee = 0.0;
        pulseCount = 0;
        lastPulseCount = 0.0;
        EEPROM.put(0, obshee);
        EEPROM.put(8, lastPulseCount);
        Serial.println("Общий литраж и последняя заправка сброшены до 0.");
      }
    } 
    else {
      Serial.println("Неправильная команда. Используйте 'set <value>' или 'reset <value>'.");
    }
  }

  // Режим змейки
  if (mode == -1) {
    snakeEffect();
    if (currentTime >= 5000) {
      mode = 0;
    }
    return;
  }

  // Переключение в режим 0 при galileobut == LOW
  if (galileobut == LOW) mode = 0;

  // Возврат в режим 0 через 10 секунд
  if (currentTime - lastButtonPressTime >= modeSwitchDelay) {
    mode = 0;
  }

  // Сброс pulseCount
  if (currentTime < 4000000000 && lastPulseTime > 0 && currentTime >= lastPulseTime && (unsigned long)(currentTime - lastPulseTime) > resetTime && (unsigned long)(currentTime - lastPulseTime) < 4000000000 && pulseCount > 0 && bypass == 1) {
    double currentLitrage = pulseCount / impuls_na_litr;
    if (!isnan(currentLitrage)) {
      obshee += currentLitrage;
      if (currentLitrage >= minLitrageThreshold) {
        lastPulseCount = currentLitrage;
        EEPROM.put(8, lastPulseCount);
      }
      pulseCount = 0;
      if (!isnan(obshee)) {
        EEPROM.put(0, obshee);
         Serial.print("Автосброс по времени (bypass): litrage = ");
         Serial.println(currentLitrage);
      }
    }
  }
  if (galileobut == 1 && pulseCount > 0 && bypass == 0) {
    double currentLitrage = pulseCount / impuls_na_litr;
    if (!isnan(currentLitrage)) {
      obshee += currentLitrage;
      if (currentLitrage >= minLitrageThreshold) {
        lastPulseCount = currentLitrage;
        EEPROM.put(8, lastPulseCount);
      }
      pulseCount = 0;
      ignorePulses = true; // Игнорировать импульсы после сброса по карточке
      if (!isnan(obshee)) {
        EEPROM.put(0, obshee);
        Serial.print("Сброс по карточке: litrage = ");
        Serial.println(currentLitrage);
      }
    }
  }

  // Обновление дисплея
  unsigned long displayValue;
  switch (mode) {
    case 1:
      displayValue = round(obshee);
      break;
    case 2:
      displayValue = round(lastPulseCount);
      break;
    case 0:
    default:
      displayValue = round(pulseCount / impuls_na_litr);
      break;
  }
  
  int displayValue1 = displayValue % 10;
  int displayValue2 = (displayValue / 10) % 10;
  int displayValue3 = (displayValue / 100) % 10;
  int displayValue4 = (displayValue / 1000) % 10;
  int displayValue5 = (displayValue / 10000) % 10;
  int displayValue6 = (displayValue / 100000) % 10;

  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, LSBFIRST, numbers[displayValue1]);
  shiftOut(dataPin, clockPin, LSBFIRST, numbers[displayValue2]);
  shiftOut(dataPin, clockPin, LSBFIRST, numbers[displayValue3]);
  shiftOut(dataPin, clockPin, LSBFIRST, numbers[displayValue4]);
  shiftOut(dataPin, clockPin, LSBFIRST, numbers[displayValue5]);
  shiftOut(dataPin, clockPin, LSBFIRST, numbers[displayValue6]);
  digitalWrite(latchPin, HIGH);
}

void snakeEffect() {
  byte numbers[] = {
    B00000011,  // 0
    B10011111,  // 1
    B00100101,  // 2
    B00001101,  // 3
    B10011001,  // 4
    B01001001,  // 5    
    B01000001,  // 6
    B00011111,  // 7
    B00000001,  // 8
    B00001001   // 9
  };

  byte displayStates[6];
  for (int digit = 0; digit < 10; digit++) {
    for (int i = 0; i < 6; i++) {
      displayStates[i] = B11111111;
    }
    for (int j = 0; j < 6; j++) {
      displayStates[j] = numbers[digit];
      digitalWrite(latchPin, LOW);
      for (int k = 5; k >= 0; k--) {
        shiftOut(dataPin, clockPin, LSBFIRST, displayStates[k]);
      }
      digitalWrite(latchPin, HIGH);
      delay(50);
    }
    delay(50);
  }
  for (int i = 0; i < 6; i++) {
    displayStates[i] = B11111111;
  }
  digitalWrite(latchPin, LOW);
  for (int k = 5; k >= 0; k--) {
    shiftOut(dataPin, clockPin, LSBFIRST, displayStates[k]);
  }
  digitalWrite(latchPin, HIGH);
  delay(50);
  mode = 0;
}

void countPulse() {
  if (ignorePulses) return; // Игнорировать импульсы, если флаг активен
  buttonState = digitalRead(inputPin);
  if (buttonState != lastButtonState) {
    if (buttonState == LOW) {
      pulseCount++;
      unsigned long currentMillis = millis();
      if (currentMillis < 4294967295) { // Защита от переполнения (значения близкие к 2^32)
        lastPulseTime = currentMillis;
      }
    }
    lastButtonState = buttonState;
  }
}

void button() {
  // Serial.println("Прерывание button вызвано");
  if (millis() - debounce >= 100) {
    bool buttonState = !digitalRead(buttonPin);
    // Serial.print("buttonPin: ");
    // Serial.print(buttonState ? "LOW (нажата)" : "HIGH");
    // Serial.print(", debounce: ");
    // Serial.println(millis() - debounce);
    if (buttonState) {
      debounce = millis();
      mode = (mode + 1) % 3;
      lastButtonPressTime = millis();
      // Serial.print("Режим переключен: mode = ");
      // Serial.println(mode);
    }
  }
}
