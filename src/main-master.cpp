#include <Arduino.h>
#include <SPI.h>

#include <GyverDBFile.h>
#include <LittleFS.h>
GyverDBFile db(&LittleFS, "/data.db");

#include <SettingsGyver.h>
SettingsGyver sett("HUB RFID", &db);
bool notice_scan_card;
bool notice_add_card;
bool alert_check_uid_DB;
bool alert_surname_name;

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
sets::Logger loggerSDcard(200);

#include <MFRC522.h>
#define RC522_SS_PIN 27
#define RC522_RST_PIN 13
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);
MFRC522::MIFARE_Key key;    // об'єкт ключа
MFRC522::StatusCode status; // об'єкт статусу
bool rfid_active = false;
String uidStr = "";
String surname_name = "";
uint8_t access_level = 0;

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
const char *DB_FILE_NAME = "/mydatabase.csv";

#include <EncButton.h>
EncButton eb(36, 39, 34);
int8_t enc_button_state = 1; // поточний стан енкодера (1-3)

#include <GyverOLED.h>
GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;
enum DisplayInfo
{
  LINE_UID, // Показує UID на першому рядку
  LINE_MENU,
  LINE_WAIT_CARD
};

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
#define CMD_OPEN 0x12
#define CMD_DENY 0x13
#define MAX_UID_LEN 16
#define MAX_TOTAL_PAYLOAD 247

// Налаштування ID для хаба
uint8_t HUB_ID = 1; // ID хаба (той самий, що HUB_ID у пристрої)

// ===== ФУНКЦІЇ РОБОТИ З SD КАРТОЮ =====
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

bool uidExists(String uid)
{
  File f = SD.open(DB_FILE_NAME);
  if (!f)
    return false;

  while (f.available())
  {
    String line = f.readStringUntil('\n');

    int commaIndex = line.indexOf(',');
    if (commaIndex == -1)
      continue;

    String uidField = line.substring(0, commaIndex);

    if (uidField == uid)
    {
      f.close();
      return true;
    }
  }

  f.close();
  return false;
}

bool addRecord(String uid, String name, int level)
{
  String dt = sett.rtc.toString();

  File f = SD.open(DB_FILE_NAME, FILE_APPEND);
  if (!f)
  {
    Serial.println("DB open error");
    return false;
  }

  f.print(uid);
  f.print(",");
  f.print(name);
  f.print(",");
  f.print(level);
  f.print(",");
  f.println(dt);

  f.close();

  Serial.println("Запис зроблено:");
  Serial.println(uid + "," + name + "," + level + "," + dt);
  return true;
}

void clear_area_for_menu()
{
  oled.clear(0, 8, 127, 63); // Очистити область під меню
}

void show_on_Display(DisplayInfo line, const String &text = "", uint8_t page = 0)
{
  switch (line)
  {
  case LINE_UID:
    oled.setScale(1);
    oled.setCursor(0, 0);
    oled.print("UID: ");
    oled.print(text);
    oled.print("      ");
    break;

  case LINE_MENU:
    clear_area_for_menu();
    oled.setScale(2);
    oled.setCursor(0, 2);
    switch (page)
    {
    case 1:
      oled.print("1. Прошити й додати в БД");
      Serial.println("OLED: 1. Прошити й додати в БД");
      break;

    case 2:
      oled.print("2. Info по картцi");
      Serial.println("OLED: 2. Info по картцi");
      break;

    case 3:
      oled.print("3. Видалити картку з БД");
      Serial.println("OLED: 3. Видалити картку з БД");
      break;
    }
    break;

  case LINE_WAIT_CARD:
    clear_area_for_menu();
    oled.setScale(2);
    oled.setCursor(0, 2);
    oled.print("Очiкую картку...");
    Serial.println("OLED: Очiкую картку...");
    break;

  default:
    break;
  }
}

void clean_adding_form()
{
  uidStr = "";
  surname_name = "";
  access_level = 0;
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
          loggerSDcard.println(String(cardSize) + " MB");
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
          readFile(SD, DB_FILE_NAME);
        }
        b.endRow();
      }
      b.endMenu(); // не забываем завершить меню
    }
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  if (b.beginGroup("Дії з карткою:"))
  {
    if (b.beginMenu("Прошивка й додавання в БД"))
    {
      b.Button("scan_card"_h, "Сканувати картку");
      b.Label("uid_label"_h, "UID картки:");
      b.Input("surname_name"_h, "Прізвище та ім'я:");
      b.Select("access_level"_h, "Рівень доступу:", "низький;середній;високий");

      b.Button("write_add_card"_h, "Прошити й додати в БД");
      bool res;
      if (b.Confirm("conf"_h, "Додати запис?", &res))
      {
        if (res)
        {
          addRecord(uidStr, surname_name, access_level);
          clean_adding_form();
          b.reload();
        }
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

  case "scan_card"_h:
    Serial.println("Натиснув сканувати картку");
    clean_adding_form();
    notice_scan_card = true;
    show_on_Display(LINE_WAIT_CARD);
    rfid_active = true;
    break;

  case "surname_name"_h:
    Serial.print("Введено ім'я: ");
    Serial.println(b.build.value);
    surname_name = b.build.value;
    break;

  case "access_level"_h:
    Serial.print("Вибрано рівень доступу: ");
    Serial.println(b.build.value);
    access_level = b.build.value.toInt();
    break;

  case "write_add_card"_h:
    Serial.print("Натиснув додати картку з UID: ");
    Serial.println(b.build.value);
    if (surname_name != "")
      notice_add_card = true;
      else
      alert_surname_name = true;

    break;
  }
}

void update(sets::Updater &upd)
{
  // отправить лог
  upd.update(H(log), logger);
  upd.update(H(logSDcard), loggerSDcard);
  upd.update("uid_label"_h, uidStr);
  if (notice_scan_card)
  {
    notice_scan_card = false;
    upd.notice("Відскануйте картку!");
  }
  if (notice_add_card)
  {
    notice_add_card = false;
    upd.confirm("conf"_h);
  }
  if (alert_check_uid_DB)
  {
    alert_check_uid_DB = false;
    upd.alert("UID вже існує в БД!");
  }
  if (alert_surname_name)
  {
    alert_surname_name = false;
    upd.alert("Введіть прізвище та ім'я!");
  }
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

// ===== buildAndSend для хаба (від хаба -> пристрій) =====
bool buildAndSend(uint8_t to, uint16_t msgId, uint8_t type, const uint8_t *payload, uint8_t len)
{
  if (len > MAX_TOTAL_PAYLOAD)
    return 0;

  uint8_t buf[256];
  size_t idx = 0;
  buf[idx++] = PREAMBLE;            // 0 – службовий байт, початок пакета
  buf[idx++] = HUB_ID;              // 1 – src: ID відправника (цей хаб)
  buf[idx++] = to;                  // 2 – dst: кому надсилаємо (DEVICE_ID)
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

  LoRa.beginPacket();
  LoRa.write(buf, idx);
  LoRa.endPacket();

  LoRa.receive();
  return 1;
}

// ===== Функція прийому і парсингу одного пакета =====
void handleIncomingPacket()
{
  int packetSize = LoRa.parsePacket(); // Перевіряємо, чи прийшов новий LoRa пакет
  if (packetSize <= 0)                 // Якщо пакет не прийшов або його розмір 0
    return;                            // Виходимо з функції, нічого робити не потрібно

  uint8_t buf[256];            // Локальний буфер для зберігання пакета
  int i = 0;                   // Лічильник зчитаних байтів
  while (i < packetSize &&     // Читаємо, поки не досягли розміру пакета
         LoRa.available() &&   // І поки є доступні байти в LoRa буфері
         i < (int)sizeof(buf)) // І поки не переповнили локальний буфер
    buf[i++] = LoRa.read();    // Зчитуємо байт у buf[i] та збільшуємо i

  Serial.print("Прийнято пакет розміром: ");
  Serial.println(i); // Лог повідомлення про розмір пакета

  if (i < 8 || buf[0] != PREAMBLE) // Мінімальний пакет 8 байт + перевірка преамбули
    return;                        // Якщо менше 8 байт або неправильна преамбула — вихід

  // Перевірка CRC пакета:
  uint8_t recv_crc = buf[i - 1];    // Останній байт пакета — CRC
  if (crc8(buf, i - 1) != recv_crc) // Перевіряємо CRC всіх попередніх байтів
  {
    Serial.println("CRC не співпадає, відміна"); // Лог про помилку CRC
    return;                                      // Пакет пошкоджений, вихід
  }

  uint8_t device = buf[1];                                     // ID пристрою-відправника
  uint8_t hub = buf[2];                                        // ID хаба-отримувача
  uint16_t msgId = (uint16_t(buf[3]) << 8) | uint16_t(buf[4]); // msgId (2 байти)
  uint8_t type = buf[5];                                       // Тип пакета (TYPE_REQ, TYPE_ACK тощо)
  uint8_t len = buf[6];                                        // Довжина payload

  // Перевіряємо, що пакет призначений хабу (HUB_ID) або broadcast (0)
  if (hub != HUB_ID && hub != 0)
    return; // Пакет не для цього хаба — ігноруємо

  // Захист від некоректного len
  if ((int)len > i - 8) // len більше, ніж доступно байт для payload
    return;             // Некоректний пакет, вихід

  if (type == TYPE_REQ) // Якщо пакет — запит від сканера
  {
    // Розбираємо payload: [name_len][name_bytes][uid_len][uid_bytes]
    uint8_t name_len = buf[7]; // Перший байт payload — довжина імені

    if (len < 2 || ((size_t)name_len > len - 2)) // Мінімум name_len + 1 byte для uid_len
    {
      Serial.println("REQ payload короткий або пошкоджене ім'я, відміна");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0); // Відправляємо відмову
      return;                                         // Вихід, пакет не валідний
    }

    String deviceName = ""; // Ініціалізуємо ім'я як пусте
    if (name_len > 0)       // Якщо ім'я є
    {
      char tmp[129];                                            // Тимчасовий буфер для імені
      size_t copy_len = min((size_t)name_len, sizeof(tmp) - 1); // Копіюємо тільки скільки влазить
      memcpy(tmp, &buf[8], copy_len);                           // Копіюємо байти імені з пакета
      tmp[copy_len] = '\0';                                     // Додаємо нуль-термінатор
      deviceName = String(tmp);                                 // Створюємо String з буфера
    }

    size_t uid_len_index = 8 + name_len; // Індекс байта, де починається uid_len
    if (uid_len_index >= (size_t)i)      // Перевірка виходу за межі буфера
    {
      Serial.println("Payload truncated before uid_len");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0); // Відправка відмови
      return;                                         // Вихід
    }

    uint8_t uid_len = buf[uid_len_index];      // Довжина UID
    if (uid_len == 0 || uid_len > MAX_UID_LEN) // Перевірка коректності UID
    {
      Serial.println("Bad uid_len");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }

    size_t uid_start = uid_len_index + 1; // Індекс початку UID
    if ((int)uid_start + uid_len > i - 1) // Перевірка, щоб UID не виходив за межі (i-1 через CRC)
    {
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
    for (uint8_t k = 0; k < uid_len; ++k) // Формуємо рядок для логу
    {
      if (uid_buf[k] < 0x10)
        Serial.print('0');
      Serial.print(uid_buf[k], HEX);
      Serial.print(' ');
      uidStr += String(uid_buf[k], HEX);
    }
    uidStr.toUpperCase(); // Приводимо до великих літер
    logger.println(sets::Logger::warn() + "UID: " + uidStr);
    Serial.println();

    // --- Відправляємо текстову відповідь "Дозволено" ---
    buildAndSend(device, msgId, CMD_OPEN, NULL, 0); // Надсилаємо відкриття
    show_on_Display(LINE_UID, uidStr);
    Serial.println("-> CMD_OPEN дозволено");
    logger.println("Дозволено");

    /*
        // Тут можна вставити перевірку за списком дозволених UID
        if (uid_allowed(uid_buf, uid_len))
            buildAndSend(device, msgId, CMD_OPEN, NULL, 0);
        else
            buildAndSend(device, msgId, CMD_DENY, NULL, 0);
    */
  }
  else // Якщо type відмінний від TYPE_REQ
  {
    Serial.print("Невідомий type "); // Логування
    Serial.println(type);
  }
}

void encoderB_tick()
{
  eb.tick();
  // обработка поворота раздельная
  if (eb.left())
  {
    enc_button_state--;
    if (enc_button_state <= 1)
      enc_button_state = 1;
    Serial.println("left");
  }

  if (eb.right())
  {
    enc_button_state++;
    if (enc_button_state >= 3)
      enc_button_state = 3;
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

  if (eb.turn())
  {
    Serial.println("turn");
    show_on_Display(LINE_MENU, "", enc_button_state);
  }
}

void setup()
{
  Serial.begin(115200);
  SPI.begin();

  // ======== BEEP ========
  beep.init(BUZZER_PIN, PWM_CHANNEL, PWM_RESOLUTION);

  //=============== ДИСПЛЕЙ ===============
  oled.init();  // инициализация
  oled.clear(); // очистить дисплей (или буфер)

  oled.home(); // курсор в 0,0
  oled.setScale(1);
  oled.autoPrintln(1);

  //============= RFID ===============
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max); // Установка усиления антенны
  rfid.PCD_AntennaOff();                    // Перезагружаем антенну
  delay(50);
  rfid.PCD_AntennaOn(); // Включаем антенну
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
  a = 5; // кількість спроб ініціалізації
  while (!SD.begin(SD_CS))
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

  File file = SD.open(DB_FILE_NAME);
  if (!file)
  {
    Serial.println("Файл не знайдено");
    Serial.println("Створення файлу...");
    oled.println("Нема файлу. Створення...");
    writeFile(SD, DB_FILE_NAME, "UID,Name,Level,DateTime \r\n");
  }
  else
  {
    Serial.println("Файл знайдено");
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
  db.init(kk::beeper_freq, 1000);
  db.init(kk::wifi_ap_ssid, "HUB_RFID");
  db.init(kk::wifi_ap_pass, "12345678");
  db.init(kk::wifi_ssid, "Krop9");
  db.init(kk::wifi_pass, "0964946190");

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
      oled.println("WiFi ");
      oled.println(WiFi.localIP());
      Serial.println(WiFi.localIP());
    }
    else
    {
      Serial.println(" Не вдалося пiдключитися до WiFi");
      oled.println("WiFi -> no");
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
      // if (WiFi.softAP("Testttt", "12345678"))
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
      WiFi.softAP("HUB-RFID_rezerv", "12345678");
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

  if (rfid_active)
  {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
      return;

    uidStr = "";
    for (uint8_t i = 0; i < rfid.uid.size; i++)
    {
      if (rfid.uid.uidByte[i] < 0x10)
        uidStr += "0";
      uidStr += String(rfid.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase(); // зробити великі літери
    Serial.println("UID: " + uidStr);

    if (uidExists(uidStr))
    {
      Serial.println("Картка вже в БД");
      alert_check_uid_DB = true;
      uidStr = "";
    }

    rfid_active = false;
    show_on_Display(LINE_UID, uidStr);
    show_on_Display(LINE_MENU, "", enc_button_state);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
}