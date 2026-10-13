#!/usr/bin/env bash
# Publie un SOS de test avec le compte de la canne, puis attend l'accusé du relais Telegram.
set -euo pipefail

BASE="$(cd "$(dirname "$0")" && pwd)"
cd "$BASE"
. ./lib.sh
export LD_LIBRARY_PATH="$BASE/mosq/usr/lib/x86_64-linux-gnu"
BIN="$BASE/mosq/usr/bin"

USER_NAME="$(env_get MQTT_USER)"
PASS="$(env_get MQTT_PASSWORD)"
PREFIX="$(env_get MQTT_TOPIC_PREFIX)"
EVENT_ID="test-$(date +%s)"
ack_file="$(mktemp)"
trap 'rm -f "$ack_file"' EXIT

"$BIN/mosquitto_sub" -h 127.0.0.1 -p 1883 -u "$USER_NAME" -P "$PASS" -t "$PREFIX/sos/ack" -W 25 >"$ack_file" &
sub_pid=$!
sleep 1
"$BIN/mosquitto_pub" -h 127.0.0.1 -p 1883 -u "$USER_NAME" -P "$PASS" -q 1 -t "$PREFIX/sos" \
  -m "{\"v\":1,\"event\":\"SOS\",\"event_id\":\"$EVENT_ID\",\"device\":\"test-vps\",\"age_s\":0,\"attempt\":1}"
echo "SOS $EVENT_ID publié, attente de l'accusé (20 s max)…"

# On attend l'accusé de CE SOS : un accusé d'une alerte précédente peut arriver entre-temps.
for _ in $(seq 40); do grep -q "$EVENT_ID" "$ack_file" && break; sleep 0.5; done
kill "$sub_pid" 2>/dev/null || true
if grep -q "$EVENT_ID" "$ack_file"; then
  echo "Accusé reçu : $(grep "$EVENT_ID" "$ack_file")"
else
  echo "Pas d'accusé. Voir : tail -n 20 $BASE/logs/telegram.log" >&2
  exit 1
fi
