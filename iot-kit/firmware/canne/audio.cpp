#include "audio.h"
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
bool appareilTrouve(const char *nom, esp_bd_addr_t adresse, int rssi) {
  Serial.printf("[audio] appareil Bluetooth trouve : \"%s\" (signal %d)\n", nom, rssi);
  if (strlen(nomRecherche) == 0) return false;
  String a = nom, b = nomRecherche;
  a.toLowerCase();
  b.toLowerCase();
  return a.indexOf(b) >= 0;   // une partie du nom suffit
}

}  // namespace

void audioBegin(const char *nomCasque) {
  nomRecherche = nomCasque;
  if (strlen(nomCasque) == 0) {
    Serial.println("[audio] BT_HEADSET_NAME vide : la canne liste les appareils trouves sans s'y connecter");
  } else {
    Serial.printf("[audio] recherche du casque \"%s\"...\n", nomCasque);
  }
  a2dp.set_local_name("Canne IFRI");
  a2dp.set_ssid_callback(appareilTrouve);
  a2dp.set_auto_reconnect(true);
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
