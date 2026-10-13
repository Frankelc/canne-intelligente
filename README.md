# IFRI · Certification SEIoT · Projet 3

## Canne et lunettes intelligentes pour personnes non voyantes

Prototype d'aide expérimentale à la mobilité : détection d'obstacles à basse et à haute hauteur, messages vocaux locaux, transmission des événements vers un serveur MQTT sur VPS et alerte Telegram.

> **Sécurité :** ce prototype pédagogique ne remplace pas une canne blanche, un accompagnateur ni un dispositif médical certifié. Ne faites pas circuler une personne les yeux bandés sans accompagnateur et parcours sécurisé. La détection et l'audio locaux doivent rester fonctionnels sans Internet.

## 1. Objectifs

- Détecter les obstacles au sol et devant la personne avec trois capteurs ultrason sur la canne.
- Détecter certains obstacles à hauteur de tête avec un capteur ToF VL53L1X sur les lunettes.
- Fournir une indication vocale locale, sans attendre le réseau.
- Envoyer mesures, événements, état de connexion et demande SOS au VPS via MQTT.
- Afficher les événements sur un tableau de bord (par exemple Node-RED) et transmettre le SOS sur Telegram depuis le VPS.
- Concevoir deux cartes électroniques dans EasyEDA ou KiCad ; le prototype peut rester sur breadboard/plaque à pastilles.

## 2. Architecture

```text
VL53L1X sur lunettes
        │ I²C
ESP32-C3 des lunettes
        │ ESP-NOW : distance hauteur + état capteur
        ▼
ESP32 de la canne ◄── 3 ultrasons (gauche, centre, droite)
   │      │
   │      ├── DFPlayer Mini + écouteurs : guidage vocal local
   │      └── Bouton SOS
   │ Wi-Fi avec accès Internet (routeur ou partage de connexion)
   ▼
VPS : broker MQTT (Mosquitto) → Node-RED / tableau de bord
                            └→ service d'alerte → Bot API Telegram → proche
```

Le Wi-Fi de la canne sert à joindre le VPS. Les lunettes n'ont pas besoin d'Internet : elles envoient leurs mesures à la canne par ESP-NOW. L'ESP32 de la canne est le nœud passerelle. Il effectue la décision et les messages audio localement ; le VPS ne participe pas à la boucle critique de guidage.

### Attention au canal radio

ESP-NOW et la connexion Wi-Fi de la canne utilisent la radio 2,4 GHz. Quand la canne rejoint un point d'accès, elle fonctionne sur le canal de ce point d'accès. Les lunettes doivent alors émettre sur le même canal. Le canal du peer ESP-NOW peut être défini à `0` pour utiliser le canal courant, mais il faut valider le comportement avec les versions de cartes et bibliothèques retenues. Tester tôt : connexion Wi-Fi de la canne, puis réception ESP-NOW continue. Pour la démonstration, réserver un point d'accès stable (routeur ou téléphone) et éviter le changement automatique de canal.

## 3. Matériel

| Élément | Qté | Fonction |
|---|---:|---|
| ESP32 DevKit | 1 | Contrôleur principal de la canne, Wi-Fi/MQTT et décision |
| ESP32-C3 Super Mini (ou autre ESP32 compatible) | 1 | Contrôleur des lunettes et ESP-NOW |
| RCWL-1601 (ou HC-SR04) | 3 | Détection gauche, centre et droite |
| VL53L1X | 1 | Mesure ToF à hauteur de tête, bus I²C |
| DFPlayer Mini + micro-SD | 1 | Lecture des messages vocaux préenregistrés |
| Écouteurs filaires | 1 | Sortie audio locale |
| Bouton poussoir | 1 | Demande SOS par appui long |
| Batterie et alimentation adaptées | 2 jeux | Alimentation de la canne et des lunettes |
| Vibreur (optionnel) | 1 | Retour tactile local |
| Fils, connecteurs, plaque et boîtiers | selon besoin | Montage et intégration |

Les broches GPIO ne sont pas imposées ici : elles dépendent des révisions exactes des cartes et du câblage final. Les choisir dans les schémas des cartes, éviter les broches réservées au démarrage/flash, puis documenter la correspondance dans le code et le schéma électrique.

### Précautions électriques

- Les GPIO des ESP32 sont en logique 3,3 V et ne doivent pas recevoir 5 V.
- Avec un HC-SR04 alimenté en 5 V, la sortie ECHO peut être à 5 V : utiliser un diviseur résistif ou un convertisseur de niveau avant le GPIO. Vérifier la fiche technique du modèle réel.
- Ne pas alimenter moteurs, DFPlayer ou modules consommateurs directement depuis un GPIO. Prévoir alimentation adaptée, masse commune et étage de commande si nécessaire.
- Vérifier la plage d'alimentation exacte du breakout VL53L1X utilisé ; certains modules intègrent un régulateur et adaptation de niveau, d'autres non.
- Les trois ultrasons sont déclenchés l'un après l'autre pour réduire les interférences acoustiques.
- Protéger les batteries contre court-circuit et décharge excessive ; utiliser un chargeur/protecteur adapté à la chimie et au montage.

## 4. Fonctionnement local et logique

La canne échantillonne les trois distances au sol séquentiellement, reçoit la distance de hauteur par ESP-NOW, rejette les valeurs invalides, puis applique une logique à seuils. Valeurs initiales à calibrer dans la salle :

| Classe | Valeur initiale | Réaction indicative |
|---|---:|---|
| Danger | < 50 cm | Stop ou annonce prioritaire |
| Attention | 50 à 120 cm | Annonce d'obstacle / direction |
| Libre | > 120 cm | Pas d'annonce répétée |

Ces seuils sont des paramètres de démonstration, pas des valeurs de sécurité garanties. Un capteur seul ne décrit pas toute la géométrie d'un obstacle. Filtrer les mesures (par exemple médiane de trois échantillons), traiter les délais et valeurs hors plage comme des erreurs, et limiter la répétition des mêmes annonces. La détection en hauteur peut être prioritaire dans l'audio. Garder les annonces brèves afin de préserver l'audition des sons ambiants.

### Messages audio suggérés

| Fichier | Message |
|---|---|
| `0001.mp3` | Obstacle à gauche |
| `0002.mp3` | Obstacle à droite |
| `0003.mp3` | Obstacle devant, allez à gauche |
| `0004.mp3` | Obstacle devant, allez à droite |
| `0005.mp3` | Stop, obstacle |
| `0006.mp3` | Attention, obstacle en hauteur |
| `0007.mp3` | Continuez tout droit |
| `0008.mp3` | Alerte envoyée |
| `0009.mp3` | Système prêt |

Nommer les fichiers selon les exigences du DFPlayer et tester d'abord la carte SD et la sortie audio séparément.

## 5. MQTT et données

### Topologie

- Le VPS héberge Mosquitto, accessible depuis Internet sur le port MQTT choisi.
- L'ESP32 de la canne se connecte au réseau Wi-Fi avec accès Internet, puis au broker MQTT du VPS.
- Node-RED (sur le VPS ou une machine autorisée) s'abonne aux sujets pour afficher valeurs et événements.
- Un service d'alerte (flow Node-RED ou petit programme) s'abonne au sujet SOS et appelle Telegram.
- Le projet reste utile hors ligne : les mesures, décisions, vibrations et voix sont locales. Les données distantes sont opportunistes.

### Sujets proposés

Préfixe du projet : `ifri/seiot/projet3/canne01/`.

| Sujet | Exemple de payload | QoS / rétention conseillée |
|---|---|---|
| `.../telemetry` | JSON avec distances, batterie, horodatage/uptime | QoS 0, non retenu (mesures fréquentes) |
| `.../event` | `{"type":"obstacle","zone":"gauche","distance_cm":42}` | QoS 1, non retenu |
| `.../status` | `{"wifi":true,"espnow":true,"uptime_s":120}` | QoS 1, retained |
| `.../sos` | `{"event":"SOS","device":"canne01","uptime_s":1234}` | QoS 1, non retenu |
| `.../availability` | `online` ou `offline` (Last Will) | QoS 1, retained |

Exemple de télémétrie :

```json
{
  "device": "canne01",
  "left_cm": 145,
  "center_cm": 48,
  "right_cm": 132,
  "head_cm": 76,
  "battery_pct": 82,
  "uptime_s": 3600
}
```

Publier la télémétrie à faible fréquence (par exemple une fois par seconde) et publier immédiatement les événements utiles. Pour le SOS, utiliser QoS 1, vérifier la connexion et afficher une confirmation locale uniquement quand l'événement a été publié/confirmé selon le mécanisme choisi. QoS 1 signifie « au moins une fois » : prévoir un `event_id` et une déduplication côté serveur pour éviter plusieurs notifications Telegram lors d'une reconnexion.

Ne pas publier chaque mesure de capteur toutes les 100 ms : cela surcharge le réseau et le tableau de bord sans apporter d'avantage à la démonstration.

### Sécurité du broker

- Créer un compte MQTT dédié à l'appareil, avec un mot de passe fort et des droits limités aux sujets de ce projet.
- Ne pas laisser Mosquitto accessible en anonyme sur Internet.
- Préférer MQTT sur TLS (port usuel 8883) avec certificat valide. Pour un prototype sur réseau de confiance, si TLS est temporairement omis, documenter cette limite et restreindre l'accès réseau par pare-feu ; ne jamais transmettre de secret Telegram à l'ESP32.
- Stocker les identifiants Wi-Fi/MQTT hors du dépôt public (fichier local non versionné ou secrets de compilation). Ne pas afficher les mots de passe ou jetons dans les slides/captures.
- Utiliser un pare-feu VPS : n'exposer que les ports nécessaires. L'interface Node-RED doit être protégée et ne pas être publiée sans authentification.

## 6. Alerte Telegram via le VPS

Le VPS, et non l'ESP32, détient le jeton du bot. Le service serveur s'abonne à `.../sos`, valide le payload et l'identifiant de l'appareil, applique une temporisation anti-doublon, puis appelle la méthode `sendMessage` de la Bot API Telegram avec le `chat_id` du proche. Il faut créer le bot avec BotFather et le destinataire doit avoir démarré le bot pour recevoir ses messages.

```text
Appui long SOS → ESP32 canne publie MQTT → VPS reçoit SOS
              → service vérifie/déduplique → Telegram avertit le proche
```

Message possible : `SOS — demande d'assistance depuis la canne canne01. Heure/état : ...`.

Configurer le jeton et le `chat_id` dans des variables d'environnement ou un fichier de secrets protégé côté VPS. Ne pas les écrire dans ce README ni dans le firmware. L'accusé local « alerte envoyée » ne doit pas laisser croire qu'un proche l'a reçue : distinguer « SOS transmis au VPS » et « confirmation Telegram » si un retour serveur est implémenté.

## 7. Déploiement du VPS (vue d'ensemble)

1. Installer un système Linux maintenu et mettre à jour les paquets.
2. Installer Mosquitto et ses outils clients, créer un utilisateur dédié, activer l'authentification et les ACL.
3. Configurer le pare-feu pour n'autoriser que les ports nécessaires ; activer TLS si possible.
4. Tester la publication et l'abonnement MQTT depuis un poste de développement avant de connecter la canne.
5. Installer Node-RED (ou un service serveur léger) et protéger son accès.
6. Créer le flow/service : abonnement MQTT, affichage de télémétrie, traitement SOS, appel Telegram et journalisation minimale.
7. Tester un SOS avec un événement simulé avant le bouton physique.
8. Redémarrer le VPS et vérifier que Mosquitto et le service d'alerte repartent automatiquement.

Les détails de commandes dépendent de la distribution et de la configuration du VPS ; ne pas copier une configuration sans vérifier les chemins, versions et règles de pare-feu de la machine utilisée.

## 8. Programme des deux ESP32

### ESP32-C3 des lunettes

1. Initialiser le VL53L1X sur I²C et vérifier la présence du capteur.
2. Lire la distance, marquer les erreurs/timeout et éviter d'envoyer une valeur périmée comme une mesure valide.
3. Initialiser ESP-NOW, cibler l'adresse MAC de la canne et envoyer un paquet compact, par exemple `{sequence, distance_mm, valid, battery_mv}`.
4. Envoyer périodiquement (ex. 5 à 10 fois/s) ; le rythme exact doit être mesuré et ne pas saturer le traitement.
5. Signaler localement une perte de liaison si elle est détectable.

### ESP32 de la canne

1. Initialiser les capteurs, le DFPlayer, le bouton SOS et les fonctions locales.
2. Connecter le Wi-Fi, obtenir l'état et le canal, puis démarrer ESP-NOW avec le canal compatible et enregistrer le peer des lunettes.
3. Recevoir les paquets ESP-NOW sans bloquer la boucle audio/capteurs ; conserver la date de dernière réception.
4. Déclencher les ultrasons en séquence, filtrer, décider, et commander audio/vibreur.
5. Gérer MQTT de manière non bloquante : reconnexion temporisée, Last Will `offline`, publication périodique et événements.
6. Détecter l'appui long SOS, publier un événement unique avec identifiant et permettre une nouvelle tentative si le réseau est indisponible.
7. Continuer sans Wi-Fi : pas de blocage durable de la logique de sécurité locale.

Ne pas faire de requête réseau bloquante dans les callbacks ESP-NOW ni dans la boucle de détection. Utiliser des temporisations avec `millis()` ou des tâches séparées. Garder les paquets réseau petits et versionnés (`"v":1`).

## 9. Plan de réalisation sur cinq jours

| Jour | Résultat attendu |
|---|---|
| J1 | Tester séparément ESP32, un ultrason, VL53L1X, DFPlayer, connexion MQTT depuis un ordinateur et broker VPS. Créer le bot Telegram et valider un message depuis un script serveur. |
| J2 | Réaliser la détection canne locale : trois ultrasons séquentiels, filtrage, décisions, messages vocaux. Établir les sujets/payloads MQTT. |
| J3 | Faire communiquer lunettes→canne par ESP-NOW, y compris pendant la connexion Wi-Fi/MQTT de la canne. Publier télémétrie et événements ; traiter SOS vers Telegram. |
| J4 | Intégrer mécaniquement et électriquement, calibrer, mesurer portée ESP-NOW, délai, précision et autonomie. Terminer les schémas/PCB et tester la perte réseau. |
| J5 | Répéter le parcours accompagné, vérifier toutes les batteries, enregistrer une vidéo de secours, préparer le tableau de bord et l'exposé. |

**Priorité si le temps manque :** sécurité et guidage local → ESP-NOW → MQTT télémétrie → alerte SOS Telegram → tableau de bord avancé → routage PCB complet.

## 10. Tests et critères de validation

- VL53L1X : comparer distances mesurées/réelles à plusieurs points et noter les cas d'échec.
- Ultrasons : tester séparément plusieurs distances, matériaux et angles ; vérifier qu'ils ne sont pas déclenchés simultanément.
- ESP-NOW : vérifier réception répétée de la distance de hauteur pendant que la canne est connectée au Wi-Fi et au broker.
- Audio : contrôler la bonne annonce, l'anti-répétition, la priorité hauteur et la disponibilité du son hors ligne.
- MQTT : tester télémétrie, événement, reconnexion, Last Will et droits d'accès.
- Telegram : envoyer un message simulé, puis tester le bouton SOS et la déduplication.
- Panne réseau : couper le Wi-Fi/Internet ; le système local doit continuer et la reconnexion ne doit pas bloquer le guidage.
- Énergie : mesurer la tension et l'autonomie réelle ; afficher une estimation de batterie seulement si elle est calibrée.
- Démo : parcours dégagé, accompagnateur à portée immédiate, vitesse lente, arrêt possible et vidéo de secours.

Consigner les résultats réellement observés. Ne pas présenter une valeur cible ou une simulation comme une mesure effectuée.

## 11. Répartition des rôles

- **R1 — Électronique/PCB :** câblage, alimentation, schémas, ERC/DRC, cartes canne et lunettes.
- **R2 — Firmware/IoT :** deux firmwares, ESP-NOW, logique, MQTT, VPS et alerte Telegram.
- **R3 — Mécanique/tests :** support des capteurs, montage, sécurité du parcours, mesures et calibration.
- **R4 — Documentation/exposé :** état de l'art, architecture, résultats, slides et vidéo de secours.

## 12. Risques et plans de repli

| Risque | Réponse prévue |
|---|---|
| Pas d'Internet ou VPS injoignable | Guidage/audio local maintenu ; tableau de bord et Telegram indisponibles, événements éventuellement mis en tampon. |
| Canal Wi-Fi changeant | Fixer le canal du point d'accès si possible ; resynchroniser/tester ESP-NOW après connexion. Démo locale sans lunettes en dernier recours. |
| Telegram indisponible | MQTT et tableau de bord continuent ; journaliser l'échec et réessayer avec temporisation. |
| Faux positifs ou mesure invalide | Filtrer, rejeter les timeouts, calibrer ; présenter les limites et ne pas dépendre du capteur seul. |
| Capteur ou module manquant | Adapter le prototype, documenter le remplacement et distinguer clairement matériel réel et conception. |
| Retard PCB | Présenter schéma, ERC/DRC disponibles et vue 3D ; prototype sur plaque à pastilles selon le temps. |
| Démo en direct échoue | Vidéo de secours, démonstration statique des messages MQTT et explication du mode dégradé. |

## 13. Livrables

- Firmware des lunettes et de la canne, avec configuration secrète exclue du dépôt.
- Schéma électrique complet des deux cartes, ERC, routage, DRC, vues 3D et Gerber/BOM si terminés.
- Configuration du broker (sans secrets), sujets MQTT et exemples de payloads.
- Flow Node-RED ou service d'alerte Telegram, sans jeton ni identifiant privé dans les fichiers livrés.
- Tableau de mesures, photos du montage, captures du tableau de bord et vidéo de démonstration.
- Présentation : besoin, architecture, fonctionnement, résultats mesurés, limites, risques et améliorations.

## 14. Améliorations futures

Écouteurs à conduction osseuse, meilleure gestion de batterie, GPS extérieur, capteurs redondants, diagnostic de liaison, mise à jour logicielle sécurisée, journal d'événements fiable, et étude utilisateur encadrée avant tout usage réel.

---

**Projet de formation IFRI · Certification SEIoT · Document de travail du groupe**
