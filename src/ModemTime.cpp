#include "ModemTime.h"
#include "ModemAT.h"
#include "ModemSocket.h"
#include <stdio.h>
#include <string.h>

static const size_t COMMAND_BYTES = 192;
static const size_t VALUE_BYTES = 160;

struct TimeState {
    ModemTimeConfig config;
    bool initialized;
    bool socketTransport;
    bool blocked;
    bool startupWindow;
    bool syncAttempted;
    bool syncing;
    bool releasing;
    char value[VALUE_BYTES];
    ModemTimeClock syncedClock;
    bool syncedClockValid;
    bool clockApplyAttempted;
};
static TimeState state = {};

static bool validText(const char *text, size_t limit) {
    return text != NULL && text[0] != '\0' && strlen(text) <= limit &&
        strpbrk(text, "\"\r\n") == NULL;
}
static bool fits(int written, size_t capacity) {
    return written >= 0 && (size_t)written < capacity;
}
static int monthDays(int year, int month) {
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return DAYS[month - 1] + (month == 2 && leap ? 1 : 0);
}
static bool validDate(const ModemTimeClock &clock) {
    return clock.year >= 2000 && clock.year <= 2099 && clock.month >= 1 &&
        clock.month <= 12 && clock.day >= 1 && clock.day <= monthDays(clock.year, clock.month) &&
        clock.hour >= 0 && clock.hour < 24 && clock.minute >= 0 && clock.minute < 60 &&
        clock.second >= 0 && clock.second < 60;
}
bool ModemTime_parseClock(const char *text, bool fieldsAreUtc, ModemTimeClock *clock) {
    if (text == NULL || clock == NULL || strlen(text) != 22 || text[0] != '"' ||
        text[21] != '"' || text[3] != '/' || text[6] != '/' || text[9] != ',' ||
        text[12] != ':' || text[15] != ':' || (text[18] != '+' && text[18] != '-')) {
        return false;
    }
    const size_t PAIRS[] = {1, 4, 7, 10, 13, 16, 19};
    int values[7];
    for (size_t index = 0; index < 7; ++index) {
        size_t position = PAIRS[index];
        if (text[position] < '0' || text[position] > '9' ||
            text[position + 1] < '0' || text[position + 1] > '9') {
            return false;
        }
        values[index] = (text[position] - '0') * 10 + text[position + 1] - '0';
    }
    ModemTimeClock parsed = {};
    parsed.year = 2000 + values[0];
    parsed.month = values[1];
    parsed.day = values[2];
    parsed.hour = values[3];
    parsed.minute = values[4];
    parsed.second = values[5];
    parsed.offsetMinutes = values[6] * 15 * (text[18] == '-' ? -1 : 1);
    if (!validDate(parsed) || parsed.offsetMinutes < -12 * 60 ||
        parsed.offsetMinutes > 14 * 60) {
        return false;
    }
    if (fieldsAreUtc) {
        int minutes = parsed.hour * 60 + parsed.minute + parsed.offsetMinutes;
        if (minutes < 0) {
            minutes += 24 * 60;
            --parsed.day;
            if (parsed.day == 0) {
                --parsed.month;
                if (parsed.month == 0) {
                    parsed.month = 12;
                    --parsed.year;
                }
                parsed.day = monthDays(parsed.year, parsed.month);
            }
        } else if (minutes >= 24 * 60) {
            minutes -= 24 * 60;
            ++parsed.day;
            if (parsed.day > monthDays(parsed.year, parsed.month)) {
                parsed.day = 1;
                ++parsed.month;
                if (parsed.month == 13) {
                    parsed.month = 1;
                    ++parsed.year;
                }
            }
        }
        parsed.hour = minutes / 60;
        parsed.minute = minutes % 60;
    }
    *clock = parsed;
    return true;
}
static void observeLine(const char *line) {
    if ((!strcmp(line, "+PBREADY") && !state.startupWindow) ||
        (!strcmp(line, "GPRS DISCONNECTION") && !state.releasing)) {
        state.blocked = true;
    }
}
void ModemTime_printLine(const char *text) {
    ModemAT_printLine(text);
}
void ModemTime_pollIdle() {
    ModemAT_pollIdle();
}
static void waitWithUrc(unsigned long durationMs) {
    unsigned long start = millis();
    while (!state.blocked && (unsigned long)(millis() - start) < durationMs) {
        ModemAT_pollIdle();
        delay(1);
    }
}
static bool query(const char *command, const char *prefix, unsigned long timeoutMs) {
    if (!state.initialized || state.blocked) {
        return false;
    }
    state.value[0] = '\0';
    if (state.socketTransport) {
        bool success = ModemSocket_executeControl(command, prefix, state.value,
            sizeof(state.value), timeoutMs);
        if (!success) {
            state.blocked = true;
        }
        return success;
    }
    ModemATResult result = ModemAT_execute(command, prefix, timeoutMs);
    ModemAT_reportResult(result);
    if (result == MODEM_AT_NO_RESPONSE || result == MODEM_AT_TIMEOUT ||
        result == MODEM_AT_OVERFLOW || result == MODEM_AT_BLOCKED) {
        state.blocked = true;
    }
    if (result != MODEM_AT_OK || state.blocked) {
        return false;
    }
    return prefix == NULL ||
        ModemAT_copyResponseValue(state.value, sizeof(state.value)) == MODEM_AT_VALUE_OK;
}
static int ipState() {
    unsigned int link;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    char extra;
    if (sscanf(state.value, "%u,%u.%u.%u.%u%c", &link, &a, &b, &c, &d, &extra) != 5 ||
        link > 1 || a > 255 || b > 255 || c > 255 || d > 255) {
        return -1;
    }
    return link == 1 && (a || b || c || d) ? 1 : link == 0 && !(a || b || c || d) ? 0 : -1;
}
static bool waitIp(bool active) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < state.config.linkTimeoutMs) {
        if (!query("AT+XIIC?", "+XIIC:", state.config.queryTimeoutMs)) {
            return false;
        }
        int link = ipState();
        if (link < 0) {
            return false;
        }
        if (link == (active ? 1 : 0)) {
            return true;
        }
        waitWithUrc(state.config.pollIntervalMs);
    }
    return false;
}
static bool beginTime(const ModemTimeConfig *config, bool socketTransport) {
    if (config == NULL || !validText(config->apn, 100) ||
        !validText(config->server, 100) ||
        strpbrk(config->server, " ,:/\t") || config->serverTimeoutSeconds < 1 ||
        config->serverTimeoutSeconds > 60 || !validText(config->timeZone, 3) ||
        config->daylightSaving > 1 || config->queryTimeoutMs == 0 ||
        config->registerTimeoutMs == 0 || config->linkTimeoutMs == 0 ||
        config->pollIntervalMs == 0 || config->syncResultTimeoutMs <
        config->serverTimeoutSeconds * 1000UL + config->queryTimeoutMs) {
        return false;
    }
    for (size_t index = 1; config->timeZone[index] != '\0'; ++index) {
        if (config->timeZone[index] < '0' || config->timeZone[index] > '9') {
            return false;
        }
    }
    unsigned int zone;
    char extra;
    if ((config->timeZone[0] != 'E' && config->timeZone[0] != 'W') ||
        sscanf(config->timeZone + 1, "%u%c", &zone, &extra) != 1 ||
        zone > (config->timeZone[0] == 'E' ? 13U : 12U)) {
        return false;
    }
    if (!ModemAT_isInitialized() ||
        (socketTransport ? !ModemAT_isRawMode() :
            (ModemAT_isRawMode() || !ModemAT_commandBoundaryReady()))) {
        return false;
    }
    state = {};
    state.config = *config;
    state.initialized = true;
    state.socketTransport = socketTransport;
    state.startupWindow = true;
    return socketTransport || ModemAT_setLineObserver(observeLine);
}
bool ModemTime_begin(const ModemTimeConfig *config) {
    return beginTime(config, false);
}
bool ModemTime_beginOnSocket(const ModemTimeConfig *config) {
    return beginTime(config, true);
}
bool ModemTime_prepareNetwork() {
    if (state.socketTransport) {
        return false;
    }
    if (!query("AT+CPIN?", "+CPIN:", state.config.queryTimeoutMs) ||
        strcmp(state.value, "READY")) {
        return false;
    }
    // End startup tolerance before applying any network settings.
    state.startupWindow = false;
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
        state.config.apn);
    if (!fits(written, sizeof(command)) ||
        !query(command, NULL, state.config.queryTimeoutMs)) {
        return false;
    }
    unsigned long start = millis();
    bool registered = false;
    while ((unsigned long)(millis() - start) < state.config.registerTimeoutMs) {
        if (!query("AT+CEREG?", "+CEREG:", state.config.queryTimeoutMs)) {
            return false;
        }
        unsigned int mode;
        unsigned int status;
        char extra;
        if (sscanf(state.value, "%u,%u%c", &mode, &status, &extra) != 2 || status == 3) {
            return false;
        }
        if (status == 1 || status == 5) {
            registered = true;
            break;
        }
        waitWithUrc(state.config.pollIntervalMs);
    }
    if (!registered || !query("AT+XIIC?", "+XIIC:", state.config.queryTimeoutMs)) {
        return false;
    }
    int link = ipState();
    if (link < 0) {
        ModemAT_printLine("[ERROR] Invalid XIIC state or IP response; PPP activation not guessed.");
        return false;
    }
    if (link == 0) {
        ModemAT_printLine("[STATE] No IP assigned; activating PPP once.");
        if (!query("AT+XIIC=1", NULL, state.config.linkTimeoutMs) || !waitIp(true)) {
            return false;
        }
    }
    char log[96];
    int printed = snprintf(log, sizeof(log), "[STATE] Data link connected, IP: %s",
        strchr(state.value, ',') + 1);
    if (!fits(printed, sizeof(log))) {
        return false;
    }
    ModemAT_printLine(log);
    return true;
}
bool ModemTime_readClock(ModemTimeClock *clock) {
    if (!query("AT+CCLK?", "+CCLK:", state.config.queryTimeoutMs)) {
        return false;
    }
    return ModemTime_parseClock(state.value, state.config.clockFieldsAreUtc, clock);
}
bool ModemTime_syncTime() {
    if (!state.initialized || state.blocked || state.syncAttempted) {
        return false;
    }
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+UPDATETIME=1,%s,%u,\"%s\",%u",
        state.config.server, state.config.serverTimeoutSeconds, state.config.timeZone,
        state.config.daylightSaving);
    if (!fits(written, sizeof(command))) {
        return false;
    }
    state.syncAttempted = true;
    state.syncing = true;
    ModemATResult result;
    if (state.socketTransport) {
        result = ModemSocket_executeControlWait(command, "+UPDATETIME:", state.value,
            sizeof(state.value), state.config.queryTimeoutMs, state.config.syncResultTimeoutMs) ?
            MODEM_AT_OK : MODEM_AT_BLOCKED;
    } else {
        result = ModemAT_executeWaitUrc(command, state.config.queryTimeoutMs,
            "+UPDATETIME:", state.value, sizeof(state.value), state.config.syncResultTimeoutMs);
    }
    state.syncing = false;
    ModemAT_reportResult(result);
    if (result == MODEM_AT_ERROR && !state.blocked) {
        ModemAT_printLine("[ERROR] Time update command rejected; no retry.");
        return false;
    }
    if (result != MODEM_AT_OK || state.blocked) {
        state.blocked = true;
        ModemAT_printLine("[ERROR] Time result unresolved; IP may remain active. No retry.");
        return false;
    }
    if (!strncmp(state.value, "Update To ", 10)) {
        ModemTimeClock updated = {};
        int consumed = 0;
        if (sscanf(state.value + 10, "%d-%d-%d,%d:%d:%d%n", &updated.year,
            &updated.month, &updated.day, &updated.hour, &updated.minute,
            &updated.second, &consumed) == 6 && consumed > 0 &&
            state.value[10 + consumed] == '\0' && validDate(updated)) {
            state.syncedClock = updated;
            state.syncedClockValid = true;
            ModemAT_printLine("[STATE] Time update notification confirmed.");
            return true;
        }
    } else if (!strcmp(state.value, "No PPP Link") || !strcmp(state.value, "Time Out") ||
        !strcmp(state.value, "Domain Name Invalid") || !strcmp(state.value, "Send Request Fail") ||
        !strcmp(state.value, "Socket Error") || !strcmp(state.value, "Time Data Is Null")) {
        ModemAT_printLine("[ERROR] Modem reported a synchronization failure; no retry.");
        return false;
    }
    state.blocked = true;
    ModemAT_printLine("[ERROR] Invalid or unknown time result; further commands blocked.");
    return false;
}


static unsigned long clockSeconds(const ModemTimeClock &clock) {
    unsigned long days = 0;
    for (int year = 2000; year < clock.year; ++year) {
        days += monthDays(year, 2) == 29 ? 366UL : 365UL;
    }
    for (int month = 1; month < clock.month; ++month) {
        days += (unsigned long)monthDays(clock.year, month);
    }
    days += (unsigned long)(clock.day - 1);
    return ((days * 24UL + (unsigned long)clock.hour) * 60UL +
        (unsigned long)clock.minute) * 60UL + (unsigned long)clock.second;
}

bool ModemTime_applySyncedClock() {
    if (!state.initialized || state.blocked || !state.syncedClockValid ||
        state.clockApplyAttempted || strcmp(state.config.timeZone, "E0") ||
        state.config.daylightSaving != 0) {
        ModemAT_printLine("[ERROR] CCLK apply requires one valid UTC E0 update, DST disabled.");
        return false;
    }
    state.clockApplyAttempted = true;
    const ModemTimeClock &clock = state.syncedClock;
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command),
        "AT+CCLK=\"%02d/%02d/%02d,%02d:%02d:%02d\"", clock.year - 2000,
        clock.month, clock.day, clock.hour, clock.minute, clock.second);
    ModemAT_printLine("[CLOCK APPLY] Writing the validated UTC Update To value to CCLK.");
    unsigned long start = millis();
    if (!fits(written, sizeof(command)) ||
        !query(command, NULL, state.config.queryTimeoutMs) ||
        !query("AT+CCLK?", "+CCLK:", state.config.queryTimeoutMs)) {
        return false;
    }
    ModemTimeClock actual = {};
    // Missing timezone is accepted only here, after this explicit E0 sync and write.
    // The generic RTC parser still requires a timezone instead of guessing one.
    const char *readback = state.value;
    char utcReadback[23];
    if (strlen(state.value) == 19 && state.value[0] == '\"' && state.value[18] == '\"') {
        int normalized = snprintf(utcReadback, sizeof(utcReadback), "%.18s+00\"", state.value);
        if (!fits(normalized, sizeof(utcReadback))) {
            return false;
        }
        readback = utcReadback;
        ModemAT_printLine("[CLOCK] CCLK omitted timezone; using the verified E0 update context.");
    }
    if (!ModemTime_parseClock(readback, false, &actual) || actual.offsetMinutes != 0) {
        ModemAT_printLine("[ERROR] CCLK readback is invalid or not UTC; TLS must not start.");
        return false;
    }
    unsigned long expectedSeconds = clockSeconds(clock);
    unsigned long actualSeconds = clockSeconds(actual);
    unsigned long toleranceSeconds = (millis() - start) / 1000UL + 2UL;
    if (actualSeconds < expectedSeconds || actualSeconds - expectedSeconds > toleranceSeconds) {
        ModemAT_printLine("[ERROR] CCLK readback differs from the applied update; TLS not started.");
        return false;
    }
    ModemAT_printLine("[CLOCK PASS] UTC CCLK write and readback confirmed.");
    return true;
}

bool ModemTime_releaseIp() {
    if (state.socketTransport) {
        return false;
    }
    if (!state.initialized || state.blocked || state.syncing) {
        return false;
    }
    state.releasing = true;
    if (!query("AT+XIIC?", "+XIIC:", state.config.queryTimeoutMs)) {
        return false;
    }
    int link = ipState();
    if (link < 0 || (link == 1 &&
        (!query("AT+XIIC=0", NULL, state.config.linkTimeoutMs) || !waitIp(false)))) {
        return false;
    }
    ModemAT_printLine("[STATE] IP released: XIIC state 0, IP 0.0.0.0.");
    return true;
}
