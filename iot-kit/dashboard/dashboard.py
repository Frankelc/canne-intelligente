"""Petit tableau de bord MQTT en lecture seule pour la démonstration IFRI.

Architecture finale : la canne n'a pas d'Internet. Les lunettes (passerelle) reçoivent ses
données en ESP-NOW et publient tout sur MQTT/TLS. Donc :
  - « availability » (online/offline, Last Will) décrit la PASSERELLE, pas la canne ;
  - la télémétrie indique si la canne est joignable (espnow) et si son casque est connecté
    (headset) ; rssi et uptime_s sont ceux des lunettes.
Le contrat de /api/state est inchangé : la page index.html interprète ces champs.
"""
import json
import os
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
from flask import Flask, Response, jsonify, request, send_file
from waitress import serve
from werkzeug.security import check_password_hash

PREFIX = os.getenv("MQTT_TOPIC_PREFIX", "ifri/seiot/projet3/canne01").strip("/")
BROKER = os.getenv("MQTT_HOST", "mosquitto")
PORT = int(os.getenv("MQTT_PORT", "1883"))
USER = os.environ["MQTT_DASHBOARD_USER"]
PASSWORD = os.environ["MQTT_DASHBOARD_PASSWORD"]
SOS_STATUS = {"envoye": "Telegram envoyé", "deja_signale": "Proche déjà prévenu"}

# Sans reverse proxy (VPS partagé, vps-user/) : HTTPS et mot de passe gérés ici.
# Avec Docker, Caddy s'en charge et ces variables restent vides.
HTTP_PORT = int(os.getenv("DASHBOARD_PORT", "8080"))
TLS_CERT = os.getenv("DASHBOARD_TLS_CERT")
TLS_KEY = os.getenv("DASHBOARD_TLS_KEY")
AUTH_USER = os.getenv("DASHBOARD_USER") if os.getenv("DASHBOARD_AUTH_HASH") else None
AUTH_HASH = os.getenv("DASHBOARD_AUTH_HASH", "")

app = Flask(__name__)


@app.before_request
def require_password():
    if AUTH_USER is None:
        return None
    auth = request.authorization
    if auth and auth.username == AUTH_USER and check_password_hash(AUTH_HASH, auth.password or ""):
        return None
    return Response("Authentification requise", 401, {"WWW-Authenticate": 'Basic realm="Canne connectee"'})


lock = threading.Lock()
state = {"telemetry": None, "telemetry_at": None, "availability": "inconnue", "events": [],
         "stats": {"annonces": 0, "hauteur": 0, "sos": 0}}
HEAD_MP3 = 6  # message n° 6 de la canne : « Attention, obstacle en hauteur »

INDEX = Path(__file__).with_name("index.html")


def add_event(event: dict) -> None:
    event["ts"] = time.time()
    state["events"].insert(0, event)
    del state["events"][50:]


def find_sos(event_id: str):
    return next((e for e in state["events"] if e["type"] == "SOS" and e.get("event_id") == event_id), None)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        client.subscribe([(f"{PREFIX}/telemetry", 0), (f"{PREFIX}/event", 1), (f"{PREFIX}/sos", 1),
                          (f"{PREFIX}/sos/ack", 1), (f"{PREFIX}/availability", 1)])
        print("Dashboard abonné aux sujets de la canne (publiés par la passerelle)")
    else:
        print(f"Connexion MQTT refusée : {reason_code}")


def on_message(client, userdata, message):
    suffix = message.topic.removeprefix(PREFIX + "/")
    text = message.payload.decode("utf-8", errors="replace")
    try:
        payload = json.loads(text)
    except ValueError:
        payload = text
    with lock:
        if suffix == "telemetry" and isinstance(payload, dict):
            state["telemetry"] = payload
            state["telemetry_at"] = time.time()
        elif suffix == "availability":
            state["availability"] = str(payload)[:20]
        elif suffix == "sos" and isinstance(payload, dict):
            # event_id = « 8 caractères hexadécimaux-numéro », ex. a3f09c1e-7 (lunettes/iot.cpp)
            event_id = str(payload.get("event_id", ""))[:40]
            if not find_sos(event_id):  # la passerelle répète le SOS jusqu'à l'accusé
                add_event({"type": "SOS", "message": "Demande d'assistance", "event_id": event_id,
                           "status": "En attente de Telegram", "confirmed": False})
                state["stats"]["sos"] += 1
        elif suffix == "sos/ack" and isinstance(payload, dict):
            event = find_sos(str(payload.get("event_id", "")))
            if event:
                event["status"] = SOS_STATUS.get(payload.get("status"), str(payload.get("status"))[:30])
                event["confirmed"] = True
        elif suffix == "event" and isinstance(payload, dict):
            event_type = str(payload.get("type", "événement"))[:30]
            add_event({"type": event_type, "message": str(payload.get("message", ""))[:120]})
            if event_type == "annonce":
                state["stats"]["annonces"] += 1
                if payload.get("mp3") == HEAD_MP3:
                    state["stats"]["hauteur"] += 1


@app.get("/")
def index():
    return send_file(INDEX)


@app.get("/api/state")
def api_state():
    with lock:
        return jsonify({**state, "now": time.time()})


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ifri-iot-dashboard")
client.username_pw_set(USER, PASSWORD)
client.on_connect = on_connect
client.on_message = on_message
client.reconnect_delay_set(min_delay=1, max_delay=60)
client.connect_async(BROKER, PORT, keepalive=45)
client.loop_start()
if TLS_CERT and TLS_KEY:
    from cheroot import wsgi
    from cheroot.ssl.builtin import BuiltinSSLAdapter

    server = wsgi.Server(("0.0.0.0", HTTP_PORT), app, numthreads=8)
    server.ssl_adapter = BuiltinSSLAdapter(TLS_CERT, TLS_KEY)
    print(f"Dashboard HTTPS sur le port {HTTP_PORT}")
    server.start()
else:
    serve(app, host="0.0.0.0", port=HTTP_PORT, threads=4)
