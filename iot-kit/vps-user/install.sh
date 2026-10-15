#!/usr/bin/env bash
# Installation SANS droits administrateur, entièrement dans le dossier de ce script (~/iot-canne).
# Pour un VPS partagé : pas de sudo, pas de Docker, pas de pare-feu, ports 80/443 non utilisés.
# Rien en dehors de ce dossier n'est modifié, sauf deux lignes ajoutées à la crontab de l'utilisateur
# (marquées « # iot-canne »), pour redémarrer les services après un reboot.
#   bash install.sh
set -euo pipefail

BASE="$(cd "$(dirname "$0")" && pwd)"
cd "$BASE"
. ./lib.sh

[ -f .env ] || die ".env manquant dans $BASE"
chmod 600 .env
mkdir -p data logs certs mosquitto run pkg

# 1. Mosquitto, extrait des paquets Ubuntu sans installation système
if [ ! -x mosq/usr/sbin/mosquitto ]; then
  step "Mosquitto"
  (cd pkg && apt download mosquitto mosquitto-clients libmosquitto1 libdlt2 libwebsockets19t64)
  for deb in pkg/*.deb; do dpkg -x "$deb" mosq; done
fi
export LD_LIBRARY_PATH="$BASE/mosq/usr/lib/x86_64-linux-gnu"
MOSQ_PASSWD="$BASE/mosq/usr/bin/mosquitto_passwd"

# 2. Python isolé dans venv/
if [ ! -x venv/bin/pip ]; then
  step "Environnement Python"
  python3 -m venv --without-pip venv
  curl -fsSL https://bootstrap.pypa.io/get-pip.py -o pkg/get-pip.py
  venv/bin/python pkg/get-pip.py -q
fi
step "Dépendances Python"
venv/bin/pip install -q -r requirements.txt

# 3. Secrets : on génère ceux qui sont encore à CHANGE_ME
step "Mots de passe"
for key in TELEGRAM_BOT_TOKEN TELEGRAM_CHAT_ID PUBLIC_HOST; do
  case "$(env_get "$key")" in ""|*REPLACE*) die "$key n'est pas renseigné dans .env" ;; esac
done
for key in MQTT_PASSWORD MQTT_TELEGRAM_PASSWORD MQTT_DASHBOARD_PASSWORD DASHBOARD_PASSWORD; do
  case "$(env_get "$key")" in ""|CHANGE_ME*) env_set "$key" "$(openssl rand -hex 16)" ;; esac
done
hash="$(venv/bin/python -c 'import sys; from werkzeug.security import generate_password_hash; print(generate_password_hash(sys.argv[1]))' "$(env_get DASHBOARD_PASSWORD)")"
env_set DASHBOARD_AUTH_HASH "'$hash'"

# 4. Certificats : autorité de certification propre au projet (l'ESP32 la connaît)
step "Certificats"
PUBLIC_HOST="$(env_get PUBLIC_HOST)"
DOMAIN="$(env_get DOMAIN)"
MQTT_HOST="$PUBLIC_HOST"
# DNS:<IP> en plus de IP:<IP> : la vérification de nom de mbedTLS (ESP32) compare le texte des
# entrées DNS ; les navigateurs, eux, utilisent l'entrée IP.
san="IP:$PUBLIC_HOST,DNS:$PUBLIC_HOST"
case "$DOMAIN" in ""|*REPLACE*) ;; *) san="$san,DNS:$DOMAIN"; MQTT_HOST="$DOMAIN" ;; esac
if [ ! -f certs/ca.crt ]; then
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 -subj "/CN=Canne IFRI SEIoT CA" \
    -keyout certs/ca.key -out certs/ca.crt 2>/dev/null
fi
if [ ! -f certs/server.crt ] || [ "$(cat certs/server.san 2>/dev/null)" != "$san" ]; then
  openssl req -newkey rsa:2048 -nodes -subj "/CN=$PUBLIC_HOST" \
    -keyout certs/server.key -out certs/server.csr 2>/dev/null
  openssl x509 -req -in certs/server.csr -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial \
    -days 825 -sha256 -out certs/server.crt 2>/dev/null -extfile <(printf '%s\n' \
      "subjectAltName=$san" "basicConstraints=CA:FALSE" \
      "keyUsage=digitalSignature,keyEncipherment" "extendedKeyUsage=serverAuth")
  echo "$san" > certs/server.san
fi
chmod 600 certs/*.key

# 5. Mosquitto : configuration, comptes et droits
step "Mosquitto : comptes et droits"
PREFIX="$(env_get MQTT_TOPIC_PREFIX)"
CANNE_USER="$(env_get MQTT_USER)"
TELEGRAM_USER="$(env_get MQTT_TELEGRAM_USER)"
DASHBOARD_MQTT_USER="$(env_get MQTT_DASHBOARD_USER)"
cat > mosquitto/mosquitto.conf <<EOF
# Généré par install.sh
persistence true
persistence_location $BASE/data/
log_dest file $BASE/logs/mosquitto.log
allow_anonymous false
password_file $BASE/mosquitto/passwd
acl_file $BASE/mosquitto/acl

# Services locaux (relais Telegram, dashboard) : boucle locale uniquement.
listener 1883 127.0.0.1
protocol mqtt

# Canne et clients externes : TLS obligatoire.
listener 8883 0.0.0.0
protocol mqtt
cafile $BASE/certs/ca.crt
certfile $BASE/certs/server.crt
keyfile $BASE/certs/server.key
tls_version tlsv1.2
EOF
rm -f mosquitto/passwd
"$MOSQ_PASSWD" -c -b mosquitto/passwd "$CANNE_USER" "$(env_get MQTT_PASSWORD)"
"$MOSQ_PASSWD" -b mosquitto/passwd "$TELEGRAM_USER" "$(env_get MQTT_TELEGRAM_PASSWORD)"
"$MOSQ_PASSWD" -b mosquitto/passwd "$DASHBOARD_MQTT_USER" "$(env_get MQTT_DASHBOARD_PASSWORD)"
cat > mosquitto/acl <<EOF
# Généré par install.sh à partir de .env

# La canne publie ses mesures et le SOS, et lit l'accusé du relais Telegram.
user $CANNE_USER
topic write $PREFIX/telemetry
topic write $PREFIX/event
topic write $PREFIX/sos
topic write $PREFIX/availability
topic read $PREFIX/sos/ack

# Le relais lit les SOS et répond par un accusé.
user $TELEGRAM_USER
topic read $PREFIX/sos
topic write $PREFIX/sos/ack

# Le dashboard lit tout et ne peut rien publier.
user $DASHBOARD_MQTT_USER
topic read $PREFIX/#
EOF
chmod 600 mosquitto/passwd mosquitto/acl

# 6. Paramètres à recopier dans firmware/lunettes/config.h
step "Paramètres ESP32"
{
  echo "// Généré par install.sh sur le VPS — à recopier dans firmware/lunettes/config.h"
  echo "#define MQTT_HOST \"$MQTT_HOST\""
  echo "#define MQTT_PORT 8883"
  echo "#define MQTT_USER \"$CANNE_USER\""
  echo "#define MQTT_PASSWORD \"$(env_get MQTT_PASSWORD)\""
  echo "#define TOPIC_PREFIX \"$PREFIX\""
  echo "#define MQTT_CA_CERT \\"
  sed 's/.*/  "&\\n" \\/' certs/ca.crt
  echo "  \"\""
} > config-mqtt.h
chmod 600 config-mqtt.h

# 7. Redémarrage automatique : uniquement des lignes ajoutées à la crontab de l'utilisateur
step "Démarrage automatique (crontab de $(whoami))"
{
  crontab -l 2>/dev/null | grep -v "# iot-canne" || true
  echo "@reboot $BASE/run.sh start >/dev/null 2>&1 # iot-canne"
  echo "*/2 * * * * $BASE/run.sh ensure >/dev/null 2>&1 # iot-canne"
} | crontab -

# 8. (Re)démarrage des services
step "Démarrage"
./run.sh restart
sleep 3
./run.sh status

cat <<EOF

=========================================================
Installation terminée (dossier $BASE).

Dashboard : https://$PUBLIC_HOST:8443
  utilisateur  : $(env_get DASHBOARD_USER)
  mot de passe : $(env_get DASHBOARD_PASSWORD)
  (certificat du projet : le navigateur affiche un avertissement la première fois)

MQTT TLS  : $MQTT_HOST:8883
ESP32     : $BASE/config-mqtt.h
Tester    : bash $BASE/test-sos.sh
=========================================================
EOF
