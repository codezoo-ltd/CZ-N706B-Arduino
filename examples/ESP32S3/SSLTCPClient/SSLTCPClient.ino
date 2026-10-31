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
#include <stdlib.h>

// Each test sends one HTTP request and receives a bounded direct UART TLS response.
// Increasing testCount consumes SIM data; failures stop without automatic retries.
static unsigned int testCount = 1;
static const char APN[] = "simplio.apn";
static const char SERVER_HOST[] = "httpbin.org";
static const uint16_t SERVER_PORT = 443;
static const uint8_t SOCKET_ID = 0;
static const char CA_FILE[] = "httpbin_ca.pem";
static const char HTTP_PATH[] = "/get";
static const char HTTP_CONNECTION[] = "close";
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static const unsigned long SOCKET_TIMEOUT_MS = 60000;
static const unsigned long RECEIVE_TIMEOUT_MS = 30000;
static const unsigned long POLL_INTERVAL_MS = 2000;
static const unsigned long TEST_INTERVAL_MS = 5000;
static const unsigned long CLOSE_URC_WAIT_MS = 5000;
static const size_t REQUEST_BUFFER_BYTES = 256;
static const size_t RESPONSE_BUFFER_BYTES = 4096;
static const size_t SERIAL_LOG_CHUNK_BYTES = 16;
static char sendData[REQUEST_BUFFER_BYTES];
static char response[RESPONSE_BUFFER_BYTES];

static const ModemSocketTlsConfig SOCKET_CONFIG = {
    APN,
    SERVER_HOST,
    SERVER_PORT,
    SOCKET_ID,
    CA_FILE,
    QUERY_TIMEOUT_MS,
    REGISTER_TIMEOUT_MS,
    LINK_TIMEOUT_MS,
    SOCKET_TIMEOUT_MS,
    RECEIVE_TIMEOUT_MS,
    POLL_INTERVAL_MS,
    CLOSE_URC_WAIT_MS
};

static bool buildRequest() {
    if (strpbrk(SERVER_HOST, " \t\r\n") || HTTP_PATH[0] != '/' ||
        strpbrk(HTTP_PATH, " \t\r\n") ||
        strpbrk(HTTP_CONNECTION, "\r\n") || HTTP_CONNECTION[0] == '\0') {
        return false;
    }
    int written = snprintf(sendData, sizeof(sendData),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: %s\r\n\r\n",
        HTTP_PATH, SERVER_HOST, HTTP_CONNECTION);
    return written > 0 && (size_t)written < sizeof(sendData);
}

// HTTP interpretation belongs to the application, not ModemSocket_recv().
static bool httpResponseComplete(size_t size) {
    const char *end = strstr(response, "\r\n\r\n");
    if (end == NULL) {
        return false;
    }
    static const char HEADER_NAME[] = "content-length";
    const char *field = NULL;
    const char *line = strstr(response, "\r\n");
    if (line == NULL) {
        return false;
    }
    line += 2;
    while (line < end) {
        const char *lineEnd = strstr(line, "\r\n");
        if (lineEnd == NULL || lineEnd > end) {
            return false;
        }
        const char *colon = (const char *)memchr(line, ':', (size_t)(lineEnd - line));
        if (colon != NULL && (size_t)(colon - line) == sizeof(HEADER_NAME) - 1) {
            bool matches = true;
            for (size_t i = 0; i < sizeof(HEADER_NAME) - 1; ++i) {
                char character = line[i];
                if (character >= 'A' && character <= 'Z') {
                    character += 'a' - 'A';
                }
                if (character != HEADER_NAME[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                if (field != NULL) {
                    return false;
                }
                field = colon + 1;
            }
        }
        line = lineEnd + 2;
    }
    if (field == NULL) {
        return false;
    }
    while (*field == ' ' || *field == '\t') {
        ++field;
    }
    if (*field < '0' || *field > '9') {
        return false;
    }
    char *numberEnd;
    unsigned long length = strtoul(field, &numberEnd, 10);
    while (*numberEnd == ' ' || *numberEnd == '\t') {
        ++numberEnd;
    }
    size_t headers = (size_t)(end + 4 - response);
    return !strncmp(numberEnd, "\r\n", 2) && length <= sizeof(response) - 1 - headers &&
        size >= headers + length;
}

static void printHttpChunk(const uint8_t *data, size_t length) {
    for (size_t offset = 0; offset < length;) {
        ModemSocket_pollIdle();
        size_t count = length - offset;
        if (count > SERIAL_LOG_CHUNK_BYTES) {
            count = SERIAL_LOG_CHUNK_BYTES;
        }
        size_t written = Serial.write(data + offset, count);
        ModemSocket_pollIdle();
        if (written == 0) {
            ModemAT_printLine("[WARNING] Serial output incomplete; UART polling continues.");
            return;
        }
        offset += written;
    }
    Serial.println();
    ModemSocket_pollIdle();
}

static bool receiveHttpResponse() {
    size_t used = 0;
    response[0] = '\0';
    unsigned long start = millis();
    unsigned long elapsed = millis() - start;
    while (elapsed < RECEIVE_TIMEOUT_MS && used < sizeof(response) - 1) {
        size_t count = 0;
        if (!ModemSocket_recv((uint8_t *)response + used, sizeof(response) - 1 - used,
            &count, RECEIVE_TIMEOUT_MS - elapsed)) {
            return false;
        }
        char log[96];
        snprintf(log, sizeof(log), "[HTTP RX] Chunk %lu bytes; total %lu bytes.",
            (unsigned long)count, (unsigned long)(used + count));
        ModemSocket_pollIdle();
        ModemAT_printLine(log);
        ModemSocket_pollIdle();
        if (count > 0) {
            printHttpChunk((const uint8_t *)response + used, count);
        }
        used += count;
        response[used] = '\0';
        if (memchr(response, '\0', used) != NULL) {
            return false;
        }
        if (httpResponseComplete(used)) {
            ModemAT_printLine("[HTTP] Content-Length response complete.");
            return !strncmp(response, "HTTP/1.1 200 ", 13) ||
                !strncmp(response, "HTTP/1.0 200 ", 13);
        }
        elapsed = millis() - start;
    }
    ModemAT_printLine("[ERROR] HTTP response incomplete or response buffer full.");
    return false;
}

static bool runTlsTest(unsigned int number) {
    char log[96];
    snprintf(log, sizeof(log), "[TEST] %u/%u", number, testCount);
    ModemAT_printLine(log);
    if (!buildRequest() || !ModemSocket_prepareNetwork()) {
        ModemAT_printLine("[FAIL] Request, network, clock or CA preparation failed.");
        return false;
    }
    if (!ModemSocket_open()) {
        ModemAT_printLine("[FAIL] TLS Socket Open failed; no request sent.");
        return false;
    }
    bool sent = ModemSocket_send(sendData);
    bool receivedOk = sent && receiveHttpResponse();
    ModemSocket_pollIdle();
    // The same parser and latched close state are used across both function calls.
    bool closed = ModemSocket_waitForClose();
    if (!closed) {
        closed = ModemSocket_close();
    }
    bool released = closed && ModemSocket_releaseIp();
    if (!sent || !receivedOk || !closed || !released) {
        ModemAT_printLine("[FAIL] HTTP/Close/IP release incomplete; no retry.");
        return false;
    }
    ModemAT_printLine("[PASS] HTTP 200 received; socket closed and IP released.");
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
    ModemAT_printLine("[INFO] SSLTCPClient / ESP32S3 Dev Module / 115200 baud.");
    ModemAT_printLine("[WARNING] SIM data is consumed. testCount defaults to one.");
    if (testCount == 0) {
        ModemAT_printLine("[DONE] testCount is zero; test disabled.");
        return;
    }
    if (!powerReady || !ModemPower_powerOn() || !ModemSocket_begin(&SOCKET_CONFIG, MDMSerial)) {
        ModemAT_printLine("[FAIL] Modem or TLS API initialization failed.");
        return;
    }
    for (unsigned int index = 0; index < testCount; ++index) {
        if (!runTlsTest(index + 1)) {
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
