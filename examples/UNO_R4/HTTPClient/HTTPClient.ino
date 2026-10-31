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

#include <ModemHttp.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Activated Vodafone IoT SIM; GET then POST run once after each reset.
static const char APN[] = "simplio.apn";
static const char SERVER_HOST[] = "postman-echo.com";
static const uint16_t SERVER_PORT = 80;
static const char GET_PATH[] = "/get?test=123";
static const char POST_PATH[] = "/post";
static const char POST_BODY[] = "{\"test\":\"123\"}";
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static const unsigned long SETUP_TIMEOUT_MS = 60000;
static const unsigned long RECEIVE_TIMEOUT_MS = 60000;
static const unsigned long POLL_INTERVAL_MS = 2000;
static const unsigned long CLOSE_TIMEOUT_MS = 5000;

static const ModemHttpConfig HTTP_CONFIG = {
    APN,
    SERVER_HOST,
    SERVER_PORT,
    QUERY_TIMEOUT_MS,
    REGISTER_TIMEOUT_MS,
    LINK_TIMEOUT_MS,
    SETUP_TIMEOUT_MS,
    RECEIVE_TIMEOUT_MS,
    POLL_INTERVAL_MS,
    CLOSE_TIMEOUT_MS
};

static bool runRequests() {
    ModemAT_printLine("[STEP 1] HTTP GET");
    ModemAT_printLine(GET_PATH);
    if (!ModemHttp_get(GET_PATH)) {
        ModemAT_printLine("[FAIL] GET did not complete; POST skipped.");
        return false;
    }
    ModemHttp_printResponse();
    if (ModemHttp_status() != 200) {
        ModemAT_printLine("[FAIL] GET HTTP status is not 200; POST skipped.");
        return false;
    }
    ModemAT_printLine("[GET PASS] Complete HTTP 200 response received.");
    if (!ModemHttp_close()) {
        ModemAT_printLine("[FAIL] GET connection closure unconfirmed; POST skipped.");
        return false;
    }

    ModemAT_printLine("[STEP 2] HTTP POST");
    ModemAT_printLine(POST_PATH);
    ModemAT_printLine(POST_BODY);
    char log[80];
    snprintf(log, sizeof(log), "[POST BYTES] %lu; exact bytes, no extra CR/LF.",
        (unsigned long)strlen(POST_BODY));
    ModemAT_printLine(log);
    if (!ModemHttp_post(POST_PATH, POST_BODY)) {
        ModemAT_printLine("[FAIL] POST did not complete; no automatic replay.");
        return false;
    }
    ModemHttp_printResponse();
    if (ModemHttp_status() != 200) {
        ModemAT_printLine("[FAIL] POST HTTP status is not 200.");
        return false;
    }
    ModemAT_printLine("[POST PASS] Complete HTTP 200 response received.");
    return true;
}

void setup() {
    bool powerReady = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    Serial1.begin(MODEM_BAUD);
    ModemAT_begin(Serial1, Serial, QUIET_MS);
    unsigned long startTime = millis();
    while (!Serial && (unsigned long)(millis() - startTime) < SERIAL_WAIT_MS) {
        delay(1);
    }
    ModemAT_printLine("[INFO] HTTPClient / UNO R4 / activated Vodafone IoT SIM.");
    ModemAT_printLine("[INFO] Serial Monitor: 115200 baud; no command input required.");
    ModemAT_printLine("[INFO] GET then POST once; SIM data is consumed.");
    if (!powerReady || !ModemPower_powerOn() || !ModemHttp_begin(&HTTP_CONFIG, Serial1)) {
        ModemAT_printLine("[FAIL] Modem or HTTP initialization failed.");
        return;
    }
    ModemAT_printLine("[STATE] Power and AT ready; preparing the network as in TCPClient.");
    bool networkReady = ModemHttp_prepareNetwork();
    bool passed = false;
    if (networkReady) {
        passed = runRequests();
    } else {
        ModemAT_printLine("[FAIL] Network preparation failed; HTTP GET/POST not started.");
    }
    // Cleanup is allowed only while the UART boundary remains known.
    bool closed = ModemHttp_close();
    bool released = closed && ModemHttp_releaseIp();
    if (passed && closed && released) {
        ModemAT_printLine("[DONE] GET and POST completed; HTTP closed and IP released.");
    } else {
        ModemAT_printLine("[DONE] Test stopped; inspect HTTP/IP state before restarting.");
    }
}

void loop() {
    // No repeated requests or automatic reconnection.
    ModemHttp_pollIdle();
}
