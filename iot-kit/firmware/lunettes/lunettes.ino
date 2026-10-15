// ============================================================
//  Projet 3 - LUNETTES INTELLIGENTES (+ passerelle vers le VPS)
//
//  - 1 capteur HC-SR04 à hauteur de tête, distance envoyée à la canne (ESP-NOW)
//  - PASSERELLE : reçoit les mesures, annonces et SOS de la canne (ESP-NOW) et les
//    publie sur le VPS (Wi-Fi + MQTT/TLS, iot.cpp). Renvoie à la canne l'état du VPS
//    et la confirmation des SOS.
//    Pourquoi ici ? La canne utilise le Bluetooth audio : son ESP32 n'a plus assez de
//    mémoire pour la connexion TLS. Les lunettes, elles, ont toute leur mémoire libre.
//
//  Carte : ESP32 DevKit V1
//  Câblage :
//    Trig -> GPIO 13 (D13)
//    Echo -> pont diviseur (1 kOhm / 2 kOhm) -> GPIO 34 (D34)
//    3 piles (4,5 V) : + -> broche VIN   - -> GND
//    VCC du HC-SR04 -> même ligne + (4,5 V)   Gnd -> GND
//
//  Arduino IDE : carte "ESP32 Dev Module"   Moniteur série : 115200 bauds
//  Bibliothèque : PubSubClient
//  Configuration : config.h (Wi-Fi + bloc MQTT du VPS), copié de config.example.h
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "protocole.h"
#include "iot.h"

// ------------------------------------------------------------
//  Broches et réglages
// ------------------------------------------------------------
const int TRIG = 13;
const int ECHO = 34;
const int LED  = 2;                 // LED bleue de la carte (GPIO 2) : clignote à chaque envoi

const long TIMEOUT_US = 25000;      // attente max de l'écho (environ 4 m)
const unsigned long INTERVALLE_MS = 100;    // une mesure et un envoi toutes les 100 ms
const unsigned long CANNE_TIMEOUT = 1500;   // ms sans nouvelles : canne absente

// Texte de chaque message vocal de la canne, pour le journal du tableau de bord.
const char* TEXTE_MSG[] = {
  "", "Obstacle à gauche", "Obstacle à droite", "Obstacle devant, allez à gauche",
  "Obstacle devant, allez à droite", "Stop, obstacle", "Attention, obstacle en hauteur",
  "Continuez tout droit", "Alerte envoyée", "Système prêt", "Alerte non envoyée"
};
const int NB_MESSAGES = 10;

uint8_t adresseDiffusion[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // envoi à tous
long historique[3] = {-1, -1, -1};
int indexHisto = 0;

// Dernières données reçues de la canne (remplies par la réception ESP-NOW)
portMUX_TYPE verrouCanne = portMUX_INITIALIZER_UNLOCKED;
PaquetCanne derniereCanne = {0, 0, -1, -1, -1, 0, 0};
volatile unsigned long canneDerniereReception = 0;
volatile int annonceEnAttente = 0;      // message vocal à publier (0 = aucun)
uint8_t dernierNumeroAnnonce = 0;

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
//  Réception ESP-NOW (données de la canne)
// ============================================================

void surReception(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 5) return;
  uint32_t code;
  memcpy(&code, data, 4);
  if (code != CODE_PROJET) return;

  if (data[4] == PAQUET_CANNE && len == sizeof(PaquetCanne)) {
    portENTER_CRITICAL(&verrouCanne);
    memcpy(&derniereCanne, data, sizeof(PaquetCanne));
    portEXIT_CRITICAL(&verrouCanne);
    canneDerniereReception = millis();
  } else if (data[4] == PAQUET_ANNONCE && len == sizeof(PaquetAnnonce)) {
    PaquetAnnonce a;
    memcpy(&a, data, sizeof(a));
    if (a.numero != dernierNumeroAnnonce) {   // la canne envoie chaque annonce 2 fois
      dernierNumeroAnnonce = a.numero;
      annonceEnAttente = a.message;
    }
  }
}

bool canneConnectee() {
  return canneDerniereReception != 0 && millis() - canneDerniereReception < CANNE_TIMEOUT;
}

// ============================================================
//  Radio : Wi-Fi (passerelle) + ESP-NOW sur le même canal que la canne.
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

  if (canal > 0) {
    Serial.printf("Connexion au Wi-Fi \"%s\" (canal %d)...\n", WIFI_SSID, canal);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);   // sinon la veille radio fait perdre des paquets de la canne
    Serial.printf("Wi-Fi connecte, canal %d\n", WiFi.channel());
  } else {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    if (canal == 0) canal = 1;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    Serial.printf("Pas de Wi-Fi : canal %d, liaison avec la canne seulement (pas de VPS)\n", canal);
  }

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
  esp_now_register_recv_cb(surReception);
  Serial.println("ESP-NOW pret : liaison avec la canne");
}

// ============================================================
//  Programme principal
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== LUNETTES INTELLIGENTES (passerelle VPS) ===");

  pinMode(TRIG, OUTPUT);
  digitalWrite(TRIG, LOW);
  pinMode(ECHO, INPUT);
  pinMode(LED, OUTPUT);

  demarrerRadio();
  iotBegin();
}

void loop() {
  unsigned long debut = millis();

  // 1. Mesure de la hauteur de tête
  historique[indexHisto] = mesureBrute();
  indexHisto = (indexHisto + 1) % 3;
  long tete = mediane3(historique[0], historique[1], historique[2]);

  // 2. Envoi à la canne : distance + état du VPS + confirmation de SOS
  PaquetLunettes p;
  p.code = CODE_PROJET;
  p.type = PAQUET_LUNETTES;
  p.teteCm = (int16_t)tete;
  p.vpsConfigure = iotIsConfigured() ? 1 : 0;
  p.vpsConnecte = iotIsConnected() ? 1 : 0;
  p.sosConfirme = iotConfirmedSos();
  esp_err_t res = esp_now_send(adresseDiffusion, (uint8_t*)&p, sizeof(p));
  digitalWrite(LED, !digitalRead(LED));

  // 3. Données de la canne -> VPS
  PaquetCanne c;
  portENTER_CRITICAL(&verrouCanne);
  c = derniereCanne;
  portEXIT_CRITICAL(&verrouCanne);
  const bool canne = canneConnectee();
  iotSetState(c.gaucheCm, c.centreCm, c.droiteCm, tete, canne, canne && c.casqueConnecte);
  if (canne && c.sosNumero != 0) iotRequestSos(c.sosNumero);

  const int annonce = annonceEnAttente;
  if (annonce >= 1 && annonce <= NB_MESSAGES) {
    annonceEnAttente = 0;
    iotReportMessage(annonce, TEXTE_MSG[annonce]);
  }

  // 4. Affichage (toutes les 500 ms)
  static unsigned long dernierAffichage = 0;
  if (millis() - dernierAffichage >= 500) {
    dernierAffichage = millis();
    if (tete < 0) Serial.print("Tete : ---  ");
    else          Serial.printf("Tete : %ld cm  ", tete);
    if (canne) Serial.printf("| Canne G:%d C:%d D:%d casque:%s ", c.gaucheCm, c.centreCm, c.droiteCm,
                             c.casqueConnecte ? "ok" : "--");
    else       Serial.print("| Canne : absente ");
    Serial.printf("| VPS : %s | envoi %s\n", iotIsConnected() ? "connecte" : "--",
                  res == ESP_OK ? "ok" : "ERREUR");
  }

  unsigned long ecoule = millis() - debut;
  if (ecoule < INTERVALLE_MS) delay(INTERVALLE_MS - ecoule);
}
