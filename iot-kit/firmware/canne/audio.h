#pragma once
// Voix de la canne envoyée en Bluetooth (A2DP) à un casque audio.
// Les 10 messages sont stockés dans la mémoire flash (audio_clips.h).
#include <Arduino.h>

const int COTE_DEUX = 0;     // message dans les deux oreilles
const int COTE_GAUCHE = 1;   // oreille gauche seulement
const int COTE_DROITE = 2;   // oreille droite seulement

// À appeler dans setup(). nomCasque : nom Bluetooth EXACT du casque (majuscules ignorées).
void audioBegin(const char *nomCasque);

// Joue le message n (1 à 10) ; remplace le message en cours s'il y en a un.
void audioPlay(int n, int cote);

// true quand le casque est connecté.
bool audioConnected();
