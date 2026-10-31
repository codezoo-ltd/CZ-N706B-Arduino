#include <Arduino.h>
#include <ModemAT.h>
#include <ModemPower.h>

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
static const size_t BRIDGE_BUFFER_SIZE = 256;

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

static uint8_t modemToPcBuffer[BRIDGE_BUFFER_SIZE];
static uint8_t pcToModemBuffer[BRIDGE_BUFFER_SIZE];
static size_t modemToPcLength = 0;
static size_t pcToModemLength = 0;
static bool bridgeActive = false;

static void forwardPending(Print &output, uint8_t *buffer, size_t *length) {
    if (*length == 0) {
        return;
    }

    size_t written = output.write(buffer, *length);
    if (written > *length) {
        written = *length;
    }
    if (written == 0) {
        return;
    }
    if (written < *length) {
        memmove(buffer, buffer + written, *length - written);
    }
    *length -= written;
}

static void collectModemBytes(void) {
    if (modemToPcLength != 0 || ModemAT_rawAvailable() == 0) {
        return;
    }

    delay(10);
    while (ModemAT_rawAvailable() > 0 && modemToPcLength < sizeof(modemToPcBuffer)) {
        int value = ModemAT_rawRead();
        if (value < 0) {
            break;
        }
        modemToPcBuffer[modemToPcLength++] = (uint8_t)value;
    }
}

static void collectPcBytes(void) {
    if (pcToModemLength != 0 || Serial.available() == 0) {
        return;
    }

    delay(10);
    while (Serial.available() > 0 && pcToModemLength < sizeof(pcToModemBuffer)) {
        int value = Serial.read();
        if (value < 0) {
            break;
        }
        pcToModemBuffer[pcToModemLength++] = (uint8_t)value;
    }
}

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
        Serial.println("[ERROR] Modem readiness check failed. Bridge was not started.");
        return;
    }
    if (!ModemAT_enterRawMode()) {
        Serial.println("[ERROR] Raw bridge transition failed. Bridge was not started.");
        return;
    }

    bridgeActive = true;
    Serial.println("[STATE] Modem ready. Bridge communication enabled.");
    Serial.println("[INFO] Serial Monitor: 115200 baud, Both NL & CR.");
}

void loop() {
    if (!bridgeActive) {
        return;
    }

    forwardPending(Serial, modemToPcBuffer, &modemToPcLength);
    collectModemBytes();
    forwardPending(Serial, modemToPcBuffer, &modemToPcLength);

    forwardPending(MDMSerial, pcToModemBuffer, &pcToModemLength);
    collectPcBytes();
    forwardPending(MDMSerial, pcToModemBuffer, &pcToModemLength);
}
