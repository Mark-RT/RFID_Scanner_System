#include <Arduino.h>
#include <SPI.h>

#include <GyverDBFile.h>
#include <LittleFS.h>
GyverDBFile db(&LittleFS, "/data.db");

#include <SettingsGyver.h>
SettingsGyver sett("HUB RFID", &db);

enum kk : size_t // ключі для зберігання в базі даних
{
  hub_id,

  mosfet_invert, // УДАЛИТЬ
  mosfet_time,   // УДАЛИТЬ

  beeper_freq,

  wifi_ap_ssid,
  wifi_ap_pass,

  wifi_ssid,
  wifi_pass,
  apply
};
sets::Logger logger(150);
sets::Logger loggerSDcard(200);

#include <MFRC522.h>
#define RC522_SS_PIN 27
#define RC522_RST_PIN 13
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);
MFRC522::MIFARE_Key key;    // об'єкт ключа
MFRC522::StatusCode status; // об'єкт статусу

#include <LoRa.h>
#define LORA_NSS_PIN 17
#define LORA_RST_PIN 16
#define LORA_DIO0_PIN 4
const unsigned long ACK_TIMEOUT = 800; // мілісекунд очікування підтвердження
const uint8_t MAX_RETRIES = 3;
uint16_t RETRIES_TIMEOUT = 500; // час до наступної спроби

#include <SD.h>
#include <FS.h>
#define SD_CS 14

#include <EncButton.h>
EncButton eb(36, 39, 34);

#include <GyverOLED.h>
GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;

#include <Blinker.h>
#define LED_R_PIN 25
#define LED_G_PIN 33
#define LED_B_PIN 32
Blinker led_R(LED_R_PIN);
Blinker led_G(LED_G_PIN);
Blinker led_B(LED_B_PIN);
enum BlinkState
{
  BLINK_IDLE,
  LED_OFF,
  LED_WAIT,
};
BlinkState blink_state = LED_WAIT;
BlinkState blink_prevState = LED_OFF;

#include <BeepESP.h>
BeepESP beep;
#define BUZZER_PIN 26
#define PWM_CHANNEL 0
#define PWM_RESOLUTION 8

enum BeepState
{
  BEEP_IDLE,
  BEEP_STOP,
  BEEP_ENTER,
  BEEP_DENIED,
  BEEP_ONCE
};
BeepState beep_state = BEEP_ONCE;
BeepState beep_prevState = BEEP_IDLE;
uint16_t beep_freq_temp = 0;

// ===== Константи (повинні співпадати з пристроєм) =====
#define PREAMBLE 0xA5
#define TYPE_REQ 0x10
#define TYPE_REG 0x01
#define CMD_OPEN 0x12
#define CMD_DENY 0x13
#define MAX_UID_LEN 16
#define MAX_TOTAL_PAYLOAD 247

// Налаштування ID для хаба
uint8_t HUB_ID = 1; // ID хаба (той самий, що HUB_ID у пристрої)

// Write to the SD card
void writeFile(fs::FS &fs, const char *path, const char *message)
{
  Serial.printf("Writing file: %s\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(message))
  {
    Serial.println("File written");
  }
  else
  {
    Serial.println("Write failed");
  }
  file.close();
}

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      logger.print("  DIR : ");
      logger.println(file.name());

      if (levels)
      {
        listDir(fs, file.name(), levels - 1);
      }
    }
    else
    {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
      logger.print("  FILE: ");
      logger.print(file.name());
      logger.print("  SIZE: ");
      logger.println(file.size());
    }
    file = root.openNextFile();
  }
}

void readFile(fs::FS &fs, const char *path)
{
  Serial.printf("Reading file: %s\n", path);
  logger.println("Reading file in serial");

  File file = fs.open(path);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return;
  }

  Serial.print("Read from file: ");
  while (file.available())
  {
    Serial.write(file.read());
  }
  file.close();
}

void build(sets::Builder &b)
{
  b.Log(H(log), logger);
  if (b.build.isAction())
  {
    Serial.print("Set: 0x");
    Serial.print(b.build.id, HEX);
    Serial.print(" = ");
    Serial.println(b.build.value);

    logger.print("Set: 0x");
    logger.println(b.build.id, HEX);
  }

  if (b.beginGroup("Назва та ID"))
  {
    b.Input(kk::hub_id, "ID хаба (за замовчуванням 1):");
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  if (b.beginGroup("MOSFET"))
  {
    b.Switch(kk::mosfet_invert, "Інверсія MOSFET:");
    b.Slider(kk::mosfet_time, "Час затримки:", 1, 60, 1, "сек");
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  {
    sets::Group g(b, "WiFi налаштування точки");
    b.Input(kk::wifi_ap_ssid, "SSID:");
    b.Pass(kk::wifi_ap_pass, "Пароль:");
  }

  if (b.beginGroup("Для розробника"))
  {
    if (b.beginMenu("Тест, SD карта"))
    {
      b.Log(H(logSDcard), loggerSDcard);
      b.Slider(kk::beeper_freq, "Частота:", 100, 5000, 50, "Гц");
      if (b.beginRow())
      {
        if (b.Button("info"))
        {
          Serial.print("info: ");
          Serial.println(b.build.pressed());
          uint64_t cardSize = SD.cardSize() / (1024 * 1024);
          Serial.printf("SD Card Size: %lluMB\n", cardSize);
          loggerSDcard.println(cardSize);
        }
        if (b.Button("dir"))
        {
          Serial.print("dir: ");
          Serial.println(b.build.pressed());
          listDir(SD, "/", 0);
        }
        if (b.Button("read"))
        {
          Serial.print("read: ");
          Serial.println(b.build.pressed());
          readFile(SD, "/database.txt");
        }
        b.endRow();
      }
      b.endMenu(); // не забываем завершить меню
    }
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  {
    sets::Group g(b, "WiFi роутер");
    b.Input(kk::wifi_ssid, "SSID:");
    b.Pass(kk::wifi_pass, "Пароль:");
  }

  if (b.beginButtons())
  {
    if (b.Button(kk::apply, "Save & Restart"))
    {
      db.update(); // сохраняем БД не дожидаясь таймаута
      ESP.restart();
    }
    b.endButtons(); // завершить кнопки
  }

  switch (b.build.id)
  {
  case kk::beeper_freq:
    Serial.print("Введено частоту: ");
    Serial.println(b.build.value);
    loggerSDcard.print("Введено частоту: ");
    loggerSDcard.println(b.build.value);
    beep_freq_temp = b.build.value;
    beep_state = BEEP_ONCE;
    break;
  }
}

void update(sets::Updater &upd)
{
  // отправить лог
  upd.update(H(log), logger);
  upd.update(H(logSDcard), loggerSDcard);
}

void blink_tick()
{
  led_R.tick();
  led_G.tick();
  led_B.tick();

  if (blink_state != blink_prevState) // проверяем смену состояния
  {
    blink_prevState = blink_state;
    led_B.stop();

    switch (blink_state)
    {
    case LED_OFF:
      led_R.stop();
      led_G.stop();
      led_B.stop();
      break;

    case LED_WAIT:
      led_B.blinkForever(300, 1500);
      logger.println("Blue LED blink");
      break;
    }
  }
}

void beep_tick(uint16_t freq) // Основна логіка писку з обробкою станів
{
  beep.tick();

  if (beep_state != beep_prevState)
  {
    beep_prevState = beep_state;

    switch (beep_state)
    {
    case BEEP_IDLE:
      break;

    case BEEP_STOP:
      beep.stop();
      Serial.println("BEEP_STOP");
      break;

    case BEEP_ENTER:
      beep.beep(1000, 2, 150, 100);
      Serial.println("BEEP_ENTER");
      break;

    case BEEP_DENIED:
      beep.beep(200, 1, 400);
      Serial.println("BEEP_DENIED");
      break;

    case BEEP_ONCE:
      beep.beep(freq, 1, 1200);
      Serial.print("BEEP_ONCE: ");
      Serial.println(freq);
      break;

    default:
      break;
    }
  }

  if (beep.isReady())
  {
    beep_state = BEEP_IDLE;
  }
}

void initFromDB() // Ініціалізація змінних з БД
{
  HUB_ID = (uint8_t)db.get(kk::hub_id);
}

// ---- CRC8 ----
uint8_t crc8(const uint8_t *data, size_t len)
{
  uint8_t crc = 0x00;
  while (len--)
  {
    uint8_t b = *data++;
    for (uint8_t i = 0; i < 8; i++)
    {
      uint8_t mix = (crc ^ b) & 0x01;
      crc >>= 1;
      if (mix)
        crc ^= 0x8C;
      b >>= 1;
    }
  }
  return crc;
}

// ===== buildAndSend для хаба (від хаба -> пристрій) =====
uint8_t buildAndSend(uint8_t to, uint16_t msgId, uint8_t type, const uint8_t *payload, uint8_t len)
{
  if (len > MAX_TOTAL_PAYLOAD)
    return 1;

  uint8_t buf[256];
  size_t idx = 0;
  buf[idx++] = PREAMBLE;
  buf[idx++] = HUB_ID;              // тут — ID хаба як SRC
  buf[idx++] = to;                  // DST = отримувач (пристрій)
  buf[idx++] = (msgId >> 8) & 0xFF; // msgId hi
  buf[idx++] = msgId & 0xFF;        // msgId lo
  buf[idx++] = type;
  buf[idx++] = len;
  if (len && payload)
  {
    memcpy(&buf[idx], payload, len);
    idx += len;
  }
  uint8_t crc = crc8(buf, idx);
  buf[idx++] = crc;

  LoRa.beginPacket();
  LoRa.write(buf, idx);
  LoRa.endPacket();
  // після відправки бажано викликати receive()
  LoRa.receive();
  return 0;
}

// ===== Проста біла книга UID для прикладу =====
// Тут — масив з пар (довжина, байти).
const uint8_t ALLOWED_UID_COUNT = 2;
const uint8_t ALLOWED_UID_LEN[ALLOWED_UID_COUNT] = {4, 7};
const uint8_t ALLOWED_UIDS[ALLOWED_UID_COUNT][MAX_UID_LEN] = {
    {0xDE, 0xAD, 0xBE, 0xEF},                  // приклад 4-байт UID
    {0x04, 0x25, 0xA3, 0x11, 0x22, 0x33, 0x44} // приклад 7-байт UID
};

bool uid_allowed(const uint8_t *uid, uint8_t len)
{
  for (uint8_t i = 0; i < ALLOWED_UID_COUNT; ++i)
  {
    if (len != ALLOWED_UID_LEN[i])
      continue;
    if (memcmp(uid, ALLOWED_UIDS[i], len) == 0)
      return true;
  }
  return false;
}

// ===== Функція прийому і парсингу одного пакета =====
void handleIncomingPacket()
{
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0)
    return;
  uint8_t buf[256];
  int i = 0;
  while (i < packetSize && LoRa.available() && i < (int)sizeof(buf))
    buf[i++] = LoRa.read();

  Serial.print("Прийнято пакет розміром: ");
  Serial.println(i);
  if (i < 8 || buf[0] != PREAMBLE)
    return;

  // Перевірка CRC:
  uint8_t recv_crc = buf[i - 1];
  if (crc8(buf, i - 1) != recv_crc)
  {
    Serial.println("CRC не співпадає, ігнор");
    return;
  }

  uint8_t device = buf[1];
  uint8_t hub = buf[2];
  uint16_t msgId = (uint16_t(buf[3]) << 8) | uint16_t(buf[4]);
  uint8_t type = buf[5];
  uint8_t len = buf[6];

  // Перевіряємо, що пакет призначений хабу (DEVICE_ID) або broadcast (0)
  if (hub != HUB_ID && hub != 0)
    return;

  // Захист від некоректного len
  if ((int)len > i - 8)
    return;

  if (type == TYPE_REQ) // обробка TYPE_REQ (запит від сканера)
  {
    // Розбираємо payload: [name_len][name_bytes][uid_len][uid_bytes]
    if (len < 2)
    {
      Serial.println("REQ payload короткий, відміна");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0); // відправляємо відміну
      return;
    }

    uint8_t name_len = buf[7];
    if ((size_t)name_len > len - 2) // мінімум uid_len + 0 uid bytes
    {
      Serial.println("Пошкоджене ім'я в payload, відміна");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }

    String deviceName = ""; // зчитуємо ім'я
    if (name_len > 0)
    {
      char tmp[129];
      size_t copy_len = min((size_t)name_len, sizeof(tmp) - 1);
      memcpy(tmp, &buf[8], copy_len);
      tmp[copy_len] = '\0';
      deviceName = String(tmp);
    }

    size_t uid_len_index = 8 + name_len;
    if (uid_len_index >= (size_t)i)
    { // вийшли за межі
      Serial.println("Payload truncated before uid_len");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }
    uint8_t uid_len = buf[uid_len_index];
    if (uid_len == 0 || uid_len > MAX_UID_LEN)
    {
      Serial.println("Bad uid_len");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }

    size_t uid_start = uid_len_index + 1;
    if ((int)uid_start + uid_len > i - 1)
    { // -1 через CRC в кінці
      Serial.println("UID bytes out of range");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }

    // Копіюємо UID у тимчасовий буфер
    uint8_t uid_buf[MAX_UID_LEN];
    memcpy(uid_buf, &buf[uid_start], uid_len);

    // Логування
    Serial.print("REQ від: ");
    Serial.print(device);
    Serial.print("  msgId: ");
    Serial.print(msgId);
    Serial.print("  Назва: '");
    Serial.print(deviceName);
    Serial.print("'  UID картки: ");
    String uidStr = "";
    for (uint8_t k = 0; k < uid_len; ++k)
    {
      if (uid_buf[k] < 0x10)
        Serial.print('0');
      Serial.print(uid_buf[k], HEX);
      Serial.print(' ');
      uidStr += String(uid_buf[k], HEX);
    }
    uidStr.toUpperCase(); // при желании сделать все символы заглавными
    logger.println(sets::Logger::warn() + "UID: " + uidStr);
    Serial.println();

    // --- Відправляємо текстову відповідь "Дозволено" ---
    buildAndSend(device, msgId, CMD_OPEN, NULL, 0);
    Serial.println("-> CMD_OPEN дозволено");
    logger.println("Дозволено");
    /*
        // Проста логіка доступу
        if (uid_allowed(uid_buf, uid_len))
        {
          // Надіслати відповідь: CMD_OPEN
          buildAndSend(from, msgId, CMD_OPEN, NULL, 0);
          Serial.println("-> OPEN sent");
        }
        else
        {
          buildAndSend(from, msgId, CMD_DENY, NULL, 0);
          Serial.println("-> DENY sent");
        }*/
  }
  else
  {
    Serial.print("Невідомий type "); // Можна обробляти інші типи (TYPE_REG і т.д.)
    Serial.println(type);
  }
}

void encoderB_tick()
{
  eb.tick();
  // обработка поворота раздельная
  if (eb.left())
  {
    Serial.println("left");
  }

  if (eb.right())
  {
    Serial.println("right");
  }

  if (eb.leftH())
  {
    Serial.println("leftH");
  }

  if (eb.rightH())
  {
    Serial.println("rightH");
  }

  if (eb.click())
  {
    Serial.println("click");
  }
}

void setup()
{
  Serial.begin(115200);
  SPI.begin();

  beep.init(BUZZER_PIN, PWM_CHANNEL, PWM_RESOLUTION);

  //=============== ДИСПЛЕЙ ===============
  oled.init();  // инициализация
  oled.clear(); // очистить дисплей (или буфер)

  oled.home(); // курсор в 0,0
  oled.setScale(1);

  //============= RFID ===============
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max); // Установка усиления антенны
  rfid.PCD_AntennaOff();                    // Перезагружаем антенну
  rfid.PCD_AntennaOn();                     // Включаем антенну
  for (byte i = 0; i < 6; i++)
  {                        // Наполняем ключ
    key.keyByte[i] = 0xFF; // Ключ по умолчанию 0xFFFFFFFFFFFF
  }
  oled.println("RFID -> OK");
  delay(200);

  //============= LoRa ===============
  Serial.print("LoRa init ");
  LoRa.setPins(LORA_NSS_PIN, LORA_RST_PIN, LORA_DIO0_PIN); // setup LoRa transceiver module
  int a = 5;                                               // кількість спроб ініціалізації LoRa
  while (!LoRa.begin(433E6))                               // 433E6 - Asia, 866E6 - Europe, 915E6 - North America
  {
    Serial.print(".");
    delay(500);
    if (!--a)
      break;
  }
  if (a > 0)
  {
    Serial.println("success");
    oled.println("LoRa -> OK");
  }
  else
  {
    Serial.println("failed");
    oled.println("LoRa -> not find");
  }
  delay(200);

  //================= SD CARD ====================
  Serial.print("Initializing SD card ");
  a = 5;                   // кількість спроб ініціалізації LoRa
  while (!SD.begin(SD_CS)) // 433E6 - Asia, 866E6 - Europe, 915E6 - North America
  {
    Serial.print(".");
    delay(500);
    if (!--a)
      break;
  }
  if (a > 0)
  {
    Serial.println("success");
    oled.println("SD -> OK");
  }
  else
  {
    Serial.println("failed");
    oled.println("SD -> not find");
  }

  if (SD.cardType() == CARD_NONE)
  {
    Serial.println("SD карта відсутня");
    oled.println("SD карта вiдсутня");
  }

  // If the data.txt file doesn't exist
  // Create a file on the SD card and write the data labels
  File file = SD.open("/database.txt");
  if (!file)
  {
    Serial.println("File doesn't exist");
    Serial.println("Creating file...");
    oled.println("Нема файлу. Створення...");
    writeFile(SD, "/database.txt", "Epoch Time, Temperature, Humidity, Pressure \r\n");
  }
  else
  {
    Serial.println("File already exists");
    oled.println("Файл -> OK");
  }
  file.close();
  listDir(SD, "/", 0);

  delay(300);

  // ======== WIFI ========
  WiFi.mode(WIFI_AP_STA);

  // ======== SETTINGS ========
  sett.begin();
  sett.onBuild(build);
  sett.onUpdate(update);
  sett.config.theme = sets::Colors::Mint; // основной цвет

  // ======== DATABASE ========
#ifdef ESP32
  LittleFS.begin(true);
#else
  LittleFS.begin();
#endif

  db.begin();
  db.init(kk::hub_id, 1);
  db.init(kk::mosfet_invert, false);
  db.init(kk::mosfet_time, 2);
  db.init(kk::beeper_freq, 1000);
  db.init(kk::wifi_ap_ssid, "HUB_RFID");
  db.init(kk::wifi_ap_pass, "12345678");
  db.init(kk::wifi_ssid, "TP-LINK_4CA4");
  db.init(kk::wifi_pass, "75813284");

  // ======== WIFI ========
  setStampZone(2); // годинний пояс

  if (db[kk::wifi_ssid].length())
  {
    WiFi.begin(db[kk::wifi_ssid], db[kk::wifi_pass]);
    Serial.print("Connect to " + db[kk::wifi_ssid]);
    int tries = 15;
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
      Serial.print('.');
      if (!--tries)
        break;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
      oled.println("WiFi пiдключений");
      Serial.println(WiFi.localIP());
    }
  }

  if (db[kk::wifi_ap_ssid].length())
  {
    int tries = 15;
    bool apCreated = false;

    while (tries--)
    {
      Serial.print("Створюю AP ");
      if (WiFi.softAP(db[kk::wifi_ap_ssid], db[kk::wifi_ap_pass]))
      {
        apCreated = true;
        break;
      }
      delay(500);
      Serial.print('.');
    }

    if (apCreated)
    {
      Serial.print("успішно: ");
      Serial.println(WiFi.softAPIP());
      oled.println("Точка доступу -> OK");
      oled.println(WiFi.softAPIP());
    }
    else
    {
      Serial.println("Не вдалося створити AP, створюємо запасний...");
      WiFi.softAP("RFID_rezerv", "12345678");
      Serial.print("AP: ");
      Serial.println(WiFi.softAPIP());
      oled.println("Точка доступу -> *OK");
      oled.println(WiFi.softAPIP());
    }
  }
  delay(600);

  initFromDB();
}

void loop()
{
  static uint32_t rebootTimer = millis(); // Важный костыль против зависания модуля!
  if (millis() - rebootTimer >= 2000)
  {                                    // Таймер с периодом 2000 мс
    rebootTimer = millis();            // Обновляем таймер
    digitalWrite(RC522_RST_PIN, HIGH); // Сбрасываем модуль
    delayMicroseconds(2);              // Ждем 2 мкс
    digitalWrite(RC522_RST_PIN, LOW);  // Отпускаем сброс
    rfid.PCD_Init();                   // Инициализируем заного
  }

  sett.tick();
  encoderB_tick();
  blink_tick();
  beep_tick(beep_freq_temp); // основна функція, яка керує станами
  handleIncomingPacket();

  //************************* РОБОТА З RFID **************************//
  /*if (!rfid.PICC_IsNewCardPresent())
    return;
  if (!rfid.PICC_ReadCardSerial())
    return;

  // Логування UID
  Serial.print("UID: ");
  for (uint8_t i = 0; i < rfid.uid.size; i++)
  {
    if (rfid.uid.uidByte[i] < 0x10)
      Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  // Підготовка msgId (інкремент один раз ДО спроб)
  uint16_t msgId = ++msgCounter;

  uint8_t tries = 0;
  bool success = false;
  uint32_t retries_timeout_temp = 0;
  while (tries < MAX_RETRIES && !success)
  {
    if (millis() - retries_timeout_temp >= RETRIES_TIMEOUT)
    {
      // Викликаємо утиліту, яка сформує payload і відправить (не інкрементує msgCounter)
      if (!sendUidWithName(HUB_ID, msgId, rfid.uid.uidByte, rfid.uid.size))
      {
        Serial.println("Failed to build/send payload");
        break; // немає сенсу повторювати, payload некоректний
      }

      // Очікуємо відповідь
      uint8_t respType;
      uint8_t respPayload[32];
      uint8_t respLen;
      if (waitForResponse(msgId, ACK_TIMEOUT, &respType, respPayload, &respLen, sizeof(respPayload)))
      {
        if (respType == CMD_OPEN)
        {
          Serial.println("OPEN command received");
          // TODO: виконати відкриття замка
        }
        else if (respType == CMD_DENY)
        {
          Serial.println("DENY command received");
        }
        success = true;
        break;
      }
      else
      {
        tries++;
        retries_timeout_temp = millis();
        Serial.print("No response, retry ");
        ;
        Serial.println(tries);
      }
    }
  }

  if (!success)
  {
    Serial.println("No response from hub");
  }

  // Завершаємо роботу з міткою
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();*/
}