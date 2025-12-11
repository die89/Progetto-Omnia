#include <WiFiS3.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// =============================
// CONFIGURAZIONE WIFI
// =============================
char ssid[] = "ClayPaky Guest";
char pass[] = "ClayPaky1528";

// =============================
// CONFIGURAZIONE THINGSPEAK
// =============================
const char* thingspeakHost = "api.thingspeak.com";
const int   thingspeakPort = 80;

const long  CHANNEL_ID     = 2862116;
const char* READ_API_KEY   = "IZCUGF40MLOCNBG9"; // per leggere field1 e field2
const char* WRITE_API_KEY  = "FBHJWWWILCJK3YFT"; // per scrivere field2..8

// =============================
// RELÈ / POMPA
// =============================
const int RELAY_PIN        = 7;

// Se il tuo modulo è attivo LOW, scambia questi due livelli
const int RELAY_ON_LEVEL   = HIGH;
const int RELAY_OFF_LEVEL  = LOW;

bool pumpState      = false;  // stato reale pompa
bool desiredPump    = false;  // stato richiesto da ThingSpeak

unsigned long lastPumpChangeMillis = 0; // per ignorare rumore BME dopo cambio pompa

// =============================
// BME280
// =============================
Adafruit_BME280 bmeTesta; // indirizzo 0x76
Adafruit_BME280 bmeBase;  // indirizzo 0x77

float tTesta = NAN, hTesta = NAN, pTesta = NAN;
float tBase  = NAN, hBase  = NAN, pBase  = NAN;
bool  sensorsDataValid = false;

// =============================
// TIMING
// =============================
const unsigned long READ_INTERVAL_MS  = 1000;   // lettura comando + BME
const unsigned long WRITE_INTERVAL_MS = 60000;  // scrittura su ThingSpeak (60 s)

unsigned long lastReadMillis  = 0;
unsigned long lastWriteMillis = 0;

// =============================
// ERRORI
// =============================
WiFiClient client;
int  bmeErrorCount       = 0;
const int MAX_BME_ERRORS = 10;  // dopo 10 letture sballate consecutive -> reset

// =============================
// PROTOTIPI
// =============================
void connectToWiFi();
bool initBME();
int  readPumpCommandFromThingSpeak();
int  readPumpStateFromThingSpeak();
bool readBME280();
bool writeAllToThingSpeak();
void softwareReset();

// =============================
// SUPPORTO
// =============================

// Reset completo di Arduino (riavvio)
void softwareReset() {
  Serial.println(">>> RESET COMPLETO ARDUINO (troppi errori sensori) <<<");
  Serial.flush();
  NVIC_SystemReset();   // funzione ufficiale su UNO R4 (Cortex-M4)
}

void applyPumpState(bool state) {
  pumpState = state;
  lastPumpChangeMillis = millis();

  if (state) {
    digitalWrite(RELAY_PIN, RELAY_ON_LEVEL);
    Serial.println("Pompa: ON");
  } else {
    digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);
    Serial.println("Pompa: OFF");
  }
}

void connectToWiFi() {
  Serial.print("Connessione al WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, pass);

  int tentativi = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    tentativi++;
    if (tentativi > 40) { // ~20 s
      Serial.println("\nErrore connessione WiFi, proseguo ma senza rete.");
      return;
    }
  }

  Serial.println("\nWiFi connesso!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

bool initBME() {
  bool ok = true;
  bmeErrorCount = 0;

  Serial.println("Inizializzazione BME280 TESTA (0x76)...");
  if (!bmeTesta.begin(0x76)) {
    Serial.println("!!! BME280 TESTA non trovato a 0x76");
    ok = false;
  } else {
    Serial.println("BME280 TESTA ok.");
  }

  Serial.println("Inizializzazione BME280 BASE (0x77)...");
  if (!bmeBase.begin(0x77)) {
    Serial.println("!!! BME280 BASE non trovato a 0x77");
    ok = false;
  } else {
    Serial.println("BME280 BASE ok.");
  }

  return ok;
}

// =============================
// THINGSPEAK: LETTURA COMANDO (field1)
// =============================

// ritorna 0/1 in caso di successo, oppure ultimo comando noto se ci sono errori
int readPumpCommandFromThingSpeak() {
  static int lastCmd = 0; // ricordiamo l’ultimo comando valido

  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi non disponibile, uso ultimo comando.");
      return lastCmd;
    }
  }

  if (!client.connect(thingspeakHost, thingspeakPort)) {
    Serial.println("Connessione a ThingSpeak fallita (read), uso ultimo comando.");
    return lastCmd;
  }

  String url = "/channels/";
  url += CHANNEL_ID;
  url += "/fields/1/last.txt?api_key=";
  url += READ_API_KEY;

  Serial.print("GET (read comando): ");
  Serial.println(url);

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + thingspeakHost + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (!client.available()) {
    if (millis() - timeout > 5000) {
      Serial.println("Timeout risposta ThingSpeak (read comando), uso ultimo comando.");
      client.stop();
      return lastCmd;
    }
  }

  // Salta header
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  String payload = client.readStringUntil('\n');
  payload.trim();
  client.stop();

  Serial.print("field1 letto: '");
  Serial.print(payload);
  Serial.println("'");

  if (payload.length() == 0) {
    Serial.println("Payload vuoto, uso ultimo comando.");
    return lastCmd;
  }

  if (payload == "0") {
    lastCmd = 0;
    return 0;
  }
  if (payload == "1") {
    lastCmd = 1;
    return 1;
  }

  int val = payload.toInt();
  if (val == 0 || val == 1) {
    lastCmd = val;
    return val;
  }

  Serial.println("Valore non valido, uso ultimo comando.");
  return lastCmd;
}

// =============================
// THINGSPEAK: LETTURA STATO POMPA (field2) AL BOOT
// =============================

int readPumpStateFromThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi non disponibile al boot, assumo pompa OFF.");
      return 0;
    }
  }

  if (!client.connect(thingspeakHost, thingspeakPort)) {
    Serial.println("Connessione TS fallita (read stato pompa boot).");
    return 0;
  }

  String url = "/channels/";
  url += CHANNEL_ID;
  url += "/fields/2/last.txt?api_key=";
  url += READ_API_KEY;

  Serial.print("GET (read stato pompa boot): ");
  Serial.println(url);

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + thingspeakHost + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (!client.available()) {
    if (millis() - timeout > 3000) {
      Serial.println("Timeout lettura stato pompa boot, assumo OFF.");
      client.stop();
      return 0;
    }
  }

  // Salta header
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  String payload = client.readStringUntil('\n');
  payload.trim();
  client.stop();

  Serial.print("STATO POMPA SU TS AL BOOT = '");
  Serial.print(payload);
  Serial.println("'");

  if (payload == "1") return 1;
  if (payload == "0") return 0;

  return 0; // fallback di sicurezza
}

// =============================
// BME280: LETTURA ROBUSTA
// =============================

bool readBME280() {
  unsigned long now = millis();
  bool ignoreErrors = (now - lastPumpChangeMillis) < 3000; // 3 s dopo cambio pompa

  tTesta = bmeTesta.readTemperature();
  hTesta = bmeTesta.readHumidity();
  pTesta = bmeTesta.readPressure() / 100.0F;

  tBase  = bmeBase.readTemperature();
  hBase  = bmeBase.readHumidity();
  pBase  = bmeBase.readPressure() / 100.0F;

  sensorsDataValid = true;

  // 1) NaN
  if (isnan(tTesta) || isnan(hTesta) || isnan(pTesta) ||
      isnan(tBase)  || isnan(hBase) || isnan(pBase)) {
    Serial.println("Errore BME: NaN.");
    sensorsDataValid = false;
  }

  // 2) Range plausibile (solo fuori finestra rumore pompa)
  if (sensorsDataValid && !ignoreErrors) {
    bool fuoriRange = false;

    if (tTesta < -40 || tTesta > 80)   fuoriRange = true;
    if (tBase  < -40 || tBase  > 80)   fuoriRange = true;

    if (hTesta < 0   || hTesta > 100)  fuoriRange = true;
    if (hBase  < 0   || hBase  > 100)  fuoriRange = true;

    if (pTesta < 300 || pTesta > 1100) fuoriRange = true;
    if (pBase  < 300 || pBase > 1100)  fuoriRange = true;

    if (fuoriRange) {
      Serial.println("Errore BME: valori fuori range.");
      sensorsDataValid = false;
    }
  }

  if (!sensorsDataValid) {
    if (ignoreErrors) {
      Serial.println("Valori BME strani ma ignorati (vicino a cambio pompa).");
    } else {
      bmeErrorCount++;
      Serial.print("Conteggio errori BME: ");
      Serial.println(bmeErrorCount);

      if (bmeErrorCount >= MAX_BME_ERRORS) {
        Serial.println("Troppi errori BME: RESET scheda.");
        softwareReset();
      }
    }
  } else {
    bmeErrorCount = 0;
  }

  Serial.println("=== Letture BME ===");
  Serial.print("TESTA -> T: "); Serial.print(tTesta); Serial.print(" °C, H: ");
  Serial.print(hTesta); Serial.print(" %, P: "); Serial.print(pTesta); Serial.println(" hPa");

  Serial.print("BASE  -> T: "); Serial.print(tBase); Serial.print(" °C, H: ");
  Serial.print(hBase); Serial.print(" %, P: "); Serial.print(pBase); Serial.println(" hPa");
  Serial.println("====================");

  if (ignoreErrors) {
    sensorsDataValid = false; // non invio questi valori su TS
  }

  return sensorsDataValid;
}

// =============================
// THINGSPEAK: SCRITTURA DATI
// =============================

bool writeAllToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi non disponibile, salto scrittura TS.");
      return false;
    }
  }

  if (!client.connect(thingspeakHost, thingspeakPort)) {
    Serial.println("Connessione a ThingSpeak fallita (write).");
    return false;
  }

  // field2 = stato pompa
  // field3..8 = sensori
  String url = "/update?api_key=";
  url += WRITE_API_KEY;
  url += "&field2="; url += String(pumpState ? 1 : 0);

  if (sensorsDataValid) {
    url += "&field3="; url += String(tTesta, 2);
    url += "&field4="; url += String(hTesta, 2);
    url += "&field5="; url += String(pTesta, 2);
    url += "&field6="; url += String(tBase, 2);
    url += "&field7="; url += String(hBase, 2);
    url += "&field8="; url += String(pBase, 2);
  }

  Serial.print("GET (write): ");
  Serial.println(url);

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + thingspeakHost + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (!client.available()) {
    if (millis() - timeout > 5000) {
      Serial.println("Timeout risposta ThingSpeak (write).");
      client.stop();
      return false;
    }
  }

  String line = client.readStringUntil('\n');
  Serial.print("Risposta write: ");
  Serial.println(line);

  client.stop();
  return true;
}

// =============================
// SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(RELAY_PIN, OUTPUT);
  applyPumpState(false);   // per sicurezza, all'accensione la tengo OFF
                           // poi la riporto allo stato TS appena possibile

  Wire.begin();
  initBME();
  connectToWiFi();

  // 🔥 Ripristino stato pompa dal cloud (field2)
  int lastState = readPumpStateFromThingSpeak();
  desiredPump = (lastState == 1);
  applyPumpState(desiredPump);

  lastReadMillis  = millis();
  lastWriteMillis = 0;
}

// =============================
// LOOP
// =============================
void loop() {
  unsigned long now = millis();

  // 1) Ogni READ_INTERVAL_MS: comando + BME
  if (now - lastReadMillis >= READ_INTERVAL_MS) {
    lastReadMillis = now;

    int cmd = readPumpCommandFromThingSpeak();
    Serial.print("Comando (field1) attuale: ");
    Serial.println(cmd);

    desiredPump = (cmd == 1);
    if (desiredPump != pumpState) {
      Serial.println("Cambio stato pompa richiesto da ThingSpeak.");
      applyPumpState(desiredPump);
    }

    readBME280();
  }

  // 2) Ogni WRITE_INTERVAL_MS: invio dati a ThingSpeak
  if (now - lastWriteMillis >= WRITE_INTERVAL_MS) {
    lastWriteMillis = now;
    Serial.println("Invio dati a ThingSpeak...");
    writeAllToThingSpeak();
  }

  delay(50);
}
