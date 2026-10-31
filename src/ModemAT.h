#ifndef CZ_N706B_MODEM_AT_H
#define CZ_N706B_MODEM_AT_H

#include <Arduino.h>

enum ModemATResult {
    MODEM_AT_OK,
    MODEM_AT_ERROR,
    MODEM_AT_NO_RESPONSE,
    MODEM_AT_TIMEOUT,
    MODEM_AT_OVERFLOW,
    MODEM_AT_BLOCKED,
    MODEM_AT_MISSING_DATA
};

enum ModemATValueResult {
    MODEM_AT_VALUE_OK,
    MODEM_AT_VALUE_NONE,
    MODEM_AT_VALUE_BUFFER_TOO_SMALL
};

void ModemAT_begin(HardwareSerial &modemUart, HardwareSerial &pcUart, unsigned long quietMs);
bool ModemAT_isInitialized(void);
bool ModemAT_resetAfterVerifiedPowerState(void);
ModemATResult ModemAT_prepareAfterPowerOn(unsigned long drainTimeoutMs, unsigned long quietMs);
void ModemAT_poll(void);
void ModemAT_pollIdle(void);
void ModemAT_printLine(const char *text);
ModemATResult ModemAT_execute(const char *command, const char *expectedPrefix, unsigned long timeoutMs);
ModemATValueResult ModemAT_copyResponseValue(char *destination, size_t destinationSize);
bool ModemAT_commandBoundaryReady(void);
// With stopOnOk, minimumProbes extends the timeout until that many AT attempts
// and a final retry interval; OK returns immediately. RX errors may stop early.
ModemATResult ModemAT_observeAt(unsigned long durationMs, unsigned long retryMs,
    unsigned int minimumProbes, unsigned long silenceMs, bool stopOnOk);
void ModemAT_reportResult(ModemATResult result);
// One idempotent AT probe; only a prior AT probe may have an unresolved final.
ModemATResult ModemAT_probeAtOnce(unsigned long timeoutMs);
bool ModemAT_enterRawMode(void);
bool ModemAT_isRawMode(void);
// Socket diagnostics may buffer raw input during PC writes without parsing it.
bool ModemAT_setRawLogBuffering(bool enabled);
bool ModemAT_rawOverflowed(void);
int ModemAT_rawRead(void);
size_t ModemAT_rawAvailable(void);

// Handlers run synchronously; do not issue commands or print from a handler.
typedef void (*ModemATLineHandler)(const char *line);
bool ModemAT_setLineObserver(ModemATLineHandler observer);
ModemATResult ModemAT_executeWaitUrc(const char *command, unsigned long commandTimeoutMs,
    const char *urcPrefix, char *value, size_t capacity, unsigned long resultTimeoutMs);
ModemATResult ModemAT_executeWithHandler(const char *command, unsigned long timeoutMs,
    ModemATLineHandler handler);
ModemATResult ModemAT_writeData(const char *command, const uint8_t *data, size_t size,
    unsigned long timeoutMs);

// Read an FSRF payload by its byte count, preserving embedded CR/LF.
ModemATResult ModemAT_readFile(const char *command, uint8_t *data, size_t expectedSize,
    unsigned long timeoutMs);

#endif
