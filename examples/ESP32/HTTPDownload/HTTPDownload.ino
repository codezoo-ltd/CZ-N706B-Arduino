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

#include <ModemHttp.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Activated Vodafone IoT SIM; one file download and full readback per reset.
static const char APN[] = "simplio.apn";
static const char SERVER_HOST[] = "dl.testfile.cc";
static const uint16_t SERVER_PORT = 80;
static const char DOWNLOAD_PATH[] = "/10kb.dat";
static const char TARGET_FILE[] = "http_10k.bin";
static const bool REMOVE_EXISTING_TARGET = true;
static const unsigned long EXPECTED_BYTES = 10240;
static const unsigned long DOWNLOAD_TIMEOUT_MS = 120000;
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

void setup() {
    bool powerReady = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    ModemAT_begin(MDMSerial, Serial, QUIET_MS);
    unsigned long startTime = millis();
    while (!Serial && (unsigned long)(millis() - startTime) < SERIAL_WAIT_MS) {
        delay(1);
    }
    ModemAT_printLine("[INFO] HTTPDownload / ESP32 Dev Module / activated Vodafone IoT SIM.");
    ModemAT_printLine("[INFO] Serial Monitor: 115200 baud; no command input required.");
    ModemAT_printLine("[INFO] One download; no speed measurement or automatic retry.");
    ModemAT_printLine(REMOVE_EXISTING_TARGET ?
        "[INFO] Existing target removal enabled; other files stay." :
        "[INFO] Existing target removal disabled.");
    ModemAT_printLine("[TARGET]");
    ModemAT_printLine(TARGET_FILE);
    ModemAT_printLine("[SOURCE]");
    ModemAT_printLine(SERVER_HOST);
    ModemAT_printLine(DOWNLOAD_PATH);
    if (!powerReady || !ModemPower_powerOn() || !ModemHttp_begin(&HTTP_CONFIG, MDMSerial)) {
        ModemAT_printLine("[FAIL] Modem or HTTP initialization failed.");
        return;
    }
    bool ready = ModemHttp_prepareNetwork();
    bool verified = false;
    if (ready) {
        verified = ModemHttp_download(DOWNLOAD_PATH, TARGET_FILE, EXPECTED_BYTES,
            NULL, DOWNLOAD_TIMEOUT_MS, REMOVE_EXISTING_TARGET);
    } else {
        ModemAT_printLine("[FAIL] Network preparation failed; download not started.");
    }
    bool closed = ModemHttp_close();
    bool released = closed && ModemHttp_releaseIp();
    if (verified && closed && released) {
        ModemAT_printLine("[DONE] Download verified; IP released; file remains on the modem.");
    } else {
        ModemAT_printLine("[DONE] Stopped; inspect file/HTTP/IP state before restarting.");
    }
}

void loop() {
    // Keep the downloaded file. No repeated downloads or automatic deletion.
    ModemHttp_pollIdle();
}
