// ============================================================
//  Projet 3 - CANNE INTELLIGENTE (programme final)
//
//  - 3 capteurs HC-SR04 (gauche, centre, droite)
//  - réception de la distance "tête" envoyée par les lunettes (ESP-NOW)
//  - décision : quel message dire
//  - messages vocaux par le DFPlayer Mini (écouteurs)
//  - bouton SOS (appui long) : alerte Telegram par Wi-Fi
//
//  Câblage (voir Documentation_Projet3.md) :
//    Gauche : Trig GPIO 13, Echo -> pont diviseur -> GPIO 34
//    Centre : Trig GPIO 14, Echo -> pont diviseur -> GPIO 35
//    Droite : Trig GPIO 27, Echo -> pont diviseur -> GPIO 32
//    DFPlayer : GPIO 17 (TX2) -> 1 kOhm -> RX ; TX -> GPIO 16 (RX2)
//    Bouton SOS : GPIO 25 <-> GND
//
//  Carte : "ESP32 Dev Module"   Moniteur série : 115200 bauds
//  Bibliothèque : DFRobotDFPlayerMini
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DFRobotDFPlayerMini.h>

// ------------------------------------------------------------
//  À CONFIGURER
//  Laisser vide ("") pour fonctionner sans Wi-Fi ni Telegram.
//  WIFI_SSID doit être IDENTIQUE dans lunettes.ino.
// ------------------------------------------------------------
const char* WIFI_SSID        = "";   // nom du partage de connexion
const char* WIFI_PASSWORD    = "";   // mot de passe du partage de connexion
const char* TELEGRAM_TOKEN   = "";   // jeton du bot (donné par @BotFather)
const char* TELEGRAM_CHAT_ID = "";   // identifiant du destinataire de l'alerte

// ------------------------------------------------------------
//  Broches
// ------------------------------------------------------------
const int NB_CAPTEURS = 3;
const int GAUCHE = 0, CENTRE = 1, DROITE = 2;
const int TRIG[NB_CAPTEURS] = {13, 14, 27};
const int ECHO[NB_CAPTEURS] = {34, 35, 32};
const char* NOM[NB_CAPTEURS] = {"G", "C", "D"};

const int BOUTON_SOS = 25;
const int DF_RX = 16;   // reçoit le TX du DFPlayer
const int DF_TX = 17;   // envoie vers le RX du DFPlayer (via 1 kOhm)

// ------------------------------------------------------------
//  Réglages
// ------------------------------------------------------------
const long SEUIL_OBSTACLE = 120;      // cm : en dessous, un capteur de la canne est "bloqué"
const long SEUIL_TETE     = 100;      // cm : obstacle à hauteur de tête
const long TIMEOUT_US     = 25000;    // attente max de l'écho (environ 4 m)
const int  PAUSE_CAPTEUR_MS = 35;     // pause après chaque mesure (évite les échos croisés)

const unsigned long MIN_ENTRE_MESSAGES = 1500;  // ms entre deux messages différents
const unsigned long REPETITION_MS      = 3000;  // ms avant de répéter le même message
const unsigned long LUNETTES_TIMEOUT   = 1500;  // ms sans nouvelles : lunettes absentes
const unsigned long DUREE_APPUI_SOS    = 1500;  // ms d'appui pour déclencher le SOS
const int VOLUME = 20;                          // volume du DFPlayer (0 à 30)

// ------------------------------------------------------------
//  Messages : numéro du fichier dans le dossier mp3 de la carte SD
// ------------------------------------------------------------
const int MSG_AUCUN         = 0;
const int MSG_OBST_GAUCHE   = 1;   // 0001.mp3 « Obstacle à gauche »
const int MSG_OBST_DROITE   = 2;   // 0002.mp3 « Obstacle à droite »
const int MSG_ALLER_GAUCHE  = 3;   // 0003.mp3 « Obstacle devant, allez à gauche »
const int MSG_ALLER_DROITE  = 4;   // 0004.mp3 « Obstacle devant, allez à droite »
const int MSG_STOP          = 5;   // 0005.mp3 « Stop, obstacle »
const int MSG_HAUTEUR       = 6;   // 0006.mp3 « Attention, obstacle en hauteur »
const int MSG_TOUT_DROIT    = 7;   // 0007.mp3 « Continuez tout droit »
const int MSG_ALERTE_OK     = 8;   // 0008.mp3 « Alerte envoyée »
const int MSG_PRET          = 9;   // 0009.mp3 « Système prêt »
const int MSG_ALERTE_ECHEC  = 10;  // 0010.mp3 « Alerte non envoyée »

// ------------------------------------------------------------
//  Paquet reçu des lunettes (identique dans lunettes.ino)
// ------------------------------------------------------------
const uint32_t CODE_PROJET = 0x43414E45;   // "CANE" : ignore les autres appareils ESP-NOW
typedef struct __attribute__((packed)) {
  uint32_t code;
  int16_t  distanceCm;   // -1 = rien détecté
  uint16_t numero;
} PaquetLunettes;

volatile int16_t teteDistance = -1;
volatile unsigned long teteDerniereReception = 0;

// ------------------------------------------------------------
//  Variables
// ------------------------------------------------------------
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini df;
bool dfOk = false;

long historique[NB_CAPTEURS][3];   // 3 dernières mesures de chaque capteur
int  indexHisto[NB_CAPTEURS] = {0, 0, 0};
long distance[NB_CAPTEURS] = {-1, -1, -1};

int dernierMessage = MSG_AUCUN;
unsigned long dernierMessageMs = 0;
bool obstacleEnCours = false;
bool annoncerToutDroit = false;

// ============================================================
//  Mesure des capteurs
// ============================================================

// Une mesure. Renvoie la distance en cm, ou -1 si rien n'est détecté.
long mesureBrute(int i) {
  // Si le capteur n'a rien vu à la mesure précédente, Echo reste à HIGH
  // jusqu'à environ 60 ms : on attend la fin avant de redéclencher.
  unsigned long t = millis();
  while (digitalRead(ECHO[i]) == HIGH && millis() - t < 100) {}

  digitalWrite(TRIG[i], LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG[i], HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG[i], LOW);

  long duree = pulseIn(ECHO[i], HIGH, TIMEOUT_US);
  if (duree == 0) return -1;
  return duree / 58;
}

// Médiane de 3 valeurs ; -1 ("rien") est traité comme très loin.
long mediane3(long a, long b, long c) {
  long m[3] = {a < 0 ? 9999 : a, b < 0 ? 9999 : b, c < 0 ? 9999 : c};
  if (m[0] > m[1]) { long t = m[0]; m[0] = m[1]; m[1] = t; }
  if (m[1] > m[2]) { long t = m[1]; m[1] = m[2]; m[2] = t; }
  if (m[0] > m[1]) { long t = m[0]; m[0] = m[1]; m[1] = t; }
  return (m[1] == 9999) ? -1 : m[1];
}

// Mesure un capteur et met à jour sa distance filtrée (médiane des 3 dernières mesures).
void mettreAJourCapteur(int i) {
  historique[i][indexHisto[i]] = mesureBrute(i);
  indexHisto[i] = (indexHisto[i] + 1) % 3;
  distance[i] = mediane3(historique[i][0], historique[i][1], historique[i][2]);
}

// ============================================================
//  Lunettes (ESP-NOW)
// ============================================================

void surReception(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(PaquetLunettes)) return;
  PaquetLunettes p;
  memcpy(&p, data, sizeof(p));
  if (p.code != CODE_PROJET) return;
  teteDistance = p.distanceCm;
  teteDerniereReception = millis();
}

bool lunettesConnectees() {
  return teteDerniereReception != 0 && millis() - teteDerniereReception < LUNETTES_TIMEOUT;
}

// ============================================================
//  Wi-Fi : canne et lunettes doivent être sur le même canal radio.
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

void forcerCanal(int canal) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
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
    Serial.printf("Wi-Fi connecte, canal %d\n", WiFi.channel());
  } else {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    if (canal == 0) canal = 1;
    forcerCanal(canal);
    Serial.printf("Pas de Wi-Fi : fonctionnement local sur le canal %d (SOS indisponible)\n", canal);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERREUR : ESP-NOW n'a pas demarre");
    return;
  }
  esp_now_register_recv_cb(surReception);
  Serial.println("ESP-NOW pret : en attente des lunettes");
}

// ============================================================
//  SOS : alerte Telegram
// ============================================================

String encoderUrl(const String& s) {
  String r;
  const char* hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

bool envoyerTelegram(const String& texte) {
  if (strlen(TELEGRAM_TOKEN) == 0 || strlen(TELEGRAM_CHAT_ID) == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();   // pas de vérification du certificat (suffisant pour un prototype)
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + TELEGRAM_TOKEN +
               "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID + "&text=" + encoderUrl(texte);
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  int code = http.GET();
  http.end();
  Serial.printf("Telegram : reponse HTTP %d\n", code);
  return code == 200;
}

void declencherSOS() {
  Serial.println(">>> SOS declenche");
  String texte = "ALERTE SOS : la personne equipee de la canne intelligente demande de l'aide.";
  texte += lunettesConnectees() ? " (lunettes connectees)" : " (lunettes non connectees)";

  bool ok = envoyerTelegram(texte);
  Serial.println(ok ? ">>> Alerte envoyee" : ">>> Echec de l'envoi de l'alerte");
  jouer(ok ? MSG_ALERTE_OK : MSG_ALERTE_ECHEC);
}

// Appui long sur le bouton : déclenche le SOS une seule fois par appui.
void gererBoutonSOS() {
  static unsigned long debutAppui = 0;
  static bool dejaDeclenche = false;

  if (digitalRead(BOUTON_SOS) == LOW) {
    if (debutAppui == 0) debutAppui = millis();
    if (!dejaDeclenche && millis() - debutAppui >= DUREE_APPUI_SOS) {
      dejaDeclenche = true;
      declencherSOS();
    }
  } else {
    debutAppui = 0;
    dejaDeclenche = false;
  }
}

// ============================================================
//  Décision et voix
// ============================================================

bool bloque(long d) { return d >= 0 && d < SEUIL_OBSTACLE; }
long espaceLibre(long d) { return d < 0 ? 9999 : d; }

int decider(long g, long c, long d, long tete) {
  if (tete >= 0 && tete < SEUIL_TETE) return MSG_HAUTEUR;   // priorité

  bool G = bloque(g), C = bloque(c), D = bloque(d);

  if (C) {
    if (G && D)  return MSG_STOP;
    if (!G && D) return MSG_ALLER_GAUCHE;
    if (G && !D) return MSG_ALLER_DROITE;
    return espaceLibre(g) >= espaceLibre(d) ? MSG_ALLER_GAUCHE : MSG_ALLER_DROITE;
  }
  if (G && D) return MSG_TOUT_DROIT;   // passage étroit : on peut avancer au centre
  if (G)      return MSG_OBST_GAUCHE;
  if (D)      return MSG_OBST_DROITE;
  return MSG_AUCUN;
}

void jouer(int msg) {
  if (dfOk) df.playMp3Folder(msg);
  dernierMessage = msg;
  dernierMessageMs = millis();
  Serial.printf("    [voix] message %d\n", msg);
}

// Décide si le message doit être dit maintenant (anti-répétition).
void annoncer(int msg) {
  unsigned long ecoule = millis() - dernierMessageMs;

  if (msg == MSG_AUCUN) {
    if (obstacleEnCours) { obstacleEnCours = false; annoncerToutDroit = true; }
    if (annoncerToutDroit && ecoule >= MIN_ENTRE_MESSAGES) {
      annoncerToutDroit = false;
      jouer(MSG_TOUT_DROIT);
    }
    return;
  }

  obstacleEnCours = true;
  annoncerToutDroit = false;

  bool urgent    = (msg == MSG_HAUTEUR && dernierMessage != MSG_HAUTEUR);
  bool nouveau   = (msg != dernierMessage && ecoule >= MIN_ENTRE_MESSAGES);
  bool repetition = (msg == dernierMessage && ecoule >= REPETITION_MS);

  if (urgent || nouveau || repetition) jouer(msg);
}

// ============================================================
//  Affichage pour le moniteur série
// ============================================================

void afficherEtat(int msg) {
  static unsigned long dernierAffichage = 0;
  if (millis() - dernierAffichage < 300) return;
  dernierAffichage = millis();

  for (int i = 0; i < NB_CAPTEURS; i++) {
    if (distance[i] < 0) Serial.printf("%s: ---  ", NOM[i]);
    else                 Serial.printf("%s:%4ld  ", NOM[i], distance[i]);
  }
  if (lunettesConnectees()) {
    if (teteDistance < 0) Serial.print("Tete: ---  ");
    else                  Serial.printf("Tete:%4d  ", teteDistance);
  } else {
    Serial.print("Tete: (lunettes absentes)  ");
  }
  Serial.printf("-> decision %d\n", msg);
}

// ============================================================
//  Programme principal
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== CANNE INTELLIGENTE ===");

  for (int i = 0; i < NB_CAPTEURS; i++) {
    pinMode(TRIG[i], OUTPUT);
    digitalWrite(TRIG[i], LOW);
    pinMode(ECHO[i], INPUT);
    for (int k = 0; k < 3; k++) historique[i][k] = -1;
  }
  pinMode(BOUTON_SOS, INPUT_PULLUP);

  dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
  dfOk = df.begin(dfSerial);
  if (dfOk) {
    df.volume(VOLUME);
    Serial.println("DFPlayer pret");
  } else {
    Serial.println("DFPlayer non detecte : verifier TX/RX, l'alimentation et la carte SD (fonctionnement sans voix)");
  }

  demarrerRadio();

  jouer(MSG_PRET);
  delay(1500);
}

void loop() {
  for (int i = 0; i < NB_CAPTEURS; i++) {
    gererBoutonSOS();
    mettreAJourCapteur(i);
    delay(PAUSE_CAPTEUR_MS);
  }

  long tete = lunettesConnectees() ? teteDistance : -1;
  int msg = decider(distance[GAUCHE], distance[CENTRE], distance[DROITE], tete);
  annoncer(msg);
  afficherEtat(msg);
}
