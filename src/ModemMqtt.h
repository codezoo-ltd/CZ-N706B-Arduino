#ifndef CZ_N706B_MODEM_MQTT_H
#define CZ_N706B_MODEM_MQTT_H
#include <Arduino.h>

struct ModemMqttConfig {
    const char *apn;
    const char *host;
    uint16_t port;
    const char *clientId;
    const char *username;
    const char *password;
    unsigned int cleanSession;
    unsigned int keepAliveSeconds;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long connectTimeoutMs;
    unsigned long receiveTimeoutMs;
    unsigned long pollIntervalMs;
    unsigned long closeTimeoutMs;
};

struct ModemMqttTlsConfig {
    const char *rootCa;
    const char *clientCertificate;
    const char *clientKey;
    unsigned int authMode;
    unsigned int sslVersion;
};

// Exclusive raw UART use; single-channel MQTT, no automatic reconnect.
bool ModemMqtt_begin(const ModemMqttConfig *config, HardwareSerial &uart);
bool ModemMqtt_prepareNetwork();
bool ModemMqtt_connect();
bool ModemMqtt_connectTls(const ModemMqttTlsConfig *tls);
// Observe delayed restart notifications, then check AT readiness once.
bool ModemMqtt_prepareReconnect(unsigned long restartWaitMs);
bool ModemMqtt_subscribe(const char *topic, unsigned int qos);
bool ModemMqtt_publish(const char *topic, const char *message, unsigned int qos,
    bool retained);
// Return one length-framed message; no payload equality or application validation.
bool ModemMqtt_receive(char *topic, size_t topicCapacity, uint8_t *data, size_t capacity,
    size_t *receivedSize);
bool ModemMqtt_unsubscribe(const char *topic);
bool ModemMqtt_disconnect(unsigned long restartWaitMs = 0);
bool ModemMqtt_releaseIp();
void ModemMqtt_pollIdle();
void ModemMqtt_wait(unsigned long durationMs);
#endif
