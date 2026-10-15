// ============================================================
//  Projet 3 - CANNE INTELLIGENTE (programme final)
//
//  - 3 capteurs HC-SR04 (gauche, centre, droite)
//  - réception de la distance "tête" envoyée par les lunettes (ESP-NOW)
//  - décision : quel message dire 
//  - messages vocaux envoyés en Bluetooth à un casque audio (audio.cpp)
//  - bouton SOS (appui long) : demande transmise aux lunettes (ESP-NOW), qui la publient
//    sur le VPS (MQTT/TLS) ; le VPS prévient Telegram et confirme
//  - mesures et annonces transmises aux lunettes pour le tableau de bord du VPS
//  La canne ne se connecte PAS à Internet : le Bluetooth audio occupe trop de mémoire pour
//  la connexion TLS. Ce sont les lunettes qui servent de passerelle (voir protocole.h).
//
//  Câblage (voir Documentation_Projet3.md) :
//    Gauche : Trig GPIO 13, Echo -> pont diviseur -> GPIO 34
//    Centre : Trig GPIO 14, Echo -> pont diviseur -> GPIO 35
//    Droite : Trig GPIO 27, Echo -> pont diviseur -> GPIO 32
//    Bouton SOS : GPIO 25 <-> GND
//    Audio : casque Bluetooth (aucun fil), nom à indiquer dans config.h (BT_HEADSET_NAME)
//
//  Carte : "ESP32 Dev Module"   Moniteur série : 115200 bauds
//  IMPORTANT : Outils -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
//              (Bluetooth + messages vocaux : le programme dépasse la taille par défaut)
//  Bibliothèque : ESP32-A2DP (pschatzmann, depuis GitHub)
//  Configuration : config.h (WIFI_SSID pour trouver le canal radio, nom du casque)
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "protocole.h"
#include "audio.h"

// ------------------------------------------------------------
//  Broches
// ------------------------------------------------------------
const int NB_CAPTEURS = 3;
const int GAUCHE = 0, CENTRE = 1, DROITE = 2;
const int TRIG[NB_CAPTEURS] = {13, 14, 27};
const int ECHO[NB_CAPTEURS] = {34, 35, 32};
const char* NOM[NB_CAPTEURS] = {"G", "C", "D"};

const int BOUTON_SOS = 25;

// ------------------------------------------------------------
//  Réglages
// ------------------------------------------------------------
const long SEUIL_CENTRE   = 120;      // cm : obstacle devant (là où la personne avance)
const long SEUIL_COTE     = 60;       // cm : obstacle sur le côté (seulement s'il est proche)
const long SEUIL_TETE     = 100;      // cm : obstacle à hauteur de tête
const long HYSTERESIS     = 20;       // cm : un obstacle détecté ne « disparaît » qu'au-delà
                                      // de seuil + 20 cm (évite qu'il clignote près du seuil)
const long TIMEOUT_US     = 25000;    // attente max de l'écho (environ 4 m)
const int  PAUSE_CAPTEUR_MS = 35;     // pause après chaque mesure (évite les échos croisés)

const unsigned long MIN_ENTRE_MESSAGES = 1500;  // ms entre deux messages
const unsigned long REPETITION_MS      = 4000;  // ms avant de répéter un obstacle DEVANT
                                                // (un obstacle sur le côté n'est dit qu'une fois)
const unsigned long LUNETTES_TIMEOUT   = 1500;  // ms sans nouvelles : lunettes absentes
const unsigned long DUREE_APPUI_SOS    = 1500;  // ms d'appui pour déclencher le SOS
const unsigned long SOS_DELAI_MS       = 20000; // sans confirmation : « Alerte non envoyée »
const unsigned long INTERVALLE_ETAT_MS = 300;   // envoi de l'état aux lunettes

// ------------------------------------------------------------
//  Messages : numéro du message vocal dans audio_clips.h
// ------------------------------------------------------------
const int MSG_AUCUN         = 0;
const int MSG_OBST_GAUCHE   = 1;   // « Obstacle à gauche »  (oreille gauche)
const int MSG_OBST_DROITE   = 2;   // « Obstacle à droite »  (oreille droite)
const int MSG_ALLER_GAUCHE  = 3;   // « Obstacle devant, allez à gauche »
const int MSG_ALLER_DROITE  = 4;   // « Obstacle devant, allez à droite »
const int MSG_STOP          = 5;   // « Stop, obstacle »
const int MSG_HAUTEUR       = 6;   // « Attention, obstacle en hauteur »
const int MSG_TOUT_DROIT    = 7;   // « Continuez tout droit »
const int MSG_ALERTE_OK     = 8;   // « Alerte envoyée »
const int MSG_PRET          = 9;   // « Système prêt » (dit quand le casque se connecte)
const int MSG_ALERTE_ECHEC  = 10;  // « Alerte non envoyée »

// ------------------------------------------------------------
//  Données reçues des lunettes (paquets décrits dans protocole.h)
// ------------------------------------------------------------
volatile int16_t teteDistance = -1;
volatile unsigned long teteDerniereReception = 0;
volatile bool vpsConfigure = false;     // les lunettes ont un VPS configuré
volatile bool vpsConnecte = false;      // les lunettes sont connectées au VPS
volatile uint8_t sosConfirme = 0;       // dernier SOS confirmé par le VPS

uint8_t adresseDiffusion[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // envoi à tous
uint8_t numeroAnnonce = 0;

// SOS en cours : numéro (1 à 255, 0 = aucun) envoyé aux lunettes jusqu'à confirmation
uint8_t sosNumero = 0;
uint8_t sosDernierNumero = 0;
unsigned long sosDebut = 0;
bool sosEchecAnnonce = false;

// ------------------------------------------------------------
//  Variables
// ------------------------------------------------------------
bool casqueConnecte = false;

long historique[NB_CAPTEURS][3];   // 3 dernières mesures de chaque capteur
int  indexHisto[NB_CAPTEURS] = {0, 0, 0};
long distance[NB_CAPTEURS] = {-1, -1, -1};

// Obstacle présent ou non pour chaque capteur, avec hystérésis (voir mettreAJourEtats)
bool bloque[NB_CAPTEURS] = {false, false, false};
bool teteBloquee = false;

int dernierMessage = MSG_AUCUN;
unsigned long dernierMessageMs = 0;
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
  if (p.code != CODE_PROJET || p.type != PAQUET_LUNETTES) return;
  teteDistance = p.teteCm;
  vpsConfigure = p.vpsConfigure;
  vpsConnecte = p.vpsConnecte;
  sosConfirme = p.sosConfirme;
  teteDerniereReception = millis();
}

// État de la canne -> lunettes (qui le publient sur le VPS).
void envoyerEtat() {
  static unsigned long dernierEnvoi = 0;
  if (millis() - dernierEnvoi < INTERVALLE_ETAT_MS) return;
  dernierEnvoi = millis();
  PaquetCanne p;
  p.code = CODE_PROJET;
  p.type = PAQUET_CANNE;
  p.gaucheCm = (int16_t)distance[GAUCHE];
  p.centreCm = (int16_t)distance[CENTRE];
  p.droiteCm = (int16_t)distance[DROITE];
  p.casqueConnecte = casqueConnecte ? 1 : 0;
  p.sosNumero = sosNumero;
  esp_now_send(adresseDiffusion, (uint8_t*)&p, sizeof(p));
}

// Message vocal joué -> lunettes (journal du tableau de bord). Envoyé 2 fois : ESP-NOW en
// diffusion ne garantit pas la réception ; les lunettes ignorent le doublon.
void envoyerAnnonce(int msg) {
  PaquetAnnonce a;
  a.code = CODE_PROJET;
  a.type = PAQUET_ANNONCE;
  a.message = (uint8_t)msg;
  a.numero = ++numeroAnnonce;
  esp_now_send(adresseDiffusion, (uint8_t*)&a, sizeof(a));
  esp_now_send(adresseDiffusion, (uint8_t*)&a, sizeof(a));
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

// La canne ne se connecte pas au Wi-Fi : elle se cale seulement sur le canal du partage de
// connexion, celui où les lunettes sont connectées, pour pouvoir leur parler en ESP-NOW.
void demarrerRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int canal = canalDuReseau(WIFI_SSID);
  if (canal == 0) {
    canal = 1;
    Serial.printf("Reseau \"%s\" introuvable : canal 1 par defaut\n", WIFI_SSID);
  }
  forcerCanal(canal);
  WiFi.setSleep(false);   // sinon la veille radio fait perdre des paquets des lunettes
  Serial.printf("Canal radio : %d (meme canal que les lunettes)\n", canal);

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
  }
  esp_now_register_recv_cb(surReception);
  Serial.println("ESP-NOW pret : liaison avec les lunettes");
}

// ============================================================
//  SOS : la canne choisit un numéro et l'envoie aux lunettes (dans PaquetCanne) jusqu'à
//  ce que les lunettes renvoient ce numéro comme confirmé par le VPS.
// ============================================================

void declencherSOS() {
  Serial.println(">>> SOS declenche");
  // Sans lunettes, ou lunettes sans VPS configuré : l'alerte ne peut pas partir.
  if (!lunettesConnectees() || !vpsConfigure) {
    Serial.println(lunettesConnectees() ? ">>> VPS non configure dans les lunettes : alerte impossible"
                                        : ">>> Lunettes absentes : alerte impossible");
    jouer(MSG_ALERTE_ECHEC);
    return;
  }
  if (sosNumero != 0) return;   // alerte déjà en cours d'envoi
  sosDernierNumero = (sosDernierNumero % 255) + 1;   // 1 à 255
  sosNumero = sosDernierNumero;
  sosDebut = millis();
  sosEchecAnnonce = false;
  Serial.printf(">>> SOS n°%u transmis aux lunettes\n", sosNumero);
  // Sans connexion au VPS pour l'instant : on prévient ; l'envoi continue en arrière-plan.
  if (!vpsConnecte) {
    sosEchecAnnonce = true;
    jouer(MSG_ALERTE_ECHEC);
  }
}

// Résultat de l'alerte, annoncé dès que les lunettes le connaissent.
void suivreSOS() {
  if (sosNumero == 0) return;
  if (sosConfirme == sosNumero) {
    Serial.printf(">>> Alerte n°%u envoyee (confirmee par le VPS)\n", sosNumero);
    sosNumero = 0;
    jouer(MSG_ALERTE_OK);
  } else if (!sosEchecAnnonce && millis() - sosDebut > SOS_DELAI_MS) {
    Serial.println(">>> Pas de confirmation apres 20 s : nouvel essai en arriere-plan");
    sosEchecAnnonce = true;
    jouer(MSG_ALERTE_ECHEC);
  }
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

long espaceLibre(long d) { return d < 0 ? 9999 : d; }

// Hystérésis : un obstacle apparaît sous le seuil et ne disparaît qu'au-dessus de
// seuil + HYSTERESIS. Une distance qui hésite autour du seuil ne fait plus « clignoter »
// l'obstacle (et ne déclenche plus des messages en boucle).
bool etatAvecHysteresis(bool etat, long d, long seuil) {
  if (!etat) return d >= 0 && d < seuil;
  return d >= 0 && d < seuil + HYSTERESIS;
}

void mettreAJourEtats(long tete) {
  for (int i = 0; i < NB_CAPTEURS; i++) {
    const long seuil = (i == CENTRE) ? SEUIL_CENTRE : SEUIL_COTE;
    bloque[i] = etatAvecHysteresis(bloque[i], distance[i], seuil);
  }
  teteBloquee = etatAvecHysteresis(teteBloquee, tete, SEUIL_TETE);
}

int decider() {
  if (teteBloquee) return MSG_HAUTEUR;   // priorité

  const bool G = bloque[GAUCHE], C = bloque[CENTRE], D = bloque[DROITE];

  if (C) {
    if (G && D)  return MSG_STOP;
    if (!G && D) return MSG_ALLER_GAUCHE;
    if (G && !D) return MSG_ALLER_DROITE;
    return espaceLibre(distance[GAUCHE]) >= espaceLibre(distance[DROITE]) ? MSG_ALLER_GAUCHE
                                                                         : MSG_ALLER_DROITE;
  }
  if (G && D) return MSG_AUCUN;   // passage étroit, rien devant : on avance, rien à dire
  if (G)      return MSG_OBST_GAUCHE;
  if (D)      return MSG_OBST_DROITE;
  return MSG_AUCUN;
}

// Obstacle DEVANT : la personne doit changer de direction ou s'arrêter.
bool estDevant(int msg) {
  return msg == MSG_ALLER_GAUCHE || msg == MSG_ALLER_DROITE || msg == MSG_STOP ||
         msg == MSG_HAUTEUR;
}

void jouer(int msg) {
  int cote = COTE_DEUX;
  if (msg == MSG_OBST_GAUCHE) cote = COTE_GAUCHE;
  if (msg == MSG_OBST_DROITE) cote = COTE_DROITE;
  audioPlay(msg, cote);
  dernierMessage = msg;
  dernierMessageMs = millis();
  Serial.printf("    [voix] message %d\n", msg);
  if (msg > 0 && msg <= MSG_ALERTE_ECHEC) envoyerAnnonce(msg);
}

// Décide si un message doit être dit maintenant. Règles :
//  - chaque nouvelle situation est annoncée une fois ;
//  - un obstacle DEVANT est répété toutes les 4 s tant qu'il est là ;
//  - un obstacle sur le CÔTÉ n'est dit qu'une fois (redit s'il disparaît puis revient) ;
//  - « Continuez tout droit » : une seule fois, quand un obstacle DEVANT vient de disparaître ;
//  - « Obstacle en hauteur » passe en priorité, sans attendre la fin du message en cours.
void annoncer(int msg) {
  static int situationPrecedente = MSG_AUCUN;
  static int aDire = MSG_AUCUN;   // nouvelle situation pas encore annoncée

  if (msg != situationPrecedente) {
    if (estDevant(situationPrecedente) && !estDevant(msg)) annoncerToutDroit = true;
    situationPrecedente = msg;
    aDire = msg;
    if (msg == MSG_HAUTEUR) {   // urgent
      annoncerToutDroit = false;
      aDire = MSG_AUCUN;
      jouer(MSG_HAUTEUR);
      return;
    }
  }
  if (estDevant(msg)) annoncerToutDroit = false;

  if (millis() - dernierMessageMs < MIN_ENTRE_MESSAGES) return;   // laisser finir le message

  if (annoncerToutDroit) {
    annoncerToutDroit = false;
    jouer(MSG_TOUT_DROIT);
  } else if (aDire != MSG_AUCUN) {
    jouer(aDire);
    aDire = MSG_AUCUN;
  } else if (estDevant(msg) && millis() - dernierMessageMs >= REPETITION_MS) {
    jouer(msg);
  }
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
  Serial.printf("Casque: %s  VPS: %s  -> decision %d\n", casqueConnecte ? "ok" : "--",
                lunettesConnectees() && vpsConnecte ? "ok" : "--", msg);
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

  demarrerRadio();
  audioBegin(BT_HEADSET_NAME);
  Serial.printf("[memoire] apres Bluetooth : libre=%u, plus grand bloc=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Annonce « Système prêt » à chaque connexion du casque.
void suivreCasque() {
  const bool connecte = audioConnected();
  if (connecte && !casqueConnecte) {
    Serial.println(">>> Casque Bluetooth connecte");
    casqueConnecte = true;
    jouer(MSG_PRET);
  } else if (!connecte && casqueConnecte) {
    Serial.println(">>> Casque Bluetooth deconnecte");
    casqueConnecte = false;
  }
}

void loop() {
  for (int i = 0; i < NB_CAPTEURS; i++) {
    gererBoutonSOS();
    mettreAJourCapteur(i);
    delay(PAUSE_CAPTEUR_MS);
  }

  long tete = lunettesConnectees() ? teteDistance : -1;
  mettreAJourEtats(tete);
  int msg = decider();
  annoncer(msg);
  afficherEtat(msg);

  envoyerEtat();
  suivreSOS();
  suivreCasque();
}
