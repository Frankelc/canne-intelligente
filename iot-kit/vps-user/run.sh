#!/usr/bin/env bash
# Gestion des services de la canne, sans droits administrateur.
#   run.sh start | stop | restart | status | ensure
# « ensure » (appelé par la crontab toutes les 2 min) redémarre seulement un service arrêté.
set -uo pipefail

BASE="$(cd "$(dirname "$0")" && pwd)"
cd "$BASE"
export LD_LIBRARY_PATH="$BASE/mosq/usr/lib/x86_64-linux-gnu"
set -a; . ./.env; set +a

SERVICES=(mosquitto telegram dashboard)

launch() {
  case "$1" in
    mosquitto)
      exec "$BASE/mosq/usr/sbin/mosquitto" -c "$BASE/mosquitto/mosquitto.conf" ;;
    telegram)
      MQTT_HOST=127.0.0.1 MQTT_PORT=1883 SOS_SEEN_FILE="$BASE/data/sos_seen.json" \
        exec "$BASE/venv/bin/python" -u "$BASE/app/bridge.py" ;;
    dashboard)
      MQTT_HOST=127.0.0.1 MQTT_PORT=1883 DASHBOARD_PORT=8443 \
        DASHBOARD_TLS_CERT="$BASE/certs/server.crt" DASHBOARD_TLS_KEY="$BASE/certs/server.key" \
        exec "$BASE/venv/bin/python" -u "$BASE/app/dashboard.py" ;;
  esac
}

pid_of() { cat "run/$1.pid" 2>/dev/null; }
running() { local pid; pid="$(pid_of "$1")"; [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; }

start_one() {
  running "$1" && return
  # Journal limité à 5 Mo
  [ -f "logs/$1.log" ] && [ "$(stat -c %s "logs/$1.log")" -gt 5000000 ] && : > "logs/$1.log"
  (launch "$1" >> "logs/$1.log" 2>&1) &
  echo $! > "run/$1.pid"
}

stop_one() {
  local pid; pid="$(pid_of "$1")"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -9 "$pid" 2>/dev/null
  fi
  rm -f "run/$1.pid"
}

case "${1:-status}" in
  start|ensure) for s in "${SERVICES[@]}"; do start_one "$s"; sleep 1; done ;;
  stop) for s in dashboard telegram mosquitto; do stop_one "$s"; done ;;
  restart) for s in dashboard telegram mosquitto; do stop_one "$s"; done
           for s in "${SERVICES[@]}"; do start_one "$s"; sleep 1; done ;;
  status) for s in "${SERVICES[@]}"; do
            if running "$s"; then echo "$s : en marche (pid $(pid_of "$s"))"; else echo "$s : ARRÊTÉ (voir logs/$s.log)"; fi
          done ;;
  *) echo "usage : $0 start|stop|restart|status|ensure"; exit 1 ;;
esac
