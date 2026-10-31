#ifndef CZ_N706B_MODEM_TIME_H
#define CZ_N706B_MODEM_TIME_H
#include <Arduino.h>

struct ModemTimeConfig {
    const char *apn;
    const char *server;
    unsigned int serverTimeoutSeconds;
    const char *timeZone;
    unsigned int daylightSaving;
    bool clockFieldsAreUtc;
    unsigned long queryTimeoutMs;
    unsigned long registerTimeoutMs;
    unsigned long linkTimeoutMs;
    unsigned long syncResultTimeoutMs;
    unsigned long pollIntervalMs;
};

struct ModemTimeClock {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int offsetMinutes;
};

bool ModemTime_begin(const ModemTimeConfig *config);
// Reuse the already prepared socket data link without changing UART ownership.
bool ModemTime_beginOnSocket(const ModemTimeConfig *config);
bool ModemTime_prepareNetwork();
bool ModemTime_readClock(ModemTimeClock *clock);
bool ModemTime_syncTime();
// Apply a validated Update To result using CCLK, then verify UTC readback.
// Requires timeZone E0 and daylightSaving 0. Existing TimeSync behavior is unchanged.
bool ModemTime_applySyncedClock();
bool ModemTime_releaseIp();
void ModemTime_pollIdle();
void ModemTime_printLine(const char *text);
// Convert only when the firmware returns UTC clock fields plus an offset.
bool ModemTime_parseClock(const char *text, bool fieldsAreUtc, ModemTimeClock *clock);
#endif
