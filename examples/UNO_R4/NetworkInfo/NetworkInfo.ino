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
static const unsigned long MONITOR_INTERVAL_MS = 10000;
static bool monitoring = false;
static bool transportFailed = false;
static unsigned long lastSnapshotMs = 0;

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
    while ((unsigned long)(millis() - start) < REGISTER_TIMEOUT_MS) {
        unsigned long remaining = REGISTER_TIMEOUT_MS - (unsigned long)(millis() - start);
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

static bool waitForLink() {
    char value[192], ip[48];
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < LINK_TIMEOUT_MS) {
        unsigned long remaining = LINK_TIMEOUT_MS - (unsigned long)(millis() - start);
        if (query("AT+XIIC?", "+XIIC:", value, sizeof(value),
                remaining < QUERY_TIMEOUT_MS ? remaining : QUERY_TIMEOUT_MS) &&
            linkAddress(value, ip, sizeof(ip))) {
            summary("DATA IP", ip);
            return true;
        }
        if (transportFailed) {
            return false;
        }
        unsigned long elapsed = (unsigned long)(millis() - start);
        if (elapsed >= LINK_TIMEOUT_MS) {
            break;
        }
        remaining = LINK_TIMEOUT_MS - elapsed;
        waitWithUrc(remaining < 2000 ? remaining : 2000);
    }
    return false;
}

static void snapshot() {
    char value[192], token[64], line[160];
    summary("STATE", "Network snapshot started");
    bool ok = query("AT+CPIN?", "+CPIN:", value, sizeof(value));
    summary("SIM", ok ? value : "Unavailable");
    ok = query("AT+CEREG?", "+CEREG:", value, sizeof(value));
    summary("NETWORK", ok ? registrationText(value) : "Unavailable");
    ok = query("AT+COPS?", "+COPS:", value, sizeof(value));
    summary("OPERATOR", ok && field(value, 2, token, sizeof(token)) ? token : "Unavailable");
    ok = query("AT+CSQ", "+CSQ:", value, sizeof(value));
    int rssi = 99, ber = 99;
    if (ok && number(value, 0, &rssi) && number(value, 1, &ber)) {
        if (rssi >= 1 && rssi <= 30) {
            snprintf(line, sizeof(line), "CSQ=%d, RSSI=%d dBm, BER code=%d", rssi, -113 + 2 * rssi, ber);
        } else if (rssi == 0 || rssi == 31) {
            snprintf(line, sizeof(line), "CSQ=%d, RSSI%s dBm, BER code=%d", rssi,
                rssi == 0 ? "<=-113" : ">=-51", ber);
        } else {
            snprintf(line, sizeof(line), "CSQ=%d, RSSI unavailable, BER code=%d", rssi, ber);
        }
        summary("SIGNAL", line);
        if (ber == 99) {
            summary("BER", "Unknown or not detectable (99)");
        }
    } else {
        summary("SIGNAL", "Unavailable");
    }
    ok = query("AT+NETMSG", "+NETMSG:", value, sizeof(value));
    summary("RADIO BAND", ok && field(value, 4, token, sizeof(token)) ? token : "Unavailable");
    ok = query("AT+NETDMSG", "+NETDMSG:", value, sizeof(value));
    summary("RADIO DETAIL RAW", ok ? value : "Unavailable");
    ok = query("AT+XIIC?", "+XIIC:", value, sizeof(value));
    if (ok && linkAddress(value, token, sizeof(token))) {
        summary("DATA", "Connected");
        summary("DATA IP", token);
    } else {
        int state = -1;
        // Reconnect only after a successful query with a recognized link state.
        // A failed query is not evidence that the modem has lost its IP address.
        if (ok && number(value, 0, &state) && (state == 0 || state == 1)) {
            summary("DATA", "No active link or valid IP; requesting reconnection");
            bool activated = query("AT+XIIC=1", NULL, NULL, 0, LINK_TIMEOUT_MS);
            if (activated && waitForLink()) {
                summary("DATA", "Reconnected");
            } else {
                summary("DATA", "Reconnection not confirmed");
                if (!transportFailed) {
                    summary("STATE", "Will check the link again in the next snapshot");
                }
            }
        } else {
            summary("DATA", "Unavailable; reconnection not attempted");
        }
    }
    if (transportFailed) {
        monitoring = false;
        summary("STATE", "Monitor stopped: unresolved communication failure; inspect diagnostics");
    } else {
        summary("STATE", "Snapshot completed; next snapshot after 10000 ms");
    }
    lastSnapshotMs = millis();
}

void setup() {
    bool powerInitialized = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    Serial1.begin(MODEM_BAUD);
    ModemAT_begin(Serial1, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!powerInitialized || !ModemPower_powerOn()) {
        summary("ERROR", "Power-on failed; test stopped");
        return;
    }
    if (strcmp(APN, "YOUR_APN") == 0 || strlen(APN) == 0 || strlen(APN) > 100 ||
        strpbrk(APN, "\"\r\n") != NULL) {
        summary("ERROR", "Set APN for your activated Vodafone IoT SIM; test stopped");
        return;
    }
    char value[192], command[144];
    if (!query("AT+CPIN?", "+CPIN:", value, sizeof(value)) || strcmp(value, "READY") != 0) {
        summary("ERROR", "SIM is not READY; test stopped");
        return;
    }
    snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    if (!query(command, NULL, NULL, 0)) {
        summary("ERROR", "APN configuration failed; test stopped");
        return;
    }
    if (!waitForRegistration()) {
        summary("ERROR", "Registration denied, unavailable or timed out; test stopped");
        return;
    }
    if (!query("AT+XIIC=1", NULL, NULL, 0, LINK_TIMEOUT_MS) || !waitForLink()) {
        summary("ERROR", "Data link activation or IP confirmation failed; test stopped");
        return;
    }
    summary("STATE", "NetworkInfo ready / activated Vodafone IoT SIM");
    monitoring = true;
    snapshot();
}

void loop() {
    ModemAT_pollIdle();
    if (monitoring && (unsigned long)(millis() - lastSnapshotMs) >= MONITOR_INTERVAL_MS) {
        snapshot();
    }
}
