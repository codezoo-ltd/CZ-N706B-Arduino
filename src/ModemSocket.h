#ifndef CZ_N706B_MODEM_SOCKET_H
#define CZ_N706B_MODEM_SOCKET_H

#include <Arduino.h>

enum ModemSocketProtocol {
    MODEM_SOCKET_TCP,
    MODEM_SOCKET_UDP,
    MODEM_SOCKET_TLS
};

struct ModemSocketConfig {
    ModemSocketProtocol protocol;
    const char *apn;
    const char *host;
    uint16_t port;
    uint8_t socketId;
    const char *payload;
    const char *caFile;
    const char *httpPath;
    unsigned int testCount;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long socketTimeoutMs;
    unsigned long receiveTimeoutMs;
    unsigned long pollIntervalMs;
    unsigned long testIntervalMs;
};

// Exclusive modem use; call once after power-on. No automatic retries or recovery.
bool ModemSocket_run(const ModemSocketConfig *config, HardwareSerial &uart);
void ModemSocket_pollIdle();

// TCP connection settings exclude application data and example repetition policy.
struct ModemSocketTcpConfig {
    const char *apn;
    const char *host;
    uint16_t port;
    uint8_t socketId;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long socketTimeoutMs;
    unsigned long receiveTimeoutMs;
    unsigned long pollIntervalMs;
    size_t readChunkBytes;
};

struct ModemSocketUdpConfig {
    const char *apn;
    const char *host;
    uint16_t port;
    uint8_t socketId;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long socketTimeoutMs;
    unsigned long receiveTimeoutMs;
    unsigned long pollIntervalMs;
    size_t readChunkBytes;
};

bool ModemSocket_begin(const ModemSocketTcpConfig *config, HardwareSerial &uart);

bool ModemSocket_begin(const ModemSocketUdpConfig *config, HardwareSerial &uart);

struct ModemSocketTlsConfig {
    const char *apn;
    const char *host;
    uint16_t port;
    uint8_t socketId;
    const char *caFile;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long socketTimeoutMs;
    unsigned long receiveTimeoutMs;
    unsigned long pollIntervalMs;
    unsigned long closeUrcWaitMs;
};

bool ModemSocket_begin(const ModemSocketTlsConfig *config, HardwareSerial &uart);

// Explicit socket API: the sketch controls order, count, cleanup and IP release.
bool ModemSocket_begin(const ModemSocketConfig *config, HardwareSerial &uart);
// Defer TLS only when a pre-TLS operation (such as time sync) needs the active IP.
bool ModemSocket_prepareNetwork(bool deferTlsConfiguration = false);
bool ModemSocket_configureTls();
// Optional TLS receive policy; select before network preparation.
bool ModemSocket_useBufferedReceive(size_t readChunkBytes);
bool ModemSocket_setQuietMode(bool enabled);
// Bounded control transactions before opening a socket; UART ownership stays here.
bool ModemSocket_executeControl(const char *command, const char *prefix,
    char *value, size_t capacity, unsigned long timeoutMs);
bool ModemSocket_executeControlWait(const char *command, const char *prefix,
    char *value, size_t capacity, unsigned long commandTimeoutMs,
    unsigned long resultTimeoutMs);
// Opt-in pre-authentication wire trace; automatically disabled before the first send.
bool ModemSocket_enableGreetingDiagnostics();
// Opt-in for server-first protocols; open waits for setup and the first buffered URC.
bool ModemSocket_waitForGreetingOnOpen(unsigned long timeoutMs);
bool ModemSocket_open();
bool ModemSocket_send(const char *payload);
bool ModemSocket_recv(uint8_t *data, size_t capacity, size_t *receivedSize,
    unsigned long timeoutMs = 0);
// True only for a parsed peer-close result at a known receive boundary.
bool ModemSocket_remoteCloseConfirmed();
// Query TLS connection state only at a known, complete receive boundary.
bool ModemSocket_confirmClosed();
bool ModemSocket_waitForClose();
bool ModemSocket_close();
bool ModemSocket_releaseIp();
void ModemSocket_wait(unsigned long durationMs);

#endif
