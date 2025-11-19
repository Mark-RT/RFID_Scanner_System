#include <SPI.h>

#include <GyverDBFile.h>
#include <LittleFS.h>
GyverDBFile db(&LittleFS, "/data.db");

#include <SettingsGyver.h>
SettingsGyver sett("RFID сканер", &db);

enum kk : size_t // ключі для зберігання в базі даних
{
  device_name,
  device_id,
  hub_id,

  relay_invert,
  relay_time,

  mosfet_invert,
  mosfet_time,
  beeper_freq,

  gpio_mode,
  gpio_state,

  wifi_ap_ssid,
  wifi_ap_pass,

  wifi_ssid,
  wifi_pass,
  apply
};
sets::Logger logger(150);

bool relay_invert_temp = false;
uint8_t relay_time_temp = 1;
bool mosfet_invert_temp = false;
uint8_t mosfet_time_temp = 1;

#include <MFRC522.h>
#define RC522_SS_PIN 21
#define RC522_RST_PIN 22
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);
MFRC522::MIFARE_Key key;    // об'єкт ключа
MFRC522::StatusCode status; // об'єкт статусу

#include <LoRa.h>
#define LORA_NSS_PIN 17
#define LORA_RST_PIN 16
#define LORA_DIO0_PIN 4
const uint16_t ACK_TIMEOUT = 1000;    // мілісекунд очікування підтвердження відповіді від хаба
const uint8_t MAX_RETRIES = 7;        // макс. кількість спроб відправки повідомлення
const uint16_t RETRIES_TIMEOUT = 400; // час до наступної спроби

#include <Blinker.h>
#define LED_R_PIN 25
#define LED_G_PIN 33
#define LED_B_PIN 32
#define RELAY_PIN 27
#define MOSFET_PIN 14
Blinker led_R(LED_R_PIN);
Blinker led_G(LED_G_PIN);
Blinker led_B(LED_B_PIN);
Blinker relay(RELAY_PIN);
Blinker mosfet(MOSFET_PIN);

enum BlinkState
{
  BLINK_IDLE,
  LED_OFF,
  LED_WAIT,
  RELAY_ON
};
BlinkState blink_state = LED_WAIT;
BlinkState blink_prevState = LED_OFF;

#include <BeepESP.h>
BeepESP beep;
#define BUZZER_PIN 13
#define PWM_CHANNEL 0
#define PWM_RESOLUTION 8

enum BeepState
{
  BEEP_IDLE,
  BEEP_STOP,
  BEEP_ENTER,
  BEEP_DENIED,
  BEEP_ONCE,
  BEEP_FOREVER,
  BEEP_WIFI_START
};
BeepState beep_state = BEEP_ONCE;
BeepState beep_prevState = BEEP_IDLE;
uint16_t beep_freq_temp = 0;

// ---- Налаштування / константи ----
#define PREAMBLE 0xA5
#define TYPE_REQ 0x10
#define CMD_OPEN 0x12
#define CMD_DENY 0x13
#define MAX_NAME_CHARS 20
#define MAX_NAME_BYTES 80 // запас байтів (20 символів * max 4 байти/символ)
#define MAX_UID_LEN 16
#define MAX_TOTAL_PAYLOAD 247 // практичний ліміт: 255 - header(8) -> ≈247

char deviceNameBuf[MAX_NAME_BYTES + 1]; // тут зберігаємо UTF-8 назву (байти)
uint8_t DEVICE_ID = 0;
uint8_t HUB_ID = 1;
uint16_t msgCounter = 0;

void build(sets::Builder &b) // БІЛДЕР ВЕБ-ІНТЕРФЕЙСУ
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
    b.Input(kk::device_name, "Назва точки:");
    b.Input(kk::device_id, "ID точки:");
    b.Input(kk::hub_id, "ID хаба (за замовчуванням 1):");
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  if (b.beginGroup("Реле"))
  {
    b.Switch(kk::relay_invert, "Інверсія реле:");
    b.Slider(kk::relay_time, "Час затримки:", 1, 60, 1, "сек");
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  if (b.beginGroup("MOSFET"))
  {
    b.Switch(kk::mosfet_invert, "Інверсія MOSFET:");
    b.Slider(kk::mosfet_time, "Час затримки:", 1, 60, 1, "сек");
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  if (b.beginGroup("GPIO"))
  {
    b.Select(kk::gpio_mode, "Режим GPIO:", "ВХІД;ВИХІД");
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
        if (b.Button("Реле"))
        {
          Serial.print("Реле: ");
          Serial.println(b.build.pressed());
          blink_state = RELAY_ON;
        }
        if (b.Button("Синій"))
        {
          Serial.print("Синій: ");
          Serial.println(b.build.pressed());
          blink_state = LED_WAIT;
        }
        if (b.Button("Forever"))
        {
          Serial.print("Forever: ");
          Serial.println(b.build.pressed());
          beep_state = BEEP_FOREVER;
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

void blink_tick(uint8_t relay_tim, uint8_t mosfet_tim)
{
  led_R.tick();
  led_G.tick();
  led_B.tick();
  relay.tick();
  mosfet.tick();

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

    case RELAY_ON:
      relay.blink(1, relay_tim * 1000, 0);   // включаем реле на relay_time секунд
      mosfet.blink(1, mosfet_tim * 1000, 0); // включаем MOSFET на mosfet_time секунд
      led_G.blink(1, relay_tim * 1000, 0);
      break;
    }
  }

  if (relay.ready())
  {
    blink_state = LED_WAIT;
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
      beep.beep(freq, 1, 800);
      Serial.print("BEEP_ONCE: ");
      Serial.println(freq);
      break;

    case BEEP_FOREVER:
      beep.beepForever(freq, 300, 600);
      Serial.print("BEEP_FOREVER: ");
      Serial.println(freq);
      break;

    case BEEP_WIFI_START:
      beep.beep(950, 3, 300, 600);
      Serial.println("BEEP_WIFI_START");
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

size_t utf8_truncate_by_chars(const char *src, char *dst, size_t max_chars, size_t dst_buf_size) // Функція обрізає UTF-8 рядок до max_chars символів
{
  // гарантуючи, що багатобайтові символи (українські) не будуть "порізані" посередині.

  // Перевірка: джерело або призначення відсутні, або розмір вихідного буфера 0.
  // У такому разі нічого зробити не можемо — повертаємо 0.
  if (!src || !dst || dst_buf_size == 0)
    return 0;

  // Індекси для проходу по вхідному та вихідному рядках
  size_t src_i = 0; // на якому байті вхідного рядка зараз знаходимось
  size_t dst_i = 0; // на якому байті вихідного буфера записуємо
  size_t chars = 0; // скільки UTF-8 символів уже скопійовано

  // Основний цикл: поки не кінець джерела і не перевищено max_chars
  while (src[src_i] != '\0' && chars < max_chars)
  {
    // Поточний байт джерела, інтерпретований як unsigned (важливо!)
    unsigned char c = (unsigned char)src[src_i];

    // Довжина символа в UTF-8 у байтах. За замовчуванням – 1.
    size_t char_len = 1;

    // Визначаємо кількість байтів у UTF-8 символі за правилами:
    if ((c & 0x80) == 0x00)
      char_len = 1; // 0xxxxxxx — ASCII (1 байт)
    else if ((c & 0xE0) == 0xC0)
      char_len = 2; // 110xxxxx — багатобайтовий символ з 2 байтів
    else if ((c & 0xF0) == 0xE0)
      char_len = 3; // 1110xxxx — 3-байтовий символ (типово для кирилиці)
    else if ((c & 0xF8) == 0xF0)
      char_len = 4; // 11110xxx — 4 байти (емодзі, рідкісні символи)
    else
      break; // якщо байт не відповідає жодному патерну — виходимо

    // Перевірка: чи є в джерелі всі необхідні байти цього символа?
    bool ok = true;
    for (size_t k = 0; k < char_len; k++)
    {
      // Якщо раптом символ обривається (недостатньо байтів) — не копіювати
      if (src[src_i + k] == '\0')
      {
        ok = false;
        break;
      }
    }
    if (!ok)
      break;

    // Перевірка: чи вистачає місця у dst для цього символу + кінцевого '\0'?
    if (dst_i + char_len >= dst_buf_size)
      break; // якщо не влазить — зупиняємо копіювання

    // Копіюємо всі байти цього UTF-8 символа у вихідний буфер
    for (size_t k = 0; k < char_len; k++)
      dst[dst_i++] = src[src_i + k];

    // Переходимо до наступного символа в src
    src_i += char_len;

    // Збільшуємо лічильник копійованих символів
    chars++;
  }

  // Завершуємо рядок нуль-термінатором
  dst[dst_i] = '\0';

  // Повертаємо кількість БАЙТІВ записаних у dst (без нуль-термінатора)
  return dst_i;
}

void initFromDB() // Ініціалізація змінних з БД
{
  relay_time_temp = db.get(kk::relay_time);
  mosfet_time_temp = db.get(kk::mosfet_time);

  relay.invert(db.get(kk::relay_invert));
  mosfet.invert(db.get(kk::mosfet_invert));

  // отримуємо ID як uint8_t
  DEVICE_ID = (uint8_t)db.get(kk::device_id);
  HUB_ID = (uint8_t)db.get(kk::hub_id);

  // прочитати назву (String) з DB і обрізати до 20 символів
  String s = db.get(kk::device_name);
  char tmp[128];
  s.toCharArray(tmp, sizeof(tmp));
  utf8_truncate_by_chars(tmp, deviceNameBuf, MAX_NAME_CHARS, sizeof(deviceNameBuf));
}

// ---- CRC8 ----
uint8_t crc8(const uint8_t *data, size_t len)
{
  uint8_t crc = 0x00; // Початкове значення CRC (seed)
  while (len--)       // Проходимо всі байти масиву
  {
    uint8_t b = *data++;            // Беремо наступний байт та зсуваємо вказівник
    for (uint8_t i = 0; i < 8; i++) // Обробляємо кожен з 8 бітів байта
    {
      uint8_t mix = (crc ^ b) & 0x01; // mix = XOR молодших бітів CRC і байта
      crc >>= 1;                      // Зсуваємо CRC праворуч
      if (mix)                        // Якщо mix = 1 – треба XOR з поліномом
        crc ^= 0x8C;                  // 0x8C — поліном CRC8 (Dallas/Maxim)
      b >>= 1;                        // Зсуваємо вхідний байт праворуч
    }
  }
  return crc; // Повертаємо обчислений CRC
}

// ---- buildAndSend: src = DEVICE_ID, dst = to ----
bool buildAndSend(uint8_t to, uint16_t msgId, uint8_t type, const uint8_t *payload, uint8_t len)
{
  if (len > MAX_TOTAL_PAYLOAD)
    return 1; // payload занадто великий

  uint8_t buf[256];
  size_t idx = 0;
  buf[idx++] = PREAMBLE;            // 0 – службовий байт, початок пакета
  buf[idx++] = DEVICE_ID;           // 1 – src: ID відправника (цей пристрій)
  buf[idx++] = to;                  // 2 – dst: кому надсилаємо (HUB_ID)
  buf[idx++] = (msgId >> 8) & 0xFF; // 3 – msgId (старший байт)
  buf[idx++] = msgId & 0xFF;        // 4 – msgId (молодший байт)
  buf[idx++] = type;                // 5 - тип пакета/запиту
  buf[idx++] = len;                 // 6 - payload довжина
  if (len && payload)
  {
    memcpy(&buf[idx], payload, len); // Копіюємо payload у пакет
    idx += len;
  }
  uint8_t crc = crc8(buf, idx); // CRC рахується по всіх попередніх байтах
  buf[idx++] = crc;             // Додаємо CRC у кінець пакета

  // --- DEBUG: вивід сформованого пакета по байтах для відладки ---
  Serial.println("Packet dump (index : 0xHEX):");
  for (size_t _i = 0; _i < idx; ++_i)
  {
    Serial.print(_i);
    Serial.print(" : 0x");
    if (buf[_i] < 0x10)
      Serial.print('0');
    Serial.print(buf[_i], HEX);
    Serial.println();
  }
  Serial.println("--- end packet ---");
  // --- end debug ---

  LoRa.beginPacket();
  LoRa.write(buf, idx);
  LoRa.endPacket();
  return 0;
}

// ---- waitForResponse: читати packetSize, мінімум 8 байт ----
// додаємо max size для outPayload
bool waitForResponse(uint16_t expectedMsgId, unsigned long timeout,
                     uint8_t *outType, uint8_t *outPayload, uint8_t *outLen,
                     size_t outPayloadMaxLen)
{
  unsigned long t0 = millis(); // Фіксуємо час старту очікування

  while (millis() - t0 < timeout) // Крутимо цикл, доки не минув timeout
  {
    int packetSize = LoRa.parsePacket(); // Перевіряємо, чи прийшов пакет LoRa
                                         // Повертає >0 якщо пакет доступний

    if (packetSize > 0) // Якщо пакет дійсно є
    {
      uint8_t buf[256]; // Буфер для зчитування всього прийнятого пакету
      int i = 0;        // Лічильник фактично прочитаних байтів

      while (i < packetSize &&     // Читаємо тільки заявлений розмір пакета
             LoRa.available() &&   // Поки є байти в LoRa буфері
             i < (int)sizeof(buf)) // І поки не переповнюємо наш локальний буфер
        buf[i++] = LoRa.read();    // Зчитуємо байт у buf[i] і збільшуємо i

      if (i < 8)  // Пакет менше мінімально дозволених 8 байт?
        continue; // Пропускаємо його (це сміття)

      if (buf[0] != PREAMBLE) // Перевірка першого байта — стартовий маркер пакета
        continue;             // Якщо не співпадає — не наш пакет

      uint8_t recv_crc = buf[i - 1];    // Останній байт отриманого пакета — CRC
      if (crc8(buf, i - 1) != recv_crc) // Обчислюємо CRC від усіх попередніх байтів
        continue;                       // CRC не співпав — пакет зіпсований, ігноруємо

      uint8_t from = buf[1];                                       // Адреса відправника пакета
      uint8_t to = buf[2];                                         // Адреса отримувача (має бути цього пристрою)
      uint16_t msgId = (uint16_t(buf[3]) << 8) | uint16_t(buf[4]); // Старший байт msgId Молодший байт msgId (двобайтове число)
      uint8_t type = buf[5];                                       // Тип повідомлення (команда/відповідь)
      uint8_t len = buf[6];                                        // Довжина payload у байтах

      // Перевірка, чи пакет адресований нам або broadcast (0)
      if (to != DEVICE_ID && to != 0)
        continue; // Не нам — пропускаємо

      if (msgId != expectedMsgId) // Перевіряємо чи це відповідь на наше запитання
        continue;                 // MsgId не співпав — не наша відповідь

      // Перевірка коректності поля len
      if ((int)len > i - 8) // len не може бути більшим, ніж реальний залишок пакета
        continue;           // Якщо більший — пакет некоректний

      if (outType)       // Якщо вказівник не NULL
        *outType = type; // Записуємо тип пакета у змінну користувача

      if (outLen)      // Якщо користувач хоче отримати довжину payload
        *outLen = len; // Записуємо її

      if (outPayload && len) // Якщо є куди копіювати payload
      {
        if ((size_t)len > outPayloadMaxLen) // Якщо payload більший, ніж дозволена довжина буфера
          continue;                         // Ігноруємо цей пакет (не влазить)

        memcpy(outPayload, &buf[7], len); // Копіюємо payload, який починається з buf[7]
      }

      return true; // Пакет валідний, правильний, і відправлений нам.
    }
  }

  return false; // Час вийшов, відповідь не отримано
}

// Оновлений sendUidWithName: НЕ інкрементує msgCounter; приймає msgId як параметр
bool sendUidWithName(uint8_t hubId, uint16_t msgId, const uint8_t *uidBytes, uint8_t uidLen)
{
  if (!uidBytes || uidLen == 0 || uidLen > MAX_UID_LEN) // Перевірка коректності UID: нема вказівника, довжина = 0, UID занадто довгий
    return false;

  size_t nameBytes = strnlen(deviceNameBuf, MAX_NAME_BYTES); // Безпечний підрахунок байтів імені (обмеження MAX_NAME_BYTES)
  if (nameBytes > MAX_NAME_BYTES)
    nameBytes = MAX_NAME_BYTES;

  // Розрахунок загальної довжини корисного навантаження
  size_t payloadLen = 1 + nameBytes + 1 + uidLen;                               // додаткові службові байти: 1 байт — довжина імені, nameBytes — сам текст імені, 1 байт — довжина UID, uidLen — сам UID
  if (payloadLen > MAX_TOTAL_PAYLOAD || payloadLen > sizeof(uint8_t) * 256 - 8) // перевірка чи не перевищили ліміт протоколу LoRa
    return false;

  uint8_t payload[256];
  size_t idx = 0;
  payload[idx++] = (uint8_t)nameBytes; // першим записуємо довжину імені
  if (nameBytes)
  {
    memcpy(&payload[idx], deviceNameBuf, nameBytes); // копіюємо ім’я цілком
    idx += nameBytes;
  }

  payload[idx++] = uidLen;                 // записуємо довжину UID
  memcpy(&payload[idx], uidBytes, uidLen); // копіюємо сам UID
  idx += uidLen;

  bool ok = (buildAndSend(hubId, msgId, TYPE_REQ, payload, (uint8_t)payloadLen) == 0); // buildAndSend повертає 1 при успіху
  LoRa.receive();                                                                      // Переводимо радіомодуль у режим прийому
  return ok;
}

void setup()
{
  Serial.begin(115200);
  SPI.begin();

  // ======== BEEP ========
  beep.init(BUZZER_PIN, PWM_CHANNEL, PWM_RESOLUTION);

  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max); // Установка усиления антенны
  rfid.PCD_AntennaOff();                    // Перезагружаем антенну
  delay(50);
  rfid.PCD_AntennaOn(); // Включаем антенну
  for (byte i = 0; i < 6; i++)
  {                        // Наполняем ключ
    key.keyByte[i] = 0xFF; // Ключ по умолчанию 0xFFFFFFFFFFFF
  }

  // ======== LORA ========
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
  a > 0 ? Serial.println("success") : Serial.println("failed");

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
  db.init(kk::device_name, "Назва...");
  db.init(kk::device_id, 0);
  db.init(kk::hub_id, 1);
  db.init(kk::relay_invert, false);
  db.init(kk::relay_time, 2);
  db.init(kk::mosfet_invert, false);
  db.init(kk::mosfet_time, 2);
  db.init(kk::beeper_freq, 1000);
  db.init(kk::gpio_mode, INPUT_PULLUP);
  db.init(kk::gpio_state, 0);
  db.init(kk::wifi_ap_ssid, "RFID");
  db.init(kk::wifi_ap_pass, "12345678");
  db.init(kk::wifi_ssid, "");
  db.init(kk::wifi_pass, "");

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
    Serial.println(WiFi.localIP());
  }

  if (db[kk::wifi_ap_ssid].length())
  {
    int triess = 15;
    bool apCreated = false;

    while (triess--)
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
  blink_tick(relay_time_temp, mosfet_time_temp);
  beep_tick(beep_freq_temp); // основна функція, яка керує станами

  //************************* РОБОТА З RFID **************************//
  if (!rfid.PICC_IsNewCardPresent())
    return;
  if (!rfid.PICC_ReadCardSerial())
    return;

  String uidStr = "";
  for (uint8_t i = 0; i < rfid.uid.size; i++)
  {
    if (rfid.uid.uidByte[i] < 0x10)
      uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase(); // зробити великі літери
  logger.println(sets::Logger::warn() + "UID: " + uidStr);
  Serial.println("UID: " + uidStr);

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
        Serial.println("Помилка відправки повідомлення");
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
          Serial.println("OPEN отримано");
          blink_state = RELAY_ON;
          beep_state = BEEP_ENTER;
        }
        else if (respType == CMD_DENY)
        {
          Serial.println("DENY command received");
          beep_state = BEEP_DENIED;
        }
        success = true;
        break;
      }
      else
      {
        tries++;
        retries_timeout_temp = millis();
        Serial.print("Нема відповіді, спроба ");
        Serial.println(tries);
      }
    }
    delay(2);
  }

  if (!success)
  {
    logger.println(sets::Logger::error() + "Нема відповіді від хаба");
    Serial.println("Нема відповіді від хаба");
  }

  // Завершаємо роботу з міткою
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}