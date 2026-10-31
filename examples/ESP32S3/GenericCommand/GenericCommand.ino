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

// Execute one query at a time, then wait one second before the next query.
static const unsigned long COMMAND_TIMEOUT_MS = 5000;
static const unsigned long COMMAND_INTERVAL_MS = 1000;

struct QueryCommand {
    const char *command;
    const char *expectedPrefix;
};

static const QueryCommand QUERIES[] = {
    {"AT", NULL},
    {"AT+CGMM", "+CGMM:"},
    {"AT+IVSN", "+IVSN:"},
    {"AT+CPIN?", "+CPIN:"},
    {"AT+CSQ", "+CSQ:"}
};
static const size_t QUERY_COUNT = sizeof(QUERIES) / sizeof(QUERIES[0]);
static size_t queryIndex = 0;
static unsigned long lastCommandFinishedMs = 0;
static bool running = false;

void setup() {
    bool powerInitialized = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    ModemAT_begin(MDMSerial, Serial, QUIET_MS);

    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!powerInitialized) {
        Serial.println("[ERROR] Power configuration is invalid.");
        return;
    }
    if (!ModemPower_powerOn()) {
        Serial.println("[ERROR] Modem readiness check failed. Automatic queries disabled.");
        return;
    }

    Serial.println("[STATE] GenericCommand automatic queries started / ESP32S3 Dev Module.");
    Serial.println("[INFO] Serial Monitor: 115200 baud; no command input required.");
    Serial.println("[INFO] Sequence: AT -> CGMM -> IVSN -> CPIN? -> CSQ -> repeat.");
    Serial.println("[INFO] Wait 1000 ms after each result; command timeout 5000 ms.");
    Serial.println("[INFO] Communication failures stop the sequence; no automatic retry.");
    lastCommandFinishedMs = millis();
    running = true;
}

void loop() {
    ModemAT_pollIdle();
    if (!running) {
        return;
    }
    if ((unsigned long)(millis() - lastCommandFinishedMs) < COMMAND_INTERVAL_MS) {
        return;
    }

    const QueryCommand &query = QUERIES[queryIndex];
    ModemATResult result = ModemAT_execute(
        query.command, query.expectedPrefix, COMMAND_TIMEOUT_MS);
    ModemAT_reportResult(result);

    // A complete error or missing-data result still has a known final boundary.
    // Never send another query after a timeout or an unresolved receive boundary.
    if (result != MODEM_AT_OK && result != MODEM_AT_ERROR &&
        result != MODEM_AT_MISSING_DATA) {
        running = false;
        Serial.println("[STATE] Automatic queries stopped. Check diagnostics before restarting.");
        return;
    }

    queryIndex = (queryIndex + 1) % QUERY_COUNT;
    if (queryIndex == 0) {
        Serial.println("[STATE] Query cycle completed; repeating after 1000 ms.");
    }
    lastCommandFinishedMs = millis();
}
