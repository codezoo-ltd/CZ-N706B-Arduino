#include <Arduino.h>
#include <ModemAT.h>
#include <ModemPower.h>

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
static const size_t MODEM_RX_BUFFER_BYTES = 1024;
static const size_t PC_RX_BUFFER_BYTES = 64;
static const size_t USB_PACKET_BYTES = 64;
static const unsigned long USB_COALESCE_MS = 2;

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

// Keep pending bytes until the destination has accepted them.
static uint8_t modemToPcBuffer[MODEM_RX_BUFFER_BYTES];
static uint8_t pcToModemBuffer[PC_RX_BUFFER_BYTES];
static size_t modemToPcLength = 0;
static size_t pcToModemLength = 0;
static unsigned long lastModemRxMs = 0;
static bool bridgeActive = false;

static void collectModemBytes() {
    while (ModemAT_rawAvailable() > 0 && modemToPcLength < sizeof(modemToPcBuffer)) {
        int value = ModemAT_rawRead();
        if (value < 0) {
            break;
        }
        modemToPcBuffer[modemToPcLength++] = (uint8_t)value;
        lastModemRxMs = millis();
    }
}

static void forwardToPc() {
    if (modemToPcLength == 0) {
        return;
    }
    if (modemToPcLength < USB_PACKET_BYTES &&
        (unsigned long)(millis() - lastModemRxMs) < USB_COALESCE_MS) {
        return;
    }
    // Bound each PC write to the available output capacity.
    int available = Serial.availableForWrite();
    if (available <= 0) {
        return;
    }
    size_t count = modemToPcLength;
    if (count > USB_PACKET_BYTES) {
        count = USB_PACKET_BYTES;
    }
    if (count > (size_t)available) {
        count = (size_t)available;
    }
    size_t written = Serial.write(modemToPcBuffer, count);
    if (written > count) {
        written = count;
    }
    if (written > 0) {
        memmove(modemToPcBuffer, modemToPcBuffer + written, modemToPcLength - written);
        modemToPcLength -= written;
        Serial.flush();
    }
}

static void collectPcBytes() {
    if (pcToModemLength != 0) {
        return;
    }
    while (Serial.available() > 0 && pcToModemLength < sizeof(pcToModemBuffer)) {
        int value = Serial.read();
        if (value < 0) {
            break;
        }
        pcToModemBuffer[pcToModemLength++] = (uint8_t)value;
    }
}

static void forwardToModem() {
    if (pcToModemLength == 0) {
        return;
    }
    // Call ESP32 Dev Module UART's mutable-buffer overload, not Print's byte loop.
    size_t written = MDMSerial.write(pcToModemBuffer, pcToModemLength);
    if (written > pcToModemLength) {
        written = pcToModemLength;
    }
    if (written > 0) {
        // UART::write waits for TX_EMPTY; wait for TX_COMPLETE before reuse.
        MDMSerial.flush();
        memmove(pcToModemBuffer, pcToModemBuffer + written, pcToModemLength - written);
        pcToModemLength -= written;
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
    // Informational marker; the PC tool confirms a model-query response.
    Serial.println("[READY] FileTransfer bridge ready.");
}

void loop() {
    if (!bridgeActive) {
        return;
    }
    collectModemBytes();
    forwardToPc();
    collectPcBytes();
    forwardToModem();
    collectModemBytes();
    forwardToPc();
}
