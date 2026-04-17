#include <WiFi.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <stdio.h>
#include <Wire.h>
#include <DHT.h>
#include <ArduinoJson.h>

#define EPD_RST_PIN 14  // Řádek 31: IO38 odpovídá Map D3
#define EPD_BUSY_PIN 8  // Řádek 15: IO3 odpovídá Map D2
#define I2C_SDA 19      // Řádek 39: IO1 odpovídá SDA
#define I2C_SCL 20      // Řádek 38: IO2 odpovídá SCL
#define Total_Off 6

#define adds_com 0x3C
#define adds_data 0x3D
#define DHTTYPE DHT22

#define SD_CS_PIN 1
#define uS_TO_S_FACTOR 1000000ULL  // Převod na mikrosekundy

String wifi_ssid, wifi_pass, THINGNAME, MQTT_HOST, AWS_IOT_PUBLISH_TOPIC, AWS_IOT_SUBSCRIBE_TOPIC;
String aws_ca_cert, aws_client_cert, aws_priv_key;

const int sdEnablePin = 7;
int interval, analogVolts;
uint8_t DHTPin = 8;
float Temperature, Humidity, correctedVolts;

char timeStrDisplay[25];
char g_datum[12];
char g_cas[10];

unsigned char finalni_cislo[15] = {};
const unsigned char cisla[10][2] = {
  { 0xbf, 0x1f }, 
  { 0x00, 0x1f }, 
  { 0xfd, 0x17 }, 
  { 0xf5, 0x1f }, 
  { 0x47, 0x1f }, 
  { 0xf7, 0x1d }, 
  { 0xff, 0x1d }, 
  { 0x21, 0x1f }, 
  { 0xff, 0x1f }, 
  { 0xf7, 0x1f }
};
const unsigned char cislaSTeckou[10][2] = {
  { 0xbf, 0x3f }, 
  { 0x00, 0x3f }, 
  { 0xfd, 0x37 }, 
  { 0xf5, 0x3f }, 
  { 0x47, 0x3f }, 
  { 0xf7, 0x3d }, 
  { 0xff, 0x3d }, 
  { 0x21, 0x3f }, 
  { 0xff, 0x3f }, 
  { 0xf7, 0x3f }
};

DHT dht(DHTPin, DHTTYPE);
WiFiClientSecure net = WiFiClientSecure();
PubSubClient client(net);

void setup() {
  // Inicializace Serialu - USB CDC musí být v menu Tools ENABLED
  Serial.begin(115200);

  mesureVoltage();

  pinMode(Total_Off, OUTPUT);
  digitalWrite(Total_Off, HIGH);
	dht.begin();

  // Inicializace I2C na pinech 1 a 2 podle tvé tabulky
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  pinMode(EPD_BUSY_PIN, INPUT_PULLUP);
  pinMode(EPD_RST_PIN, OUTPUT);
  pinMode(sdEnablePin, OUTPUT);
  digitalWrite(sdEnablePin, HIGH);
  delay(200);

  while (!SD.begin(1)) { // 15 je pin CS (D8)
    Serial.println("Chyba inicializace karty!");
    delay(1000);
  }

  // Načtení wifi SSID a hesla z sd karty 
  File connection = SD.open("/config/connection.txt");
  if (connection) {
    // Přečte první řádek (SSID)
    wifi_ssid = connection.readStringUntil('\n');
    wifi_ssid.trim();

    // Přečte druhý řádek (Heslo)
    wifi_pass = connection.readStringUntil('\n');
    wifi_pass.trim();

    connection.close();
    Serial.println("WiFi udaje nacteny:");
  }

  File awsValues = SD.open("/config/aws-values.txt");
  if (awsValues) {
    // Přečte první řádek (SSID)
    THINGNAME = awsValues.readStringUntil('\n');
    THINGNAME.trim();

    // Přečte druhý řádek (Heslo)
    MQTT_HOST = awsValues.readStringUntil('\n');
    MQTT_HOST.trim();

    // Přečte druhý řádek (Heslo)
    AWS_IOT_PUBLISH_TOPIC = awsValues.readStringUntil('\n');
    AWS_IOT_PUBLISH_TOPIC.trim();

    // Přečte druhý řádek (Heslo)
    AWS_IOT_SUBSCRIBE_TOPIC = awsValues.readStringUntil('\n');
    AWS_IOT_SUBSCRIBE_TOPIC.trim();

    // Přečte druhý řádek (Heslo)
    interval = awsValues.readStringUntil('\n').toInt();

    awsValues.close();
    Serial.println("WiFi udaje nacteny:");
  }

   File cert1 = SD.open("/config/ca_cert.txt");
  if (cert1) {
    aws_ca_cert = cert1.readString();
    cert1.close();
    aws_ca_cert.trim();
  }

  // Načtení Client Cert
  File cert2 = SD.open("/config/client_cert.txt");
  if (cert2) {
    aws_client_cert = cert2.readString();
    cert2.close();
    aws_client_cert.trim();
  }

  // Načtení Private Key
  File priv_key = SD.open("/config/priv_key.txt");
  if (priv_key) {
    aws_priv_key = priv_key.readString();
    priv_key.close();
    aws_priv_key.trim();
  }
  digitalWrite(sdEnablePin, LOW);

  sensorReadings();
  zjistiAVypisCisloNaDisplej(); 
  EPD_1in9_init();
  EPD_1in9_lut_5S();
  
  Serial.println("Zapisuji data...");
  EPD_1in9_Write_Screen(finalni_cislo);

  EPD_1in9_sleep();
  setupWiFi(); // Zavolá pripojení k síti
  NTPConnect();
  connectAWS();
  
  publishMessage();
  writeData();
  
  digitalWrite(Total_Off, LOW);
  Serial.println("Hotovo, jdu spat.");
  esp_sleep_enable_timer_wakeup(interval * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void loop() {}

void setupWiFi() {
  Serial.print("Pripojuji se k WiFi: ");
  Serial.println(wifi_ssid);

  WiFi.begin(wifi_ssid, wifi_pass);
  delay(200);

  // Cekani na pripojeni (max 20 sekund)
  int pokusy = 0;
  while (WiFi.status() != WL_CONNECTED && pokusy < 40) {
    delay(500);
    Serial.print(".");
    pokusy++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi pripojena!");
    Serial.print("IP adresa: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nPripojeni se nezdarilo. Zkontroluj heslo.");
  }
}

void connectAWS() {
  // Nastavení certifikátů
  net.setCACert(aws_ca_cert.c_str());
  net.setCertificate(aws_client_cert.c_str());
  net.setPrivateKey(aws_priv_key.c_str());
  
  client.setServer(MQTT_HOST.c_str(), 8883);
  
  Serial.print("Pripojuji se k AWS IoT...");
  for (int attempts = 0; attempts < 30; attempts++) {
    if (client.connect(THINGNAME.c_str())) {
      // Subscribe to MQTT topic
      client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC.c_str());
      Serial.println("AWS IoT Connected!");
      delay(1000);
      return;
    }
    
    // Pokud se nepodařilo, počkáme a zkusíme to v další otáčce
    Serial.print(".");
    delay(1000);
  }
  Serial.println(" Pripojeno k AWS!");
}

void NTPConnect() {
  // Set time using SNTP
  Serial.print("Setting time using SNTP");
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  while (now < 1510592825) { // January 13, 2018
    delay(500);
    Serial.print(".");
    Serial.print(now);
    now = time(nullptr);
  }
  Serial.println("done!");
}

void publishMessage() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  char timeStr[25];
  // např. "2025-11-30 15:42:10"
  strftime(timeStr, sizeof(timeStr), "%d-%m-%Y %H:%M:%S", timeinfo);
  strcpy(timeStrDisplay, timeStr);
  char* castDatum = strtok(timeStr, " "); 
  char* castCas = strtok(NULL, " "); 

  // Uložíme do globálních proměnných, aby k nim měly přístup ostatní funkce
  if (castDatum != NULL) strcpy(g_datum, castDatum);
  if (castCas != NULL)   strcpy(g_cas, castCas);
  
  // Create JSON document for message
  StaticJsonDocument<300> doc;
  doc["date"] = g_datum;      // Teď můžeš poslat datum zvlášť
  doc["time"] = g_cas;        // A čas zvlášť
  doc["humidity"] = Humidity;
  doc["temperature"] = Temperature;
  doc["Voltage"] = correctedVolts;

  // Serialize JSON document to string
  char jsonBuffer[300];
  serializeJson(doc, jsonBuffer);
  size_t n = serializeJson(doc, jsonBuffer);
  if (n == 0) {
      Serial.println("CHYBA: Serializace JSON selhala (dokument je prázdný)");
      return;
  }

  // Publish message to MQTT topic
  client.publish(AWS_IOT_PUBLISH_TOPIC.c_str(), jsonBuffer);
}

void writeData(){
  char fileName[20];
  snprintf(fileName, sizeof(fileName), "/data/%c%c-%c%c-%c%c.csv", g_datum[0], g_datum[1], g_datum[3], g_datum[4], g_datum[8], g_datum[9]);
  digitalWrite(sdEnablePin, HIGH);
  delay(100);
  File dataFile = SD.open(fileName, FILE_WRITE);
  if (dataFile) {
    // 2. Pokud je soubor prázdný (velikost je 0), zapíšeme hlavičku
    if (dataFile.size() == 0) {
      dataFile.println("Cas;Teplota[C];Vlhkost[%]; Napětí[mV]");
      Serial.println("Zapsana hlavicka CSV.");
    }

    // 3. Zápis samotných dat (oddělených středníkem)
    dataFile.print(g_cas);
    dataFile.print(";");
    dataFile.print(Temperature); // Tady by byla tvoje proměnná s teplotou
    dataFile.print(";");
    dataFile.println(Humidity);   // println zajistí odřádkování na nový řádek
    dataFile.print(";");
    dataFile.print(correctedVolts); // Tady by byla tvoje proměnná s teplotou

    // 4. Uzavření souboru (DŮLEŽITÉ - bez toho se data na kartu fyzicky neuloží!)
    dataFile.close();
    Serial.println("Data zapsána.");
  } else {
    Serial.println("Chyba při otevírání data.csv!");
  }
  digitalWrite(sdEnablePin, LOW);
}

void EPD_1in9_ReadBusy(void) {
  Serial.print("Cekam na BUSY...");
  delay(50);
  // Pokud se zde program sekne, zkus zmenit na == 0
  while(digitalRead(EPD_BUSY_PIN) == 0) {
    delay(10);
  }
  Serial.println(" OK");
}

void EPD_1in9_init(void) {
  digitalWrite(EPD_RST_PIN, 1);
  delay(100);
  digitalWrite(EPD_RST_PIN, 0);
  delay(20);
  digitalWrite(EPD_RST_PIN, 1);
  delay(100);

  Wire.beginTransmission(adds_com);
  Wire.write(0x2B); // POWER_ON
  Wire.endTransmission();
  delay(20);

  Wire.beginTransmission(adds_com);
  Wire.write(0xA7); // boost
  Wire.write(0xE0); // TSON 
  Wire.endTransmission();
  delay(20);
}

void EPD_1in9_Write_Screen(unsigned char *image) {
  Wire.beginTransmission(adds_com);
  Wire.write(0xAC); Wire.write(0x2B); Wire.write(0x40);
  Wire.write(0xA9); Wire.write(0xA8);
  Wire.endTransmission();

  Wire.beginTransmission(adds_data);
  for(char j = 0 ; j < 15 ; j++ ) Wire.write(image[j]);
  Wire.write(0x00);
  Wire.endTransmission();

  Wire.beginTransmission(adds_com);
  Wire.write(0xAB); Wire.write(0xAA); Wire.write(0xAF);
  Wire.endTransmission();

  EPD_1in9_ReadBusy();

  Wire.beginTransmission(adds_com);
  Wire.write(0xAE); Wire.write(0x28); Wire.write(0xAD);
  Wire.endTransmission();
}

void EPD_1in9_lut_5S(void) {
  Wire.beginTransmission(adds_com);
  Wire.write(0x82); Wire.write(0x28); Wire.write(0x20); Wire.write(0xA8);
  Wire.write(0xA0); Wire.write(0x50); Wire.write(0x65);
  Wire.endTransmission();
}

void EPD_1in9_sleep(void) {
  Wire.beginTransmission(adds_com);
  Wire.write(0x28); // POWER_OFF
  EPD_1in9_ReadBusy();
  Wire.write(0xAD); // DEEP_SLEEP
  Wire.endTransmission();
}

void zjistiAVypisCisloNaDisplej() {
  int value = round(Temperature * 100);
  int value2 = round(Humidity * 100); 

  int c0 = (value / 10000) % 10;
  int c1 = (value / 1000) % 10;
  int c2 = (value / 100) % 10;
  int c3 = (value / 10) % 10;
  int c5 = (value2 / 1000) % 10;
  int c6 = (value2 / 100) % 10;
  int c7 = (value2 / 10) % 10;

  if (c0 == 1){
    finalni_cislo[0] = 0xff;
  }
  else{
    finalni_cislo[0] = 0x00;
  }
  finalni_cislo[1] = cisla[c1][0];
  finalni_cislo[2] = cisla[c1][1];
  finalni_cislo[3] = cislaSTeckou[c2][0];
  finalni_cislo[4] = cislaSTeckou[c2][1];
  finalni_cislo[5] = cisla[c5][0];
  finalni_cislo[6] = cisla[c5][1];
  finalni_cislo[7] = cislaSTeckou[c6][0];
  finalni_cislo[8] = cislaSTeckou[c6][1];
  finalni_cislo[9] = cislaSTeckou[c7][0]; 
  finalni_cislo[10] = cislaSTeckou[c7][1];
  finalni_cislo[11] = cisla[c3][0]; 
  finalni_cislo[12] = cisla[c3][1];
  finalni_cislo[13] = 0x05; 
  finalni_cislo[14] = 0x00; 
}

void sensorReadings(){
  Humidity = dht.readHumidity();
  Temperature = dht.readTemperature();
	Serial.print("Humidity: ");
	Serial.println(Humidity);
	Serial.print("Temperature: ");
	Serial.println(Temperature);

  // Check if any reads failed and exit early (to try again).
  if (isnan(Humidity) || isnan(Temperature)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    return;
  }
}

void mesureVoltage(){
  analogVolts = analogReadMilliVolts(0);
  correctedVolts = (analogVolts * 2.0) / 1000.0;
}








// #include <WiFi.h>
// #include <WiFiClientSecure.h>
// #include <PubSubClient.h>
// #include <ArduinoJson.h>
// #include <Wire.h>
// #include "DHT.h"
// #include <time.h>  // Standardní ESP32 knihovna pro čas
// #include <SPI.h>
// #include <SD.h>

// #define DHTTYPE DHT22

// // --- PINY PRO FIREBEETLE 2 ESP32-C6 ---
// #define DHTPin 15        // Pin D2
// #define CS_PIN 10        // Pin D9 (Standardní CS pro SPI na C6)
// #define SD_ENABLE_PIN 0  // Pokud tvoje deska vyžaduje speciální enable (často GPIO0/D11)

// // E-PAPER PINY (Upraveno pro ESP32-C6)
// #define EPD_RST_PIN 18  // Pin D13
// #define EPD_BUSY_PIN 1  // Pin D12
// #define adds_com 0x3C
// #define adds_data 0x3D

// DHT dht(DHTPin, DHTTYPE);
// float Temperature;
// float Humidity;
// int interval;

// char timeStrDisplay[25];
// char g_datum[12];
// char g_cas[10];

// // WiFi a AWS proměnné
// String wifi_ssid, wifi_pass;
// String aws_ca_cert, aws_client_cert, aws_priv_key;
// String THINGNAME, MQTT_HOST, AWS_IOT_PUBLISH_TOPIC, AWS_IOT_SUBSCRIBE_TOPIC;

// unsigned long lastMillisMessage = 0;
// WiFiClientSecure net;
// PubSubClient client(net);

// unsigned char finalni_cislo[15] = {};

// // Fonty (ponechány v .ino, pokud nejsou v externím souboru)
// const unsigned char cisla[10][2] = {
//   { 0xbf, 0x1f }, { 0x00, 0x1f }, { 0xfd, 0x17 }, { 0xf5, 0x1f }, { 0x47, 0x1f }, { 0xf7, 0x1d }, { 0xff, 0x1d }, { 0x21, 0x1f }, { 0xff, 0x1f }, { 0xf7, 0x1f }
// };

// const unsigned char cislaSTeckou[10][2] = {
//   { 0xbf, 0x3f }, { 0x00, 0x3f }, { 0xfd, 0x37 }, { 0xf5, 0x3f }, { 0x47, 0x3f }, { 0xf7, 0x3d }, { 0xff, 0x3d }, { 0x21, 0x3f }, { 0xff, 0x3f }, { 0xf7, 0x3f }
// };

// // --- PROTOTYPY FUNKCÍ ---
// void connectAWS();
// void NTPConnect();
// void zjistiAVypisCisloNaDisplej();
// void EPD_1in9_init();
// void EPD_1in9_lut_5S();
// void EPD_1in9_Write_Screen(unsigned char *image);
// void EPD_1in9_ReadBusy();
// void EPD_1in9_sleep();

// void setup() {
//   Serial.begin(9600);
//   dht.begin();

//   pinMode(SD_ENABLE_PIN, OUTPUT);
//   digitalWrite(SD_ENABLE_PIN, HIGH);
//   delay(100);

//   // Na C6 je standardní SPI na GPIO 18, 19, 20. CS je 10.
//   if (!SD.begin(CS_PIN)) {
//     Serial.println("Chyba inicializace SD karty!");
//   } else {
//     // Načítání konfigurací (zkráceno pro přehlednost)
//     File connection = SD.open("/config/connection.txt");
//     if (connection) {
//       wifi_ssid = connection.readStringUntil('\n');
//       wifi_ssid.trim();
//       wifi_pass = connection.readStringUntil('\n');
//       wifi_pass.trim();
//       connection.close();
//     }

//     // Načtení AWS a certifikátů (použij stejnou logiku jako v původním kódu)
//     // ... (zde doplň načítání certifikátů ze souborů do Stringů)
//   }

//   digitalWrite(SD_ENABLE_PIN, LOW);

//   connectAWS();

//   // Inicializace I2C a Displeje
//   Wire.begin(4, 5);  // SDA=D2(4), SCL=D1(5)
//   pinMode(EPD_BUSY_PIN, INPUT);
//   pinMode(EPD_RST_PIN, OUTPUT);

//   // Měření a zobrazení
//   Humidity = dht.readHumidity();
//   Temperature = dht.readTemperature();

//   zjistiAVypisCisloNaDisplej();
//   EPD_1in9_init();
//   EPD_1in9_lut_5S();
//   EPD_1in9_Write_Screen(finalni_cislo);
//   EPD_1in9_sleep();
// }

// void loop() {
// }

// void NTPConnect() {
//   Serial.print("Nastavuji čas přes SNTP...");
//   // ESP32 formát pro TZ string a servery
//   configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

//   time_t now = time(nullptr);
//   int retry = 0;
//   while (now < 1510592825 && retry < 20) {
//     delay(500);
//     Serial.print(".");
//     now = time(nullptr);
//     retry++;
//   }
//   Serial.println(" hotovo!");
// }

// void connectAWS() {
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

//   Serial.println("Pripojuji k WiFi...");
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }

//   NTPConnect();

//   // --- OPRAVA PRO ESP32 (mbedTLS místo BearSSL) ---
//   net.setCACert(aws_ca_cert.c_str());
//   net.setCertificate(aws_client_cert.c_str());
//   net.setPrivateKey(aws_priv_key.c_str());

//   client.setServer(MQTT_HOST.c_str(), 8883);

//   Serial.println("Pripojuji k AWS IoT...");
//   if (client.connect(THINGNAME.c_str())) {
//     Serial.println("AWS Connected!");
//   } else {
//     Serial.print("AWS Failed, rc=");
//     Serial.println(client.state());
//   }
// }

// void publishMessage() {
//   time_t now = time(nullptr);
//   struct tm timeinfo;
//   localtime_r(&now, &timeinfo);

//   char timeStr[25];
//   strftime(timeStr, sizeof(timeStr), "%d-%m-%Y %H:%M:%S", &timeinfo);

//   // ArduinoJson v7 syntaxe
//   JsonDocument doc;
//   doc["humidity"] = Humidity;
//   doc["temperature"] = Temperature;
//   doc["time"] = timeStr;

//   char jsonBuffer[256];
//   serializeJson(doc, jsonBuffer);
//   client.publish(AWS_IOT_PUBLISH_TOPIC.c_str(), jsonBuffer);
// }

// void zjistiAVypisCisloNaDisplej() {
//   int value = round(Temperature * 100);
//   int value2 = round(Humidity * 100);

//   int c1 = (value / 1000) % 10;  // Desítky
//   int c2 = (value / 100) % 10;   // Jednotky
//   int c3 = (value / 10) % 10;    // Desetiny

//   int c5 = (value2 / 1000) % 10;  // Vlhkost desítky
//   int c6 = (value2 / 100) % 10;   // Vlhkost jednotky
//   int c7 = (value2 / 10) % 10;    // Vlhkost desetiny

//   // Reset pole
//   memset(finalni_cislo, 0, 15);

//   finalni_cislo[1] = cisla[c1][0];
//   finalni_cislo[2] = cisla[c1][1];
//   finalni_cislo[3] = cislaSTeckou[c2][0];
//   finalni_cislo[4] = cislaSTeckou[c2][1];
//   finalni_cislo[5] = cisla[c5][0];
//   finalni_cislo[6] = cisla[c5][1];
//   finalni_cislo[7] = cislaSTeckou[c6][0];
//   finalni_cislo[8] = cislaSTeckou[c6][1];

//   finalni_cislo[9] = cislaSTeckou[c7][0];
//   finalni_cislo[10] = cislaSTeckou[c7][1];
//   finalni_cislo[11] = cisla[c3][0];
//   finalni_cislo[12] = cisla[c3][1];
//   finalni_cislo[13] = 0x05;
// }

// // --- E-PAPER FUNKCE (Zůstávají beze změny, jen přidány do celku) ---
// void EPD_1in9_ReadBusy(void) {
//   while (1) {  //=1 BUSY;
//     if (digitalRead(EPD_BUSY_PIN) == 1)
//       break;
//     delay(1);
//   }
// }

// void EPD_1in9_init(void) {
//   digitalWrite(EPD_RST_PIN, 1);
//   delay(200);
//   digitalWrite(EPD_RST_PIN, 0);
//   delay(20);
//   digitalWrite(EPD_RST_PIN, 1);
//   delay(200);

//   Wire.beginTransmission(adds_com);
//   Wire.write(0x2B);
//   Wire.endTransmission();
//   delay(10);
//   Wire.beginTransmission(adds_com);
//   Wire.write(0xA7);
//   Wire.write(0xE0);
//   Wire.endTransmission();
// }

// void EPD_1in9_Write_Screen(unsigned char *image) {
//   Wire.beginTransmission(adds_com);
//   Wire.write(0xAC);
//   Wire.write(0x2B);
//   Wire.write(0x40);
//   Wire.write(0xA9);
//   Wire.write(0xA8);
//   Wire.endTransmission();

//   Wire.beginTransmission(adds_data);
//   for (int j = 0; j < 15; j++) Wire.write(image[j]);
//   Wire.write(0x00);
//   Wire.endTransmission();

//   Wire.beginTransmission(adds_com);
//   Wire.write(0xAB);
//   Wire.write(0xAA);
//   Wire.write(0xAF);
//   Wire.endTransmission();
//   EPD_1in9_ReadBusy();
// }

// void EPD_1in9_sleep(void) {
//   Wire.beginTransmission(adds_com);
//   Wire.write(0x28);
//   EPD_1in9_ReadBusy();
//   Wire.write(0xAD);
//   Wire.endTransmission();
// }

// void EPD_1in9_lut_5S(void) {
//   Wire.beginTransmission(adds_com);
//   Wire.write(0x82);
//   Wire.write(0x28);
//   Wire.write(0x20);
//   Wire.write(0xA8);
//   Wire.write(0xA0);
//   Wire.write(0x50);
//   Wire.write(0x65);
//   Wire.endTransmission();
// }