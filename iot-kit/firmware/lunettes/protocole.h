#pragma once
// Messages ESP-NOW échangés entre la canne et les lunettes.
// CE FICHIER DOIT ÊTRE IDENTIQUE dans firmware/canne/ et firmware/lunettes/.
//
// Rôles : la canne gère capteurs, décision et voix Bluetooth ; les lunettes mesurent la
// hauteur de tête ET servent de passerelle vers le VPS (Wi-Fi + MQTT/TLS), car un seul
// ESP32 n'a pas assez de mémoire pour le Bluetooth audio et le TLS en même temps.
#include <stdint.h>

const uint32_t CODE_PROJET = 0x43414E45;   // "CANE" : ignore les autres appareils ESP-NOW

enum TypePaquet : uint8_t {
  PAQUET_LUNETTES = 1,   // lunettes -> canne, toutes les 100 ms
  PAQUET_CANNE    = 2,   // canne -> lunettes, toutes les 300 ms
  PAQUET_ANNONCE  = 3,   // canne -> lunettes, à chaque message vocal
};

// Lunettes -> canne
typedef struct __attribute__((packed)) {
  uint32_t code;
  uint8_t  type;             // PAQUET_LUNETTES
  int16_t  teteCm;           // -1 = rien détecté
  uint8_t  vpsConfigure;     // 1 = bloc MQTT présent dans config.h des lunettes
  uint8_t  vpsConnecte;      // 1 = connexion MQTT/TLS au VPS établie
  uint8_t  sosConfirme;      // numéro du dernier SOS confirmé par le VPS (0 = aucun)
} PaquetLunettes;

// Canne -> lunettes
typedef struct __attribute__((packed)) {
  uint32_t code;
  uint8_t  type;             // PAQUET_CANNE
  int16_t  gaucheCm;         // -1 = rien détecté
  int16_t  centreCm;
  int16_t  droiteCm;
  uint8_t  casqueConnecte;   // 1 = casque Bluetooth connecté
  uint8_t  sosNumero;        // SOS en attente de confirmation (0 = aucun)
} PaquetCanne;

// Canne -> lunettes : message vocal joué (pour le journal du tableau de bord)
typedef struct __attribute__((packed)) {
  uint32_t code;
  uint8_t  type;             // PAQUET_ANNONCE
  uint8_t  message;          // numéro du message (1 à 10)
  uint8_t  numero;           // compteur, pour ignorer les doublons
} PaquetAnnonce;
