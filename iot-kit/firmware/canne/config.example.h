#pragma once

// Copier vers config.h (dans ce dossier) puis renseigner. Ne pas publier config.h.

#define DEVICE_ID "canne01"

// Partage de connexion du téléphone. WIFI_SSID doit être IDENTIQUE dans lunettes.ino :
// les lunettes s'en servent pour trouver le canal radio de la canne.
// Laisser "" pour fonctionner sans Wi-Fi (guidage seul, sans SOS ni tableau de bord).
#define WIFI_SSID "i14 Pro Max"
#define WIFI_PASSWORD "prudencem"

// Casque audio Bluetooth : son nom (ou une partie, majuscules ignorées), tel qu'il
// apparaît dans la liste Bluetooth d'un téléphone. Laisser "" : la canne affiche dans le
// moniteur série les appareils trouvés, pour connaître le nom exact.
#define BT_HEADSET_NAME ""

#define MQTT_HOST ""
#define MQTT_PORT 8883
#define MQTT_USER "canne01"
#define MQTT_PASSWORD ""
#define TOPIC_PREFIX "ifri/seiot/projet3/canne01"
#define MQTT_CA_CERT ""
// --- Fin du bloc ---

// 1 = se connecter sans vérifier le certificat du VPS (banc de test uniquement).
#define MQTT_ALLOW_INSECURE 0

// Batterie (3 piles AA, mesurée sur VIN) : pont diviseur vers une broche ADC1 libre (GPIO33, 36 ou 39 ; les GPIO 32, 34
// et 35 servent aux ultrasons). -1 = pas de mesure, le tableau de bord affiche « — ».
#define BATTERY_ADC_PIN -1
#define BATTERY_DIVIDER_RATIO 2.0f  // 2.0 pour deux résistances égales (ex. 100 kΩ + 100 kΩ) : 4,8 V -> 2,4 V sur l'ADC
