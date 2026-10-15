#include "audio.h"
#include <Preferences.h>
#include "BluetoothA2DPSource.h"
#include "audio_clips.h"

namespace {

BluetoothA2DPSource a2dp;
portMUX_TYPE verrou = portMUX_INITIALIZER_UNLOCKED;

// Message en cours (partagé entre loop() et la tâche Bluetooth)
const int16_t *clip = nullptr;
uint32_t clipLen = 0;
uint32_t position = 0;      // en échantillons de sortie (44 100 Hz)
int cote = COTE_DEUX;
uint32_t generation = 0;    // change à chaque nouveau message

const char *nomRecherche = "";

// Le casque reçoit du 44 100 Hz ; les messages sont en 11 025 Hz : 4 échantillons
// de sortie par échantillon stocké, avec interpolation linéaire.
const uint32_t FACTEUR = 44100 / CLIP_SAMPLE_RATE;

// Appelée par la pile Bluetooth : remplit n trames stéréo (silence s'il n'y a rien à dire).
int32_t fournirSon(Frame *trames, int32_t n) {
  const int16_t *c;
  uint32_t len, p, gen;
  int ct;
  portENTER_CRITICAL(&verrou);
  c = clip; len = clipLen; p = position; ct = cote; gen = generation;
  portEXIT_CRITICAL(&verrou);

  for (int32_t i = 0; i < n; i++) {
    int16_t s = 0;
    if (c != nullptr) {
      const uint32_t idx = p / FACTEUR;
      if (idx + 1 < len) {
        const int32_t a = c[idx];
        const int32_t b = c[idx + 1];
        s = (int16_t)(a + (b - a) * (int32_t)(p % FACTEUR) / (int32_t)FACTEUR);
        p++;
      } else {
        c = nullptr;   // message terminé
      }
    }
    trames[i].channel1 = (ct == COTE_DROITE) ? 0 : s;   // gauche
    trames[i].channel2 = (ct == COTE_GAUCHE) ? 0 : s;   // droite
  }

  portENTER_CRITICAL(&verrou);
  if (generation == gen) {   // pas de nouveau message entre-temps
    position = p;
    if (c == nullptr) clip = nullptr;
  }
  portEXIT_CRITICAL(&verrou);
  return n;
}

// Appelée pour chaque appareil Bluetooth trouvé : true = s'y connecter.
// Nom EXACT exigé (majuscules et espaces autour ignorés) : la canne ne se connecte qu'au
// casque choisi, jamais à un appareil dont le nom ne fait que lui ressembler.
bool appareilTrouve(const char *nom, esp_bd_addr_t adresse, int rssi) {
  if (strlen(nomRecherche) == 0) {
    Serial.printf("[audio] appareil Bluetooth trouve : \"%s\" (signal %d)\n", nom, rssi);
    return false;
  }
  String a = nom, b = nomRecherche;
  a.trim();
  b.trim();
  const bool cestLui = a.equalsIgnoreCase(b);
  Serial.printf("[audio] appareil Bluetooth trouve : \"%s\" (signal %d)%s\n", nom, rssi,
                cestLui ? " -> c'est le casque, connexion" : " -> ignore");
  return cestLui;
}

}  // namespace

void audioBegin(const char *nomCasque) {
  nomRecherche = nomCasque;
  if (strlen(nomCasque) == 0) {
    Serial.println("[audio] BT_HEADSET_NAME vide : la canne liste les appareils trouves sans s'y connecter");
  } else {
    Serial.printf("[audio] recherche du casque \"%s\"...\n", nomCasque);
  }
  // La bibliothèque retient l'adresse du dernier casque (mémoire permanente, espace
  // "connected_bda", clé "src_bda") et s'y reconnecte en priorité. Si le nom configuré a
  // changé, on efface cette adresse pour chercher directement le nouveau casque.
  // (clean_last_connection() de la bibliothèque n'efface rien avant start().)
  Preferences prefs;
  prefs.begin("canne", false);
  if (prefs.getString("nom_casque", "") != String(nomCasque)) {
    Preferences bda;
    if (bda.begin("connected_bda", false)) {
      bda.remove("src_bda");
      bda.end();
    }
    prefs.putString("nom_casque", nomCasque);
    Serial.println("[audio] nouveau casque configure : l'ancien casque est oublie");
  }
  prefs.end();

  a2dp.set_local_name("Canne IFRI");
  a2dp.set_ssid_callback(appareilTrouve);
  a2dp.set_auto_reconnect(true, 5);   // 5 essais sur le dernier casque, puis recherche par nom
  a2dp.set_data_callback_in_frames(fournirSon);
  a2dp.set_volume(100);   // 0 à 127
  a2dp.start();
}

void audioPlay(int n, int c) {
  if (n < 1 || n > CLIP_COUNT) return;
  portENTER_CRITICAL(&verrou);
  clip = CLIP_DATA[n];
  clipLen = CLIP_LEN[n];
  position = 0;
  cote = c;
  generation++;
  portEXIT_CRITICAL(&verrou);
}

bool audioConnected() {
  return a2dp.is_connected();
}
