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

static void checkVersion() {
    char value[128];
    ModemATResult result = ModemAT_execute("AT+CGMM", "+CGMM:", AT_TIMEOUT_MS);
    ModemAT_reportResult(result);
    if (result != MODEM_AT_OK) {
        return;
    }
    if (ModemAT_copyResponseValue(value, sizeof(value)) != MODEM_AT_VALUE_OK) {
        return;
    }

    result = ModemAT_execute("AT+IVSN", "+IVSN:", AT_TIMEOUT_MS);
    ModemAT_reportResult(result);
    if (result != MODEM_AT_OK) {
        return;
    }
    if (ModemAT_copyResponseValue(value, sizeof(value)) != MODEM_AT_VALUE_OK) {
        return;
    }
}

static bool checkAt(unsigned long timeoutMs) {
    ModemATResult result = ModemAT_execute("AT", NULL, timeoutMs);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK;
}

static void printHelp() {
    Serial.println("[STATE] CheckModule / ESP32 Dev Module; SIM/network not required");
    Serial.println(
        "[STATE] Serial=115200; MDMSerial=115200; GPIO17 TX->modem RX, GPIO16 RX<-modem TX");
    Serial.println(
        "[STATE] ON_OFF=GPIO18 active HIGH; LDO_EN=GPIO5 active HIGH; host IO=3.3V; common GND");
    Serial.println("[STATE] Modem regulated 5V, >=2A recommended; check supply/backfeed wiring");
    Serial.println(
        "[STATE] h help | i AT/model/version | o power on + identify | f normal power off");
    Serial.println(
        "[STATE] Test: boot -> identify -> f -> verify OFF -> o -> identify; CR/LF ignored");
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

    printHelp();
    if (!powerInitialized) {
        Serial.println("[STATE] Power configuration is invalid");
        return;
    }
    if (ModemPower_powerOn()) {
        checkVersion();
    }
}

void loop() {
    ModemAT_pollIdle();
    if (!Serial.available()) {
        return;
    }

    int input = Serial.read();
    if (input == '\r' || input == '\n') {
        return;
    }
    if (input == 'h') {
        printHelp();
    } else if (input == 'i') {
        if (checkAt(AT_TIMEOUT_MS)) {
            checkVersion();
        }
    } else if (input == 'o') {
        if (ModemPower_powerOn()) {
            checkVersion();
        }
    } else if (input == 'f') {
        ModemPower_powerOff();
    } else {
        Serial.println("[STATE] Unknown menu key; h for help (no AT bridge)");
    }
}
