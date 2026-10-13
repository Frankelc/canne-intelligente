# Kit IoT — ESP32, MQTT, VPS et Telegram

Ce kit est le point de départ de la partie IoT du projet IFRI SEIoT. Il met en place le chemin **ESP32 de la canne → MQTT/TLS → VPS → Telegram**. L'ESP32 des lunettes échange ses mesures avec la canne en ESP-NOW ; le guidage audio reste local.

## Contenu

- `docker-compose.yml` : broker Mosquitto, relais Telegram, tableau de bord web et Caddy (HTTPS).
- `deploy/` : script d'installation sur le VPS, renouvellement du certificat, test SOS, génération des paramètres ESP32.
- `caddy/Caddyfile` : HTTPS et mot de passe devant le dashboard.
- `mosquitto/` : configuration TLS, contrôle d'accès MQTT.
- `telegram-bridge/` : service Python abonné au SOS et appelant Telegram.
- `dashboard/` : tableau de bord web en lecture seule (télémétrie, liaisons, événements/SOS).
- `firmware/canne/` : sketch Arduino de départ pour Wi-Fi, TLS/MQTT, ESP-NOW RX et bouton SOS.
- `.env.example` : paramètres serveur à copier dans `.env`.

Pour comprendre les raisons des choix, les alternatives et les lacunes restantes, lire [CHOIX_IOT.md](CHOIX_IOT.md).

## Prérequis

- Un VPS Ubuntu 22.04/24.04 (ou Debian 12) avec accès SSH et `sudo`.
- Un nom de domaine qui pointe vers l'IP du VPS (enregistrement A). Sans domaine : créer un sous-domaine gratuit sur [duckdns.org](https://www.duckdns.org) et y mettre l'IP du VPS.
- Un bot Telegram et le chat ID du destinataire (voir plus bas).

Les firmwares de `firmware/` sont les programmes de l'équipe, reliés au VPS.

## Installation sur le VPS (une commande)

Depuis le PC, copier le dossier puis se connecter :

```sh
scp -r iot-kit utilisateur@IP_DU_VPS:~/
ssh utilisateur@IP_DU_VPS
cd ~/iot-kit
sudo bash deploy/deploy.sh      # 1er lancement : crée .env et s'arrête
nano .env                       # renseigner DOMAIN, ACME_EMAIL, TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID
sudo bash deploy/deploy.sh      # installation complète
```

Le script `deploy/deploy.sh` :

1. installe Docker, certbot et ufw ;
2. vérifie que le domaine pointe bien vers le VPS ;
3. génère les mots de passe MQTT et dashboard laissés à `CHANGE_ME` dans `.env` ;
4. crée les comptes MQTT `canne01` (publication) et `dashboard` (lecture seule) ;
5. ouvre les ports SSH, 80 (renouvellement du certificat), 443 (dashboard) et 8883 (MQTT TLS) ;
6. obtient un certificat Let's Encrypt, renouvelé automatiquement par certbot (`deploy/renew-hook.sh` le recopie et redémarre Mosquitto et Caddy) ;
7. lance Mosquitto, le relais Telegram, le dashboard et Caddy, puis teste le port 8883 ;
8. écrit `deploy/config-mqtt.h` : domaine, identifiants et certificat racine à recopier dans `firmware/canne/config.h`.

Il est relançable : les mots de passe et le certificat déjà créés sont conservés. Si l'hébergeur a son propre pare-feu (panneau web), y ouvrir aussi 80, 443 et 8883.

Si le copier-coller depuis Windows a ajouté des fins de ligne CRLF (`$'
': command not found`), lancer `sed -i 's/
$//' deploy/*.sh` puis relancer.

**Tester sans ESP32 :** `sudo bash deploy/test-sos.sh` publie un SOS avec le compte de la canne. La notification doit arriver sur Telegram et l'événement doit s'afficher sur le dashboard. Attendre `SOS_COOLDOWN_SECONDS` (30 s) entre deux tests.

`deploy.sh` génère `mosquitto/acl` à partir de `.env`, avec trois comptes aux droits minimaux : `canne01` publie ses mesures et le SOS et lit l'accusé ; `telegram` lit le SOS et publie l'accusé ; `dashboard` lit tout et ne publie rien. Chaque conteneur ne reçoit que ses propres secrets.

## VPS partagé sans droits administrateur (`vps-user/`) — installation actuelle

Le VPS du projet (185.215.164.23) est partagé : les ports 80/443 appartiennent à un autre service, et le compte `groupe3` n'a ni `sudo` ni Docker. Le dossier `vps-user/` installe donc tout dans `~/iot-canne`, sans rien modifier d'autre sur la machine :

- Mosquitto est extrait des paquets Ubuntu (sans installation système), Python tourne dans `~/iot-canne/venv` ;
- MQTT/TLS sur le port **8883**, dashboard HTTPS + mot de passe sur le port **8443** ;
- certificats signés par une autorité propre au projet (`certs/ca.crt`), connue de l'ESP32 ; le navigateur affiche un avertissement la première fois ;
- redémarrage automatique : deux lignes `# iot-canne` dans la crontab de `groupe3`.

Commandes sur le VPS :

```sh
bash ~/iot-canne/run.sh status          # état des 3 services
bash ~/iot-canne/run.sh restart
bash ~/iot-canne/test-sos.sh            # SOS de test → Telegram
tail -f ~/iot-canne/logs/telegram.log   # journaux : mosquitto, telegram, dashboard
grep DASHBOARD_PASSWORD ~/iot-canne/.env
```

Mise à jour depuis le PC : copier les fichiers modifiés (`vps-user/*`, et `bridge.py`, `dashboard.py`, `index.html` dans `~/iot-canne/app/`), puis `bash ~/iot-canne/install.sh`.

## Création du bot Telegram

1. Dans Telegram, créer un bot avec `@BotFather` et copier son token dans `.env`.
2. Ouvrir une conversation avec ce bot et envoyer `/start`.
3. Renseigner l'identifiant de chat autorisé dans `TELEGRAM_CHAT_ID`.
4. Redémarrer le bridge : `docker compose up -d --build telegram-bridge`.
5. Ne jamais mettre le token dans le firmware, le README, Git ou une capture d'écran.

## Firmwares ESP32

Les programmes de l'équipe (`Code/canne_finale.ino`, `Code/lunettes.ino`) sont conservés tels quels comme référence. Les versions à flasher sont dans `firmware/`.

Arduino IDE, cœur **Arduino-ESP32 3.x**, carte **ESP32 Dev Module** pour les deux. Bibliothèques : **DFRobotDFPlayerMini** et **PubSubClient** (canne).

### Canne — `firmware/canne/`

`canne.ino` est le programme de l'équipe (capteurs, décision, voix, réception des lunettes), avec ces seuls changements :

- le SOS ne contacte plus Telegram directement : le jeton du bot n'est plus dans la canne, et le guidage ne se fige plus pendant l'envoi ;
- l'envoi est fait par `iot.cpp` (cœur 0) vers le VPS, répété jusqu'à confirmation ; « Alerte envoyée » (0008) est jouée à la confirmation, « Alerte non envoyée » (0010) immédiatement sans Wi-Fi ou après 20 s sans confirmation, et l'envoi continue en arrière-plan ;
- distances, état des lunettes et messages joués partent vers le tableau de bord ;
- la veille radio est coupée une fois connecté (`WiFi.setSleep(false)`), sinon des paquets des lunettes se perdent ;
- Wi-Fi et VPS sont réglés dans `config.h` au lieu du haut du programme.

1. Copier `config.example.h` en `config.h`. Renseigner `WIFI_SSID` / `WIFI_PASSWORD` (partage de connexion du téléphone).
2. Remplacer le bloc MQTT par le contenu de `deploy/config-mqtt.h` généré sur le VPS (domaine, identifiants, certificat racine).
3. Flasher et ouvrir le moniteur série (115200 bauds) : `Wi-Fi connecte, canal N`, puis `[iot] MQTT connecté au VPS (TLS)`.

### Lunettes — `firmware/lunettes/`

Programme de l'équipe, inchangé : HC-SR04 à hauteur de tête, envoi ESP-NOW en diffusion toutes les 100 ms, paquet filtré par le code `CANE`. Renseigner `WIFI_SSID` avec **le même nom** que dans la canne.

**Règle radio commune** : canne et lunettes cherchent le réseau `WIFI_SSID` au démarrage et se placent sur son canal (canal 1 s'il est absent). En pratique : **activer le partage de connexion avant d'allumer la canne et les lunettes**, et ne pas le couper pendant la démo. Si le partage redémarre sur un autre canal, redémarrer les deux cartes.

## Sujets MQTT

Préfixe par défaut : `ifri/seiot/projet3/canne01`.

| Sujet | Émetteur | Usage | QoS |
|---|---|---|---:|
| `.../telemetry` | canne | Mesures toutes les secondes | 0 |
| `.../event` | canne | Message vocal joué | 0 |
| `.../sos` | canne | SOS, répété toutes les 5 s jusqu'à l'accusé | 0 |
| `.../sos/ack` | relais | Accusé : `envoye` ou `deja_signale` | 1 |
| `.../availability` | canne | `online` / `offline` (retenu, Last Will) | 1 |

Télémétrie : `{"v":1,"device":"canne01","left_cm":85,"center_cm":null,"right_cm":140,"head_cm":null,"battery_pct":null,"espnow":true,"rssi":-61,"sos_pending":false,"uptime_s":42}`. Pour une distance, `null` signifie « rien détecté dans la portée » (affiché « Libre »). `battery_pct` reste `null` tant que le pont diviseur n'est pas câblé.

SOS : `{"v":1,"event":"SOS","event_id":"a1b2c3d4","device":"canne01","age_s":0,"attempt":1,"glasses":true}`.

**Fiabilité du SOS.** PubSubClient ne publie qu'en QoS 0. La fiabilité vient donc d'un accusé applicatif : la canne réémet le même `event_id` toutes les 5 s, y compris après une coupure réseau, pendant au plus 30 min. Le relais envoie Telegram une seule fois par `event_id` et garde la liste des SOS traités sur disque (volume `telegram_data`), même après un redémarrage. Il ne publie l'accusé qu'après l'acceptation du message par Telegram ; c'est alors seulement que la canne joue « Alerte envoyée ». Si l'alerte part en retard, le message Telegram l'indique.

## Tableau de bord

`https://DOMAIN`, protégé par `DASHBOARD_USER` / `DASHBOARD_PASSWORD` (affichés à la fin de `deploy.sh`). Page : `dashboard/index.html`.

- **Radar de la canne** : vue de dessus des 3 ultrasons (−30°, 0°, +30°), colorés par zone (danger < 50 cm, attention 50–120 cm, libre).
- **Deux zones couvertes** : vue de profil, zone basse (canne) et zone haute (lunettes).
- **Ce qu'entend la personne** : dernier message vocal en grand. Le bouton « Faire parler le projecteur » le lit aussi à voix haute (synthèse vocale du navigateur, à activer d'un clic avant la démo).
- **SOS** : alerte plein écran, suivie jusqu'à « Telegram envoyé ».
- **Mode simulation** : `https://DOMAIN/?demo=1` joue un scénario fictif pour répéter au projecteur sans la canne. Un bandeau jaune et noir indique « SIMULATION » : ne pas l'utiliser pendant la vraie démo.

Le port 8080 n'est pas ouvert sur Internet ; en dépannage : `ssh -L 8080:127.0.0.1:8080 utilisateur@IP_DU_VPS`, puis `http://127.0.0.1:8080`.

## Dépannage rapide

- **`deploy.sh` échoue au certificat** : le domaine ne pointe pas encore vers le VPS (attendre la propagation DNS) ou le port 80 est fermé chez l'hébergeur.
- **VPS joignable mais MQTT refuse la connexion** : vérifier DNS, port 8883, pare-feu, certificat, date/heure, login et ACL.
- **TLS échoue dans PubSubClient** : charger la CA correcte dans `setCACert`; ne pas contourner la validation en production.
- **MQTT fonctionne mais ESP-NOW est perdu** : vérifier que les deux cartes sont sur le même canal après connexion Wi-Fi, les adresses MAC et le format/taille du paquet.
- **Pas de Telegram** : vérifier que le bot a reçu `/start`, le token/chat_id, les logs bridge et la connectivité HTTPS sortante du VPS.
- **Wi-Fi absent** : la voix et la détection locales doivent continuer ; seul le tableau de bord et l'alerte distante sont indisponibles.

## Ordre de validation conseillé

1. Depuis un PC, publier manuellement un SOS au broker TLS et vérifier Telegram.
2. Connecter l'ESP32 de la canne au Wi-Fi puis au broker, publier un message de test.
3. Faire fonctionner ESP-NOW lunettes→canne pendant que MQTT est connecté (le dashboard doit afficher les lunettes comme connectées).
4. Brancher le bouton SOS et valider une notification unique.
5. Brancher les vraies mesures et le dashboard ; couper Internet et vérifier que le guidage local reste actif.

Voir le README parent pour les objectifs, le planning sur cinq jours, le matériel, les précautions électriques et les critères de démonstration.
