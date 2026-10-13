"""Relais MQTT SOS vers Telegram, avec accusé de réception vers la canne.

La canne republie le même SOS (même event_id) toutes les 5 s tant qu'elle n'a pas reçu
d'accusé sur <préfixe>/sos/ack. Le relais n'envoie Telegram qu'une fois par event_id
(liste conservée dans /data, donc après un redémarrage aussi) et répond à chaque répétition.
Sans accusé, la canne continue d'essayer : un échec Telegram n'est jamais perdu en silence.
Le jeton Telegram reste sur le VPS.
"""
import json
import os
import re
import time
from pathlib import Path

import paho.mqtt.client as mqtt
import requests

BROKER = os.getenv("MQTT_HOST", "mosquitto")
PORT = int(os.getenv("MQTT_PORT", "1883"))
USER = os.environ["MQTT_TELEGRAM_USER"]
PASSWORD = os.environ["MQTT_TELEGRAM_PASSWORD"]
PREFIX = os.getenv("MQTT_TOPIC_PREFIX", "ifri/seiot/projet3/canne01").strip("/")
TOKEN = os.environ["TELEGRAM_BOT_TOKEN"]
CHAT_IDS = [c.strip() for c in os.environ["TELEGRAM_CHAT_ID"].split(",") if c.strip()]
COOLDOWN = int(os.getenv("SOS_COOLDOWN_SECONDS", "30"))
SEEN_FILE = Path(os.getenv("SOS_SEEN_FILE", "/data/sos_seen.json"))

SOS_TOPIC = f"{PREFIX}/sos"
ACK_TOPIC = f"{PREFIX}/sos/ack"
EVENT_ID = re.compile(r"^[A-Za-z0-9_-]{1,40}$")
MAX_SEEN = 300


def load_seen() -> dict[str, str]:
    try:
        data = json.loads(SEEN_FILE.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_seen() -> None:
    while len(seen) > MAX_SEEN:
        seen.pop(next(iter(seen)))
    tmp = SEEN_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(seen), encoding="utf-8")
    tmp.replace(SEEN_FILE)


seen = load_seen()
last_sent = 0.0


def send_telegram(text: str) -> bool:
    """Envoie à chaque destinataire ; succès si au moins un l'a reçu."""
    delivered = False
    for chat_id in CHAT_IDS:
        try:
            response = requests.post(
                f"https://api.telegram.org/bot{TOKEN}/sendMessage",
                json={"chat_id": chat_id, "text": text}, timeout=10,
            )
            if response.ok and response.json().get("ok"):
                delivered = True
            else:
                print(f"Telegram a refusé l'envoi vers {chat_id} : HTTP {response.status_code} {response.text[:200]}")
        except (requests.RequestException, ValueError) as exc:
            # Ne pas afficher exc : son message contient l'URL, donc le jeton du bot.
            print(f"Telegram injoignable ({type(exc).__name__})")
    return delivered


def ack(client, event_id: str, status: str) -> None:
    client.publish(ACK_TOPIC, json.dumps({"event_id": event_id, "status": status}), qos=1)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("MQTT connecté ; abonnement SOS actif")
        client.subscribe(SOS_TOPIC, qos=1)
    else:
        print(f"Connexion MQTT refusée : {reason_code}")


def on_message(client, userdata, message):
    global last_sent
    try:
        data = json.loads(message.payload.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        print("SOS ignoré : JSON invalide")
        return
    if not isinstance(data, dict) or data.get("event") != "SOS":
        print("Message ignoré : type inattendu")
        return
    event_id = str(data.get("event_id", ""))
    if not EVENT_ID.match(event_id):
        print("SOS ignoré : event_id invalide")
        return
    if event_id in seen:
        ack(client, event_id, seen[event_id])  # répétition : l'accusé précédent a pu être perdu
        return

    now = time.time()
    if now - last_sent < COOLDOWN:
        status = "deja_signale"
        print(f"SOS {event_id} reçu pendant la temporisation : proche déjà prévenu")
    else:
        device = str(data.get("device", "canne"))[:40]
        text = f"🆘 SOS — {device}\nLa personne demande de l'aide."
        try:
            age = int(data.get("age_s", 0))
        except (TypeError, ValueError):
            age = 0
        if age >= 60:
            text += f"\nAlerte retardée de {age // 60} min (réseau indisponible)."
        if not send_telegram(text):
            print(f"SOS {event_id} non transmis ; la canne va réessayer")
            return
        last_sent = now
        status = "envoye"
        print(f"Alerte Telegram envoyée pour {device} ({event_id})")

    seen[event_id] = status
    try:
        save_seen()
    except OSError as exc:
        print(f"Liste des SOS non enregistrée : {exc}")
    ack(client, event_id, status)


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ifri-telegram-bridge")
client.username_pw_set(USER, PASSWORD)
# Trafic interne au réseau Docker privé ; seul le port TLS 8883 est publié.
client.on_connect = on_connect
client.on_message = on_message
client.reconnect_delay_set(min_delay=1, max_delay=60)
client.connect_async(BROKER, PORT, keepalive=45)
client.loop_forever(retry_first_connection=True)
