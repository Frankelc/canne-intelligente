#pragma once

// Configuration des LUNETTES (passerelle vers le VPS) : copier ce fichier sous le nom
// config.h, puis renseigner. Ne pas publier config.h.

// Identifiant du système sur le VPS (topics MQTT, tableau de bord).
#define DEVICE_ID "canne01"

// Partage de connexion du téléphone. WIFI_SSID doit être IDENTIQUE dans canne/config.h :
// la canne s'en sert pour se caler sur le même canal radio que les lunettes.
// Laisser "" pour fonctionner sans Wi-Fi (liaison avec la canne seule, sans SOS ni tableau de bord).
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// --- Remplacer ce bloc par le contenu généré sur le VPS (vps-user/install.sh) ---
#define MQTT_HOST ""
#define MQTT_PORT 8883
#define MQTT_USER "canne01"
#define MQTT_PASSWORD ""
#define TOPIC_PREFIX "ifri/seiot/projet3/canne01"
#define MQTT_CA_CERT ""
// --- Fin du bloc ---

// 1 = se connecter sans vérifier le certificat du VPS (banc de test uniquement).
#define MQTT_ALLOW_INSECURE 0

// Batterie des lunettes (3 piles AA, mesurée sur VIN) : pont diviseur vers une broche ADC1
// libre (GPIO 32, 33, 35, 36 ou 39 ; le GPIO 34 sert au capteur ultrason).
// -1 = pas de mesure, le tableau de bord affiche « — ».
#define BATTERY_ADC_PIN -1
#define BATTERY_DIVIDER_RATIO 2.0f  // 2.0 pour deux résistances égales (ex. 100 kΩ + 100 kΩ) : 4,8 V -> 2,4 V sur l'ADC
