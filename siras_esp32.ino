/*
 * SIRAS · Kilimagua — Firmware ESP32 <-> Firebase Realtime Database
 *
 * Librerías (Gestor de librerías de Arduino IDE):
 *   - "Firebase Arduino Client Library for ESP8266 and ESP32" (Mobizt)
 *   - "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
 * Placa: ESP32 Dev Module
 *
 * Esquema de datos (el mismo que usa la web):
 *   /siras/sensores   { humedad %, temperatura °C, bateria %, nivelTanque L, ultimaLectura ms }
 *   /siras/actuadores { modo "auto"|"manual", valvulaA, valvulaB, valvulaPrincipal (bool) }
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <DHT.h>

// ======================= 1. CREDENCIALES (rellenar) =======================
#define WIFI_SSID      "TU_WIFI"
#define WIFI_PASSWORD  "TU_PASSWORD_WIFI"
#define API_KEY        "TU_API_KEY"                                   // Firebase > Config. del proyecto
#define DATABASE_URL   "https://TU_PROYECTO-default-rtdb.firebaseio.com"
#define USER_EMAIL     "esp32@tu-proyecto.com"                        // usuario creado en Authentication
#define USER_PASSWORD  "PASSWORD_DEL_ESP32"

// ============================ 2. PINES ====================================
#define PIN_SOIL       34   // sonda capacitiva de humedad (ADC1)
#define PIN_BAT        35   // divisor de tensión de la batería (ADC1)
#define PIN_DHT        4    // DHT22
#define PIN_TRIG       5    // ultrasonidos JSN-SR04T (nivel del tanque)
#define PIN_ECHO       18   // (usar divisor 5V->3.3V en ECHO)
#define PIN_RELAY_A    26   // electroválvula sector A
#define PIN_RELAY_B    27   // electroválvula sector B
#define PIN_RELAY_MAIN 25   // válvula principal del tanque
#define RELAY_ON  LOW       // módulos de relé habituales: activo en LOW
#define RELAY_OFF HIGH

// ================= 3. PARÁMETROS (editables desde la web) ==================
// Estos son los valores por defecto. Al conectar, el ESP32 lee /siras/config
// y los sustituye; si ese nodo está vacío, publica estos valores.
int   soilRawDry = 3200, soilRawWet = 1400;      // calibración sonda de humedad
float batVEmpty = 11.0, batVFull = 12.8;         // batería 0 % / 100 %
float tankDistEmptyCm = 100.0, tankDistFullCm = 20.0, tankCapacityL = 1000.0;
float moistOn = 30.0, moistOff = 35.0;           // histéresis de humedad (%)
float tempMax = 30.0;                            // bloqueo por temperatura (°C)
float tankMinL = 50.0;                           // nivel mínimo del tanque (L)
unsigned long maxRiegoMs = 20UL * 60UL * 1000UL; // máx. riego continuo
unsigned long lockoutMs  = 10UL * 60UL * 1000UL; // pausa tras el máximo
unsigned long pushMs     = 5000;                 // envío de sensores a Firebase

// Fijos en firmware (hardware)
const float BAT_DIV_RATIO = 5.0;                 // (R1+R2)/R2 del divisor, p.ej. 30k + 7.5k
const unsigned long SENSOR_MS = 2000;            // lectura local de sensores
const unsigned long SYNC_MS   = 2000;            // lectura de órdenes desde Firebase
const unsigned long CONFIG_MS = 10000;           // lectura de /siras/config

// ============================== ESTADO ====================================
DHT dht(PIN_DHT, DHT22);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

float moist = NAN, temp = NAN, battery = 0, tankL = 0;

String remoteMode = "auto";
bool remoteA = false, remoteB = false, remoteMain = false;   // órdenes manuales

bool outA = false, outB = false, outMain = false;            // estado real de los relés
bool irrigating = false;                                     // decisión del modo auto
unsigned long openSince = 0, lockUntil = 0;
bool pushActuators = false;                                  // hay que informar a la web

unsigned long tSensor = 0, tPush = 0, tSync = 0, tConfig = 0;
bool configLoaded = false;

// ============================ UTILIDADES ==================================
void setOutputs(bool a, bool b, bool mainV) {
  if (a != outA || b != outB || mainV != outMain) pushActuators = true;
  outA = a; outB = b; outMain = mainV;
  digitalWrite(PIN_RELAY_A,    a     ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_B,    b     ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_MAIN, mainV ? RELAY_ON : RELAY_OFF);
}

float readSoilPercent() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(PIN_SOIL); delay(5); }
  float raw = sum / 10.0;
  float pct = (soilRawDry - raw) * 100.0 / (soilRawDry - soilRawWet);
  return constrain(pct, 0.0, 100.0);
}

float readBatteryPercent() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(PIN_BAT); delay(2); }
  float volts = (sum / 10.0) / 4095.0 * 3.3 * BAT_DIV_RATIO;   // aprox.; calibrar con multímetro
  float pct = (volts - batVEmpty) * 100.0 / (batVFull - batVEmpty);
  return constrain(pct, 0.0, 100.0);
}

float readTankLitres() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(12);
  digitalWrite(PIN_TRIG, LOW);
  long us = pulseIn(PIN_ECHO, HIGH, 30000);
  if (us == 0) return tankL;                                   // sin eco: conserva el último valor
  float cm = us * 0.0343f / 2.0f;
  float frac = (tankDistEmptyCm - cm) / (tankDistEmptyCm - tankDistFullCm);
  return constrain(frac, 0.0f, 1.0f) * tankCapacityL;
}

void readSensors() {
  moist = readSoilPercent();
  float t = dht.readTemperature();
  if (!isnan(t)) temp = t;               // si falla, se queda el último valor válido
  battery = readBatteryPercent();
  tankL = readTankLitres();
  Serial.printf("Humedad %.1f%% | Temp %.1fC | Bat %.0f%% | Tanque %.0f L\n", moist, temp, battery, tankL);
}

// ============================ FIREBASE ====================================
void pushSensorsToFirebase() {
  FirebaseJson json;
  json.set("humedad",     roundf(moist * 10) / 10);
  if (!isnan(temp)) json.set("temperatura", roundf(temp * 10) / 10);
  json.set("bateria",     roundf(battery));
  json.set("nivelTanque", roundf(tankL));
  json.set("ultimaLectura/.sv", "timestamp");                  // marca de tiempo del servidor
  if (!Firebase.RTDB.updateNode(&fbdo, "/siras/sensores", &json))
    Serial.println("Error sensores: " + fbdo.errorReason());
}

void pushActuatorsToFirebase() {
  FirebaseJson json;
  json.set("valvulaA", outA);
  json.set("valvulaB", outB);
  json.set("valvulaPrincipal", outMain);
  if (Firebase.RTDB.updateNode(&fbdo, "/siras/actuadores", &json)) pushActuators = false;
  else Serial.println("Error actuadores: " + fbdo.errorReason());
}

void syncFromFirebase() {
  if (!Firebase.RTDB.getJSON(&fbdo, "/siras/actuadores")) {
    Serial.println("Error leyendo ordenes: " + fbdo.errorReason());
    return;
  }
  FirebaseJson &json = fbdo.jsonObject();
  FirebaseJsonData d;
  if (json.get(d, "modo") && d.success)             remoteMode = d.to<String>();
  if (json.get(d, "valvulaA") && d.success)         remoteA    = d.to<bool>();
  if (json.get(d, "valvulaB") && d.success)         remoteB    = d.to<bool>();
  if (json.get(d, "valvulaPrincipal") && d.success) remoteMain = d.to<bool>();
}


// ===================== CONFIGURACIÓN REMOTA (/siras/config) ===============
bool readParam(FirebaseJson &j, const char *key, float &out, float lo, float hi) {
  FirebaseJsonData d;
  if (j.get(d, key) && d.success) { out = constrain(d.to<float>(), lo, hi); return true; }
  return false;
}

void publishConfig() {
  FirebaseJson j;
  j.set("moistOn", moistOn);           j.set("moistOff", moistOff);
  j.set("tempMax", tempMax);           j.set("tankMinL", tankMinL);
  j.set("maxRiegoMin", (int)(maxRiegoMs / 60000UL));
  j.set("lockoutMin",  (int)(lockoutMs / 60000UL));
  j.set("pushSec",     (int)(pushMs / 1000UL));
  j.set("soilRawDry", soilRawDry);     j.set("soilRawWet", soilRawWet);
  j.set("tankDistEmptyCm", tankDistEmptyCm); j.set("tankDistFullCm", tankDistFullCm);
  j.set("tankCapacityL", tankCapacityL);
  j.set("batVEmpty", batVEmpty);       j.set("batVFull", batVFull);
  Firebase.RTDB.updateNode(&fbdo, "/siras/config", &j);
}

void syncConfig() {
  if (!Firebase.RTDB.getJSON(&fbdo, "/siras/config")) {
    Serial.println("Error leyendo config: " + fbdo.errorReason());
    return;
  }
  configLoaded = true;
  if (fbdo.dataType() == "null") { publishConfig(); return; }   // primera vez: publica los valores por defecto
  FirebaseJson &j = fbdo.jsonObject();
  float v;
  // Cada valor se limita a un rango seguro aunque llegue algo raro.
  readParam(j, "moistOn", moistOn, 5, 80);
  readParam(j, "moistOff", moistOff, 10, 95);
  readParam(j, "tempMax", tempMax, 20, 45);
  readParam(j, "tankMinL", tankMinL, 0, 500);
  if (readParam(j, "maxRiegoMin", v, 1, 60))  maxRiegoMs = (unsigned long)v * 60000UL;
  if (readParam(j, "lockoutMin", v, 1, 120))  lockoutMs  = (unsigned long)v * 60000UL;
  if (readParam(j, "pushSec", v, 2, 300))     pushMs     = (unsigned long)v * 1000UL;
  if (readParam(j, "soilRawDry", v, 500, 4095)) soilRawDry = (int)v;
  if (readParam(j, "soilRawWet", v, 0, 3500))   soilRawWet = (int)v;
  readParam(j, "tankDistEmptyCm", tankDistEmptyCm, 20, 400);
  readParam(j, "tankDistFullCm", tankDistFullCm, 5, 300);
  readParam(j, "tankCapacityL", tankCapacityL, 100, 5000);
  readParam(j, "batVEmpty", batVEmpty, 6, 14);
  readParam(j, "batVFull", batVFull, 8, 16);
  // Coherencia: si no cuadran, se corrige para evitar divisiones por cero o riego sin histéresis.
  if (moistOff <= moistOn) moistOff = moistOn + 2;
  if (soilRawDry <= soilRawWet) { soilRawDry = 3200; soilRawWet = 1400; }
  if (tankDistEmptyCm <= tankDistFullCm) { tankDistEmptyCm = 100; tankDistFullCm = 20; }
  if (batVFull <= batVEmpty) { batVEmpty = 11.0; batVFull = 12.8; }
  Serial.printf("Config aplicada: on<%.0f off>%.0f Tmax %.1f tanqueMin %.0f L maxRiego %lu min\n",
                moistOn, moistOff, tempMax, tankMinL, maxRiegoMs / 60000UL);
}

// ============================ CONTROL =====================================
void controlLoop() {
  unsigned long now = millis();

  if (remoteMode == "manual") {
    irrigating = false;
    setOutputs(remoteA, remoteB, remoteMain);
  } else {
    // ---- MODO AUTOMÁTICO ----
    bool sensorFail = isnan(temp) || isnan(moist);
    if (sensorFail || temp > tempMax || tankL < tankMinL || now < lockUntil) {
      irrigating = false;                                       // bloqueo de seguridad
    } else if (moist < moistOn) {
      irrigating = true;
    } else if (moist >= moistOff) {
      irrigating = false;
    }
    setOutputs(irrigating, irrigating, irrigating);             // A + B + principal
  }

  // ---- SEGURIDAD COMÚN: nunca más de maxRiegoMs con válvulas abiertas ----
  bool anyOpen = outA || outB || outMain;
  if (anyOpen) {
    if (openSince == 0) openSince = now;
    if (now - openSince > maxRiegoMs) {
      Serial.println("SEGURIDAD: tiempo máximo de riego alcanzado, cerrando.");
      irrigating = false;
      lockUntil = now + lockoutMs;
      remoteA = remoteB = remoteMain = false;
      setOutputs(false, false, false);
      openSince = 0;
    }
  } else {
    openSince = 0;
  }
}

// ============================== SETUP/LOOP ================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK " + WiFi.localIP().toString() : " sin WiFi (modo local)");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY_A, OUTPUT); pinMode(PIN_RELAY_B, OUTPUT); pinMode(PIN_RELAY_MAIN, OUTPUT);
  setOutputs(false, false, false);                              // arranque siempre con todo cerrado
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  analogReadResolution(12);
  dht.begin();

  connectWiFi();

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  fbdo.setBSSLBufferSize(4096, 1024);
  Firebase.reconnectWiFi(true);
  Firebase.begin(&config, &auth);

  readSensors();
}

void loop() {
  unsigned long now = millis();

  if (now - tSensor >= SENSOR_MS) { tSensor = now; readSensors(); }

  if (Firebase.ready()) {
    if (!configLoaded || now - tConfig >= CONFIG_MS) { tConfig = now; syncConfig(); }
    if (now - tSync >= SYNC_MS) { tSync = now; syncFromFirebase(); }
  }

  controlLoop();                       // funciona aunque no haya WiFi ni Firebase

  if (Firebase.ready()) {
    if (now - tPush >= pushMs) { tPush = now; pushSensorsToFirebase(); }
    if (pushActuators) pushActuatorsToFirebase();
  }
}
