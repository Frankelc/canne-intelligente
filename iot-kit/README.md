# Kit IoT : canne, lunettes, VPS et Telegram

Partie IoT du projet 3 (IFRI, certification SEIoT) : canne et lunettes intelligentes pour personnes non voyantes.

Chemin des données dans l'architecture finale, validée sur le matériel (SOS Telegram compris) :

```text
Casque Bluetooth ◄── Canne (ESP32) ◄──ESP-NOW──► Lunettes (ESP32, passerelle) ──MQTT/TLS 8883──► VPS ──► Telegram
   voix A2DP        3 ultrasons, décision,        capteur tête + Wi-Fi                          Mosquitto, relais Telegram,
                    voix, bouton SOS              publie tout sur le VPS                         tableau de bord HTTPS 8443
```

- **La canne n'a pas d'Internet.** Son ESP32 n'a pas assez de mémoire pour le Bluetooth audio et une connexion TLS en même temps (13 Ko libres avant la répartition, 73 Ko après). Elle mesure, décide et parle dans le casque Bluetooth, même sans réseau.
- **Les lunettes servent de passerelle.** Elles reçoivent de la canne les distances, les annonces et les SOS (ESP-NOW), les publient sur le VPS, et renvoient à la canne l'état du VPS et la confirmation des SOS.
- **Le VPS** reçoit tout en MQTT/TLS, prévient le proche sur Telegram et affiche le tableau de bord.

Les raisons de ces choix sont détaillées dans [CHOIX_IOT.md](CHOIX_IOT.md).

## Contenu

| Dossier | Rôle |
|---|---|
| `firmware/canne/` | Canne : 3 HC-SR04, décision, voix Bluetooth (clips en mémoire flash), bouton SOS, ESP-NOW |
| `firmware/lunettes/` | Lunettes : HC-SR04 tête, ESP-NOW, passerelle Wi-Fi + MQTT/TLS (`iot.cpp`) |
| `dashboard/` | Tableau de bord Flask (`dashboard.py`) et sa page (`index.html`) |
| `telegram-bridge/` | Relais SOS vers Telegram (`bridge.py`), avec accusé de réception |
| `vps-user/` | **Installation actuelle** sur le VPS partagé, sans `sudo` ni Docker |
| `mosquitto/` | Configuration du courtier MQTT |
| `deploy/`, `docker-compose.yml`, `caddy/` | Variante Docker pour un VPS dont on est administrateur (non utilisée) |
| `Code/` | Premiers programmes de l'équipe, gardés pour mémoire (obsolètes) |

## Installation actuelle : VPS partagé (`vps-user/`)

Le VPS du projet (185.215.164.23) est partagé : les ports 80 et 443 appartiennent à un autre service, et le compte `groupe3` n'a ni `sudo` ni Docker. `vps-user/install.sh` installe donc tout dans `~/iot-canne`, sans rien modifier d'autre sur la machine :

- Mosquitto est extrait des paquets Ubuntu (sans installation système) ; Python tourne dans `~/iot-canne/venv` ;
- MQTT/TLS sur le port **8883**, tableau de bord HTTPS avec mot de passe sur le port **8443** ;
- certificats signés par une autorité propre au projet (`certs/ca.crt`), connue de l'ESP32 des lunettes ; le navigateur affiche un avertissement la première fois ;
- redémarrage automatique : deux lignes `# iot-canne` dans la crontab de `groupe3` ;
- à la fin, `~/iot-canne/config-mqtt.h` contient le bloc MQTT à recopier dans `firmware/lunettes/config.h`.

Commandes sur le VPS :

```sh
bash ~/iot-canne/run.sh status          # état des 3 services
bash ~/iot-canne/run.sh restart
bash ~/iot-canne/test-sos.sh            # SOS de test vers Telegram, sans ESP32
tail -f ~/iot-canne/logs/telegram.log   # journaux : mosquitto, telegram, dashboard
grep DASHBOARD_PASSWORD ~/iot-canne/.env
```

### Mettre à jour le tableau de bord ou le relais

Depuis le PC (PowerShell), dans le dossier `iot-kit` :

```sh
scp telegram-bridge/bridge.py dashboard/dashboard.py dashboard/index.html groupe3@185.215.164.23:~/iot-canne/app/
ssh groupe3@185.215.164.23 "bash ~/iot-canne/run.sh restart"
```

Si `vps-user/*` a changé, copier aussi ces fichiers dans `~/iot-canne/` puis lancer `bash ~/iot-canne/install.sh` (relançable : mots de passe et certificats existants conservés).

## Firmwares ESP32

Arduino IDE, cœur **Arduino-ESP32 3.x**, carte **ESP32 Dev Module**, moniteur série à 115200 bauds. Les fichiers `config.h` contiennent des secrets : ils ne sont jamais publiés (voir `.gitignore`). On les crée en copiant `config.example.h`.

### Canne : `firmware/canne/`

- Bibliothèque **ESP32-A2DP** (pschatzmann, depuis GitHub).
- **Outils → Partition Scheme → « Huge APP (3MB No OTA/1MB SPIFFS) »** : le Bluetooth et les messages vocaux dépassent la taille par défaut.
- `config.h` : `WIFI_SSID` (le même nom que dans les lunettes ; la canne ne s'y connecte pas, elle s'en sert pour se caler sur le même canal radio) et `BT_HEADSET_NAME` (nom **exact** du casque, majuscules ignorées : la canne ne se connecte à aucun autre appareil ; laisser vide pour lister les appareils trouvés dans le moniteur série). Casque du projet : « Amis Hi580MAX ».
- Câblage : gauche Trig 13 / Echo 34, centre Trig 14 / Echo 35, droite Trig 27 / Echo 32 (Echo par pont diviseur), bouton SOS entre GPIO 25 et GND.

### Lunettes : `firmware/lunettes/`

- Bibliothèque **PubSubClient**.
- `config.h` : `WIFI_SSID` / `WIFI_PASSWORD` (partage de connexion du téléphone), puis le bloc MQTT généré par `install.sh` sur le VPS (`config-mqtt.h`). `BATTERY_ADC_PIN` reste à `-1` tant que le pont diviseur de la batterie n'est pas câblé.
- Câblage : Trig 13, Echo 34 par pont diviseur (1 kΩ / 2 kΩ), 3 piles AA sur VIN.
- Moniteur série attendu : `[iot] MQTT connecté au VPS (TLS)`.

`protocole.h` décrit les paquets ESP-NOW. **Il doit être identique dans les deux dossiers.**

**Règle radio.** Les lunettes se connectent au partage de connexion ; la canne cherche le même réseau et se place sur son canal (canal 1 s'il est absent). Il faut donc **activer le partage de connexion avant d'allumer les deux cartes**, et ne pas le couper pendant la démo. Si le partage redémarre sur un autre canal, redémarrer les deux cartes.

### Guidage vocal (canne)

| Situation | Seuil | Annonce |
|---|---|---|
| Obstacle à hauteur de tête (lunettes) | < 100 cm | « Attention, obstacle en hauteur », prioritaire |
| Obstacle devant | < 120 cm | « Obstacle devant, allez à gauche/droite » ou « Stop, obstacle », répété toutes les 4 s |
| Obstacle sur le côté | < 60 cm | « Obstacle à gauche/droite », dit une fois |
| Fin d'un obstacle devant | | « Continuez tout droit », une fois |

Un obstacle n'est considéré comme parti qu'au-dessus de son seuil + 20 cm : cette marge évite les annonces qui « clignotent » quand la distance hésite autour du seuil.

## Sujets MQTT

Préfixe : `ifri/seiot/projet3/canne01`. Toutes les cartes publient **via les lunettes**.

| Sujet | Émetteur | Contenu | QoS |
|---|---|---|---:|
| `.../telemetry` | lunettes | Mesures, chaque seconde | 0 |
| `.../event` | lunettes | Message vocal joué par la canne | 0 |
| `.../sos` | lunettes | SOS, répété toutes les 5 s jusqu'à l'accusé | 0 |
| `.../sos/ack` | relais | Accusé : `envoye` ou `deja_signale` | 1 |
| `.../availability` | lunettes | `online` / `offline` de la **passerelle** (retenu, Last Will) | 1 |

Télémétrie :

```json
{"v":1,"device":"canne01","left_cm":85,"center_cm":null,"right_cm":140,"head_cm":null,
 "battery_pct":null,"espnow":true,"headset":true,"rssi":-61,"sos_pending":false,"uptime_s":42}
```

- distances : `null` signifie « rien détecté dans la portée » ; gauche, centre et droite sont aussi à `null` quand la canne n'est pas joignable ;
- `espnow` : la canne est joignable par les lunettes ; `headset` : le casque Bluetooth de la canne est connecté ;
- `rssi`, `uptime_s` et `battery_pct` sont ceux **des lunettes**.

Annonce : `{"v":1,"type":"annonce","mp3":6,"message":"Attention, obstacle en hauteur"}` (`mp3` est le numéro du message, de 1 à 10).

SOS : `{"v":1,"event":"SOS","event_id":"a3f09c1e-7","device":"canne01","age_s":0,"attempt":1,"glasses":true}`. L'identifiant est formé de 8 caractères hexadécimaux, d'un tiret et du numéro de SOS choisi par la canne.

**Fiabilité du SOS.** Appui long (1,5 s) sur le bouton : la canne envoie un numéro de SOS aux lunettes, qui le publient toutes les 5 s (30 min au plus) jusqu'à l'accusé. PubSubClient ne publie qu'en QoS 0 : la fiabilité vient de cet accusé applicatif. Le relais n'envoie Telegram qu'une fois par `event_id` (liste gardée sur disque, même après un redémarrage) et ne publie l'accusé qu'une fois le message accepté par Telegram. Les lunettes transmettent alors la confirmation à la canne, qui dit « Alerte envoyée ». Sans confirmation au bout de 20 s, elle dit « Alerte non envoyée » et l'envoi continue en arrière-plan.

## Tableau de bord

`https://185.215.164.23:8443`, protégé par mot de passe (`DASHBOARD_USER` / `DASHBOARD_PASSWORD` dans `~/iot-canne/.env`). Il ne fait que refléter l'état : le guidage se fait dans la canne, sans Internet.

- **Chaîne de liaison** : casque Bluetooth, canne (`espnow`), passerelle (`availability`), VPS et Telegram, chacun avec son état ; en dessous, batterie et Wi-Fi des lunettes, temps de marche de la passerelle, SOS en cours.
- **Radar de la canne** : vue de dessus des 3 ultrasons avec les vrais seuils (côtés 60 cm, devant 120 cm) et la marge de 20 cm. Chaque état a une couleur, une forme et un mot (« Annoncé », « Marge », « Hors seuil », « Libre »).
- **Deux hauteurs couvertes** : vue de profil, tête (lunettes, seuil 100 cm) et sol devant (canne, seuil 120 cm).
- **Ce qu'entend la personne** : dernier message joué dans le casque. Le bouton « Voix » le lit aussi à voix haute sur l'ordinateur (à activer d'un clic avant la démo).
- **SOS** : alerte plein écran, suivie jusqu'à « Telegram envoyé ».
- **Mode projecteur** : bouton « Projecteur » (ou `?theme=clair`) pour un fond clair et un texte agrandi, plus lisibles au vidéoprojecteur. Le choix est mémorisé.
- **Mode simulation** : `?demo=1` rejoue en boucle un parcours d'une minute au format réel (obstacles, casque déconnecté, passerelle hors ligne, SOS). `&t=25` démarre à la 25e seconde. Un bandeau bleu signale la simulation : ne pas l'utiliser pendant la vraie démo.

Le contrat de `/api/state` est stable : `telemetry`, `telemetry_at`, `availability`, `events`, `stats`, `now`.

## Dépannage rapide

- **Passerelle hors ligne** au tableau de bord : vérifier le partage de connexion et le moniteur série des lunettes (`MQTT échec`, état TLS, mémoire libre).
- **Canne injoignable** alors que la passerelle est en ligne : canne éteinte, hors de portée, ou sur un autre canal radio (redémarrer les deux cartes après avoir activé le partage).
- **Casque déconnecté** : vérifier `BT_HEADSET_NAME` et que le casque n'est pas connecté à un autre appareil.
- **Pas de Telegram** : vérifier que le bot a reçu `/start`, le jeton et le chat ID dans `.env`, puis `logs/telegram.log`.
- **Sans Internet** : la détection et la voix continuent ; seuls le tableau de bord et l'alerte à distance sont indisponibles.

## Variante Docker (VPS administré)

Pour un VPS dont on est administrateur, `deploy/deploy.sh` installe la même chaîne avec Docker, un certificat Let's Encrypt (domaine requis) et Caddy devant le tableau de bord. Elle n'est pas utilisée sur le VPS actuel. Le bloc MQTT généré (`deploy/config-mqtt.h`) se recopie aussi dans `firmware/lunettes/config.h`.

## Création du bot Telegram

1. Dans Telegram, créer un bot avec `@BotFather` et copier son jeton dans `.env` sur le VPS.
2. Ouvrir une conversation avec ce bot et envoyer `/start`.
3. Renseigner l'identifiant de chat dans `TELEGRAM_CHAT_ID` (plusieurs possibles, séparés par des virgules).
4. Redémarrer : `bash ~/iot-canne/run.sh restart`.
5. Ne jamais mettre le jeton dans un firmware, un document, Git ou une capture d'écran.
