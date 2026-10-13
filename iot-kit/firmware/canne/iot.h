#pragma once
// Partie connectée de la canne : MQTT/TLS vers le VPS (télémétrie, journal, SOS).
// Tout tourne dans une tâche FreeRTOS sur le cœur 0 : une coupure réseau ou une connexion
// TLS lente ne bloque jamais la boucle capteurs/voix (loop(), cœur 1).
// Le Wi-Fi et ESP-NOW sont démarrés par canne.ino (demarrerRadio) ; ce module les utilise.
#include <Arduino.h>

// À appeler dans setup(), après demarrerRadio().
void iotBegin();

// État courant pour le dashboard. Distances en cm, -1 = rien détecté.
void iotSetState(long gaucheCm, long centreCm, long droiteCm, long teteCm, bool lunettesOk);

// Signale au dashboard un message vocal joué (numéro du MP3 et texte).
void iotReportMessage(int mp3, const char *text);

// Déclenche un SOS. Il est réémis toutes les 5 s, même après une coupure réseau,
// jusqu'à ce que le VPS confirme l'envoi Telegram (30 min au plus).
void iotRequestSos();
bool iotSosPending();
// true une seule fois quand le VPS a confirmé le SOS (jouer « Alerte envoyée »).
bool iotTakeSosConfirmed();
// true une seule fois si le SOS n'est pas confirmé après 20 s (jouer « Alerte non envoyée ») ;
// la canne continue malgré tout d'essayer en arrière-plan.
bool iotTakeSosLate();
