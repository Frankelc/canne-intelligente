#pragma once
// Partie connectée du système, portée par les LUNETTES (passerelle) :
// MQTT/TLS vers le VPS (télémétrie, journal, SOS).
// La canne n'a plus de connexion Internet : son ESP32 manque de mémoire pour le Bluetooth
// audio et le TLS en même temps. Elle envoie ses données aux lunettes par ESP-NOW.
// Tout tourne dans une tâche FreeRTOS sur le cœur 0 : une coupure réseau ou une connexion
// TLS lente ne bloque jamais la boucle capteur/ESP-NOW (loop(), cœur 1).
#include <Arduino.h>

// À appeler dans setup(), après la connexion Wi-Fi.
void iotBegin();

// true si la liaison au VPS est configurée (MQTT_HOST et certificat, ou mode non vérifié).
bool iotIsConfigured();

// true quand la connexion MQTT/TLS au VPS est établie.
bool iotIsConnected();

// État courant pour le tableau de bord. Distances en cm, -1 = rien détecté.
void iotSetState(long gaucheCm, long centreCm, long droiteCm, long teteCm, bool canneConnectee,
                 bool casqueConnecte);

// Signale au tableau de bord un message vocal joué par la canne (numéro et texte).
void iotReportMessage(int mp3, const char *text);

// SOS demandé par la canne (numéro 1 à 255). Réémis toutes les 5 s jusqu'à ce que le VPS
// confirme l'envoi Telegram (30 min au plus).
void iotRequestSos(uint8_t numero);

// Numéro du dernier SOS confirmé par le VPS (0 = aucun), renvoyé à la canne.
uint8_t iotConfirmedSos();
