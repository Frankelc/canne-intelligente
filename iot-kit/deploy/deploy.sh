#!/usr/bin/env bash
# Installe le kit IoT sur un VPS Ubuntu/Debian, depuis le dossier iot-kit :
#   sudo bash deploy/deploy.sh
# Relançable sans risque : les mots de passe déjà générés et le certificat existant sont conservés.
set -euo pipefail

KIT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KIT_DIR"
. deploy/lib.sh

[ "$(id -u)" -eq 0 ] || die "Lancer avec sudo : sudo bash deploy/deploy.sh"

if [ ! -f .env ]; then
  cp .env.example .env
  chmod 600 .env
  die "Fichier .env créé. Renseigner DOMAIN, ACME_EMAIL, TELEGRAM_BOT_TOKEN et TELEGRAM_CHAT_ID, puis relancer."
fi
chmod 600 .env

# 1. Paramètres obligatoires
for key in DOMAIN ACME_EMAIL TELEGRAM_BOT_TOKEN TELEGRAM_CHAT_ID; do
  value="$(env_get "$key")"
  case "$value" in ""|*REPLACE*) die "$key n'est pas renseigné dans .env" ;; esac
done
DOMAIN="$(env_get DOMAIN)"
ACME_EMAIL="$(env_get ACME_EMAIL)"

# 2. Paquets système et Docker
step "Installation des paquets"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq ca-certificates curl openssl certbot ufw >/dev/null
if ! command -v docker >/dev/null; then
  step "Installation de Docker"
  curl -fsSL https://get.docker.com | sh
fi
docker compose version >/dev/null || die "Docker Compose v2 introuvable"

# 3. Vérification DNS : le domaine doit pointer vers ce VPS, sinon Let's Encrypt échoue
step "Vérification DNS de $DOMAIN"
public_ip="$(curl -4 -fsS https://api.ipify.org || true)"
dns_ip="$(getent ahostsv4 "$DOMAIN" | awk 'NR==1{print $1}' || true)"
if [ -z "$dns_ip" ]; then
  die "$DOMAIN ne résout vers aucune adresse. Créer l'enregistrement DNS A vers $public_ip."
elif [ -n "$public_ip" ] && [ "$dns_ip" != "$public_ip" ]; then
  warn "$DOMAIN pointe vers $dns_ip, mais ce VPS a l'IP $public_ip."
fi

# 4. Secrets : on génère ceux qui sont encore à CHANGE_ME
step "Génération des mots de passe"
for key in MQTT_PASSWORD MQTT_TELEGRAM_PASSWORD MQTT_DASHBOARD_PASSWORD DASHBOARD_PASSWORD; do
  case "$(env_get "$key")" in ""|CHANGE_ME*) env_set "$key" "$(openssl rand -hex 16)" ;; esac
done
hash="$(docker run --rm caddy:2.8 caddy hash-password --plaintext "$(env_get DASHBOARD_PASSWORD)")"
env_set DASHBOARD_PASSWORD_HASH "'$hash'"

# 5. Comptes et droits MQTT : chaque compte n'a accès qu'aux sujets dont il a besoin
step "Création des comptes MQTT"
CANNE_USER="$(env_get MQTT_USER)"
TELEGRAM_USER="$(env_get MQTT_TELEGRAM_USER)"
DASHBOARD_MQTT_USER="$(env_get MQTT_DASHBOARD_USER)"
PREFIX="$(env_get MQTT_TOPIC_PREFIX)"
rm -f mosquitto/passwd
docker run --rm -v "$KIT_DIR/mosquitto:/mosquitto/config" eclipse-mosquitto:2 sh -c \
  'mosquitto_passwd -c -b /mosquitto/config/passwd "$1" "$2" &&
   mosquitto_passwd -b /mosquitto/config/passwd "$3" "$4" &&
   mosquitto_passwd -b /mosquitto/config/passwd "$5" "$6"' _ \
  "$CANNE_USER" "$(env_get MQTT_PASSWORD)" \
  "$TELEGRAM_USER" "$(env_get MQTT_TELEGRAM_PASSWORD)" \
  "$DASHBOARD_MQTT_USER" "$(env_get MQTT_DASHBOARD_PASSWORD)"

cat > mosquitto/acl <<EOF
# Généré par deploy/deploy.sh à partir de .env : modifier .env puis relancer le script.

# La canne publie ses mesures et le SOS, et lit l'accusé du relais Telegram.
user $CANNE_USER
topic write $PREFIX/telemetry
topic write $PREFIX/event
topic write $PREFIX/sos
topic write $PREFIX/availability
topic read $PREFIX/sos/ack

# Le relais lit les SOS et répond par un accusé ; il n'a pas accès aux autres sujets.
user $TELEGRAM_USER
topic read $PREFIX/sos
topic write $PREFIX/sos/ack

# Le dashboard lit tout et ne peut rien publier.
user $DASHBOARD_MQTT_USER
topic read $PREFIX/#
EOF
chown 1883:1883 mosquitto/passwd mosquitto/acl
chmod 600 mosquitto/passwd mosquitto/acl

# 6. Pare-feu : SSH, 80 (renouvellement du certificat), 443 (dashboard), 8883 (MQTT TLS)
step "Pare-feu"
for port in $(sshd -T 2>/dev/null | awk '$1=="port"{print $2}'); do ufw allow "$port/tcp" >/dev/null; done
ufw allow 22/tcp >/dev/null
for port in 80 443 8883; do ufw allow "$port/tcp" >/dev/null; done
ufw --force enable >/dev/null
warn "Ouvrir aussi les ports 80, 443 et 8883 dans le pare-feu de l'hébergeur s'il en a un."

# 7. Certificat Let's Encrypt (RSA : chaîne simple vers ISRG Root X1 pour l'ESP32)
step "Certificat TLS"
if [ ! -f "/etc/letsencrypt/live/$DOMAIN/fullchain.pem" ]; then
  certbot certonly --standalone --non-interactive --agree-tos -m "$ACME_EMAIL" \
    -d "$DOMAIN" --key-type rsa --deploy-hook "$KIT_DIR/deploy/renew-hook.sh"
fi
chmod +x deploy/*.sh
bash deploy/renew-hook.sh

# 8. Démarrage des services (force-recreate : prise en compte des nouveaux comptes et de l'ACL)
step "Démarrage des conteneurs"
docker compose up -d --build --force-recreate
sleep 5
docker compose ps

# 9. Contrôle TLS depuis l'extérieur du conteneur
step "Test du port MQTT TLS"
if echo | openssl s_client -connect "$DOMAIN:8883" -servername "$DOMAIN" 2>/dev/null | grep -q "Verify return code: 0"; then
  echo "MQTT TLS OK sur $DOMAIN:8883"
else
  warn "Le test TLS sur $DOMAIN:8883 a échoué : voir docker compose logs mosquitto"
fi

# 10. Fichier prêt à copier dans firmware/canne/config.h
bash deploy/esp32-config.sh

cat <<EOF

=========================================================
Déploiement terminé.

Dashboard : https://$DOMAIN
  utilisateur  : $(env_get DASHBOARD_USER)
  mot de passe : $(env_get DASHBOARD_PASSWORD)

Paramètres ESP32 : deploy/config-mqtt.h (à recopier dans firmware/canne/config.h)
Tester Telegram  : sudo bash deploy/test-sos.sh
Logs             : docker compose logs -f
=========================================================
EOF
