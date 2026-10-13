# Partie IoT — choix d'architecture, raisons et travaux restants

**Projet :** IFRI · Certification SEIoT · Projet 3 — Canne et lunettes intelligentes  
**État :** dossier de conception et kit de démarrage. Le kit n'est pas encore déployé sur le VPS ni intégré à vos firmwares définitifs.

## 1. Résumé des décisions

| Sujet | Choix retenu pour le prototype | Pourquoi | Autres possibilités |
|---|---|---|---|
| Communication lunettes → canne | ESP-NOW | Lien direct entre les deux cartes, sans routeur ni Internet. Convient aux petites mesures envoyées régulièrement. | Wi-Fi TCP/UDP demande un point d'accès; câble est simple mais gêne la mobilité; BLE ajoute une pile et une mise au point supplémentaires. |
| Accès Internet | Wi-Fi sur l'ESP32 de la canne | La canne est le point central; un seul appareil doit joindre le VPS. | Modem GSM/LTE ajoute matériel, coût, carte SIM et consommation; partage de connexion du téléphone suffit pour la démonstration. |
| Protocole canne → VPS | MQTT | Léger, publication par sujets, abonnements simultanés du dashboard et du relais Telegram, gestion de présence et qualité de service. | HTTP/REST est simple pour envoyer une requête isolée, mais il faut construire autour la diffusion temps réel, les abonnements et les reconnexions. |
| Courtier MQTT | Mosquitto sur VPS | Open source, léger et adapté à un seul prototype; le groupe contrôle les données et les comptes. | Broker hébergé simplifie l'exploitation mais dépend d'un fournisseur et peut imposer un compte/quotas; ThingsBoard/Cloud demande moins d'administration mais réduit la maîtrise. |
| Dashboard | Petite page web Flask abonnée à MQTT | Suffisant pour quelques valeurs et événements, déployé avec les mêmes services Docker; pas de flow à maintenir. | Node-RED est plus visuel et rapide pour construire des flows; c'est un bon choix si le formateur l'exige ou si le dashboard doit être étendu. Grafana est pertinent pour des historiques, mais demande une base de données. |
| Alerte | Le VPS reçoit le sujet SOS et appelle le bot Telegram | Le token reste côté serveur, le firmware reste simple, et la même arrivée MQTT peut être journalisée ou montrée à l'écran. | Appel direct Telegram par l'ESP32 évite le bridge, mais expose le token dans le firmware et lie le code embarqué au service externe. |
| Deux comptes MQTT | `canne01` publie; `dashboard` lit | Réduit les droits de chaque client et évite que le tableau de bord publie des commandes à la canne. | Un compte unique est plus simple au début, mais donne trop de droits et complique la rotation des secrets. |
| Sécurité du transport | TLS côté appareil vers le VPS; trafic en clair réservé au réseau Docker privé | Protège les identifiants et messages sur Internet sans imposer TLS entre conteneurs locaux. Seul 8883 est publié par la configuration. | HTTP/MQTT en clair est plus simple au banc, mais ne convient pas à une connexion publique. MQTT sur VPN est une autre bonne solution si le VPN existe déjà. |
| Guidage | Décision et audio sur la canne, indépendants du VPS | La latence et Internet ne doivent pas arrêter les annonces locales. MQTT sert à la supervision et au SOS distant. | Décider sur le serveur introduirait délai, dépendance réseau et panne de guidage hors ligne. |

## 2. Architecture retenue

```text
HC-SR04 tête → ESP32 lunettes
             │ ESP-NOW
             ▼
Ultrasons → ESP32 canne → audio local / vibration
                  │ Wi-Fi + MQTT/TLS
                  ▼
             VPS Mosquitto
               ├── dashboard web (lecture seule)
               └── relais Telegram (lecture SOS)
```

La canne est la passerelle. Les lunettes n'ont pas besoin de se connecter au routeur. La canne garde la responsabilité des annonces vocales et des décisions locales. En cas de perte Internet, elle doit toujours détecter et annoncer localement ; le dashboard et Telegram deviennent temporairement indisponibles.

### Point radio à vérifier

La connexion Wi-Fi de la canne et ESP-NOW partagent la radio et le canal Wi-Fi. Règle de l'équipe : au démarrage, canne et lunettes cherchent le réseau `WIFI_SSID` et se placent sur son canal (canal 1 sinon). Le partage de connexion doit donc être actif avant l'allumage et garder son canal. La veille radio de la canne est désactivée (`WiFi.setSleep(false)`), sinon des paquets ESP-NOW sont perdus. Si le Wi-Fi est absent au démarrage, la canne guide seule, sans SOS ni dashboard, jusqu'au redémarrage suivant.

## 3. Pourquoi MQTT pour ce projet

Le projet a plusieurs consommateurs des mêmes données : écran de supervision, journal d'événements, relais Telegram. Avec MQTT, l'ESP32 publie une fois sous un sujet et chacun des services concernés s'abonne. Les sujets séparent les usages (`telemetry`, `event`, `sos`, `availability`) et évitent de faire appeler directement le dashboard depuis le firmware.

MQTT ne rend pas automatiquement le SOS fiable, et PubSubClient ne publie qu'en QoS 0. Le SOS repose donc sur un accusé applicatif : la canne réémet le même `event_id` toutes les 5 s jusqu'à recevoir `sos/ack`. Le relais déduplique les identifiants (liste conservée sur disque) et n'envoie l'accusé qu'une fois le message accepté par Telegram.

HTTP serait une bonne alternative si le seul besoin était d'envoyer occasionnellement un SOS à une API web. Pour le flux périodique et les abonnés dashboard + Telegram, le broker MQTT est plus adapté au périmètre du projet.

## 4. Pourquoi le dashboard est une page Flask

La page incluse est une démonstration simple : elle reçoit les sujets MQTT et affiche les dernières distances, l'état ESP-NOW et les événements. Elle n'ajoute pas de base de données ni de service externe. C'est proportionné au parcours de démonstration et au délai annoncé.

Limites assumées : l'historique est gardé en mémoire et disparaît au redémarrage; les valeurs ne sont pas un historique de mesure certifié; l'accès public passe par Caddy (HTTPS + mot de passe) sur `https://DOMAIN`. Si l'équipe a besoin de courbes sur plusieurs jours, alertes configurables ou un éditeur visuel, Node-RED et une base de données deviennent plus intéressants.

Le dashboard n'invente pas les champs manquants : les distances gauche/centre/droite et la batterie resteront vides tant que le firmware ne les publie pas avec les noms attendus (`left_cm`, `center_cm`, `right_cm`, `battery_pct`).

## 5. Pourquoi envoyer Telegram depuis le VPS

Le jeton Telegram est une clé secrète. Le garder sur le VPS évite de l'inclure dans le firmware diffusé, les dépôts, les captures ou les binaires. Le VPS reçoit seulement le sujet MQTT `.../sos`, valide le type d'événement, limite les répétitions rapprochées et appelle la Bot API Telegram.

L'anti-répétition du bridge fourni est en mémoire : les identifiants déjà vus sont oubliés au redémarrage. Pour un prototype, c'est suffisant pour une démonstration; pour un système plus fiable, enregistrer les IDs traités dans un stockage persistant et faire remonter une confirmation au dispositif. Il faut aussi distinguer dans l'audio « SOS publié » de « proche notifié » : la canne ne peut pas promettre que Telegram a livré le message sans retour applicatif.

## 6. Ce qui est en place

- Installation du VPS en une commande (`deploy/deploy.sh`) : Docker, certificat Let's Encrypt renouvelé automatiquement, pare-feu, comptes, ACL.
- Trois comptes MQTT aux droits minimaux (canne, relais Telegram, dashboard) ; chaque conteneur ne reçoit que ses propres secrets.
- MQTT uniquement en TLS depuis Internet (port 8883) ; le firmware vérifie le certificat du broker (ISRG Root X1).
- SOS fiable de bout en bout : réémission par la canne, accusé du relais, déduplication persistante, mention du retard dans Telegram, plusieurs destinataires possibles.
- Le jeton Telegram n'apparaît jamais dans les journaux (les erreurs réseau ne sont pas affichées en entier).
- Dashboard en HTTPS avec mot de passe : distances, lunettes, batterie, signal Wi-Fi, état en ligne/hors ligne, détection des données périmées, journal des annonces et suivi de chaque SOS.
- Firmware canne : programme de l'équipe relié au VPS ; réseau dans une tâche séparée (cœur 0), jamais bloquant pour le guidage ; jeton Telegram retiré du firmware ; reconnexion MQTT automatique ; mesure batterie optionnelle.
- Firmware lunettes : programme de l'équipe, inchangé.
- Test du SOS depuis le VPS sans ESP32 (`deploy/test-sos.sh`).

## 7. Ce qui reste à faire

- **R1** : câbler le pont diviseur de la batterie si la mesure doit apparaître au dashboard, et renseigner les broches réelles.
- **Équipe** : créer le domaine, démarrer le bot Telegram, lancer `deploy.sh` sur le VPS, enregistrer `0010.mp3` (« Alerte non envoyée ») sur la carte SD si ce n'est pas fait.
- **Limites assumées du prototype** : un SOS en attente est perdu si la canne redémarre ; la liaison ESP-NOW n'est pas chiffrée (filtrée par le code `CANE`) ; l'historique du dashboard est en mémoire.

## 8. Ce qui n'a pas pu être vérifié

- Les firmwares n'ont pas été compilés (pas d'arduino-cli sur le poste de préparation) : compiler dans l'Arduino IDE avec le cœur ESP32 3.x.
- La pile Docker n'a pas été lancée de bout en bout ; la logique du relais et du dashboard a été testée avec MQTT et Telegram simulés.

## 9. Plan court de validation

1. **PC → VPS :** connexion MQTT TLS avec l'utilisateur `canne01`; vérifier que l'ACL autorise la publication et que le dashboard reçoit un JSON exemple.
2. **SOS côté VPS :** publier un payload SOS de test; vérifier une notification Telegram, puis tester l'anti-doublon.
3. **ESP32 canne → VPS :** joindre le Wi-Fi et le broker, publier un message; vérifier TLS, reconnexion et affichage.
4. **Lunettes → canne en simultané :** démarrer ESP-NOW pendant que Wi-Fi/MQTT est actif et vérifier la fraîcheur de la mesure ToF.
5. **Bouton physique :** appui long → annonce locale immédiate + événement distant, sans blocage du cycle capteurs.
6. **Mode dégradé :** couper Internet et VPS; la détection et l'audio doivent continuer localement; reconnecter et vérifier le comportement SOS en attente/reprise.
7. **Démo :** confirmer que toutes les valeurs affichées proviennent des mesures, tester le parcours accompagné et expliquer les limites du prototype.

## 10. Recommandation de périmètre

Pour le délai de cinq jours, garder l'IoT à trois résultats démontrables : (1) le VPS reçoit la télémétrie MQTT, (2) le dashboard affiche les mesures de la canne, (3) un SOS déclenche Telegram. Ne pas ajouter GPS, historique complexe, commandes distantes ou application mobile tant que ces trois scénarios ne sont pas testés. La partie mobilité reste locale et doit être présentée comme prototype expérimental.

---

Les secrets d'accès et les données personnelles ne doivent pas figurer dans ce document. Remplir les paramètres directement sur le VPS et dans les fichiers locaux non versionnés.
