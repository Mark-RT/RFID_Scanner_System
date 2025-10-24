#include <SPI.h>

#include <GyverDBFile.h>
#include <LittleFS.h>
GyverDBFile db(&LittleFS, "/data.db");

#include <SettingsGyver.h>
SettingsGyver sett("RFID сканер", &db);

enum kk : size_t // ключі для зберігання в базі даних
{
  hub_id,

  beeper_freq,

  wifi_ap_ssid,
  wifi_ap_pass,

  wifi_ssid,
  wifi_pass,
  apply
};
sets::Logger logger(150);

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

#define BUZZER_PIN 13
#define PWM_CHANNEL 0
#define PWM_RESOLUTION 8

enum BeepState
{
  BEEP_IDLE,
  BEEP_STOP,
  BEEP_ACTIVE,
  BEEP_PAUSE,
  BEEP_ENTER,
  BEEP_DENIED,
  BEEP_ONCE,
  BEEP_WIFI_START,
  BEEP_CONTINUOUS
};
BeepState beep_state = BEEP_ONCE;
BeepState beep_prevState = BEEP_CONTINUOUS;

uint16_t beep_freq = 0;
uint16_t beep_freq_temp = 0;
uint8_t beep_count = 0;
uint8_t beep_current = 0;
uint16_t beep_onTime = 0;
uint16_t beep_offTime = 0;
unsigned long beep_timer = 0;
bool beep_isOn = false;

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

  {
    sets::Group g(b, "WiFi налаштування точки");
    b.Input(kk::wifi_ap_ssid, "SSID:");
    b.Pass(kk::wifi_ap_pass, "Пароль:");
  }

  if (b.beginGroup("Для розробника"))
  {
    if (b.beginMenu("Тест"))
    {
      b.Slider(kk::beeper_freq, "Частота:", 100, 5000, 50, "Гц");
      if (b.beginRow())
      {
        if (b.Button("Синій"))
        {
          Serial.print("Синій: ");
          Serial.println(b.build.pressed());
          blink_state = LED_WAIT;
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
    logger.print("Введено частоту: ");
    logger.println(b.build.value);
    beep_freq_temp = b.build.value;
    beep_state = BEEP_ONCE;
    break;
  }
}

void update(sets::Updater &upd)
{
  // отправить лог
  upd.update(H(log), logger);
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

void beep_init() // Ініціалізація
{
  ledcSetup(PWM_CHANNEL, 2000, PWM_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
}

void beep_start(uint16_t freq, uint8_t count, uint16_t onTime, uint16_t offTime) // Запуск послідовності писків
{
  beep_freq = freq;
  beep_count = count;
  beep_current = 0;
  beep_onTime = onTime;
  beep_offTime = offTime;
  beep_isOn = false;
  beep_timer = millis();
  beep_state = BEEP_ACTIVE;
}

void beep_stop() // Зупинка будь-якого писку
{
  ledcWrite(PWM_CHANNEL, 0);
  beep_isOn = false;
  beep_current = 0;
  beep_state = BEEP_IDLE;
}

void beep_tick() // Функція обробки писку (асинхронна)
{
  unsigned long now = millis();

  switch (beep_state)
  {
  case BEEP_IDLE:
    // нічого не робимо
    break;

  case BEEP_ACTIVE:
    if (!beep_isOn)
    {
      ledcWriteTone(PWM_CHANNEL, beep_freq);
      beep_isOn = true;
      beep_timer = now;
    }
    else if (now - beep_timer >= beep_onTime)
    {
      ledcWrite(PWM_CHANNEL, 0);
      beep_isOn = false;
      beep_timer = now;
      beep_current++;
      if (beep_current >= beep_count)
      {
        beep_stop();
      }
      else
      {
        beep_state = BEEP_PAUSE;
      }
    }
    break;

  case BEEP_PAUSE:
    if (now - beep_timer >= beep_offTime)
    {
      beep_state = BEEP_ACTIVE;
    }
    break;
  }
}

void beep_logic(uint16_t freq) // Основна логіка писку з обробкою станів
{
  if (beep_state != beep_prevState)
  {
    beep_prevState = beep_state;

    switch (beep_state)
    {
    case BEEP_STOP:
      beep_stop();
      Serial.println("BEEP_STOP");
      break;

    case BEEP_ENTER:
      beep_start(1000, 2, 300, 500);
      Serial.println("BEEP_ENTER");
      break;

    case BEEP_DENIED:
      beep_start(200, 2, 300, 400);
      Serial.println("BEEP_DENIED");
      break;

    case BEEP_ONCE:
      beep_start(freq, 1, 1200, 0);
      Serial.print("BEEP_ONCE: ");
      Serial.println(freq);
      break;

    case BEEP_WIFI_START:
      beep_start(950, 3, 300, 600);
      Serial.println("BEEP_WIFI_START");
      break;

    case BEEP_CONTINUOUS:
      ledcWriteTone(PWM_CHANNEL, freq);
      Serial.println("BEEP_CONTINUOUS");
      break;

    default:
      break;
    }
  }

  beep_tick(); // асинхронне оновлення
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
  Serial.println("LoRa packet received");
  uint8_t buf[256];
  int i = 0;
  while (i < packetSize && LoRa.available() && i < (int)sizeof(buf))
    buf[i++] = LoRa.read();

  Serial.print("Received packet of size ");
  Serial.println(i);
  if (i < 8 || buf[0] != PREAMBLE)
    return;

  // Перевірка CRC:
  uint8_t recv_crc = buf[i - 1];
  if (crc8(buf, i - 1) != recv_crc)
  {
    Serial.println("CRC mismatch, ignoring packet");
    return;
  }

  uint8_t from = buf[1];
  uint8_t to = buf[2];
  uint16_t msgId = (uint16_t(buf[3]) << 8) | uint16_t(buf[4]);
  uint8_t type = buf[5];
  uint8_t len = buf[6];

  // Перевіряємо, що пакет призначений хабу (DEVICE_ID) або broadcast (0)
  if (to != HUB_ID && to != 0)
    return;

  // Захист від некоректного len
  if ((int)len > i - 8)
    return;

  // Обробка TYPE_REQ (запит від сканера)
  if (type == TYPE_REQ)
  {
    // Розбираємо payload: [name_len][name_bytes][uid_len][uid_bytes]
    if (len < 2)
    {
      Serial.println("REQ payload too short");
      // Можна відправити DENY
      buildAndSend(from, msgId, CMD_DENY, NULL, 0);
      return;
    }

    uint8_t name_len = buf[7];
    if ((size_t)name_len > len - 2)
    { // мінімум uid_len + 0 uid bytes
      Serial.println("Bad name_len in payload");
      buildAndSend(from, msgId, CMD_DENY, NULL, 0);
      return;
    }

    // Зчитати імя (не обов'язково нуль-терміноване)
    String deviceName = "";
    if (name_len > 0)
    {
      char tmp[129];
      size_t copy_len = min((size_t)name_len, sizeof(tmp) - 1);
      memcpy(tmp, &buf[8], copy_len);
      tmp[copy_len] = '\0';
      deviceName = String(tmp);
    }

    // Де індекс uid_len?
    size_t uid_len_index = 8 + name_len;
    if (uid_len_index >= (size_t)i)
    { // вийшли за межі
      Serial.println("Payload truncated before uid_len");
      buildAndSend(from, msgId, CMD_DENY, NULL, 0);
      return;
    }
    uint8_t uid_len = buf[uid_len_index];
    if (uid_len == 0 || uid_len > MAX_UID_LEN)
    {
      Serial.println("Bad uid_len");
      buildAndSend(from, msgId, CMD_DENY, NULL, 0);
      return;
    }

    size_t uid_start = uid_len_index + 1;
    if ((int)uid_start + uid_len > i - 1)
    { // -1 через CRC в кінці
      Serial.println("UID bytes out of range");
      buildAndSend(from, msgId, CMD_DENY, NULL, 0);
      return;
    }

    // Копіюємо UID у тимчасовий буфер
    uint8_t uid_buf[MAX_UID_LEN];
    memcpy(uid_buf, &buf[uid_start], uid_len);

    // Логування
    Serial.print("REQ від: ");
    Serial.print(from);
    Serial.print("  msgId: ");
    Serial.print(msgId);
    Serial.print("  Назва: '");
    Serial.print(deviceName);
    Serial.print("'  UID картки: ");
    for (uint8_t k = 0; k < uid_len; ++k)
    {
      if (uid_buf[k] < 0x10)
        Serial.print('0');
      Serial.print(uid_buf[k], HEX);
      Serial.print(' ');
    }
    Serial.println();

    // --- Відправляємо текстову відповідь "Дозволено" ---
    buildAndSend(from, msgId, CMD_OPEN, NULL, 0);
    Serial.println("-> CMD_OPEN дозволено");
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

void setup()
{
  Serial.begin(115200);
  SPI.begin();

  beep_init();

  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max); // Установка усиления антенны
  rfid.PCD_AntennaOff();                    // Перезагружаем антенну
  rfid.PCD_AntennaOn();                     // Включаем антенну
  for (byte i = 0; i < 6; i++)
  {                        // Наполняем ключ
    key.keyByte[i] = 0xFF; // Ключ по умолчанию 0xFFFFFFFFFFFF
  }
  Serial.println("RFID OK!");
  delay(1000);

  LoRa.setPins(LORA_NSS_PIN, LORA_RST_PIN, LORA_DIO0_PIN); // setup LoRa transceiver module
  int a = 5;                                               // кількість спроб ініціалізації LoRa
  Serial.print("LoRa init");
  while (!LoRa.begin(433E6)) // 433E6 - Asia, 866E6 - Europe, 915E6 - North America
  {
    Serial.print(".");
    delay(500);
    if (!--a)
      break;
  }
  a > 0 ? Serial.println("LoRa init success") : Serial.println("LoRa init failed");
  Serial.println();

  WiFi.mode(WIFI_AP_STA); // ======== WIFI ========

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
  db.init(kk::beeper_freq, 1000);
  db.init(kk::wifi_ap_ssid, "HUB_RFID");
  db.init(kk::wifi_ap_pass, "12345678");
  db.init(kk::wifi_ssid, "");
  db.init(kk::wifi_pass, "");

  // часовой пояс для rtc
  setStampZone(2);

  if (db[kk::wifi_ssid].length())
  {
    WiFi.begin(db[kk::wifi_ssid], db[kk::wifi_pass]);
    Serial.print("Connect STA");
    int tries = 15;
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
      Serial.print('.');
      if (!--tries)
        break;
    }
    Serial.println();
    Serial.print("IP STA: ");
    Serial.println(WiFi.localIP());
  }

  if (db[kk::wifi_ap_ssid].length())
  {
    int triess = 15;
    bool apCreated = false;

    while (triess--)
    {
      Serial.print("Create AP");
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
      Serial.print("AP створено успішно: ");
      Serial.println(WiFi.softAPIP());
    }
    else
    {
      Serial.println("Не вдалося створити AP, створюємо запасний...");
      WiFi.softAP("RFID_rezerv", "12345678");
      Serial.print("AP: ");
      Serial.println(WiFi.softAPIP());
    }
  }
  initFromDB();
}

void loop()
{
  /*static uint32_t rebootTimer = millis(); // Важный костыль против зависания модуля!
  if (millis() - rebootTimer >= 2000)
  {                                    // Таймер с периодом 2000 мс
    rebootTimer = millis();            // Обновляем таймер
    digitalWrite(RC522_RST_PIN, HIGH); // Сбрасываем модуль
    delayMicroseconds(2);              // Ждем 2 мкс
    digitalWrite(RC522_RST_PIN, LOW);  // Отпускаем сброс
    rfid.PCD_Init();                   // Инициализируем заного
  }*/

  sett.tick();
  blink_tick();
  beep_logic(beep_freq_temp); // основна функція, яка керує станами
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