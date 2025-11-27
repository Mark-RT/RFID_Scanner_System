#include <Arduino.h>
#include <SPI.h>

#include <GyverDBFile.h>
#include <LittleFS.h>
GyverDBFile db(&LittleFS, "/data.db");

#include <SettingsGyver.h>
SettingsGyver sett("HUB RFID", &db);
bool notice_success;
bool notice_scan_card;
bool notice_add_card;
bool notice_edit_card;
bool alert_check_uid_DB;
bool alert;
bool alert_surname_name;
bool alert_find_uid_DB;

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
sets::Logger logger(200);
sets::Logger loggerSDcard(1000);

#include <MFRC522.h>
#define RC522_SS_PIN 27
#define RC522_RST_PIN 13
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);
MFRC522::MIFARE_Key keyDefault; // об'єкт стандартного ключа
MFRC522::MIFARE_Key keyESP;     // об'єкт ключа
MFRC522::StatusCode status;     // об'єкт статусу
byte block = 7;
byte macBytes[6];
bool rfid_active = false;
bool rfid_delete_confirm = false;
String uidStr = "";
String uidStr_temp = "";
String surname_name = "";
uint8_t access_level = 0;
enum RfidState
{
  RFID_IDLE,
  RFID_ADD_CARD,
  RFID_SCAN_CARD,
  RFID_FAST_ADD,
  RFID_EDIT_CARD_DB,
  RFID_DELETE_FROM_DB
};
RfidState rfid_state = RFID_IDLE;

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
String name_DB;
int8_t access_level_DB;
String date_time_DB;

#include <EncButton.h>
EncButton eb(36, 39, 34);
int8_t enc_button_state = 1; // поточний стан енкодера (1-3)

#include <GyverOLED.h>
GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;
bool back_to_home = false;
uint32_t timing_back_to_home = 0;
enum DisplayInfo
{
  LINE_UID, // Показує UID на першому рядку
  LINE_MENU,
  LINE_DELETE_SUCCESS,
  LINE_UNKNOWN_UID,
  LINE_ERROR,
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
  LED_OK,
  LED_DENIED
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
  BEEP_OK,
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

void readFile(fs::FS &fs, const char *path, int linesPerChunk = 5)
{
  loggerSDcard.printf("Reading file: %s\n", path);

  File file = fs.open(path);
  if (!file)
  {
    loggerSDcard.println("Failed to open file for reading");
    return;
  }

  int lineCount = 0;
  String buffer = "";

  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();

    // Додаємо рядок в буфер
    buffer += line + "\n";
    lineCount++;

    // Якщо зібрали linesPerChunk рядків — виводимо
    if (lineCount >= linesPerChunk)
    {
      loggerSDcard.println(buffer); // вивід пачки

      // очищаємо для наступних рядків
      buffer = "";
      lineCount = 0;
    }
  }

  // Якщо файл закінчився, а в буфері щось є
  if (lineCount > 0)
  {
    loggerSDcard.println(buffer);
  }

  file.close();
}

bool deleteUserFromDB(String uid)
{
  File inFile = SD.open(DB_FILE_NAME, FILE_READ);
  if (!inFile)
    return false;

  // Тимчасовий файл
  File outFile = SD.open("/tmp.csv", FILE_WRITE);
  if (!outFile)
  {
    inFile.close();
    return false;
  }

  bool deleted = false;

  while (inFile.available())
  {
    String line = inFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;

    int c1 = line.indexOf(',');
    if (c1 == -1)
      continue;

    String uidField = line.substring(0, c1);

    // Якщо UID співпав — пропускаємо (видаляємо)
    if (uidField == uid)
    {
      deleted = true;
      continue; // НЕ записуємо рядок в новий файл
    }

    // Інакше — копіюємо рядок у новий файл
    outFile.println(line);
  }

  inFile.close();
  outFile.close();

  // Якщо запис видалено — замінюємо файл
  if (deleted)
  {
    SD.remove(DB_FILE_NAME);             // видаляємо старий
    SD.rename("/tmp.csv", DB_FILE_NAME); // замінюємо новим
  }
  else
  {
    SD.remove("/tmp.csv"); // UID не знайдено — тимчасовий файл зайвий
  }

  return deleted;
}

bool editUser(const String &uid, const String &newName, int newLevel)
{
  File f = SD.open(DB_FILE_NAME, FILE_READ);
  if (!f)
  {
    Serial.println("Error opening DB for edit");
    return false;
  }

  // Тимчасовий файл
  File tmp = SD.open("/tmp.csv", FILE_WRITE);
  if (!tmp)
  {
    Serial.println("Error creating temp file");
    f.close();
    return false;
  }

  bool replaced = false;

  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
    {
      continue;
    }

    // Заголовок не чіпаємо
    if (line.startsWith("UID,"))
    {
      tmp.println(line);
      continue;
    }

    // Парсимо UID
    int c1 = line.indexOf(',');
    if (c1 == -1)
    {
      tmp.println(line);
      continue;
    }

    String uidField = line.substring(0, c1);

    if (uidField == uid)
    {
      // Формуємо новий рядок
      String newLine = uid + ",";
      newLine += newName + ",";
      newLine += String(newLevel) + ",";
      newLine += sett.rtc.toString(); // дата/час

      tmp.println(newLine);
      replaced = true;
    }
    else
    {
      tmp.println(line); // залишаємо як було
    }
  }

  f.close();
  tmp.close();

  // Замінюємо старий файл новим
  SD.remove(DB_FILE_NAME);
  SD.rename("/tmp.csv", DB_FILE_NAME);

  return replaced;
}

bool findUser(String uid, String &outName, int8_t &outLevel, String &outDateTime)
{
  File f = SD.open(DB_FILE_NAME);
  if (!f)
    return false;

  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;

    // Парсимо першу кому (UID)
    int c1 = line.indexOf(',');
    if (c1 == -1)
      continue;

    String uidField = line.substring(0, c1);

    if (uidField == uid)
    {
      // Знайшли запис! Тепер парсимо інше
      int c2 = line.indexOf(',', c1 + 1);
      int c3 = line.indexOf(',', c2 + 1);

      if (c2 == -1 || c3 == -1)
      {
        f.close();
        return false; // некоректний рядок
      }

      outName = line.substring(c1 + 1, c2);
      outLevel = line.substring(c2 + 1, c3).toInt();
      outDateTime = line.substring(c3 + 1);

      f.close();
      return true;
    }
  }

  f.close();
  return false; // UID не знайдено
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

// ===== ФУНКЦІЇ РОБОТИ З ДИСПЛЕЄМ =====
void clear_area_for_menu()
{
  oled.clear(0, 8, 127, 63); // Очистити область під меню
}

void show_on_Display(DisplayInfo line, const String &text = "", uint8_t page = 0)
{
  if (line != LINE_UID)
  {
    clear_area_for_menu();
    oled.setScale(2);
    oled.setCursor(0, 2);
  }
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
    switch (page)
    {
    case 1:
      oled.print("1. Info по картцi");
      Serial.println("OLED: 1. Info по картцi");
      break;

    case 2:
      oled.print("2. Швидко додати картку");
      Serial.println("OLED: 2. Швидко додати картку");
      break;

    case 3:
      oled.print("3. Видалити картку з БД");
      Serial.println("OLED: 3. Видалити картку з БД");
      break;
    }
    break;

  case LINE_DELETE_SUCCESS:
    Serial.println("Запис успішно видалено!");
    oled.print("Видалено успiшно!");
    break;

  case LINE_ERROR:
    Serial.println("Помилка!");
    oled.print("Помилка!");
    break;

  case LINE_UNKNOWN_UID:
    Serial.println("Невідомий UID!");
    oled.print("Невiдомий UID!");
    break;

  case LINE_WAIT_CARD:
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

// ===== ФУНКЦІЇ НАЛАШТУВАННЯ СТОРІНКИ =====
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
          notice_success = true;
          b.reload();
        }
      }
      b.endMenu(); // не забываем завершить меню
    }

    if (b.beginMenu("Редагувати користувача"))
    {
      b.Button("scan_card_edit"_h, "Сканувати картку");
      b.Label("uid_label_edit"_h, "UID картки:");
      b.Input("surname_name_edit"_h, "Прізвище та ім'я:");
      b.Select("access_level_edit"_h, "Рівень доступу:", "низький;середній;високий");

      b.Button("edit_button"_h, "Редагувати");
      bool res;
      if (b.Confirm("conf_edit"_h, "Редагувати цей запис?", &res))
      {
        if (res)
        {
          editUser(uidStr, surname_name, access_level);
          clean_adding_form();
          notice_success = true;
          b.reload();
        }
      }
      b.endMenu(); // не забываем завершить меню
    }
    b.endGroup(); // НЕ ЗАБЫВАЕМ ЗАВЕРШИТЬ ГРУППУ
  }

  {
    sets::Group g(b, "WiFi налаштування точки");
    b.Input(kk::wifi_ap_ssid, "SSID:");
    b.Pass(kk::wifi_ap_pass, "Пароль:");
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
    rfid_state = RFID_ADD_CARD;
    break;

  case "scan_card_edit"_h:
    Serial.println("Натиснув сканувати картку edit");
    clean_adding_form();
    notice_scan_card = true;
    show_on_Display(LINE_WAIT_CARD);
    rfid_active = true;
    rfid_state = RFID_EDIT_CARD_DB;
    break;

  case "surname_name"_h:
    Serial.print("Введено ім'я: ");
    Serial.println(b.build.value);
    surname_name = b.build.value;
    break;

  case "surname_name_edit"_h:
    Serial.print("Введено ім'я: ");
    Serial.println(b.build.value);
    surname_name = b.build.value;
    break;

  case "access_level"_h:
    Serial.print("Вибрано рівень доступу: ");
    Serial.println(b.build.value);
    access_level = b.build.value.toInt();
    break;

  case "access_level_edit"_h:
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

  case "edit_button"_h:
    Serial.print("Натиснув Редагувати цей запис з UID: ");
    Serial.println(b.build.value);
    if (surname_name != "")
      notice_edit_card = true;
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
  upd.update("uid_label_edit"_h, uidStr);
  upd.update("surname_name_edit"_h, surname_name);
  upd.update("access_level_edit"_h, access_level);
  if (notice_success)
  {
    notice_success = false;
    upd.notice("Успішно!");
  }
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
  if (notice_edit_card)
  {
    notice_edit_card = false;
    upd.confirm("conf_edit"_h);
  }
  if (alert)
  {
    alert = false;
    upd.alert("ERROR!");
  }
  if (alert_check_uid_DB)
  {
    alert_check_uid_DB = false;
    upd.alert("UID вже існує в БД!");
  }
  if (alert_find_uid_DB)
  {
    alert_find_uid_DB = false;
    upd.alert("UID немає в БД!");
  }
  if (alert_surname_name)
  {
    alert_surname_name = false;
    upd.alert("Введіть прізвище та ім'я!");
  }
}

// ===== ІНШІ ФУНКЦІЇ =====
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

    case LED_OK:
      Serial.println(LED_OK);
      led_G.blink(1, 600);
      break;

    case LED_DENIED:
      Serial.println(LED_DENIED);
      led_R.blink(1, 500);
      break;
    }
  }

  if (led_R.ready() || led_G.ready())
  {
    Serial.println("Перестав мерехкотіти");
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

    case BEEP_OK:
      beep.beep(1200, 2, 200, 100);
      Serial.println("BEEP_OK");
      break;

    case BEEP_DENIED:
      beep.beep(250, 1, 500);
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

char *getChipID(uint8_t length = 12) // Дізнатись MAC ESP32
{
  static char idBuf[13]; // максимум 12 символов + '\0'

  uint64_t mac = ESP.getEfuseMac();

  // Печатаем полный MAC как 12 HEX символов
  sprintf(idBuf, "%012llX", mac);

  // Ограничиваем длину
  if (length > 12)
    length = 12;

  // Отрезаем лишнее
  idBuf[length] = '\0';

  return idBuf;
}

void initFromDB() // Ініціалізація змінних з БД
{
  HUB_ID = (uint8_t)db.get(kk::hub_id);
  led_R.invert(1);
  led_R.blink(1, 200, 0);
  led_G.invert(1);
  led_G.blink(1, 200, 0);
  led_B.invert(1);
  led_B.blink(1, 200, 0);
}

uint8_t crc8(const uint8_t *data, size_t len) // ---- CRC8 ----
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

void clean_temp_DB_values()
{
  name_DB = "";
  access_level_DB = 0;
  date_time_DB = "";
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

  uint8_t accessLevel = buf[1];                                // Рівень доступу пристрою-відправника
  uint8_t device = buf[2];                                     // ID пристрою-відправника
  uint8_t hub = buf[3];                                        // ID хаба-отримувача
  uint16_t msgId = (uint16_t(buf[4]) << 8) | uint16_t(buf[5]); // msgId (2 байти)
  uint8_t type = buf[6];                                       // Тип пакета (TYPE_REQ, TYPE_ACK тощо)
  uint8_t len = buf[7];                                        // Довжина payload

  // Перевіряємо, що пакет призначений хабу (HUB_ID) або broadcast (0)
  if (hub != HUB_ID && hub != 0)
    return; // Пакет не для цього хаба — ігноруємо

  // Захист від некоректного len
  if ((int)len > i - 8) // len більше, ніж доступно байт для payload
    return;             // Некоректний пакет, вихід

  if (type == TYPE_REQ) // Якщо пакет — запит від сканера
  {
    // Розбираємо payload: [name_len][name_bytes][uid_len][uid_bytes]
    uint8_t name_len = buf[8]; // Перший байт payload — довжина імені

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
      memcpy(tmp, &buf[9], copy_len);                           // Копіюємо байти імені з пакета
      tmp[copy_len] = '\0';                                     // Додаємо нуль-термінатор
      deviceName = String(tmp);                                 // Створюємо String з буфера
    }

    size_t uid_len_index = 9 + name_len; // Індекс байта, де починається uid_len
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

    // === ПЕРЕВІРКА 6 MAC-БАЙТ ІЗ КАРТКИ ===

    // Обчислюємо індекс початку цих 6 байтів
    size_t mac_start = uid_start + uid_len;

    // Перевіряємо, що в payload є місце для 6 байт
    if (mac_start + 6 > (size_t)(8 + len)) // 8 — заг. заголовок LoRa пакета
    {
      Serial.println("MAC bytes missing in payload");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }

    // Читаємо 6 байт
    uint8_t cardMac[6];
    memcpy(cardMac, &buf[mac_start], 6);

    // Порівнюємо з macBytes[]
    bool mac_ok = true;
    for (uint8_t j = 0; j < 6; j++)
    {
      if (cardMac[j] != macBytes[j])
      {
        mac_ok = false;
        break;
      }
    }

    if (!mac_ok)
    {
      Serial.println("MAC не співпадає! Відмова.");
      Serial.print("MAC картки: ");
      for (int j = 0; j < 6; j++)
      {
        Serial.print(cardMac[j], HEX);
        Serial.print(" ");
      }
      Serial.println();

      Serial.print("MAC ESP:    ");
      for (int j = 0; j < 6; j++)
      {
        Serial.print(macBytes[j], HEX);
        Serial.print(" ");
      }
      Serial.println();

      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }

    Serial.println("MAC збігається — ок");

    // Логування
    Serial.print("REQ від: ");
    Serial.print(device);
    Serial.print("  msgId: ");
    Serial.print(msgId);
    Serial.print("  Назва: '");
    Serial.print(deviceName);
    Serial.print("'  UID картки: ");

    String uidDeviceStr = "";
    for (uint8_t k = 0; k < uid_len; ++k) // Формуємо рядок для логу
    {
      if (uid_buf[k] < 0x10)
        uidDeviceStr += "0"; // Додаємо провідний нуль для однобайтових значень
      uidDeviceStr += String(uid_buf[k], HEX);
    }
    uidDeviceStr.toUpperCase(); // Приводимо до великих літер
    Serial.println(uidDeviceStr);
    show_on_Display(LINE_UID, uidDeviceStr);

    if (findUser(uidDeviceStr, name_DB, access_level_DB, date_time_DB)) // Перевірка наявності UID в БД
    {
      Serial.println("Прийнятий UID є в БД");

      if (access_level_DB >= accessLevel)
      {
        logger.println(sets::Logger::warn() + "UID: " + uidDeviceStr + " дозволено");
        buildAndSend(device, msgId, CMD_OPEN, NULL, 0);
      }
      else
      {
        Serial.println("Рівень доступу не ваш, відмова!");
        logger.println(sets::Logger::error() + "UID: " + uidDeviceStr + " відмова по рівню");
        buildAndSend(device, msgId, CMD_DENY, NULL, 0);
        return;
      }
    }
    else
    {
      Serial.println("Прийнятий UID відсутній в БД, відмова!");
      logger.println(sets::Logger::error() + "UID: " + uidDeviceStr + " відмова, не знайдено");
      buildAndSend(device, msgId, CMD_DENY, NULL, 0);
      return;
    }
    clean_temp_DB_values();
  }
  else // Якщо type відмінний від TYPE_REQ
  {
    Serial.print("Невідомий type "); // Логування
    Serial.println(type);
  }
}

void back_to_main_menu(bool was_change = 0)
{
  if (was_change)
  {
    back_to_home = false;
    timing_back_to_home = millis();
  }

  if (!back_to_home && millis() - timing_back_to_home > 3000)
  {
    show_on_Display(LINE_MENU, "", enc_button_state);
    back_to_home = true;
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
    switch (enc_button_state)
    {
    case 1:
      rfid_active = true;
      rfid_state = RFID_SCAN_CARD;
      show_on_Display(LINE_WAIT_CARD);
      break;

    case 2:
      rfid_active = true;
      rfid_state = RFID_FAST_ADD;
      show_on_Display(LINE_WAIT_CARD);
      break;

    case 3:
      if (!rfid_delete_confirm)
      {
        rfid_active = true;
        rfid_state = RFID_DELETE_FROM_DB;
        show_on_Display(LINE_WAIT_CARD);
      }
      else
      {
        if (deleteUserFromDB(uidStr_temp))
        {
          show_on_Display(LINE_DELETE_SUCCESS);
          blink_state = LED_OK;
          beep_state = BEEP_OK;
        }

        else
          show_on_Display(LINE_ERROR);
        back_to_main_menu(1);
        uidStr_temp = "";
      }
      break;

    default:
      break;
    }

    rfid_delete_confirm = false;
  }

  if (eb.turn())
  {
    Serial.println("turn");
    back_to_home = true;
    show_on_Display(LINE_MENU, "", enc_button_state);
    rfid_state = RFID_IDLE;
    rfid_active = false;
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
  uint64_t mac = ESP.getEfuseMac();
  for (byte i = 0; i < 6; i++)
  {                               // Наполняем ключ
    keyDefault.keyByte[i] = 0xFF; // Ключ по умолчанию 0xFFFFFFFFFFFF
    keyESP.keyByte[i] = (mac >> (8 * (5 - i))) & 0xFF;
    macBytes[i] = (mac >> (8 * (5 - i))) & 0xFF;
  }
  for (int i = 0; i < 6; i++)
  {
    Serial.printf("%02X", macBytes[i]);
  }
  Serial.println();

  oled.println("RFID -> OK");
  delay(100);

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
  delay(100);

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
  delay(100);

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
      oled.print("WiFi ");
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
  back_to_main_menu();

  //************************* РОБОТА З RFID **************************//
  if (rfid_active)
  {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
      return;

    /* Аутентификация сектора, указываем блок безопасности #7 и ключ A */
    status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &keyDefault, &(rfid.uid));
    if (status == MFRC522::STATUS_OK)
    {
      /* Запись блока, указываем блок безопасности #7 */
      uint8_t secBlockDump[16];
      // Ключ А
      for (int i = 0; i < 6; i++)
      {
        secBlockDump[i] = macBytes[i];
      }
      // Access bits
      secBlockDump[6] = 0x7F;
      secBlockDump[7] = 0x07;
      secBlockDump[8] = 0x88;
      // User byte
      secBlockDump[9] = 0xFF;
      // Key B
      for (int i = 0; i < 6; i++)
      {
        secBlockDump[10 + i] = macBytes[i];
      }

      status = rfid.MIFARE_Write(block, secBlockDump, 16); // Пишем массив в блок 7
      if (status != MFRC522::STATUS_OK)
      {                                       // Если не окэй
        Serial.println("Смена ключа error!"); // Выводим ошибку
        alert = true;
        blink_state = LED_DENIED;
        beep_state = BEEP_DENIED;
        return;
      }
      Serial.println("Смена ключа УСПЕХ!");

      /* Запись блока, указываем блок данных #6 */
      uint8_t dataToWrite[16];
      for (int i = 0; i < 6; i++)
      {
        dataToWrite[i] = macBytes[i];
      }
      dataToWrite[6] = 0xAB;
      dataToWrite[7] = 0xCD;
      for (int i = 8; i < 14; i++)
      {
        dataToWrite[i] = macBytes[i - 8];
      }
      dataToWrite[14] = 0xAB;
      dataToWrite[15] = 0xCD;
      status = rfid.MIFARE_Write(block - 1, dataToWrite, 16); // Пишем массив в блок -1
      if (status != MFRC522::STATUS_OK)
      {                                            // Если не окэй
        Serial.println("Запись в блок №6 error!"); // Выводим ошибку
        blink_state = LED_DENIED;
        beep_state = BEEP_DENIED;
      }
      Serial.println("Запись в блок №6 УСПЕХ!");
    }
    else
    {
      Serial.println("Стандартний ключ не підходить!");
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      delay(50);
    }

    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    {
      Serial.println("Не удалось перечитать карту после смены ключа");
      return;
    }

    status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_B, block, &keyESP, &(rfid.uid));
    if (status != MFRC522::STATUS_OK)
    {
      Serial.println("Помилка авторизації"); // Выводим ошибку
      return;
    }
    else
    {
      /* Чтение блока, указываем блок данных #block-1 */
      uint8_t dataBlock[18];                                  // Буфер для чтения
      uint8_t size = sizeof(dataBlock);                       // Размер буфера
      status = rfid.MIFARE_Read(block - 1, dataBlock, &size); // Читаем 6 блок в буфер
      if (status != MFRC522::STATUS_OK)
      {                               // Если не окэй
        Serial.println("Read error"); // Выводим ошибку
        return;
      }
      Serial.print("Data:"); // Выводим 16 байт в формате HEX
      for (uint8_t i = 0; i < 16; i++)
      {
        Serial.print("0x");
        Serial.print(dataBlock[i], HEX);
        Serial.print(", ");
      }
      Serial.println("");
      /* --- Проверка первых 6 байт на совпадение с macBytes[] --- */
      bool match = true;
      for (int i = 0; i < 6; i++)
      {
        if (dataBlock[i] != macBytes[i])
        {
          match = false;
          break;
        }
      }

      if (!match)
      {
        Serial.println("ERROR: MAC наповнення блоку не підходить!");
        return;
      }
    }

    uidStr = "";
    for (uint8_t i = 0; i < rfid.uid.size; i++)
    {
      if (rfid.uid.uidByte[i] < 0x10)
        uidStr += "0";
      uidStr += String(rfid.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase(); // зробити великі літери
    Serial.println("UID: " + uidStr);
    show_on_Display(LINE_UID, uidStr);

    switch (rfid_state)
    {
    case RFID_IDLE:
      break;

    case RFID_ADD_CARD:
      if (uidExists(uidStr))
      {
        Serial.println("Картка вже є в БД");
        alert_check_uid_DB = true;
        uidStr = "";
      }
      show_on_Display(LINE_MENU, "", enc_button_state);
      break;

    case RFID_EDIT_CARD_DB:
      if (findUser(uidStr, name_DB, access_level_DB, date_time_DB))
      {
        Serial.println("Користувача для редагування знайдено!");
        Serial.println("Name: " + name_DB);
        Serial.println("Level: " + String(access_level_DB));
        Serial.println("DateTime: " + date_time_DB);
        surname_name = name_DB;
        access_level = access_level_DB;
        clean_temp_DB_values();
      }
      else
      {
        clean_adding_form();
        alert_find_uid_DB = true;
      }
      show_on_Display(LINE_MENU, "", enc_button_state);
      break;

    case RFID_SCAN_CARD:
      if (findUser(uidStr, name_DB, access_level_DB, date_time_DB))
      {
        Serial.println("Користувача знайдено!");
        Serial.println("Name: " + name_DB);
        Serial.println("Level: " + String(access_level_DB));
        Serial.println("DateTime: " + date_time_DB);
        clear_area_for_menu();
        oled.setScale(1);
        oled.setCursor(0, 3);
        oled.println(name_DB);
        oled.println(access_level_DB);
        oled.println(date_time_DB);
        blink_state = LED_OK;
        beep_state = BEEP_OK;
      }
      else
      {
        clear_area_for_menu();
        show_on_Display(LINE_UNKNOWN_UID);
        blink_state = LED_DENIED;
        beep_state = BEEP_DENIED;
      }
      uidStr = "";
      clean_temp_DB_values();
      back_to_main_menu(1);
      break;

    case RFID_FAST_ADD:
      if (uidExists(uidStr))
      {
        Serial.println("Картка вже є в БД");
        clear_area_for_menu();
        oled.setScale(2);
        oled.setCursor(0, 2);
        oled.println("Був такий UID!");
        blink_state = LED_DENIED;
        beep_state = BEEP_DENIED;
      }
      else
      {
        if (addRecord(uidStr, "---- ----", 0))
        {
          Serial.println("Картку швидко додано в БД");
          clear_area_for_menu();
          oled.setScale(2);
          oled.setCursor(0, 2);
          oled.println("Додано!");
          blink_state = LED_OK;
          beep_state = BEEP_OK;
        }
        else
        {
          Serial.println("Помилка при швидкому додаванні в БД");
          clear_area_for_menu();
          show_on_Display(LINE_ERROR);
          blink_state = LED_DENIED;
          beep_state = BEEP_DENIED;
        }
      }
      uidStr = "";
      back_to_main_menu(1);
      break;

    case RFID_DELETE_FROM_DB:
      if (findUser(uidStr, name_DB, access_level_DB, date_time_DB))
      {
        Serial.println("Користувача для видалення знайдено!");
        Serial.println("Name: " + name_DB);
        Serial.println("Level: " + String(access_level_DB));
        Serial.println("DateTime: " + date_time_DB);
        clear_area_for_menu();
        oled.setScale(1);
        oled.setCursor(0, 2);
        oled.println(name_DB);
        oled.println(access_level_DB);
        oled.println(date_time_DB);
        oled.setScale(2);
        oled.print("Видалити?");
        uidStr_temp = uidStr;
        rfid_delete_confirm = true;
      }
      else
      {
        clear_area_for_menu();
        show_on_Display(LINE_UNKNOWN_UID);
        blink_state = LED_DENIED;
        beep_state = BEEP_DENIED;
        uidStr = "";
        back_to_main_menu(1);
      }
      clean_temp_DB_values();
      break;

    default:
      break;
    }

    rfid_state = RFID_IDLE;
    rfid_active = false;
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
}