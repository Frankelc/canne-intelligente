#pragma once

// Configuration de la CANNE : copier ce fichier sous le nom config.h, puis renseigner.
// La canne ne se connecte pas à Internet (c'est le rôle des lunettes) : elle n'a besoin
// que de ces deux réglages.

// Nom du partage de connexion du téléphone, IDENTIQUE à celui de lunettes/config.h.
// La canne ne s'y connecte pas : elle s'en sert pour se caler sur le même canal radio que
// les lunettes (indispensable pour ESP-NOW).
#define WIFI_SSID ""

// Casque audio Bluetooth : son nom EXACT (majuscules ignorées), tel qu'il
// apparaît dans la liste Bluetooth d'un téléphone. Laisser "" : la canne affiche dans le
// moniteur série les appareils trouvés, pour connaître le nom exact.
#define BT_HEADSET_NAME ""
