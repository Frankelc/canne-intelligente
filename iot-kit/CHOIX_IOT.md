# Partie IoT : choix d'architecture, raisons et limites

**Projet :** IFRI · Certification SEIoT · Projet 3 : canne et lunettes intelligentes
**État :** architecture finale déployée sur le VPS et validée sur le matériel, SOS Telegram compris.

## 1. Résumé des décisions

| Sujet | Choix retenu | Pourquoi | Autres possibilités |
|---|---|---|---|
| Voix | Casque Bluetooth (A2DP) piloté par la canne, clips vocaux en mémoire flash | Aucun fil vers l'oreille ; un obstacle sur le côté est annoncé dans l'oreille correspondante ; plus de carte SD ni de module audio à câbler | Le DFPlayer (module MP3 + carte micro-SD) de la première version n'est plus utilisé |
| Liaison canne ↔ lunettes | ESP-NOW | Lien direct entre les deux cartes, sans routeur, en quelques millisecondes | Wi-Fi TCP/UDP demande un point d'accès ; un câble gêne la mobilité ; BLE ajoute une pile de plus |
| Accès Internet | **Wi-Fi des lunettes** (passerelle) | La canne n'a pas assez de mémoire pour le Bluetooth audio et TLS en même temps ; les lunettes ont toute leur mémoire libre | Modem GSM/LTE : matériel, carte SIM et consommation en plus ; le partage de connexion du téléphone suffit au prototype |
| Protocole vers le VPS | MQTT/TLS | Léger ; un message publié une fois est reçu par le tableau de bord et le relais Telegram ; présence (Last Will) intégrée | HTTP convient à une requête isolée, mais il faudrait reconstruire la diffusion en direct et les reconnexions |
| Courtier MQTT | Mosquitto sur le VPS | Open source, léger ; le groupe garde la maîtrise des comptes et des données | Un courtier hébergé dépend d'un fournisseur et de ses quotas |
| Tableau de bord | Page Flask abonnée à MQTT | Suffisant pour quelques valeurs en direct ; tourne sans `sudo`, sans base de données, sans étape de compilation | Node-RED plus visuel ; Grafana utile pour des historiques, mais demande une base de données |
| Alerte | Le VPS reçoit le SOS et appelle le bot Telegram | Le jeton du bot reste sur le serveur, jamais dans un firmware | Un appel direct par l'ESP32 exposerait le jeton et demanderait du TLS supplémentaire |
| Comptes MQTT | `canne01` (passerelle), `telegram`, `dashboard` | Chacun a les droits minimaux ; le tableau de bord ne peut rien publier | Un compte unique donne trop de droits |
| Sécurité | TLS de bout en bout sur Internet (8883 et 8443), mot de passe sur le tableau de bord | Identifiants et messages protégés sur le réseau public | MQTT en clair acceptable au banc de test seulement |
| Guidage | Décision et voix dans la canne, indépendantes du réseau | Le guidage ne doit ni attendre ni dépendre d'Internet | Décider sur le serveur ajouterait du délai et une panne de guidage hors ligne |

## 2. Architecture retenue

```text
                       ESP-NOW (mesures, annonces, SOS)
Casque ◄── Bluetooth ── CANNE ◄──────────────────────────► LUNETTES ── Wi-Fi + MQTT/TLS 8883 ──► VPS 185.215.164.23
            (voix)      3 ultrasons                         ultrason tête                        ├── Mosquitto
                        décision                            passerelle                           ├── relais Telegram ──► proche
                        bouton SOS        ◄── état du VPS, confirmation SOS ──                   └── tableau de bord HTTPS 8443
```

**Pourquoi la passerelle est dans les lunettes.** Au départ, la canne faisait tout. Avec le Bluetooth audio actif, il ne restait que 13 184 octets de mémoire libre, alors qu'une connexion TLS en demande bien plus : la canne échouait à joindre le VPS. En déplaçant le Wi-Fi et TLS dans les lunettes, la canne remonte à 72 924 octets libres et chaque carte fait ce qu'elle peut faire de manière fiable.

**Ce que fait chaque carte.**

- **Canne** : mesure gauche/centre/droite, reçoit la distance tête, décide et parle dans le casque. Elle envoie aux lunettes ses mesures (toutes les 300 ms), chaque annonce jouée et la demande de SOS.
- **Lunettes** : mesurent la hauteur de tête (toutes les 100 ms), l'envoient à la canne avec l'état du VPS et le numéro du dernier SOS confirmé, et publient tout sur le VPS.

**Conséquence pour la supervision.** Le message `availability` (`online`/`offline`, Last Will) décrit désormais la **passerelle**. L'état de la canne vient du champ `espnow` de la télémétrie, celui du casque du champ `headset`. Quand la passerelle est hors ligne, le tableau de bord indique que l'état de la canne n'est plus connu, sans prétendre qu'elle est en panne : elle continue de guider.

### Point radio

Le Wi-Fi des lunettes et ESP-NOW partagent la même radio, donc le même canal. Au démarrage, les lunettes se connectent au partage de connexion du téléphone ; la canne cherche ce même réseau (`WIFI_SSID`, sans s'y connecter) et se place sur son canal. Le partage doit donc être actif avant d'allumer les cartes et garder son canal pendant la démo.

## 3. Pourquoi MQTT

Plusieurs services consomment les mêmes données : le tableau de bord, le journal et le relais Telegram. Avec MQTT, la passerelle publie une fois par sujet et chaque service s'abonne. Les sujets séparent les usages (`telemetry`, `event`, `sos`, `sos/ack`, `availability`).

MQTT ne rend pas le SOS fiable à lui seul, et PubSubClient ne publie qu'en QoS 0. Le SOS repose donc sur un accusé applicatif : les lunettes republient le même `event_id` (8 caractères hexadécimaux, un tiret, le numéro de SOS) toutes les 5 s jusqu'à recevoir `sos/ack`. Le relais élimine les doublons (liste conservée sur disque) et n'envoie l'accusé qu'une fois le message accepté par Telegram. La canne ne dit « Alerte envoyée » qu'à ce moment-là : elle ne promet jamais une alerte qui n'est pas partie.

## 4. Pourquoi un tableau de bord Flask

La page reçoit les sujets MQTT et affiche l'état en direct. Le VPS n'offre ni `sudo` ni Node : une page en HTML, CSS et JavaScript simples, servie par Flask en HTTPS, ne demande aucune installation système ni étape de compilation.

Elle montre la chaîne de liaison complète (casque, canne, passerelle, VPS, Telegram), le radar avec les vrais seuils de la canne, la vue de profil tête/sol, le dernier message vocal, le journal et les SOS. Elle propose un mode projecteur (fond clair, texte agrandi) et un mode simulation (`?demo=1`) pour répéter sans matériel.

Limites assumées : l'historique est gardé en mémoire et disparaît au redémarrage ; ce n'est pas un enregistrement certifié des mesures.

## 5. Pourquoi Telegram passe par le VPS

Le jeton du bot est une clé secrète. Il reste sur le VPS : il n'apparaît dans aucun firmware, document ni capture. Le relais valide le message, limite les alertes rapprochées (30 s) et n'affiche jamais le jeton dans ses journaux.

## 6. Ce qui est en place

- VPS partagé sans droits administrateur : Mosquitto, relais Telegram et tableau de bord dans `~/iot-canne`, redémarrage automatique par la crontab.
- MQTT uniquement en TLS depuis Internet (8883) ; le firmware des lunettes vérifie le certificat du VPS.
- Trois comptes MQTT aux droits minimaux.
- SOS fiable de bout en bout : réémission par la passerelle, accusé du relais, élimination des doublons, mention du retard dans Telegram, confirmation vocale sur la canne.
- Canne : voix Bluetooth stéréo, annonces avec seuils et marge anti-clignotement, aucun besoin d'Internet.
- Lunettes : capteur tête et passerelle, tâche réseau sur un cœur séparé, reconnexion automatique.
- Tableau de bord HTTPS avec mot de passe, mode projecteur et mode simulation.

## 7. Limites du prototype

- Un SOS en attente est perdu si la **canne** redémarre avant l'accusé (son numéro est en mémoire vive). Un redémarrage des lunettes, lui, ne le perd pas : la canne continue de le demander tant qu'elle n'a pas de confirmation.
- La liaison ESP-NOW n'est pas chiffrée ; elle est seulement filtrée par le code `CANE`.
- Le certificat du VPS est signé par une autorité propre au projet : le navigateur affiche un avertissement la première fois.
- La batterie des lunettes n'est mesurée que si le pont diviseur est câblé (`BATTERY_ADC_PIN`).
- Le système dépend du partage de connexion d'un téléphone pour la supervision et le SOS ; le guidage, lui, n'en dépend pas.

## 8. Plan de validation (rejouable avant la démo)

1. **VPS** : `bash ~/iot-canne/run.sh status`, puis `bash ~/iot-canne/test-sos.sh` : une notification Telegram doit arriver.
2. **Passerelle** : allumer le partage, puis les lunettes ; le tableau de bord passe « Passerelle : en ligne ».
3. **Canne** : allumer la canne ; « Canne : joignable », puis « Casque : connecté » et l'annonce « Système prêt ».
4. **Guidage** : présenter un obstacle devant, sur les côtés, puis à hauteur de tête ; vérifier les annonces dans le casque et sur le tableau de bord.
5. **SOS** : appui long sur le bouton ; alerte plein écran, notification unique sur Telegram, « Alerte envoyée » dans le casque.
6. **Mode dégradé** : couper le partage ; la canne continue de guider, le tableau de bord affiche « Passerelle hors ligne ».

---

Aucun secret ni aucune donnée personnelle ne figure dans ce document. Les paramètres se renseignent directement sur le VPS et dans les `config.h` locaux, jamais publiés.
