#ifndef CZ_N706B_MODEM_HTTP_H
#define CZ_N706B_MODEM_HTTP_H
#include <Arduino.h>

struct ModemHttpConfig {
    const char *apn;
    const char *host;
    uint16_t port;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long setupTimeoutMs;
    unsigned long receiveTimeoutMs;
    unsigned long pollIntervalMs;
    unsigned long closeTimeoutMs;
};

struct ModemHttpTlsConfig {
    const char *caFile;
    unsigned int sslVersion;
};

// Exclusive raw UART ownership after power-on; no automatic request retry.
bool ModemHttp_begin(const ModemHttpConfig *config, HardwareSerial &uart);
// Server authentication is always enabled; no certificate upload or deletion.
bool ModemHttp_beginTls(const ModemHttpConfig *config, const ModemHttpTlsConfig *tls,
    HardwareSerial &uart);
bool ModemHttp_prepareNetwork();
bool ModemHttp_get(const char *path);
bool ModemHttp_post(const char *path, const char *body);
// Download to a new modem file; verify size and full readback.
// A non-NULL expectedPattern additionally checks every byte against that repeating pattern.
// Existing targets are preserved unless removeExisting is explicitly true.
// Only the named target is removed; absence must be confirmed. No throughput measurement.
bool ModemHttp_download(const char *path, const char *fileName, unsigned long expectedBytes,
    const char *expectedPattern, unsigned long timeoutMs, bool removeExisting = false);
unsigned int ModemHttp_status();
const char *ModemHttp_body();
void ModemHttp_printResponse();
bool ModemHttp_close();
bool ModemHttp_releaseIp();
void ModemHttp_pollIdle();
#endif
