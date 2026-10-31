#include <Arduino.h>
#include <ModemAT.h>
#include <ModemPower.h>

// ESP32 Dev Module; explicit UART2 pins for the CZ-N706B modem.
static const unsigned long PC_BAUD = 115200;
static const unsigned long MODEM_BAUD = 115200;
static const uint8_t ON_OFF = 18;
static const uint8_t LDO_EN = 5;
static const int8_t MODEM_RX_PIN = 16;
static const int8_t MODEM_TX_PIN = 17;

HardwareSerial MDMSerial(2); // Use ESP32 UART2.

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

#include <ModemTime.h>
#include <stdio.h>

static const char APN[] = "simplio.apn";
static const char TIME_SERVER[] = "time.windows.com";
static const unsigned int SERVER_TIMEOUT_SECONDS = 10;
static const char TIME_ZONE[] = "E9";
static const unsigned int DAYLIGHT_SAVING = 0;
// Verified AT reference firmware returns UTC clock fields with a timezone suffix.
// Set false if your firmware returns local clock fields; never apply the offset twice.
static const bool CLOCK_FIELDS_ARE_UTC = true;
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static const unsigned long SYNC_RESULT_TIMEOUT_MS = 20000;
static const unsigned long POLL_INTERVAL_MS = 2000;

static const ModemTimeConfig TIME_CONFIG = {
    APN,
    TIME_SERVER,
    SERVER_TIMEOUT_SECONDS,
    TIME_ZONE,
    DAYLIGHT_SAVING,
    CLOCK_FIELDS_ARE_UTC,
    QUERY_TIMEOUT_MS,
    REGISTER_TIMEOUT_MS,
    LINK_TIMEOUT_MS,
    SYNC_RESULT_TIMEOUT_MS,
    POLL_INTERVAL_MS
};

static bool prepareNetwork() {
    return ModemTime_prepareNetwork();
}

static bool readClock(const char *label) {
    ModemTimeClock clock = {};
    ModemTime_printLine(label);
    if (!ModemTime_readClock(&clock)) {
        ModemTime_printLine(
            "[WARNING] RTC response is missing or invalid; inspect the raw response.");
        return false;
    }
    char log[128];
    int written = snprintf(log, sizeof(log),
        "[CLOCK LOCAL] %04d-%02d-%02d %02d:%02d:%02d; UTC offset %+d minutes.",
        clock.year, clock.month, clock.day, clock.hour, clock.minute, clock.second,
        clock.offsetMinutes);
    if (written < 0 || (size_t)written >= sizeof(log)) {
        return false;
    }
    ModemTime_printLine(log);
    return true;
}

static bool syncTime() {
    return ModemTime_syncTime();
}

static bool releaseIp() {
    return ModemTime_releaseIp();
}

static void runTimeTest() {
    ModemTime_printLine("[STEP 1] Prepare SIM, APN, registration and IP.");
    if (!prepareNetwork()) {
        ModemTime_printLine("[FAIL] Network preparation failed; inspect link state. No retry.");
        return;
    }
    ModemTime_printLine("[STEP 2] Read RTC before synchronization.");
    // An invalid initial date is diagnostic only. A blocked UART still prevents sync.
    readClock("[CLOCK BEFORE]");
    ModemTime_printLine("[STEP 3] Request one time update and wait for its terminal notification.");
    bool synced = syncTime();
    bool clockRead = false;
    if (synced) {
        ModemTime_printLine("[STEP 4] Read RTC after synchronization.");
        clockRead = readClock("[CLOCK AFTER]");
    }
    ModemTime_printLine("[STEP 5] Release IP only when the response boundary is known.");
    bool released = releaseIp();
    if (synced && clockRead && released) {
        ModemTime_printLine("[PASS] Time update, RTC read and IP release confirmed.");
    } else {
        ModemTime_printLine("[FAIL] Time test incomplete; inspect time result and link state.");
    }
}

void setup() {
    bool powerReady = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    ModemAT_begin(MDMSerial, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!powerReady || !ModemPower_powerOn()) {
        ModemAT_printLine("[FAIL] Modem readiness check failed; test not started.");
        return;
    }
    ModemTime_printLine("[INFO] TimeSync / ESP32 Dev Module / 115200 baud.");
    ModemTime_printLine("[WARNING] SIM data is consumed. Synchronization runs once only.");
    ModemTime_printLine(CLOCK_FIELDS_ARE_UTC ?
        "[CLOCK MODE] UTC fields: apply the returned offset once for local display." :
        "[CLOCK MODE] Local fields: keep the returned clock fields without shifting.");
    if (!ModemTime_begin(&TIME_CONFIG)) {
        ModemTime_printLine("[FAIL] Shared AT readiness or time configuration failed.");
        return;
    }
    runTimeTest();
    ModemTime_printLine("[DONE] One run only. No retry or automatic repeat; reset to run again.");
}

void loop() {
    // Passive UART handling only; no repeated time update or data activation.
    ModemTime_pollIdle();
}
