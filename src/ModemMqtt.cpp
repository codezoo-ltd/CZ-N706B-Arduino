#include "ModemMqtt.h"
#include "ModemAT.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t COMMAND_BYTES = 1024;
static const size_t CONTROL_BYTES = COMMAND_BYTES;
static const size_t VALUE_BYTES = 256;
static const size_t TOPIC_BYTES = 128;
static const size_t MESSAGE_BYTES = 2048;
static const size_t PUBLISH_BYTES = 512;
static const unsigned long QUIET_MS = 100;

struct MqttState {
    ModemMqttConfig config;
    HardwareSerial *uart;
    bool initialized;
    bool blocked;
    bool finalOk;
    bool finalError;
    bool connected;
    bool connectionAttempted;
    bool ready;
    bool rebooted;
    bool disconnected;
    bool messageReady;
    bool messageNotice;
    const char *prefix;
    char value[VALUE_BYTES];
    char control[CONTROL_BYTES];
    size_t controlSize;
    char topic[TOPIC_BYTES];
    uint8_t message[MESSAGE_BYTES];
    size_t messageSize;
    size_t remaining;
};
static MqttState state = {};

static void fail(const char *reason) {
    state.blocked = true;
    ModemAT_printLine(reason);
}
static bool validText(const char *value, size_t limit, bool allowEmpty = false) {
    return value != NULL && (allowEmpty || value[0] != '\0') && strlen(value) <= limit &&
        strpbrk(value, "\"\r\n") == NULL;
}
static bool fits(int size, size_t capacity) {
    if (size < 0 || (size_t)size >= capacity) {
        ModemAT_printLine("[ERROR] Command exceeds its buffer; nothing sent.");
        return false;
    }
    return true;
}
static bool ready() {
    return state.initialized && ModemAT_isRawMode() && !state.blocked;
}
static void controlLine() {
    state.control[state.controlSize] = '\0';
    const char *line = state.control;
    if (!strcmp(line, "OK")) {
        state.finalOk = true;
    } else if (!strcmp(line, "ERROR") || !strncmp(line, "+CME ERROR", 10) ||
        !strncmp(line, "+CMS ERROR", 10)) {
        state.finalError = true;
        ModemAT_printLine("[RX ERROR]");
        ModemAT_printLine(line);
    }
    if (!strcmp(line, "+PBREADY")) {
        state.rebooted = true;
        state.connected = false;
        state.ready = false;
    }
    if (!strncmp(line, "+MQTTDISCONNED:", 15) || !strcmp(line, "GPRS DISCONNECTION")) {
        state.connected = false;
        state.disconnected = true;
    }
    if (state.prefix != NULL && !strncmp(line, state.prefix, strlen(state.prefix))) {
        const char *value = line + strlen(state.prefix);
        while (*value == ' ') {
            ++value;
        }
        if (strlen(value) >= sizeof(state.value)) {
            fail("[ERROR] Query value overflow.");
        } else {
            strcpy(state.value, value);
        }
    }
    state.controlSize = 0;
}
static void consume(uint8_t byte) {
    if (state.remaining > 0) {
        state.message[state.messageSize++] = byte;
        --state.remaining;
        if (state.remaining == 0) {
            state.messageReady = true;
            state.messageNotice = true;
        }
        return;
    }
    if (byte == '\r' || byte == '\n') {
        if (state.controlSize > 0) {
            controlLine();
        }
        return;
    }
    if (state.controlSize + 1 >= sizeof(state.control)) {
        fail("[ERROR] MQTT control line overflow; further commands blocked.");
        return;
    }
    state.control[state.controlSize++] = (char)byte;
    state.control[state.controlSize] = '\0';
    if (byte == ',' && !strncmp(state.control, "+MQTTSUB:", 9)) {
        unsigned long messageId;
        unsigned long length;
        char topic[TOPIC_BYTES];
        int consumed = 0;
        if (sscanf(state.control + 9, " %lu,\"%127[^\"]\",%lu,%n", &messageId, topic,
            &length, &consumed) == 3 && consumed > 0 &&
            (size_t)consumed + 9 == state.controlSize) {
            if (length > sizeof(state.message) || state.messageReady) {
                fail("[ERROR] MQTT message exceeds receive capacity or pending slot.");
                return;
            }
            strcpy(state.topic, topic);
            state.messageSize = 0;
            state.remaining = (size_t)length;
            state.controlSize = 0;
            if (length == 0) {
                state.messageReady = true;
                state.messageNotice = true;
            }
        }
    }
}
static void pump() {
    while (state.initialized && !state.blocked && ModemAT_rawAvailable() > 0) {
        int byte = ModemAT_rawRead();
        if (byte < 0) {
            break;
        }
        consume((uint8_t)byte);
    }
    if (state.messageNotice) {
        state.messageNotice = false;
        ModemAT_printLine("[URC] MQTT message captured by its declared byte count.");
    }
}
void ModemMqtt_pollIdle() {
    if (state.initialized && ModemAT_isRawMode()) {
        pump();
    } else {
        ModemAT_pollIdle();
    }
}
void ModemMqtt_wait(unsigned long durationMs) {
    unsigned long start = millis();
    while (ready() && (unsigned long)(millis() - start) < durationMs) {
        pump();
        delay(1);
    }
}
static bool query(const char *command, const char *prefix, unsigned long timeoutMs,
    bool sensitive = false) {
    if (!ready()) {
        return false;
    }
    ModemMqtt_wait(QUIET_MS);
    if (state.blocked || state.remaining > 0 || state.controlSize > 0) {
        fail("[ERROR] Incomplete UART response; next command not sent.");
        return false;
    }
    state.finalOk = false;
    state.finalError = false;
    state.prefix = prefix;
    state.value[0] = '\0';
    ModemAT_printLine("[TX command]");
    ModemAT_printLine(sensitive ? "AT+MQTTCONNPARAM=<credentials hidden>" : command);
    unsigned long start = millis();
    size_t length = strlen(command);
    for (size_t i = 0; i < length + 2; ++i) {
        pump();
        uint8_t byte = i < length ? (uint8_t)command[i] : i == length ? '\r' : '\n';
        if (state.blocked || (unsigned long)(millis() - start) >= timeoutMs ||
            state.uart->write(byte) != 1) {
            fail("[ERROR] MQTT UART write failed or timed out.");
            return false;
        }
        state.uart->flush();
    }
    while ((unsigned long)(millis() - start) < timeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        if (state.finalOk) {
            if (prefix != NULL && state.value[0] == '\0') {
                ModemAT_printLine("[ERROR] Final OK without expected query data.");
                return false;
            }
            if (prefix != NULL) {
                ModemAT_printLine(state.value);
            }
            return true;
        }
        delay(1);
    }
    fail("[ERROR] MQTT command timeout; further commands blocked.");
    return false;
}
static bool unsignedValue(unsigned long *value) {
    char extra;
    return sscanf(state.value, "%lu%c", value, &extra) == 1;
}
static int ipState() {
    unsigned int link;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    char extra;
    if (sscanf(state.value, "%u,%u.%u.%u.%u%c", &link, &a, &b, &c, &d, &extra) != 5 ||
        link > 1 || a > 255 || b > 255 || c > 255 || d > 255) {
        return -1;
    }
    return link == 1 && (a || b || c || d) ? 1 : link == 0 && !(a || b || c || d) ? 0 : -1;
}
static bool waitIp(bool active) {
    unsigned long start = millis();
    unsigned long elapsed = 0;
    while (elapsed < state.config.linkTimeoutMs) {
        if (!query("AT+XIIC?", "+XIIC:", state.config.queryTimeoutMs)) {
            return false;
        }
        int link = ipState();
        if (link < 0) {
            return false;
        }
        if (link == (active ? 1 : 0)) {
            return true;
        }
        elapsed = millis() - start;
        if (elapsed >= state.config.linkTimeoutMs) {
            break;
        }
        unsigned long remaining = state.config.linkTimeoutMs - elapsed;
        ModemMqtt_wait(remaining < state.config.pollIntervalMs ?
            remaining : state.config.pollIntervalMs);
        elapsed = millis() - start;
    }
    return false;
}
bool ModemMqtt_begin(const ModemMqttConfig *config, HardwareSerial &uart) {
    if (config == NULL || !validText(config->apn, 100) || !validText(config->host, 100) ||
        strpbrk(config->host, " :/\t") || config->port == 0 ||
        !validText(config->clientId, 64) || !validText(config->username, 64, true) ||
        !validText(config->password, 64, true) || config->cleanSession > 1 ||
        config->keepAliveSeconds < 20 || config->keepAliveSeconds > 180 ||
        config->queryTimeoutMs == 0 || config->registerTimeoutMs == 0 ||
        config->linkTimeoutMs == 0 || config->connectTimeoutMs == 0 ||
        config->receiveTimeoutMs == 0 || config->pollIntervalMs == 0 ||
        config->closeTimeoutMs == 0) {
        return false;
    }
    if (!ModemAT_enterRawMode()) {
        return false;
    }
    state = {};
    state.config = *config;
    state.uart = &uart;
    state.initialized = true;
    return true;
}
bool ModemMqtt_prepareNetwork() {
    if (!ready() || state.connectionAttempted || state.connected) {
        return false;
    }
    char command[COMMAND_BYTES];
    if (!query("AT+CPIN?", "+CPIN:", state.config.queryTimeoutMs) ||
        strcmp(state.value, "READY")) {
        return false;
    }
    int written = snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
        state.config.apn);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config.queryTimeoutMs)) {
        return false;
    }
    unsigned long start = millis();
    unsigned long elapsed = 0;
    bool registered = false;
    while (elapsed < state.config.registerTimeoutMs) {
        if (!query("AT+CEREG?", "+CEREG:", state.config.queryTimeoutMs)) {
            return false;
        }
        unsigned int mode;
        unsigned int status;
        char extra;
        if (sscanf(state.value, "%u,%u%c", &mode, &status, &extra) != 2 || status == 3) {
            return false;
        }
        if (status == 1 || status == 5) {
            registered = true;
            break;
        }
        elapsed = millis() - start;
        if (elapsed >= state.config.registerTimeoutMs) {
            break;
        }
        unsigned long remaining = state.config.registerTimeoutMs - elapsed;
        ModemMqtt_wait(remaining < state.config.pollIntervalMs ?
            remaining : state.config.pollIntervalMs);
        elapsed = millis() - start;
    }
    if (!registered || !query("AT+XIIC?", "+XIIC:", state.config.queryTimeoutMs)) {
        return false;
    }
    int link = ipState();
    if (link < 0 || (link == 0 &&
        (!query("AT+XIIC=1", NULL, state.config.linkTimeoutMs) || !waitIp(true)))) {
        return false;
    }
    char log[96];
    snprintf(log, sizeof(log), "[STATE] Data link connected, IP: %s",
        strchr(state.value, ',') + 1);
    ModemAT_printLine(log);
    state.ready = true;
    state.rebooted = false;
    return true;
}
static bool waitMqttState(bool connected, unsigned long timeoutMs) {
    unsigned long start = millis();
    unsigned long elapsed = 0;
    while (elapsed < timeoutMs) {
        if (state.rebooted) {
            state.connected = false;
            return !connected;
        }
        if (!query("AT+MQTTSTATE?", "+MQTTSTATE:", state.config.queryTimeoutMs)) {
            return false;
        }
        unsigned long value;
        if (!unsignedValue(&value) || value > 1) {
            return false;
        }
        state.connected = value == 1;
        if (state.connected == connected) {
            return true;
        }
        elapsed = millis() - start;
        if (elapsed >= timeoutMs) {
            break;
        }
        unsigned long remaining = timeoutMs - elapsed;
        ModemMqtt_wait(remaining < state.config.pollIntervalMs ?
            remaining : state.config.pollIntervalMs);
        elapsed = millis() - start;
    }
    return false;
}
static bool connectMqtt(const ModemMqttTlsConfig *tls) {
    if (!ready() || !state.ready || state.connectionAttempted) {
        return false;
    }
    if (!query("AT+MQTTSTATE?", "+MQTTSTATE:", state.config.queryTimeoutMs)) {
        return false;
    }
    unsigned long current;
    if (!unsignedValue(&current) || current > 1) {
        return false;
    }
    if (current != 0) {
        state.connected = true;
        ModemAT_printLine("[ERROR] Existing MQTT connection preserved; test stopped.");
        return false;
    }
    if (!query("AT+MQTTMUX=0", NULL, state.config.queryTimeoutMs)) {
        return false;
    }
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+MQTTTLS=\"sslmode\",%u",
        tls == NULL ? 0U : 1U);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config.queryTimeoutMs)) {
        return false;
    }
    if (tls != NULL) {
        written = snprintf(command, sizeof(command), "AT+MQTTTLS=\"authmode\",%u",
            tls->authMode);
        if (!fits(written, sizeof(command)) ||
            !query(command, NULL, state.config.queryTimeoutMs)) {
            return false;
        }
        const char *names[] = {"rootca", "clientcert", "clientkey"};
        const char *files[] = {tls->rootCa, tls->clientCertificate, tls->clientKey};
        for (size_t index = 0; index < 3; ++index) {
            written = snprintf(command, sizeof(command), "AT+MQTTTLS=\"%s\",\"%s\"",
                names[index], files[index]);
            if (!fits(written, sizeof(command)) ||
                !query(command, NULL, state.config.queryTimeoutMs)) {
                return false;
            }
        }
        written = snprintf(command, sizeof(command), "AT+MQTTTLS=\"sslversion\",%u",
            tls->sslVersion);
        if (!fits(written, sizeof(command)) ||
            !query(command, NULL, state.config.queryTimeoutMs)) {
            return false;
        }
    }
    written = snprintf(command, sizeof(command), "AT+MQTTCONNPARAM=\"%s\",\"%s\",\"%s\"",
        state.config.clientId, state.config.username, state.config.password);
    if (!fits(written, sizeof(command)) ||
        !query(command, NULL, state.config.queryTimeoutMs, true)) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+MQTTCONN=\"%s:%u\",%u,%u",
        state.config.host, state.config.port, state.config.cleanSession,
        state.config.keepAliveSeconds);
    if (!fits(written, sizeof(command))) {
        return false;
    }
    state.connectionAttempted = true;
    state.disconnected = false;
    return query(command, NULL, state.config.connectTimeoutMs) &&
        waitMqttState(true, state.config.connectTimeoutMs);
}
bool ModemMqtt_connect() {
    return connectMqtt(NULL);
}
bool ModemMqtt_connectTls(const ModemMqttTlsConfig *tls) {
    if (tls == NULL || !validText(tls->rootCa, 100) ||
        !validText(tls->clientCertificate, 100) || !validText(tls->clientKey, 100) ||
        tls->authMode != 1 || tls->sslVersion != 3) {
        ModemAT_printLine("[ERROR] Invalid mutual TLS settings; nothing sent.");
        return false;
    }
    return connectMqtt(tls);
}
bool ModemMqtt_prepareReconnect(unsigned long restartWaitMs) {
    if (!ready() || state.connectionAttempted || state.connected) {
        return false;
    }
    ModemAT_printLine("[STATE] Observing restart notifications before reconnecting.");
    ModemMqtt_wait(restartWaitMs);
    if (!ready() || state.remaining > 0 || state.controlSize > 0) {
        return false;
    }
    state.messageReady = false;
    state.messageNotice = false;
    state.messageSize = 0;
    return query("AT", NULL, state.config.closeTimeoutMs);
}
bool ModemMqtt_subscribe(const char *topic, unsigned int qos) {
    if (!ready() || !state.connected || !validText(topic, TOPIC_BYTES - 1) || qos > 2) {
        return false;
    }
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+MQTTSUB=\"%s\",%u", topic, qos);
    return fits(written, sizeof(command)) && query(command, NULL, state.config.queryTimeoutMs);
}
bool ModemMqtt_publish(const char *topic, const char *message, unsigned int qos,
    bool retained) {
    if (!ready() || !state.connected || !validText(topic, TOPIC_BYTES - 1) ||
        strpbrk(topic, "+#") || !validText(message, PUBLISH_BYTES) || qos > 2) {
        return false;
    }
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+MQTTPUB=%u,%u,\"%s\",\"%s\"",
        retained ? 1U : 0U, qos, topic, message);
    return fits(written, sizeof(command)) && query(command, NULL, state.config.queryTimeoutMs);
}
bool ModemMqtt_receive(char *topic, size_t topicCapacity, uint8_t *data, size_t capacity,
    size_t *receivedSize) {
    if (receivedSize != NULL) {
        *receivedSize = 0;
    }
    if (!ready() || topic == NULL || data == NULL || receivedSize == NULL) {
        return false;
    }
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < state.config.receiveTimeoutMs) {
        pump();
        if (state.blocked) {
            return false;
        }
        if (state.messageReady) {
            if (strlen(state.topic) >= topicCapacity || state.messageSize > capacity) {
                ModemAT_printLine("[ERROR] Caller receive buffer too small; message retained.");
                return false;
            }
            strcpy(topic, state.topic);
            memcpy(data, state.message, state.messageSize);
            *receivedSize = state.messageSize;
            state.messageReady = false;
            state.messageSize = 0;
            return true;
        }
        if (!state.connected || state.disconnected || state.rebooted) {
            return false;
        }
        delay(1);
    }
    if (state.remaining > 0 || state.controlSize > 0) {
        fail("[ERROR] Incomplete MQTT receive frame; further commands blocked.");
    }
    ModemAT_printLine("[ERROR] MQTT receive wait expired; no automatic retry.");
    return false;
}
bool ModemMqtt_unsubscribe(const char *topic) {
    if (!ready() || !state.connected || !validText(topic, TOPIC_BYTES - 1)) {
        return false;
    }
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+MQTTUNSUB=\"%s\"", topic);
    return fits(written, sizeof(command)) && query(command, NULL, state.config.queryTimeoutMs);
}
bool ModemMqtt_disconnect(unsigned long restartWaitMs) {
    if (!ready() || !state.connectionAttempted) {
        return false;
    }
    bool ok = true;
    if (!state.rebooted && !state.disconnected) {
        ok = query("AT+MQTTDISCONN", NULL, state.config.closeTimeoutMs);
    }
    if (!ok) {
        return false;
    }
    ModemMqtt_wait(restartWaitMs);
    if (!waitMqttState(false, state.config.closeTimeoutMs)) {
        return false;
    }
    state.connectionAttempted = false;
    state.connected = false;
    ModemAT_printLine("[STATE] MQTT disconnection confirmed.");
    return true;
}
bool ModemMqtt_releaseIp() {
    if (!ready() || state.connectionAttempted || state.connected) {
        return false;
    }
    if (state.rebooted) {
        // PBREADY indicates a reboot, but AT readiness and the IP state still need checking.
        if (!query("AT", NULL, state.config.closeTimeoutMs)) {
            return false;
        }
    }
    if (!query("AT+XIIC?", "+XIIC:", state.config.queryTimeoutMs)) {
        return false;
    }
    int link = ipState();
    if (link < 0 || (link == 1 &&
        (!query("AT+XIIC=0", NULL, state.config.linkTimeoutMs) || !waitIp(false)))) {
        return false;
    }
    state.ready = false;
    state.rebooted = false;
    ModemAT_printLine("[STATE] IP release confirmed: XIIC state 0, IP 0.0.0.0.");
    return true;
}
