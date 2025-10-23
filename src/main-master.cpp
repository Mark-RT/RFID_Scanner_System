#include <SPI.h>

#include "wifiupdate.h"
wifiupdate ota(0);
bool wifi_flag = true;
bool ota_flag = false;

#include <MFRC522.h>
#define RC522_SS_PIN 27
#define RC522_RST_PIN 13
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);
MFRC522::MIFARE_Key key;    // Объект ключа
MFRC522::StatusCode status; // Объект статуса

void setup()
{
  Serial.begin(115200);
  SPI.begin();

  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max); // Установка усиления антенны
  rfid.PCD_AntennaOff();                    // Перезагружаем антенну
  rfid.PCD_AntennaOn();                     // Включаем антенну
  for (byte i = 0; i < 6; i++)
  {                        // Наполняем ключ
    key.keyByte[i] = 0xFF; // Ключ по умолчанию 0xFFFFFFFFFFFF
  }
  Serial.println("RFID OK!");
}

void loop()
{
  static uint32_t rebootTimer = millis(); // Важный костыль против зависания модуля!
  if (millis() - rebootTimer >= 1000)
  {                                    // Таймер с периодом 1000 мс
    rebootTimer = millis();            // Обновляем таймер
    digitalWrite(RC522_RST_PIN, HIGH); // Сбрасываем модуль
    delayMicroseconds(2);              // Ждем 2 мкс
    digitalWrite(RC522_RST_PIN, LOW);  // Отпускаем сброс
    rfid.PCD_Init();                   // Инициализируем заного
  }

  ota.loops(ota_flag);

  if (!rfid.PICC_IsNewCardPresent())
    return; // Если новая метка не поднесена - вернуться в начало loop
  if (!rfid.PICC_ReadCardSerial())
    return; // Если метка не читается - вернуться в начало loop

  Serial.print("UID: ");
  for (uint8_t i = 0; i < 4; i++)
  {                                         // Цикл на 4 итерации
    Serial.print("0x");                     // В формате HEX
    Serial.print(rfid.uid.uidByte[i], HEX); // Выводим UID по байтам
    Serial.print(", ");
  }
  Serial.println("");

  /* Аутентификация сектора, указываем блок безопасности #7 и ключ A */
  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 7, &key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK)
  {                               // Если не окэй
    Serial.println("Auth error"); // Выводим ошибку
    return;
  }

  /* Запись блока, указываем блок данных #6 */
  uint8_t dataToWrite[16] = {// Массив на запись в блок
                             0x00, 0x00, 0x00, 0x00,
                             0xAA, 0xBB, 0xCC, 0xDD,
                             0xAA, 0xBB, 0xCC, 0xDD,
                             0x00, 0x00, 0x00, 0x00};
  status = rfid.MIFARE_Write(6, dataToWrite, 16); // Пишем массив в блок 6
  if (status != MFRC522::STATUS_OK)
  {                                // Если не окэй
    Serial.println("Write error"); // Выводим ошибку
    return;
  }
  /* Чтение блока, указываем блок данных #6 */
  uint8_t dataBlock[18];                          // Буфер для чтения
  uint8_t size = sizeof(dataBlock);               // Размер буфера
  status = rfid.MIFARE_Read(6, dataBlock, &size); // Читаем 6 блок в буфер
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
  rfid.PICC_HaltA(); // Завершаем работу с меткой
  rfid.PCD_StopCrypto1();
}