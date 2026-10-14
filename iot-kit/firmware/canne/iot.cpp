#include "iot.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

namespace {

struct MessageEvent {
  uint8_t mp3;
  char text[56];
};

constexpr uint32_t MQTT_RETRY_MS = 5000;
constexpr uint32_t TELEMETRY_PERIOD_MS = 1000;
constexpr uint32_t SOS_RETRY_MS = 5000;
constexpr uint32_t SOS_LATE_MS = 20000;
constexpr uint32_t SOS_EXPIRY_MS = 30UL * 60UL * 1000UL;

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
bool tlsReady = false;

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
long distances[4] = {-1, -1, -1, -1};  // gauche, centre, droite, tête
bool glassesOk = false;

QueueHandle_t messageQueue;

volatile bool sosRequested = false;
volatile bool sosPending = false;
volatile bool sosConfirmed = false;
volatile bool sosLate = false;
bool sosLateSignaled = false;
char sosId[12] = "";
uint32_t sosCreatedMs = 0;
uint32_t sosLastSendMs = 0;
uint16_t sosAttempts = 0;

String topic(const char *suffix) {
  return String(TOPIC_PREFIX) + "/" + suffix;
}

int batteryPercent() {
  if (BATTERY_ADC_PIN < 0) return -1;
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATTERY_ADC_PIN);
  const float volts = sum / 16.0f / 1000.0f * BATTERY_DIVIDER_RATIO;
  // Courbe de décharge approximative de 3 piles alcalines AA en série (4,8 V neuves).
  // 0 % à 4,2 V : en dessous, l'ESP32 et les HC-SR04 deviennent instables.
  static const float table[][2] = {{4.20f, 0}, {4.30f, 15}, {4.40f, 35}, {4.50f, 55},
                                   {4.60f, 75}, {4.70f, 90}, {4.80f, 100}};
  if (volts <= table[0][0]) return 0;
  for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++) {
    if (volts <= table[i][0]) {
      const float t = (volts - table[i - 1][0]) / (table[i][0] - table[i - 1][0]);
      return (int)(table[i - 1][1] + t * (table[i][1] - table[i - 1][1]) + 0.5f);
    }
  }
  return 100;
}

void onMqttMessage(char *topicName, uint8_t *payload, unsigned int length) {
  if (!sosPending || String(topicName) != topic("sos/ack")) return;
  char text[160];
  const unsigned int n = min(length, (unsigned int)sizeof(text) - 1);
  memcpy(text, payload, n);
  text[n] = '\0';
  if (strstr(text, sosId) != nullptr) {
    sosPending = false;
    sosConfirmed = true;
    Serial.printf("[iot] SOS %s confirmé par le VPS : %s\n", sosId, text);
  }
}

void maintainMqtt() {
  static uint32_t lastAttemptMs = 0;
  if (!tlsReady || mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (lastAttemptMs != 0 && millis() - lastAttemptMs < MQTT_RETRY_MS) return;
  lastAttemptMs = millis();
  const String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const String availability = topic("availability");
  // Connexion TLS bloquante quelques secondes : sans effet sur loop(), qui tourne sur l'autre cœur.
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD, availability.c_str(), 1, true, "offline")) {
    mqtt.publish(availability.c_str(), "online", true);
    mqtt.subscribe(topic("sos/ack").c_str(), 1);
    Serial.println("[iot] MQTT connecté au VPS (TLS)");
  } else {
    char erreurTls[100] = "";
    const int codeTls = tlsClient.lastError(erreurTls, sizeof(erreurTls));
    Serial.printf("[iot] MQTT échec, état=%d, TLS=%d (%s), mémoire libre=%u, plus grand bloc=%u\n",
                  mqtt.state(), codeTls, erreurTls, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
}

void handleSos() {
  if (sosRequested) {
    sosRequested = false;
    if (!sosPending) {
      snprintf(sosId, sizeof(sosId), "%08lx", (unsigned long)esp_random());
      sosCreatedMs = millis();
      sosLastSendMs = 0;
      sosAttempts = 0;
      sosConfirmed = false;
      sosLate = false;
      sosLateSignaled = false;
      sosPending = true;
      Serial.printf("[iot] SOS %s créé\n", sosId);
    }
  }
  if (!sosPending) return;
  const uint32_t age = millis() - sosCreatedMs;
  if (!sosLateSignaled && age > SOS_LATE_MS) {
    sosLateSignaled = true;
    sosLate = true;
  }
  if (age > SOS_EXPIRY_MS) {
    sosPending = false;
    Serial.printf("[iot] SOS %s abandonné : aucune confirmation en 30 min\n", sosId);
    return;
  }
  if (!mqtt.connected()) return;
  if (sosLastSendMs != 0 && millis() - sosLastSendMs < SOS_RETRY_MS) return;
  sosLastSendMs = millis();
  sosAttempts++;
  bool lunettes;
  portENTER_CRITICAL(&lock);
  lunettes = glassesOk;
  portEXIT_CRITICAL(&lock);
  char payload[220];
  snprintf(payload, sizeof(payload),
           "{\"v\":1,\"event\":\"SOS\",\"event_id\":\"%s\",\"device\":\"%s\",\"age_s\":%lu,\"attempt\":%u,\"glasses\":%s}",
           sosId, DEVICE_ID, (unsigned long)(age / 1000), sosAttempts, lunettes ? "true" : "false");
  mqtt.publish(topic("sos").c_str(), payload, false);
  Serial.printf("[iot] SOS %s publié (essai %u)\n", sosId, sosAttempts);
}

void publishMessages() {
  MessageEvent event;
  while (xQueueReceive(messageQueue, &event, 0) == pdTRUE) {
    if (!mqtt.connected()) continue;  // journal non critique : pas de file d'attente
    char payload[200];
    snprintf(payload, sizeof(payload), "{\"v\":1,\"type\":\"annonce\",\"mp3\":%u,\"message\":\"%s\"}",
             event.mp3, event.text);
    mqtt.publish(topic("event").c_str(), payload, false);
  }
}

void appendNumber(char *out, size_t size, const char *key, long value) {
  const size_t used = strlen(out);
  if (value < 0) snprintf(out + used, size - used, ",\"%s\":null", key);
  else snprintf(out + used, size - used, ",\"%s\":%ld", key, value);
}

void publishTelemetry() {
  if (!mqtt.connected()) return;
  long d[4];
  bool lunettes;
  portENTER_CRITICAL(&lock);
  memcpy(d, distances, sizeof(d));
  lunettes = glassesOk;
  portEXIT_CRITICAL(&lock);

  // null = rien détecté dans la portée du capteur (le dashboard affiche « Libre »).
  char payload[320];
  snprintf(payload, sizeof(payload), "{\"v\":1,\"device\":\"%s\"", DEVICE_ID);
  appendNumber(payload, sizeof(payload), "left_cm", d[0]);
  appendNumber(payload, sizeof(payload), "center_cm", d[1]);
  appendNumber(payload, sizeof(payload), "right_cm", d[2]);
  appendNumber(payload, sizeof(payload), "head_cm", lunettes ? d[3] : -1);
  appendNumber(payload, sizeof(payload), "battery_pct", batteryPercent());
  const size_t used = strlen(payload);
  snprintf(payload + used, sizeof(payload) - used,
           ",\"espnow\":%s,\"rssi\":%d,\"sos_pending\":%s,\"uptime_s\":%lu}",
           lunettes ? "true" : "false", (int)WiFi.RSSI(), sosPending ? "true" : "false",
           (unsigned long)(millis() / 1000));
  mqtt.publish(topic("telemetry").c_str(), payload, false);
}

void iotTask(void *) {
  uint32_t lastTelemetryMs = 0;
  for (;;) {
    maintainMqtt();
    mqtt.loop();
    handleSos();
    publishMessages();
    if (millis() - lastTelemetryMs >= TELEMETRY_PERIOD_MS) {
      lastTelemetryMs = millis();
      publishTelemetry();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void iotBegin() {
  messageQueue = xQueueCreate(8, sizeof(MessageEvent));
  if (strlen(MQTT_HOST) == 0) {
    Serial.println("[iot] MQTT_HOST vide : pas de VPS (dashboard et SOS indisponibles)");
  } else if (strlen(MQTT_CA_CERT) > 0) {
    tlsClient.setCACert(MQTT_CA_CERT);
    tlsReady = true;
  } else if (MQTT_ALLOW_INSECURE) {
    tlsClient.setInsecure();
    tlsReady = true;
    Serial.println("[iot] ATTENTION : certificat du VPS non vérifié (MQTT_ALLOW_INSECURE)");
  } else {
    Serial.println("[iot] MQTT_CA_CERT vide : MQTT désactivé (voir deploy/config-mqtt.h)");
  }
  tlsClient.setHandshakeTimeout(10);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(20);
  mqtt.setSocketTimeout(5);
  xTaskCreatePinnedToCore(iotTask, "iot", 12288, nullptr, 1, nullptr, 0);
}

bool iotIsConfigured() {
  return tlsReady;
}

bool iotIsConnected() {
  return mqtt.connected();
}

void iotSetState(long gaucheCm, long centreCm, long droiteCm, long teteCm, bool lunettesOk) {
  portENTER_CRITICAL(&lock);
  distances[0] = gaucheCm;
  distances[1] = centreCm;
  distances[2] = droiteCm;
  distances[3] = teteCm;
  glassesOk = lunettesOk;
  portEXIT_CRITICAL(&lock);
}

void iotReportMessage(int mp3, const char *text) {
  if (messageQueue == nullptr) return;
  MessageEvent event{};
  event.mp3 = (uint8_t)mp3;
  // Le texte part dans du JSON : on retire guillemets et antislashs.
  size_t i = 0;
  for (; text[i] != '\0' && i < sizeof(event.text) - 1; i++) {
    event.text[i] = (text[i] == '"' || text[i] == '\\') ? ' ' : text[i];
  }
  event.text[i] = '\0';
  xQueueSend(messageQueue, &event, 0);
}

void iotRequestSos() {
  sosRequested = true;
}

bool iotSosPending() {
  return sosPending || sosRequested;
}

bool iotTakeSosConfirmed() {
  if (!sosConfirmed) return false;
  sosConfirmed = false;
  return true;
}

bool iotTakeSosLate() {
  if (!sosLate) return false;
  sosLate = false;
  return true;
}
