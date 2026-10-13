#!/usr/bin/env bash
# Écrit deploy/config-mqtt.h : les lignes MQTT à recopier dans firmware/canne/config.h.
# Contient un mot de passe : ne pas le committer ni le partager.
set -euo pipefail

KIT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KIT_DIR"
. deploy/lib.sh

out=deploy/config-mqtt.h
# Racine de confiance de Let's Encrypt, utilisée par l'ESP32 pour vérifier le broker.
ca="$(curl -fsSL https://letsencrypt.org/certs/isrgrootx1.pem)"
grep -q "BEGIN CERTIFICATE" <<<"$ca" || die "Téléchargement du certificat ISRG Root X1 impossible"

{
  echo "// Généré par deploy/esp32-config.sh — à recopier dans firmware/canne/config.h"
  echo "#define MQTT_HOST \"$(env_get DOMAIN)\""
  echo "#define MQTT_PORT 8883"
  echo "#define MQTT_USER \"$(env_get MQTT_USER)\""
  echo "#define MQTT_PASSWORD \"$(env_get MQTT_PASSWORD)\""
  echo "#define TOPIC_PREFIX \"$(env_get MQTT_TOPIC_PREFIX)\""
  echo "#define MQTT_CA_CERT \\"
  sed 's/.*/  "&\\n" \\/' <<<"$ca"
  echo "  \"\""
} > "$out"
chmod 600 "$out"
echo "Paramètres ESP32 écrits dans $out"
