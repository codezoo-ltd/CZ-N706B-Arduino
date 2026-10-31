#include <Arduino.h>
#include <ModemAT.h>
#include <ModemPower.h>

// CZ-N706B board specification v0.7 p.3; CZ_Arduino_Shield schematic.
static const unsigned long PC_BAUD = 115200;
static const unsigned long MODEM_BAUD = 115200;
static const uint8_t ON_OFF = 2;
static const uint8_t LDO_EN = 8;
static const unsigned long SERIAL_WAIT_MS = 2000;
static const unsigned long LDO_START_DELAY_MS = 100;
static const unsigned long POWER_ON_HIGH_MS = 1100;
static const unsigned long POWER_OFF_HIGH_MS = 3100;
static const unsigned long POWER_KEY_RELEASE_DELAY_MS = 100;
static const unsigned long POWER_ON_RX_DRAIN_TIMEOUT_MS = 500;
static const unsigned long POWER_ON_RX_DRAIN_QUIET_MS = 20;
static const unsigned long BOOT_TIMEOUT_MS = 20000;
static const unsigned long AT_TIMEOUT_MS = 1500;
static const unsigned long QUIET_MS = 100;
static const unsigned long POWER_AT_RETRY_MS = 1000;
static const unsigned long POWER_OFF_CONFIRM_MS = 10000;
static const unsigned long POWER_OFF_SILENCE_MS = 3000;
static const unsigned int POWER_OFF_MIN_PROBES = 3;

static const ModemPowerConfig POWER_CONFIG = {
    ON_OFF,
    LDO_EN,
    LDO_START_DELAY_MS,
    POWER_ON_HIGH_MS,
    POWER_OFF_HIGH_MS,
    POWER_KEY_RELEASE_DELAY_MS,
    POWER_ON_RX_DRAIN_TIMEOUT_MS,
    POWER_ON_RX_DRAIN_QUIET_MS,
    AT_TIMEOUT_MS,
    BOOT_TIMEOUT_MS,
    POWER_AT_RETRY_MS,
    POWER_OFF_CONFIRM_MS,
    POWER_OFF_SILENCE_MS,
    POWER_OFF_MIN_PROBES
};

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Use the APN supplied for your activated CodeZoo Vodafone IoT SIM.
static const char APN[] = "simplio.apn";
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static bool transportFailed = false;

static void waitWithUrc(unsigned long duration) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < duration) {
        ModemAT_pollIdle();
        delay(1);
    }
}

// Split CSV while respecting quoted fields. Never return a truncated field.
static bool field(const char *text, unsigned int wanted, char *output, size_t capacity) {
    bool quoted = false;
    unsigned int index = 0;
    const char *start = text;
    for (const char *p = text; ; ++p) {
        if (*p == '"') {
            quoted = !quoted;
        }
        if ((*p == ',' && !quoted) || *p == '\0') {
            if (index == wanted) {
                const char *end = p;
                while (start < end && (*start == ' ' || *start == '\t')) {
                    ++start;
                }
                while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
                    --end;
                }
                if (end - start >= 2 && *start == '"' && end[-1] == '"') {
                    ++start;
                    --end;
                }
                size_t length = (size_t)(end - start);
                if (quoted || length == 0 || length >= capacity) {
                    return false;
                }
                memcpy(output, start, length);
                output[length] = '\0';
                return true;
            }
            ++index;
            start = p + 1;
        }
        if (*p == '\0') {
            return false;
        }
    }
}

static bool number(const char *text, unsigned int index, int *value) {
    char token[16];
    if (!field(text, index, token, sizeof(token))) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(token, &end, 10);
    if (end == token || *end != '\0' || parsed < 0 || parsed > 255) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool query(const char *command, const char *prefix, char *output,
    size_t capacity, unsigned long timeout = QUERY_TIMEOUT_MS) {
    if (output != NULL && capacity > 0) {
        output[0] = '\0';
    }
    if (transportFailed) {
        return false;
    }
    ModemATResult result = ModemAT_execute(command, prefix, timeout);
    ModemAT_reportResult(result);
    if (result == MODEM_AT_NO_RESPONSE || result == MODEM_AT_TIMEOUT ||
        result == MODEM_AT_OVERFLOW || result == MODEM_AT_BLOCKED) {
        transportFailed = true;
    }
    if (result != MODEM_AT_OK) {
        return false;
    }
    if (prefix == NULL) {
        return true;
    }
    return ModemAT_copyResponseValue(output, capacity) == MODEM_AT_VALUE_OK;
}

static void summary(const char *label, const char *value) {
    char line[240];
    snprintf(line, sizeof(line), "[%s] %s", label, value);
    ModemAT_printLine(line);
}

static bool registered(const char *value) {
    int status = 0;
    return number(value, 1, &status) && (status == 1 || status == 5);
}

static const char *registrationText(const char *value) {
    int status = 0;
    if (!number(value, 1, &status)) {
        return "Unavailable";
    }
    switch (status) {
        case 0: return "Not registered";
        case 1: return "Registered: home";
        case 2: return "Searching";
        case 3: return "Registration denied";
        case 4: return "Registration unknown";
        case 5: return "Registered: roaming";
        default: return "Other registration state; inspect raw response";
    }
}

static bool linkAddress(const char *value, char *ip, size_t capacity) {
    int state = 0;
    if (!number(value, 0, &state) || state != 1 || !field(value, 1, ip, capacity)) {
        return false;
    }
    unsigned int a, b, c, d;
    char extra;
    return sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 &&
        a <= 255 && b <= 255 && c <= 255 && d <= 255 &&
        (a != 0 || b != 0 || c != 0 || d != 0);
}

static bool waitForRegistration() {
    char value[192];
    unsigned long start = millis();
    while (true) {
        unsigned long elapsedAtStart = (unsigned long)(millis() - start);
        if (elapsedAtStart >= REGISTER_TIMEOUT_MS) {
            break;
        }
        unsigned long remaining = REGISTER_TIMEOUT_MS - elapsedAtStart;
        if (query("AT+CEREG?", "+CEREG:", value, sizeof(value),
                remaining < QUERY_TIMEOUT_MS ? remaining : QUERY_TIMEOUT_MS)) {
            summary("NETWORK", registrationText(value));
            if (registered(value)) {
                return true;
            }
            int status = 0;
            if (number(value, 1, &status) && status == 3) {
                return false;
            }
        }
        if (transportFailed) {
            return false;
        }
        unsigned long elapsed = (unsigned long)(millis() - start);
        if (elapsed >= REGISTER_TIMEOUT_MS) {
            break;
        }
        remaining = REGISTER_TIMEOUT_MS - elapsed;
        waitWithUrc(remaining < 2000 ? remaining : 2000);
    }
    return false;
}

// Link state is read fresh for every operation; no cached connection flag is used.
enum LinkState {
    LINK_UNKNOWN,
    LINK_DOWN,
    LINK_NO_IP,
    LINK_UP
};
// Explicit declaration keeps the custom return type visible to Arduino preprocessing.
static LinkState readLink(unsigned long timeout, char *ip, size_t capacity);
static bool powerReady = false;
static bool apnConfigured = false;

static LinkState readLink(unsigned long timeout, char *ip, size_t capacity) {
    char value[192];
    ip[0] = '\0';
    if (!query("AT+XIIC?", "+XIIC:", value, sizeof(value), timeout)) {
        return LINK_UNKNOWN;
    }
    int state = -1;
    if (!number(value, 0, &state)) {
        return LINK_UNKNOWN;
    }
    if (state == 1 && linkAddress(value, ip, capacity)) {
        return LINK_UP;
    }
    if (state == 0 && field(value, 1, ip, capacity) && strcmp(ip, "0.0.0.0") == 0) {
        return LINK_DOWN;
    }
    if (state == 1) {
        return LINK_NO_IP;
    }
    return LINK_UNKNOWN;
}

static bool confirmLink(bool connected) {
    unsigned long start = millis();
    while (true) {
        unsigned long elapsed = (unsigned long)(millis() - start);
        if (elapsed >= LINK_TIMEOUT_MS) {
            break;
        }
        unsigned long remaining = LINK_TIMEOUT_MS - elapsed;
        char ip[48];
        LinkState state = readLink(
            remaining < QUERY_TIMEOUT_MS ? remaining : QUERY_TIMEOUT_MS, ip, sizeof(ip));
        if (connected && state == LINK_UP) {
            summary("DATA", "Connected");
            summary("DATA IP", ip);
            return true;
        }
        if (!connected && state == LINK_DOWN) {
            summary("DATA", "Disconnected; IP released");
            return true;
        }
        if (transportFailed) {
            break;
        }
        elapsed = (unsigned long)(millis() - start);
        if (elapsed >= LINK_TIMEOUT_MS) {
            break;
        }
        remaining = LINK_TIMEOUT_MS - elapsed;
        waitWithUrc(remaining < 2000 ? remaining : 2000);
    }
    summary("ERROR", connected ? "Connection not confirmed" : "Disconnection not confirmed");
    return false;
}

static bool simReady() {
    char value[192];
    if (!query("AT+CPIN?", "+CPIN:", value, sizeof(value)) || strcmp(value, "READY") != 0) {
        summary("ERROR", "SIM is not READY; operation stopped");
        return false;
    }
    summary("SIM", "READY");
    return true;
}

static bool configureApn() {
    if (strlen(APN) == 0 || strlen(APN) > 100 || strpbrk(APN, "\"\r\n") != NULL) {
        summary("ERROR", "Invalid APN configuration");
        return false;
    }
    char command[144];
    snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    if (!query(command, NULL, NULL, 0)) {
        summary("ERROR", "APN configuration failed; operation stopped");
        return false;
    }
    apnConfigured = true;
    return true;
}

static bool connectNetwork() {
    if (!simReady()) {
        return false;
    }
    char ip[48];
    LinkState state = readLink(QUERY_TIMEOUT_MS, ip, sizeof(ip));
    if (state == LINK_UNKNOWN) {
        summary("ERROR", "Link status unavailable; activation not attempted");
        return false;
    }
    if (state == LINK_UP) {
        summary("DATA", "Already connected; activation skipped");
        summary("DATA IP", ip);
        return true;
    }
    if (!apnConfigured && !configureApn()) {
        return false;
    }
    if (!waitForRegistration()) {
        summary("ERROR", "Registration not confirmed; activation not attempted");
        return false;
    }
    if (!query("AT+XIIC=1", NULL, NULL, 0, LINK_TIMEOUT_MS)) {
        summary("ERROR", "Activation command failed");
        return false;
    }
    return confirmLink(true);
}

static bool disconnectNetwork() {
    char ip[48];
    LinkState state = readLink(QUERY_TIMEOUT_MS, ip, sizeof(ip));
    if (state == LINK_DOWN) {
        summary("DATA", "Already disconnected; deactivation skipped");
        return true;
    }
    if (state == LINK_UNKNOWN) {
        summary("ERROR", "Link status unavailable; deactivation not attempted");
        return false;
    }
    if (!query("AT+XIIC=0", NULL, NULL, 0, LINK_TIMEOUT_MS)) {
        summary("ERROR", "Deactivation command failed");
        return false;
    }
    return confirmLink(false);
}

static void showStatus() {
    char value[192], ip[48];
    bool ok = query("AT+CEREG?", "+CEREG:", value, sizeof(value));
    summary("NETWORK", ok ? registrationText(value) : "Unavailable");
    LinkState state = readLink(QUERY_TIMEOUT_MS, ip, sizeof(ip));
    if (state == LINK_UP) {
        summary("DATA", "Connected");
        summary("DATA IP", ip);
    } else if (state == LINK_DOWN) {
        summary("DATA", "Disconnected; IP released");
    } else if (state == LINK_NO_IP) {
        summary("DATA", "Link state 1 without a valid IPv4 address");
    } else {
        summary("DATA", "Unavailable");
    }
}

static void printHelp() {
    Serial.println("[INFO] NetworkConnect / UNO R4 / activated Vodafone IoT SIM");
    Serial.println("[INFO] Serial Monitor: 115200 baud; CR/LF ignored.");
    Serial.println("[INFO] c connect | d disconnect | r reconnect | s status | h help");
    Serial.println("[INFO] Enter one menu key and wait for Menu ready before the next key.");
}

// Drop keys entered during a blocking operation; never replay queued control actions.
static void finishOperation() {
    int queued = Serial.available();
    while (queued > 0) {
        Serial.read();
        --queued;
    }
    if (transportFailed) {
        summary("STATE", "Control blocked: inspect diagnostics and restart; no automatic recovery");
    } else {
        summary("STATE", "Menu ready");
    }
}

void setup() {
    bool initialized = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    Serial1.begin(MODEM_BAUD);
    ModemAT_begin(Serial1, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    printHelp();
    if (!initialized || !ModemPower_powerOn()) {
        summary("ERROR", "Power-on failed; network control disabled");
        return;
    }
    powerReady = true;
    if (simReady() && configureApn()) {
        connectNetwork();
    }
    finishOperation();
}

void loop() {
    ModemAT_pollIdle();
    if (Serial.available() <= 0) {
        return;
    }
    int key = Serial.read();
    if (key == '\r' || key == '\n') {
        return;
    }
    if (key == 'h') {
        printHelp();
        return;
    }
    if (!powerReady || transportFailed) {
        summary("STATE", "Control blocked: inspect diagnostics and restart");
        return;
    }
    if (key == 'c') {
        connectNetwork();
    } else if (key == 'd') {
        disconnectNetwork();
    } else if (key == 'r') {
        if (disconnectNetwork()) {
            connectNetwork();
        } else {
            summary("STATE", "Reconnect aborted: disconnection was not confirmed");
        }
    } else if (key == 's') {
        showStatus();
    } else {
        summary("INPUT", "Unknown menu key; enter h for help");
    }
    finishOperation();
}
