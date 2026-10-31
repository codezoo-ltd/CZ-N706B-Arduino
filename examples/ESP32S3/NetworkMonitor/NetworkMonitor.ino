#include <Arduino.h>
#include <ModemAT.h>
#include <ModemPower.h>

// ESP32S3 Dev Module; explicit UART2 pins for the CZ-N706B modem.
static const unsigned long PC_BAUD = 115200;
static const unsigned long MODEM_BAUD = 115200;
static const uint8_t ON_OFF = 2;
static const uint8_t LDO_EN = 1;
static const int8_t MODEM_RX_PIN = 37;
static const int8_t MODEM_TX_PIN = 38;

HardwareSerial MDMSerial(2); // Use ESP32-S3 UART2.

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
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;

static const unsigned long MONITOR_INTERVAL_MS = 5000;
static bool monitoring = false;
static unsigned long lastCycleMs = 0;
struct Sample {
    bool registrationValid, signalValid, linkValid;
    int registration, csq, ber, link;
    char ip[48];
};
static Sample previous = {};

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
static const char *registrationName(int n) {
    switch (n) {
        case 0:
            return "not registered";
        case 1:
            return "home";
        case 2:
            return "searching";
        case 3:
            return "denied";
        case 4:
            return "unknown";
        case 5:
            return "roaming";
        default:
            return "other";
    }
}
static bool query(const char *command, const char *prefix, char *value, size_t size) {
    value[0] = '\0';
    ModemATResult r = ModemAT_execute(command, prefix, QUERY_TIMEOUT_MS);
    ModemAT_reportResult(r);
    if (r == MODEM_AT_NO_RESPONSE || r == MODEM_AT_TIMEOUT || r == MODEM_AT_OVERFLOW ||
        r == MODEM_AT_BLOCKED) {
        monitoring = false;
        ModemAT_printLine(
            "[STATE] Monitor stopped: unresolved communication failure; inspect diagnostics.");
        return false;
    }
    return r == MODEM_AT_OK && ModemAT_copyResponseValue(value, size) == MODEM_AT_VALUE_OK;
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
    if (!query("AT+CPIN?", "+CPIN:", value, sizeof(value)) || strcmp(value, "READY")) {
        ModemAT_printLine("[ERROR] SIM is not READY; monitor not started.");
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
            snprintf(line, sizeof(line), "[STATE] Initial data link connected, IP: %s", link.ip);
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
static void snapshot() {
    Sample s = {};
    char value[192], line[240];
    s.registrationValid =
        query("AT+CEREG?", "+CEREG:", value, sizeof(value)) && number(value, 1, &s.registration);
    if (!monitoring) {
        return;
    }
    s.signalValid = query("AT+CSQ", "+CSQ:", value, sizeof(value)) && number(value, 0, &s.csq) &&
                    number(value, 1, &s.ber) && (s.csq <= 31 || s.csq == 99) &&
                    (s.ber <= 7 || s.ber == 99);
    if (!monitoring) {
        return;
    }
    s.linkValid = query("AT+XIIC?", "+XIIC:", value, sizeof(value)) && readLink(value, &s);
    if (!monitoring) {
        return;
    }
    // Invalid samples never replace the last confirmed value.
    if (s.registrationValid) {
        if (!previous.registrationValid) {
            snprintf(line, sizeof(line), "[INITIAL] Registration: %s (%d)",
                     registrationName(s.registration), s.registration);
            ModemAT_printLine(line);
        } else if (s.registration != previous.registration) {
            snprintf(line, sizeof(line), "[CHANGE] Registration: %s (%d) -> %s (%d)",
                     registrationName(previous.registration), previous.registration,
                     registrationName(s.registration), s.registration);
            ModemAT_printLine(line);
        }
        previous.registrationValid = true;
        previous.registration = s.registration;
    }
    if (s.signalValid) {
        if (!previous.signalValid || s.csq != previous.csq || s.ber != previous.ber) {
            snprintf(line, sizeof(line), "[%s] Signal: CSQ=%d, BER=%d%s",
                     previous.signalValid ? "CHANGE" : "INITIAL", s.csq, s.ber,
                     s.csq == 99 ? " (RSSI unavailable)" : "");
            ModemAT_printLine(line);
        }
        previous.signalValid = true;
        previous.csq = s.csq;
        previous.ber = s.ber;
    }
    if (s.linkValid) {
        if (!previous.linkValid || s.link != previous.link || strcmp(s.ip, previous.ip)) {
            snprintf(line, sizeof(line), "[%s] Link: %s, IP: %s",
                     previous.linkValid ? "CHANGE" : "INITIAL",
                     s.link ? "connected" : "disconnected", s.ip);
            ModemAT_printLine(line);
        }
        previous.linkValid = true;
        previous.link = s.link;
        memcpy(previous.ip, s.ip, sizeof(previous.ip));
    }
    char signal[48];
    if (s.signalValid) {
        snprintf(signal, sizeof(signal), "CSQ=%d, BER=%d", s.csq, s.ber);
    } else {
        snprintf(signal, sizeof(signal), "Unavailable");
    }
    snprintf(line, sizeof(line),
             "[STATUS] %lu ms | Registration: %s | Signal: %s | Link: %s | IP: %s", millis(),
             s.registrationValid ? registrationName(s.registration) : "Unavailable", signal,
             s.linkValid ? (s.link ? "connected" : "disconnected") : "Unavailable",
             s.linkValid ? s.ip : "Unavailable");
    ModemAT_printLine(line);
    lastCycleMs = millis();
}
void setup() {
    bool ready = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    ModemAT_begin(MDMSerial, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!ready || !ModemPower_powerOn()) {
        ModemAT_printLine("[ERROR] Modem readiness check failed; monitor not started.");
        return;
    }
    ModemAT_printLine("[INFO] NetworkMonitor / ESP32S3 Dev Module / 115200 baud.");
    ModemAT_printLine(
        "[INFO] Setup: SIM -> APN -> registration -> XIIC activation -> IP confirmation.");
    ModemAT_printLine(
        "[INFO] Query order: CEREG? -> CSQ -> XIIC?; wait 5000 ms after each completed cycle.");
    ModemAT_printLine(
        "[INFO] URCs are printed as raw diagnostics; state changes are detected by polling.");
    if (!prepareNetwork()) {
        monitoring = false;
        ModemAT_printLine("[ERROR] Initial network setup failed; monitor not started.");
        return;
    }
    ModemAT_printLine("[INFO] Monitoring started; no automatic reconnection after link loss.");
    monitoring = true;
    snapshot();
}
void loop() {
    ModemAT_pollIdle();
    if (monitoring && (unsigned long)(millis() - lastCycleMs) >= MONITOR_INTERVAL_MS) {
        snapshot();
    }
}