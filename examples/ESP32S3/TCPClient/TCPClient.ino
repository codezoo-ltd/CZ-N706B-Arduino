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

#include <ModemSocket.h>
#include <stdio.h>
#include <string.h>

// Each test sends once, reads modem-buffered data after a URC, and closes the socket.
// Increasing testCount consumes more SIM data; no automatic retries are performed.
static unsigned int testCount = 1;
static const char APN[] = "simplio.apn";
static const char SERVER_HOST[] = "modules.telit.com";
static const uint16_t SERVER_PORT = 10510;
static const uint8_t SOCKET_ID = 0;
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static const unsigned long SOCKET_TIMEOUT_MS = 60000;
static const unsigned long RECEIVE_TIMEOUT_MS = 30000;
static const unsigned long POLL_INTERVAL_MS = 2000;
static const unsigned long TEST_INTERVAL_MS = 5000;
static const size_t READ_CHUNK_BYTES = 1024;
static const size_t RECEIVE_BUFFER_BYTES = 1024;
static const size_t SEND_BUFFER_BYTES = 513;
static char sendData[SEND_BUFFER_BYTES] = "Hello";

static const ModemSocketTcpConfig SOCKET_CONFIG = {
    APN,
    SERVER_HOST,
    SERVER_PORT,
    SOCKET_ID,
    QUERY_TIMEOUT_MS,
    REGISTER_TIMEOUT_MS,
    LINK_TIMEOUT_MS,
    SOCKET_TIMEOUT_MS,
    RECEIVE_TIMEOUT_MS,
    POLL_INTERVAL_MS,
    READ_CHUNK_BYTES
};

static bool runTcpTest(unsigned int number) {
    char log[96];
    snprintf(log, sizeof(log), "[TEST] %u/%u", number, testCount);
    ModemAT_printLine(log);
    if (!ModemSocket_prepareNetwork()) {
        ModemAT_printLine("[FAIL] SIM, registration or IP readiness was not confirmed.");
        return false;
    }
    if (!ModemSocket_open()) {
        ModemAT_printLine("[FAIL] Socket Open failed; no send was attempted.");
        return false;
    }
    ModemAT_printLine("[STATE] Socket Open confirmed.");
    bool sent = ModemSocket_send(sendData);
    static uint8_t received[RECEIVE_BUFFER_BYTES];
    size_t receivedSize = 0;
    bool receivedOk = sent && ModemSocket_recv(received, sizeof(received), &receivedSize);
    if (receivedOk) {
        snprintf(log, sizeof(log), "[RX DATA] %lu bytes:", (unsigned long)receivedSize);
        ModemAT_printLine(log);
        Serial.write(received, receivedSize);
        Serial.println();
    }
    // Close is separate from receive success. Never release IP after a failed Close.
    bool closed = ModemSocket_close();
    bool released = closed && ModemSocket_releaseIp();
    if (!sent || !receivedOk || !closed || !released) {
        ModemAT_printLine("[FAIL] Send/Recv/Close/IP release not completed; no retry.");
        return false;
    }
    ModemAT_printLine("[PASS] Send/receive completed; socket closed and IP released.");
    return true;
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
    ModemAT_printLine("[INFO] TCPClient / ESP32S3 Dev Module / 115200 baud.");
    ModemAT_printLine("[WARNING] SIM data is consumed. testCount defaults to one.");
    if (testCount == 0) {
        ModemAT_printLine("[DONE] testCount is zero; test disabled.");
        return;
    }
    if (!powerReady || !ModemPower_powerOn() || !ModemSocket_begin(&SOCKET_CONFIG, MDMSerial)) {
        ModemAT_printLine("[FAIL] Modem or TCP API initialization failed.");
        return;
    }
    for (unsigned int index = 0; index < testCount; ++index) {
        if (!runTcpTest(index + 1)) {
            ModemAT_printLine("[DONE] Stopped; inspect socket/IP state before restarting.");
            return;
        }
        if (index + 1 < testCount) {
            ModemSocket_wait(TEST_INTERVAL_MS);
        }
    }
    ModemAT_printLine("[DONE] Requested tests completed. No automatic restart.");
}

void loop() {
    // No network commands or automatic test restarts are issued here.
    ModemSocket_pollIdle();
}
