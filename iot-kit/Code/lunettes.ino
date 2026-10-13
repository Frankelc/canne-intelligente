// ============================================================
//  Projet 3 - LUNETTES INTELLIGENTES
//
//  - 1 capteur HC-SR04 à hauteur de tête
//  - envoie la distance mesurée à la canne par ESP-NOW (sans fil)
//
//  Câblage :
//    Trig -> GPIO 13
//    Echo -> pont diviseur (1 kOhm / 2 kOhm) -> GPIO 34
//    VCC -> VIN (3 piles 4,5 V)   Gnd -> GND
//
//  Carte : "ESP32 Dev Module"   Moniteur série : 115200 bauds
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ------------------------------------------------------------
//  À CONFIGURER : même nom de réseau que dans canne_finale.ino
//  (sert uniquement à trouver le canal radio de la canne).
//  Laisser vide ("") si la canne n'utilise pas le Wi-Fi.
// ------------------------------------------------------------
const char* WIFI_SSID = "";

// ------------------------------------------------------------
//  Broches et réglages
// ------------------------------------------------------------
const int TRIG = 13;
const int ECHO = 34;
const int LED  = 2;                 // LED bleue de la carte : clignote à chaque envoi

const long TIMEOUT_US = 25000;      // attente max de l'écho (environ 4 m)
const unsigned long INTERVALLE_MS = 100;   // une mesure et un envoi toutes les 100 ms

// ------------------------------------------------------------
//  Paquet envoyé à la canne (identique dans canne_finale.ino)
// ------------------------------------------------------------
const uint32_t CODE_PROJET = 0x43414E45;   // "CANE"
typedef struct __attribute__((packed)) {
  uint32_t code;
  int16_t  distanceCm;   // -1 = rien détecté
  uint16_t numero;
} PaquetLunettes;

uint8_t adresseDiffusion[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // envoi à tous
uint16_t numeroPaquet = 0;
long historique[3] = {-1, -1, -1};
int indexHisto = 0;

// ============================================================
//  Mesure
// ============================================================

long mesureBrute() {
  unsigned long t = millis();
  while (digitalRead(ECHO) == HIGH && millis() - t < 100) {}

  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duree = pulseIn(ECHO, HIGH, TIMEOUT_US);
  if (duree == 0) return -1;
  return duree / 58;
}

long mediane3(long a, long b, long c) {
  long m[3] = {a < 0 ? 9999 : a, b < 0 ? 9999 : b, c < 0 ? 9999 : c};
  if (m[0] > m[1]) { long t = m[0]; m[0] = m[1]; m[1] = t; }
  if (m[1] > m[2]) { long t = m[1]; m[1] = m[2]; m[2] = t; }
  if (m[0] > m[1]) { long t = m[0]; m[0] = m[1]; m[1] = t; }
  return (m[1] == 9999) ? -1 : m[1];
}

// ============================================================
//  Radio : même canal que la canne
//  Règle commune : canal du réseau WIFI_SSID s'il est trouvé, sinon canal 1.
// ============================================================

int canalDuReseau(const char* ssid) {
  if (strlen(ssid) == 0) return 0;
  int n = WiFi.scanNetworks();
  int canal = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) { canal = WiFi.channel(i); break; }
  }
  WiFi.scanDelete();
  return canal;
}

void demarrerRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int canal = canalDuReseau(WIFI_SSID);
  if (canal == 0) canal = 1;

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.printf("Canal radio : %d\n", canal);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERREUR : ESP-NOW n'a pas demarre");
    return;
  }

  esp_now_peer_info_t pair = {};
  memcpy(pair.peer_addr, adresseDiffusion, 6);
  pair.channel = 0;          // canal actuel
  pair.encrypt = false;
  if (esp_now_add_peer(&pair) != ESP_OK) {
    Serial.println("ERREUR : impossible d'ajouter le destinataire ESP-NOW");
    return;
  }
  Serial.println("ESP-NOW pret : envoi vers la canne");
}

// ============================================================
//  Programme principal
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== LUNETTES INTELLIGENTES ===");

  pinMode(TRIG, OUTPUT);
  digitalWrite(TRIG, LOW);
  pinMode(ECHO, INPUT);
  pinMode(LED, OUTPUT);

  demarrerRadio();
}

void loop() {
  unsigned long debut = millis();

  historique[indexHisto] = mesureBrute();
  indexHisto = (indexHisto + 1) % 3;
  long d = mediane3(historique[0], historique[1], historique[2]);

  PaquetLunettes p;
  p.code = CODE_PROJET;
  p.distanceCm = (int16_t)d;
  p.numero = numeroPaquet++;
  esp_err_t res = esp_now_send(adresseDiffusion, (uint8_t*)&p, sizeof(p));

  digitalWrite(LED, !digitalRead(LED));
  if (d < 0) Serial.printf("Tete : --- cm   (envoi %s)\n", res == ESP_OK ? "ok" : "ERREUR");
  else       Serial.printf("Tete : %ld cm   (envoi %s)\n", d, res == ESP_OK ? "ok" : "ERREUR");

  unsigned long ecoule = millis() - debut;
  if (ecoule < INTERVALLE_MS) delay(INTERVALLE_MS - ecoule);
}
