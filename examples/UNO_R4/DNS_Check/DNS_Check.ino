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

static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const char APN[] = "simplio.apn";
static const char DNS_HOST[] = "www.baidu.com";
static const unsigned long DNS_TIMEOUT_MS = 60000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;

struct Sample {
    int link;
    char ip[48];
};
// Copy one CSV field without truncation; ignore commas inside quoted fields.
static bool field(const char *text, unsigned int wanted, char *out, size_t capacity) {
    bool quoted = false;
    unsigned int index = 0;
    const char *start = text;
    for (const char *p = text;; ++p) {
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
                size_t length = (size_t)(end - start);
                if (quoted || !length || length >= capacity) {
                    return false;
                }
                memcpy(out, start, length);
                out[length] = '\0';
                return true;
            }
            ++index;
            start = p + 1;
        }
        if (!*p) {
            return false;
        }
    }
}
static bool number(const char *text, unsigned int index, int *out) {
    char token[16];
    if (!field(text, index, token, sizeof(token))) {
        return false;
    }
    char *end;
    long n = strtol(token, &end, 10);
    if (end == token || *end || n < 0 || n > 255) {
        return false;
    }
    *out = (int)n;
    return true;
}
static bool query(const char *command, const char *prefix, char *value, size_t size) {
    ModemATResult result = ModemAT_execute(command, prefix, QUERY_TIMEOUT_MS);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK && ModemAT_copyResponseValue(value, size) == MODEM_AT_VALUE_OK;
}

static bool readLink(const char *value, Sample *s) {
    if (!number(value, 0, &s->link) || (s->link != 0 && s->link != 1)) {
        return false;
    }
    if (!field(value, 1, s->ip, sizeof(s->ip))) {
        return false;
    }
    unsigned int a, b, c, d;
    char extra;
    if (sscanf(s->ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255) {
        return false;
    }
    if (s->link == 1 && a == 0 && b == 0 && c == 0 && d == 0) {
        return false;
    }
    return true;
}
// Startup commands are separate from passive monitoring.
static bool startupCommand(const char *command, unsigned long timeout) {
    ModemATResult r = ModemAT_execute(command, NULL, timeout);
    ModemAT_reportResult(r);
    return r == MODEM_AT_OK;
}
static void waitWithUrc(unsigned long duration) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < duration) {
        ModemAT_pollIdle();
        delay(1);
    }
}
static bool prepareNetwork() {
    char value[192], command[144];
    if (!strlen(APN) || strlen(APN) > 100 || !strcmp(APN, "YOUR_APN") || strpbrk(APN, "\"\r\n")) {
        ModemAT_printLine("[ERROR] Configure a valid APN before starting.");
        return false;
    }
    snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    if (!startupCommand(command, QUERY_TIMEOUT_MS)) {
        return false;
    }
    ModemAT_printLine("[STATE] Waiting for home or roaming registration.");
    unsigned long start = millis();
    bool registered = false;
    while ((unsigned long)(millis() - start) < REGISTER_TIMEOUT_MS) {
        unsigned long remaining = REGISTER_TIMEOUT_MS - (unsigned long)(millis() - start);
        ModemATResult r = ModemAT_execute(
            "AT+CEREG?", "+CEREG:", remaining < QUERY_TIMEOUT_MS ? remaining : QUERY_TIMEOUT_MS);
        ModemAT_reportResult(r);
        int status;
        if (r != MODEM_AT_OK) {
            return false;
        }
        if (ModemAT_copyResponseValue(value, sizeof(value)) != MODEM_AT_VALUE_OK ||
            !number(value, 1, &status)) {
            return false;
        }
        if (status == 1 || status == 5) {
            registered = true;
            break;
        }
        if (status == 3) {
            ModemAT_printLine("[ERROR] Network registration denied.");
            return false;
        }
        unsigned long elapsed = (unsigned long)(millis() - start);
        if (elapsed >= REGISTER_TIMEOUT_MS) {
            break;
        }
        remaining = REGISTER_TIMEOUT_MS - elapsed;
        waitWithUrc(remaining < 2000 ? remaining : 2000);
    }
    if (!registered) {
        ModemAT_printLine("[ERROR] Network registration timed out.");
        return false;
    }
    // Activate once during setup. Never reconnect from snapshot().
    if (!startupCommand("AT+XIIC=1", LINK_TIMEOUT_MS)) {
        return false;
    }
    start = millis();
    while ((unsigned long)(millis() - start) < LINK_TIMEOUT_MS) {
        unsigned long remaining = LINK_TIMEOUT_MS - (unsigned long)(millis() - start);
        ModemATResult r = ModemAT_execute(
            "AT+XIIC?", "+XIIC:", remaining < QUERY_TIMEOUT_MS ? remaining : QUERY_TIMEOUT_MS);
        ModemAT_reportResult(r);
        Sample link = {};
        if (r != MODEM_AT_OK) {
            return false;
        }
        if (ModemAT_copyResponseValue(value, sizeof(value)) != MODEM_AT_VALUE_OK ||
            !readLink(value, &link)) {
            return false;
        }
        if (link.link == 1) {
            char line[96];
            snprintf(line, sizeof(line), "[STATE] Data link connected, IP: %s", link.ip);
            ModemAT_printLine(line);
            return true;
        }
        unsigned long elapsed = (unsigned long)(millis() - start);
        if (elapsed >= LINK_TIMEOUT_MS) {
            break;
        }
        remaining = LINK_TIMEOUT_MS - elapsed;
        waitWithUrc(remaining < 2000 ? remaining : 2000);
    }
    ModemAT_printLine("[ERROR] Initial IP confirmation timed out.");
    return false;
}

static bool runCheck() {
    if (strlen(DNS_HOST) == 0 || strlen(DNS_HOST) > 100 ||
        strpbrk(DNS_HOST, "\"\r\n \t") != NULL) {
        ModemAT_printLine("[ERROR] Configure a valid DNS host, up to 100 bytes.");
        return false;
    }
    ModemAT_printLine("[STEP 1-4] APN -> registration -> link activation -> IP confirmation.");
    if (!prepareNetwork()) {
        return false;
    }
    char value[192];
    int attached;
    ModemAT_printLine("[STEP 5] Checking packet attach state.");
    if (!query("AT+CGATT?", "+CGATT:", value, sizeof(value)) || !number(value, 0, &attached) ||
        attached != 1) {
        ModemAT_printLine("[ERROR] Packet attach is not confirmed.");
        return false;
    }
    ModemAT_printLine("[STEP 6] Running one DNS query; no automatic retry.");
    // Match IPv4 specifically so an empty IPv6 response cannot overwrite it.
    char command[128];
    int lengthWritten = snprintf(command, sizeof(command), "AT+NWDNS=\"%s\"", DNS_HOST);
    if (lengthWritten < 0 || (size_t)lengthWritten >= sizeof(command)) {
        ModemAT_printLine("[ERROR] DNS command exceeds the buffer capacity.");
        return false;
    }
    ModemATResult result = ModemAT_execute(command, "+NWDNS: IPV4,", DNS_TIMEOUT_MS);
    ModemAT_reportResult(result);
    if (result != MODEM_AT_OK ||
        ModemAT_copyResponseValue(value, sizeof(value)) != MODEM_AT_VALUE_OK) {
        return false;
    }
    size_t length = strlen(value);
    if (length < 3 || value[0] != '"' || value[length - 1] != '"') {
        return false;
    }
    value[length - 1] = '\0';
    Sample resolved = {};
    char linkValue[80];
    snprintf(linkValue, sizeof(linkValue), "1,%s", value + 1);
    if (!readLink(linkValue, &resolved)) {
        return false;
    }
    ModemAT_printLine("[PASS] IPv4 DNS resolution completed; server connectivity was not tested.");
    return true;
}
void setup() {
    bool ready = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    Serial1.begin(MODEM_BAUD);
    ModemAT_begin(Serial1, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!ready || !ModemPower_powerOn()) {
        ModemAT_printLine("[ERROR] Modem readiness check failed.");
        return;
    }
    ModemAT_printLine("[INFO] DNS_Check / UNO R4 / 115200 baud.");
    bool passed = runCheck();
    ModemAT_printLine(passed ? "[DONE] Check completed once. Data link remains active."
                             : "[FAIL] Check stopped. Inspect the last command and response.");
    ModemAT_printLine("[INFO] No automatic repeat or reconnection. Reset to run again.");
}
void loop() {
    // Consume diagnostic URCs only; never issue periodic test commands.
    ModemAT_pollIdle();
}
