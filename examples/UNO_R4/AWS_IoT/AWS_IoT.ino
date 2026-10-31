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

#include <ModemMqtt.h>
#include <stdio.h>
#include <string.h>

// Each test publishes once and waits for one PC-published message, then disconnects.
// Increasing testCount consumes more SIM data; no automatic replay or reconnect.
static unsigned int testCount = 1;
static unsigned int reconnectCount = 1;
static const char ROOT_CA_FILE[] = "aws_ca.pem";
static const char CLIENT_CERT_FILE[] = "aws_cert.pem";
static const char CLIENT_KEY_FILE[] = "aws_key.pem";
static const unsigned int TLS_AUTH_MODE = 1;
static const unsigned int TLS_VERSION = 3;
static const unsigned long RESTART_WAIT_MS = 10000;
static const char APN[] = "simplio.apn";
static const char BROKER_HOST[] = "YOUR_ENDPOINT-ats.iot.ap-northeast-2.amazonaws.com";
static const uint16_t BROKER_PORT = 8883;
static const char CLIENT_ID[] = "N706_Client_01";
static const char USERNAME[] = "";
static const char PASSWORD[] = "";
static const unsigned int CLEAN_SESSION = 1;
static const unsigned int KEEP_ALIVE_SECONDS = 60;
static const char SUBSCRIBE_TOPIC[] = "n706ea/topic/get";
static const char PUBLISH_TOPIC[] = "n706ea/topic/put";
static const unsigned int SUBSCRIBE_QOS = 1;
static const unsigned int PUBLISH_QOS = 1;
static const bool RETAINED = false;
static const char PUBLISH_MESSAGE[] = "Hello EMQX";
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static const unsigned long CONNECT_TIMEOUT_MS = 60000;
static const unsigned long RECEIVE_TIMEOUT_MS = 60000;
static const unsigned long POLL_INTERVAL_MS = 2000;
static const unsigned long CLOSE_TIMEOUT_MS = 20000;
static const unsigned long TEST_INTERVAL_MS = 5000;
static const size_t TOPIC_BUFFER_BYTES = 128;
static const size_t RECEIVE_BUFFER_BYTES = 2048;
static const size_t SERIAL_LOG_CHUNK_BYTES = 16;

static const ModemMqttConfig MQTT_CONFIG = {
    APN,
    BROKER_HOST,
    BROKER_PORT,
    CLIENT_ID,
    USERNAME,
    PASSWORD,
    CLEAN_SESSION,
    KEEP_ALIVE_SECONDS,
    QUERY_TIMEOUT_MS,
    REGISTER_TIMEOUT_MS,
    LINK_TIMEOUT_MS,
    CONNECT_TIMEOUT_MS,
    RECEIVE_TIMEOUT_MS,
    POLL_INTERVAL_MS,
    CLOSE_TIMEOUT_MS
};

static const ModemMqttTlsConfig TLS_CONFIG = {
    ROOT_CA_FILE,
    CLIENT_CERT_FILE,
    CLIENT_KEY_FILE,
    TLS_AUTH_MODE,
    TLS_VERSION
};

static void printMessage(const char *topic, const uint8_t *data, size_t length) {
    char log[96];
    ModemMqtt_pollIdle();
    ModemAT_printLine("[RX TOPIC]");
    ModemAT_printLine(topic);
    snprintf(log, sizeof(log), "[RX DATA] %lu bytes:", (unsigned long)length);
    ModemAT_printLine(log);
    for (size_t offset = 0; offset < length;) {
        ModemMqtt_pollIdle();
        size_t size = length - offset;
        if (size > SERIAL_LOG_CHUNK_BYTES) {
            size = SERIAL_LOG_CHUNK_BYTES;
        }
        size_t written = Serial.write(data + offset, size);
        ModemMqtt_pollIdle();
        if (written == 0) {
            ModemAT_printLine("[WARNING] PC output incomplete; modem data was captured.");
            return;
        }
        offset += written;
    }
    Serial.println();
    ModemMqtt_pollIdle();
}

static bool connectAwsIoT() {
    return ModemMqtt_prepareNetwork() && ModemMqtt_connectTls(&TLS_CONFIG);
}

static bool exchangeMessages(bool initial) {
    // Preserve the reference order on the first connection: publish, then subscribe.
    bool subscribed = !initial && ModemMqtt_subscribe(SUBSCRIBE_TOPIC, SUBSCRIBE_QOS);
    if (!initial && !subscribed) {
        return false;
    }
    bool published = ModemMqtt_publish(PUBLISH_TOPIC, PUBLISH_MESSAGE, PUBLISH_QOS, RETAINED);
    if (initial) {
        subscribed = published && ModemMqtt_subscribe(SUBSCRIBE_TOPIC, SUBSCRIBE_QOS);
    }
    bool received = false;
    if (published && subscribed) {
        ModemAT_printLine("[PUBLISH OK] Confirm the message in MQTT Explorer.");
        ModemAT_printLine("[READY] Publish one PC message to the subscribed topic:");
        ModemAT_printLine(SUBSCRIBE_TOPIC);
        static char topic[TOPIC_BUFFER_BYTES];
        static uint8_t data[RECEIVE_BUFFER_BYTES];
        size_t count = 0;
        received = ModemMqtt_receive(topic, sizeof(topic), data, sizeof(data), &count);
        if (received) {
            printMessage(topic, data, count);
            received = !strcmp(topic, SUBSCRIBE_TOPIC);
        }
    }
    bool unsubscribed = !subscribed || ModemMqtt_unsubscribe(SUBSCRIBE_TOPIC);
    return published && received && unsubscribed;
}

static bool finishSession(bool exchanged) {
    bool disconnected = ModemMqtt_disconnect(RESTART_WAIT_MS);
    if (!disconnected) {
        return false;
    }
    bool settled = ModemMqtt_prepareReconnect(0);
    bool released = settled && ModemMqtt_releaseIp();
    return exchanged && released;
}

static bool runAwsTest(unsigned int number) {
    char log[96];
    snprintf(log, sizeof(log), "[TEST] %u/%u", number, testCount);
    ModemAT_printLine(log);
    if (!connectAwsIoT()) {
        finishSession(false);
        return false;
    }
    bool exchanged = exchangeMessages(true);
    for (unsigned int index = 0; exchanged && index < reconnectCount; ++index) {
        if (!ModemMqtt_disconnect(RESTART_WAIT_MS) || !ModemMqtt_prepareReconnect(0)) {
            return false;
        }
        snprintf(log, sizeof(log), "[RECONNECT] %u/%u", index + 1, reconnectCount);
        ModemAT_printLine(log);
        if (!connectAwsIoT()) {
            finishSession(false);
            return false;
        }
        exchanged = exchangeMessages(false);
    }
    return finishSession(exchanged);
}

void setup() {
    bool powerReady = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    Serial1.begin(MODEM_BAUD);
    ModemAT_begin(Serial1, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    ModemAT_printLine("[INFO] AWS_IoT / UNO R4 / 115200 baud.");
    ModemAT_printLine("[WARNING] SIM data is consumed on every connection and exchange.");
    char settings[96];
    snprintf(settings, sizeof(settings), "[INFO] Tests: %u; reconnects per test: %u.",
        testCount, reconnectCount);
    ModemAT_printLine(settings);
    if (testCount == 0) {
        ModemAT_printLine("[DONE] testCount is zero; test disabled.");
        return;
    }
    if (strstr(BROKER_HOST, "YOUR_ENDPOINT") != NULL) {
        ModemAT_printLine("[ERROR] Set your AWS IoT endpoint before running.");
        return;
    }
    if (!powerReady || !ModemPower_powerOn() || !ModemMqtt_begin(&MQTT_CONFIG, Serial1)) {
        ModemAT_printLine("[FAIL] Modem or MQTT initialization failed.");
        return;
    }
    for (unsigned int index = 0; index < testCount; ++index) {
        if (!runAwsTest(index + 1)) {
            ModemAT_printLine("[FAIL] AWS test incomplete; inspect modem state. No retry.");
            return;
        }
        ModemAT_printLine("[PASS] AWS exchanges completed; MQTT and IP released.");
        if (index + 1 < testCount) {
            ModemMqtt_wait(TEST_INTERVAL_MS);
        }
    }
    ModemAT_printLine("[DONE] Requested tests completed. No automatic restart.");
}

void loop() {
    // Passive UART handling only; no automatic publish or reconnect.
    ModemMqtt_pollIdle();
}
