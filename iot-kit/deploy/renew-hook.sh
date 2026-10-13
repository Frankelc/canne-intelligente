#!/usr/bin/env bash
# Copie le certificat Let's Encrypt dans iot-kit/certs et recharge les services.
# Appelé par deploy.sh, puis automatiquement par certbot à chaque renouvellement.
set -euo pipefail

KIT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KIT_DIR"
. deploy/lib.sh

LIVE="${RENEWED_LINEAGE:-/etc/letsencrypt/live/$(env_get DOMAIN)}"
[ -f "$LIVE/fullchain.pem" ] || die "Certificat introuvable dans $LIVE"

# Mosquitto tourne sous l'utilisateur 1883 dans son conteneur.
install -d -m 755 certs
install -m 644 -o 1883 -g 1883 "$LIVE/fullchain.pem" certs/fullchain.pem
install -m 600 -o 1883 -g 1883 "$LIVE/privkey.pem" certs/privkey.pem

if [ -n "$(docker compose ps -q --status running mosquitto 2>/dev/null)" ]; then
  docker compose restart mosquitto caddy
fi
