#include "ModemHttp.h"
#include "ModemAT.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t CONTROL_BYTES = 1024;
static const size_t HEADER_BYTES = 3072;
static const size_t BODY_BYTES = 4096;
static const size_t COMMAND_BYTES = 256;
static const size_t FILE_READ_BYTES = 32;
static const size_t FILE_PREVIEW_BYTES = 32;
static const unsigned long FILE_SPACE_MARGIN_BYTES = 1024;
static const unsigned long CONTROL_QUIET_MS = 100;
static const unsigned long CERT_REPLY_QUIET_MS = 150;
static const unsigned long CERT_REPLY_LIMIT_MS = 1000;

enum HttpPhase {
    HTTP_CONTROL,
    HTTP_STATUS,
    HTTP_HEADERS,
    HTTP_FIXED_BODY,
    HTTP_CHUNK_SIZE,
    HTTP_CHUNK_BODY,
    HTTP_CHUNK_CR,
    HTTP_CHUNK_LF,
    HTTP_TRAILERS
};
struct HttpState {
    const ModemHttpConfig *config;
    const ModemHttpTlsConfig *tls;
    HardwareSerial *uart;
    bool initialized;
    bool blocked;
    bool finalOk;
    bool finalError;
    bool prompt;
    bool promptPadding;
    bool closed;
    bool mayBeOpen;
    bool networkOwned;
    bool networkReady;
    bool responseStarted;
    bool complete;
    bool chunked;
    bool hasLength;
    HttpPhase phase;
    unsigned int status;
    unsigned long contentLength;
    unsigned long remaining;
    unsigned long lastRxMs;
    char control[CONTROL_BYTES];
    char value[192];
    char lastError[192];
    char headers[HEADER_BYTES];
    char body[BODY_BYTES];
    size_t controlSize;
    size_t headerSize;
    size_t bodySize;
    const char *prefix;
    const char *targetFile;
    bool listingFiles;
    bool targetExists;
    bool listInvalid;
    bool fileStatusSeen;
    bool fileStatusOk;
    unsigned long downloadedBytes;
    unsigned long fileErrorCode;
    char fileStatusLine[96];
    bool fileReadActive;
    bool fileReadHeader;
    size_t fileReadExpected;
    size_t fileReadCount;
    size_t fileReadRemaining;
    uint8_t fileData[FILE_READ_BYTES];
};
static HttpState state = {};

static void fail(const char *reason) {
    state.blocked = true;
    ModemAT_printLine(reason);
}
static bool fits(int size, size_t capacity) {
    if (size < 0 || (size_t)size >= capacity) {
        ModemAT_printLine("[ERROR] Command exceeds its buffer; nothing sent.");
        return false;
    }
    return true;
}
static bool validText(const char *value, size_t maximum) {
    return value != NULL && strlen(value) > 0 && strlen(value) <= maximum &&
        strpbrk(value, "\"\r\n") == NULL;
}
static bool sameText(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) {
            return false;
        }
    }
    return *a == *b;
}
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    size_t size = strlen(text);
    while (size > 0 && (text[size - 1] == ' ' || text[size - 1] == '\t')) {
        text[--size] = '\0';
    }
    return text;
}
static bool appendBody(uint8_t byte) {
    if (byte == 0) {
        fail("[ERROR] Binary NUL is unsupported in this text HTTP example.");
        return false;
    }
    if (state.bodySize >= sizeof(state.body) - 1) {
        fail("[ERROR] HTTP body exceeds the example receive buffer.");
        return false;
    }
    state.body[state.bodySize++] = (char)byte;
    state.body[state.bodySize] = '\0';
    return true;
}
static void completeResponse() {
    state.complete = true;
    state.phase = HTTP_CONTROL;
}
static void responseLine() {
    char *line = state.control;
    if (state.phase == HTTP_STATUS) {
        unsigned int major;
        unsigned int minor;
        if (sscanf(line, "HTTP/%u.%u %u", &major, &minor, &state.status) != 3 ||
            major != 1 || state.status < 200 || state.status > 599) {
            fail("[ERROR] Unsupported HTTP status line.");
            return;
        }
        state.phase = HTTP_HEADERS;
    } else if (state.phase == HTTP_HEADERS) {
        if (!*line) {
            if (state.chunked && state.hasLength) {
                fail("[ERROR] Ambiguous HTTP response framing.");
            } else if (state.chunked) {
                state.phase = HTTP_CHUNK_SIZE;
            } else if (state.hasLength) {
                state.remaining = state.contentLength;
                if (state.remaining == 0) {
                    completeResponse();
                } else {
                    state.phase = HTTP_FIXED_BODY;
                }
            } else if (state.status == 204 || state.status == 304) {
                completeResponse();
            } else {
                fail("[ERROR] HTTP response needs Content-Length or chunked framing.");
            }
            return;
        }
        if (state.headerSize + strlen(line) + 2 >= sizeof(state.headers)) {
            fail("[ERROR] HTTP headers exceed the example buffer.");
            return;
        }
        size_t length = strlen(line);
        memcpy(state.headers + state.headerSize, line, length);
        state.headerSize += length;
        state.headers[state.headerSize++] = '\n';
        state.headers[state.headerSize] = '\0';
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            fail("[ERROR] Malformed HTTP header.");
            return;
        }
        *colon = '\0';
        char *value = trim(colon + 1);
        if (sameText(line, "Content-Length")) {
            char *end = NULL;
            unsigned long count = strtoul(value, &end, 10);
            if (*value < '0' || *value > '9' || *end != '\0' || state.hasLength ||
                count >= sizeof(state.body)) {
                fail("[ERROR] Invalid, duplicate or oversized Content-Length.");
                return;
            }
            state.hasLength = true;
            state.contentLength = count;
        } else if (sameText(line, "Transfer-Encoding")) {
            if (!sameText(value, "chunked") || state.chunked) {
                fail("[ERROR] Unsupported Transfer-Encoding.");
                return;
            }
            state.chunked = true;
        } else if (sameText(line, "Content-Encoding") && !sameText(value, "identity")) {
            fail("[ERROR] Compressed HTTP bodies are not decoded by this example.");
        }
    } else if (state.phase == HTTP_CHUNK_SIZE) {
        char *extension = strchr(line, ';');
        if (extension != NULL) {
            *extension = '\0';
        }
        char *end = NULL;
        unsigned long count = strtoul(line, &end, 16);
        if (!isxdigit((unsigned char)*line) || *end != '\0' ||
            count > sizeof(state.body) - 1 - state.bodySize) {
            fail("[ERROR] Invalid or oversized HTTP chunk.");
            return;
        }
        state.remaining = count;
        state.phase = count == 0 ? HTTP_TRAILERS : HTTP_CHUNK_BODY;
    } else if (state.phase == HTTP_TRAILERS && !*line) {
        completeResponse();
    }
}
static const char *protocolName() {
    return state.tls == NULL ? "HTTP" : "HTTPS";
}
static bool protocolPrefix(const char *line, const char *suffix) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "+%s%s:", protocolName(), suffix);
    return !strncmp(line, prefix, strlen(prefix));
}
static void controlLine() {
    const char *line = state.control;
    if (state.listingFiles && *line) {
        const char *record = line;
        if (!strncmp(record, "+FSLIST:", 8)) {
            record = trim((char *)record + 8);
        } else if (*record == '+') {
            record = "";
        }
        const char *comma = strrchr(record, ',');
        if (comma != NULL) {
            char *end = NULL;
            strtoul(comma + 1, &end, 10);
            if (end == comma + 1 || *end != '\0') {
                state.listInvalid = true;
            }
            const char *name = record;
            size_t size = (size_t)(comma - record);
            if (size >= 2 && name[0] == '"' && name[size - 1] == '"') {
                ++name;
                size -= 2;
            }
            if (size == strlen(state.targetFile) && !strncmp(name, state.targetFile, size)) {
                state.targetExists = true;
            }
        } else if (*record && strcmp(record, "OK") && strcmp(record, "ERROR") &&
            strcmp(record, "AT+FSLIST?")) {
            state.listInvalid = true;
        }
    }
    if (!strncmp(line, "+FILEHTTPSTAT:", 14)) {
        unsigned int mode;
        unsigned int status;
        unsigned long length;
        char extra;
        int fields = sscanf(line + 14, " %u,%u,%lu%c", &mode, &status, &length, &extra);
        state.fileStatusSeen = true;
        state.fileStatusOk = fields == 3 && mode == 0 && status == 1;
        state.downloadedBytes = state.fileStatusOk ? length : 0;
        state.fileErrorCode = fields == 2 ? status :
            fields == 3 && status == 0 ? length : 0;
        if (!state.fileStatusOk) {
            // Failed downloads may leave unframed response bytes in the UART.
            // Stop the pump at the result line; do not parse or discard that tail.
            state.blocked = true;
        }
        strncpy(state.fileStatusLine, line, sizeof(state.fileStatusLine) - 1);
        state.fileStatusLine[sizeof(state.fileStatusLine) - 1] = '\0';
        return;
    }
    if (protocolPrefix(line, "RECV")) {
        if (state.responseStarted) {
            fail("[ERROR] Repeated HTTPRECV header; use recvmode 0.");
            return;
        }
        state.responseStarted = true;
        state.phase = HTTP_STATUS;
        const char *status = strchr(line, ':') + 1;
        while (*status == ' ') {
            ++status;
        }
        if (*status) {
            memmove(state.control, status, strlen(status) + 1);
            responseLine();
        }
        return;
    }
    if (!strcmp(line, "OK")) {
        state.finalOk = true;
    } else if (!strcmp(line, "ERROR") || !strncmp(line, "+CME ERROR", 10) ||
        !strncmp(line, "+CMS ERROR", 10) || protocolPrefix(line, "SETUP") ||
        protocolPrefix(line, "ACTION") || protocolPrefix(line, "SEND")) {
        const char *result = strchr(line, ':');
        if (!protocolPrefix(line, "SETUP") || result == NULL ||
            strcmp(trim((char *)result + 1), "OK") != 0) {
            state.finalError = true;
            strncpy(state.lastError, line, sizeof(state.lastError) - 1);
            state.lastError[sizeof(state.lastError) - 1] = '\0';
        } else {
            state.finalOk = true;
        }
    }
    if (protocolPrefix(line, "CLOSED")) {
        state.closed = true;
        state.mayBeOpen = false;
    }
    if (state.prefix != NULL && !strncmp(line, state.prefix, strlen(state.prefix))) {
        const char *value = line + strlen(state.prefix);
        while (*value == ' ') {
            ++value;
        }
        if (strlen(value) >= sizeof(state.value)) {
            fail("[ERROR] Query value exceeds its buffer.");
        } else {
            strcpy(state.value, value);
        }
    }
    // Do not print while parsing: drain modem UART before console output.
}
static void consume(uint8_t byte) {
    state.lastRxMs = millis();
    if (state.fileReadActive && state.fileReadRemaining > 0) {
        state.fileData[state.fileReadCount++] = byte;
        --state.fileReadRemaining;
        return;
    }
    if (state.phase == HTTP_FIXED_BODY || state.phase == HTTP_CHUNK_BODY) {
        if (!appendBody(byte)) {
            return;
        }
        if (--state.remaining == 0) {
            if (state.phase == HTTP_FIXED_BODY) {
                completeResponse();
            } else {
                state.phase = HTTP_CHUNK_CR;
            }
        }
        return;
    }
    if (state.phase == HTTP_CHUNK_CR || state.phase == HTTP_CHUNK_LF) {
        bool cr = state.phase == HTTP_CHUNK_CR;
        if (byte != (cr ? '\r' : '\n')) {
            fail("[ERROR] Invalid HTTP chunk delimiter.");
        } else {
            state.phase = cr ? HTTP_CHUNK_LF : HTTP_CHUNK_SIZE;
        }
        return;
    }
    if (state.phase == HTTP_CONTROL && state.promptPadding) {
        state.promptPadding = false;
        if (byte == ' ') {
            return;
        }
    }
    if (state.phase == HTTP_CONTROL && state.controlSize == 0 && byte == '>') {
        state.prompt = true;
        state.promptPadding = true;
        return;
    }
    if (byte == '\r') {
        return;
    }
    if (byte == '\n') {
        state.control[state.controlSize] = '\0';
        if (state.phase == HTTP_CONTROL) {
            controlLine();
        } else {
            responseLine();
        }
        state.controlSize = 0;
        return;
    }
    if (state.controlSize >= sizeof(state.control) - 1) {
        fail("[ERROR] HTTP/control line exceeds its buffer.");
        return;
    }
    state.control[state.controlSize++] = (char)byte;
    if (state.fileReadActive && !state.fileReadHeader && byte == ',' &&
        state.controlSize > 6 && !strncmp(state.control, "+FSRF:", 6)) {
        state.control[state.controlSize] = '\0';
        unsigned long count;
        int consumed = 0;
        if (sscanf(state.control + 6, " %lu,%n", &count, &consumed) != 1 ||
            consumed <= 0 || (size_t)consumed + 6 != state.controlSize ||
            count != state.fileReadExpected || count > sizeof(state.fileData)) {
            fail("[ERROR] Invalid binary file-read header.");
            return;
        }
        state.fileReadHeader = true;
        state.fileReadRemaining = (size_t)count;
        state.controlSize = 0;
    }
}
static void pump() {
    while (!state.blocked && ModemAT_rawAvailable() > 0) {
        int byte = ModemAT_rawRead();
        if (byte < 0) {
            break;
        }
        consume((uint8_t)byte);
    }
}
static void waitIdle(unsigned long duration);

static bool sendBytes(const uint8_t *bytes, size_t size, unsigned long timeout) {
    unsigned long startTime = millis();
    for (size_t offset = 0; offset < size; ++offset) {
        pump();
        if (state.blocked || state.finalError || state.closed) {
            return false;
        }
        if ((unsigned long)(millis() - startTime) >= timeout ||
            state.uart->write(bytes[offset]) != 1) {
            fail("[ERROR] UART send failed or timed out; no automatic retry.");
            return false;
        }
        // Match the board-verified TCPClient transmit path.
        state.uart->flush();
    }
    return true;
}
static bool start(const char *command, const char *prefix = NULL, bool immediate = false) {
    if (!immediate) {
        waitIdle(CONTROL_QUIET_MS);
    }
    pump();
    if (state.blocked || state.phase != HTTP_CONTROL || state.controlSize != 0) {
        return false;
    }
    state.finalOk = false;
    state.finalError = false;
    state.lastError[0] = '\0';
    state.prompt = false;
    state.prefix = prefix;
    state.value[0] = '\0';
    ModemAT_printLine("[TX command]");
    ModemAT_printLine(command);
    // A previous peer closure does not prevent a new control command.
    bool closed = state.closed;
    state.closed = false;
    bool sent = sendBytes((const uint8_t *)command, strlen(command), state.config->queryTimeoutMs) &&
        sendBytes((const uint8_t *)"\r\n", 2, state.config->queryTimeoutMs);
    state.closed = state.closed || closed;
    return sent;
}
static bool query(const char *command, const char *prefix, unsigned long timeout) {
    if (!start(command, prefix)) {
        return false;
    }
    unsigned long startTime = millis();
    while ((unsigned long)(millis() - startTime) < timeout) {
        pump();
        if (state.blocked || state.finalError) {
            if (state.lastError[0] != '\0') {
                ModemAT_printLine(state.lastError);
            }
            return false;
        }
        if (state.finalOk) {
            if (prefix != NULL && state.value[0] == '\0') {
                ModemAT_printLine("[ERROR] Final OK received without expected query data.");
                return false;
            }
            if (prefix != NULL) {
                ModemAT_printLine(prefix);
                ModemAT_printLine(state.value);
            }
            return true;
        }
        delay(1);
    }
    fail("[ERROR] AT response timeout; no further commands will be sent.");
    return false;
}
static void waitIdle(unsigned long duration) {
    unsigned long startTime = millis();
    while ((unsigned long)(millis() - startTime) < duration && !state.blocked) {
        pump();
        delay(1);
    }
}
bool ModemHttp_begin(const ModemHttpConfig *config, HardwareSerial &uart) {
    if (config == NULL || !validText(config->apn, 100) || !validText(config->host, 150) ||
        config->port == 0 || config->queryTimeoutMs == 0 || config->registerTimeoutMs == 0 ||
        config->linkTimeoutMs == 0 || config->setupTimeoutMs == 0 ||
        config->receiveTimeoutMs == 0 || config->pollIntervalMs == 0 ||
        config->closeTimeoutMs == 0 || !ModemAT_enterRawMode()) {
        return false;
    }
    state = {};
    state.config = config;
    state.uart = &uart;
    state.initialized = true;
    return true;
}
bool ModemHttp_beginTls(const ModemHttpConfig *config, const ModemHttpTlsConfig *tls,
    HardwareSerial &uart) {
    if (tls == NULL || !validText(tls->caFile, 100) ||
        strpbrk(tls->caFile, ",/\\") != NULL || tls->sslVersion != 3 ||
        !ModemHttp_begin(config, uart)) {
        return false;
    }
    state.tls = tls;
    return true;
}
static bool checkCertificate(const char *command, const char *expected) {
    const char *prefix = "+CERTCHECK:";
    if (!start(command, prefix)) {
        return false;
    }
    unsigned long startTime = millis();
    while ((unsigned long)(millis() - startTime) < state.config->queryTimeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        // Some firmware emits the terminal certificate token without CR/LF.
        if (state.value[0] == '\0' && state.controlSize > strlen(prefix) &&
            !strncmp(state.control, prefix, strlen(prefix)) &&
            (unsigned long)(millis() - state.lastRxMs) >= CERT_REPLY_QUIET_MS) {
            state.control[state.controlSize] = '\0';
            char *value = trim(state.control + strlen(prefix));
            if (!strcmp(value, expected) || !strcmp(value, "ERROR")) {
                consume('\n');
            }
        }
        if (state.value[0] != '\0') {
            ModemAT_printLine(prefix);
            ModemAT_printLine(state.value);
            if (strcmp(state.value, expected) != 0) {
                ModemAT_printLine("[ERROR] Stored CA certificate check failed.");
                return false;
            }
            // The named result is terminal. A separate final OK is optional.
            unsigned long settleStart = millis();
            while ((unsigned long)(millis() - settleStart) < CERT_REPLY_LIMIT_MS) {
                pump();
                if (state.blocked || state.finalError) {
                    return false;
                }
                if ((unsigned long)(millis() - state.lastRxMs) >= CERT_REPLY_QUIET_MS &&
                    state.controlSize == 0 && state.phase == HTTP_CONTROL) {
                    ModemAT_printLine("[STATE] Stored CA certificate confirmed.");
                    return true;
                }
                delay(1);
            }
            fail("[ERROR] Certificate reply boundary remains incomplete.");
            return false;
        }
        delay(1);
    }
    fail("[ERROR] CERTCHECK confirmation timeout; no further commands will be sent.");
    return false;
}
static bool configureTls() {
    if (state.tls == NULL) {
        return true;
    }
    ModemAT_printLine("[PREPARE] Stored CA certificate and HTTPS TLS settings");
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+CERTCHECK=\"%s\"", state.tls->caFile);
    if (!fits(written, sizeof(command))) {
        return false;
    }
    char expected[128];
    written = snprintf(expected, sizeof(expected), "%s,OK", state.tls->caFile);
    if (!fits(written, sizeof(expected)) || !checkCertificate(command, expected)) {
        return false;
    }
    // Inspect the clock; never replace it with a hard-coded test date.
    if (!query("AT+CCLK?", "+CCLK:", state.config->queryTimeoutMs)) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+HTTPSCFG=\"sslversion\",%u",
        state.tls->sslVersion);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs) ||
        !query("AT+HTTPSCFG=\"authmode\",1", NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+HTTPSCFG=\"cacert\",\"%s\"",
        state.tls->caFile);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs) ||
        !query("AT+HTTPSCFG?", "+HTTPSCFG:", state.config->queryTimeoutMs)) {
        return false;
    }
    written = snprintf(expected, sizeof(expected), "%u,1,%s,", state.tls->sslVersion,
        state.tls->caFile);
    if (!fits(written, sizeof(expected)) || strncmp(state.value, expected, strlen(expected))) {
        ModemAT_printLine("[ERROR] HTTPS TLS configuration readback does not match.");
        return false;
    }
    ModemAT_printLine("[STATE] Stored CA selected; TLS 1.2 server authentication enabled.");
    return true;
}
static int linkState() {
    unsigned int link;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    char extra;
    if (sscanf(state.value, " %u,%u.%u.%u.%u%c", &link, &a, &b, &c, &d, &extra) != 5 ||
        link > 1 || a > 255 || b > 255 || c > 255 || d > 255) {
        return -1;
    }
    return link == 1 && (a || b || c || d) ? 1 : 0;
}
static bool waitRegistration() {
    unsigned long startTime = millis();
    unsigned long elapsed = millis() - startTime;
    while (elapsed < state.config->registerTimeoutMs) {
        unsigned long remaining = state.config->registerTimeoutMs - elapsed;
        unsigned long timeout = remaining < state.config->queryTimeoutMs ?
            remaining : state.config->queryTimeoutMs;
        if (!query("AT+CEREG?", "+CEREG:", timeout)) {
            return false;
        }
        unsigned int mode;
        unsigned int status;
        char extra;
        if (sscanf(state.value, "%u,%u%c", &mode, &status, &extra) != 2) {
            ModemAT_printLine("[ERROR] Invalid CEREG response; registration unconfirmed.");
            return false;
        }
        if (status == 1 || status == 5) {
            ModemAT_printLine("[STATE] Network registration confirmed.");
            return true;
        }
        if (status == 3) {
            ModemAT_printLine("[ERROR] Network registration denied.");
            return false;
        }
        ModemAT_printLine("[NETWORK] Not registered yet; waiting before the next CEREG query.");
        elapsed = millis() - startTime;
        if (elapsed >= state.config->registerTimeoutMs) {
            break;
        }
        remaining = state.config->registerTimeoutMs - elapsed;
        waitIdle(remaining < state.config->pollIntervalMs ? remaining : state.config->pollIntervalMs);
        elapsed = millis() - startTime;
    }
    ModemAT_printLine("[ERROR] Network registration wait expired.");
    return false;
}
static bool waitIp() {
    unsigned long startTime = millis();
    while ((unsigned long)(millis() - startTime) < state.config->linkTimeoutMs) {
        if (!query("AT+XIIC?", "+XIIC:", state.config->queryTimeoutMs)) {
            return false;
        }
        int link = linkState();
        if (link < 0) {
            ModemAT_printLine("[ERROR] Invalid XIIC response; IP unconfirmed.");
            return false;
        }
        if (link == 1) {
            return true;
        }
        waitIdle(state.config->pollIntervalMs);
    }
    ModemAT_printLine("[ERROR] IP acquisition wait expired.");
    return false;
}
bool ModemHttp_prepareNetwork() {
    if (!state.initialized || state.blocked || state.mayBeOpen) {
        return false;
    }
    state.networkReady = false;
    ModemAT_printLine("[PREPARE] SIM readiness");
    if (!query("AT+CPIN?", "+CPIN:", state.config->queryTimeoutMs) ||
        strcmp(state.value, "READY") != 0) {
        ModemAT_printLine("[ERROR] SIM READY was not confirmed.");
        return false;
    }
    ModemAT_printLine("[PREPARE] APN configuration");
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
        state.config->apn);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    ModemAT_printLine("[PREPARE] LTE registration");
    if (!waitRegistration()) {
        return false;
    }
    ModemAT_printLine("[PREPARE] Data link and IP address");
    if (!query("AT+XIIC?", "+XIIC:", state.config->queryTimeoutMs)) {
        return false;
    }
    int link = linkState();
    if (link < 0) {
        ModemAT_printLine("[ERROR] Invalid XIIC response; activation not attempted.");
        return false;
    }
    if (link == 0) {
        if (!query("AT+XIIC=1", NULL, state.config->linkTimeoutMs)) {
            return false;
        }
        state.networkOwned = true;
        if (!waitIp()) {
            return false;
        }
    } else {
        state.networkOwned = true;
        ModemAT_printLine("[STATE] Existing IP is valid; XIIC activation skipped.");
    }
    ModemAT_printLine("[STATE] Data link connected, IP:");
    ModemAT_printLine(strchr(state.value, ',') + 1);
    if (!configureTls()) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+%sPARA=recvmode,0", protocolName());
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+%sPARA=keepalive,0", protocolName());
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    state.networkReady = true;
    ModemAT_printLine(state.tls == NULL ?
        "[STATE] Network ready; HTTP GET/POST test may start." :
        "[STATE] Network ready; HTTPS GET/POST test may start.");
    return true;
}
static bool request(const char *path, const char *body) {
    if (!state.initialized || !state.networkReady || state.blocked || state.mayBeOpen ||
        !validText(path, 100) || *path != '/') {
        return false;
    }
    size_t length = body == NULL ? 0 : strlen(body);
    if (body != NULL && (length == 0 || length > 2048)) {
        return false;
    }
    char command[COMMAND_BYTES];
    char action[64];
    int written = snprintf(command, sizeof(command), "AT+%sPARA=url,\"%s%s\"",
        protocolName(), state.config->host, path);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+%sPARA=port,%u",
        protocolName(), state.config->port);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    written = body == NULL ? snprintf(action, sizeof(action), "AT+%sACTION=0", protocolName()) :
        snprintf(action, sizeof(action), "AT+%sACTION=2,%u,2",
            protocolName(), (unsigned int)length);
    if (!fits(written, sizeof(action))) {
        return false;
    }
    state.phase = HTTP_CONTROL;
    state.responseStarted = state.complete = state.chunked = state.hasLength = false;
    state.status = 0;
    state.contentLength = state.remaining = 0;
    state.headerSize = state.bodySize = 0;
    state.headers[0] = state.body[0] = '\0';
    state.closed = false;
    state.mayBeOpen = true;
    written = snprintf(command, sizeof(command), "AT+%sSETUP", protocolName());
    if (!fits(written, sizeof(command)) ||
        !query(command, NULL, state.config->setupTimeoutMs) || state.closed) {
        return false;
    }
    // No delay or unrelated query between SETUP and ACTION.
    if (!start(action, NULL, true) || state.closed) {
        return false;
    }
    unsigned long startTime = millis();
    bool sent = body == NULL;
    while ((unsigned long)(millis() - startTime) < state.config->receiveTimeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            if (state.lastError[0] != '\0') {
                ModemAT_printLine(state.lastError);
            }
            return false;
        }
        if (!sent && state.prompt) {
            if (state.closed || !sendBytes((const uint8_t *)body, length,
                state.config->queryTimeoutMs)) {
                return false;
            }
            sent = true;
            ModemAT_printLine("[TX DATA] Exact POST body sent without extra CR/LF.");
        }
        if (state.complete && state.finalOk && sent) {
            return true;
        }
        if (state.closed && !state.complete) {
            ModemAT_printLine("[ERROR] HTTP connection closed before the response completed.");
            return false;
        }
        delay(1);
    }
    fail("[ERROR] HTTP request/response timeout; request will not be replayed.");
    return false;
}
bool ModemHttp_get(const char *path) {
    return request(path, NULL);
}
bool ModemHttp_post(const char *path, const char *body) {
    return request(path, body);
}
static bool decimalValue(unsigned long *number) {
    char *end = NULL;
    unsigned long value = strtoul(state.value, &end, 10);
    if (state.value[0] < '0' || state.value[0] > '9' || *end != '\0') {
        ModemAT_printLine("[ERROR] Invalid file size/free-space response.");
        return false;
    }
    *number = value;
    return true;
}
static bool listDownloadTarget(const char *fileName) {
    state.targetFile = fileName;
    state.targetExists = false;
    state.listInvalid = false;
    state.listingFiles = true;
    bool listed = query("AT+FSLIST?", NULL, state.config->queryTimeoutMs);
    state.listingFiles = false;
    if (!listed || state.listInvalid) {
        ModemAT_printLine("[FAIL] File listing was not confirmed.");
        return false;
    }
    return true;
}
static bool downloadPreflight(const char *fileName, unsigned long expectedBytes,
    bool removeExisting) {
    if (!listDownloadTarget(fileName)) {
        return false;
    }
    if (state.targetExists) {
        if (!removeExisting) {
            ModemAT_printLine("[STOP] Target already exists; removal is disabled.");
            return false;
        }
        ModemAT_printLine("[PREPARE] Removing only the configured download target:");
        ModemAT_printLine(fileName);
        char command[COMMAND_BYTES];
        int written = snprintf(command, sizeof(command), "AT+FSDF=\"%s\"", fileName);
        if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
            ModemAT_printLine("[FAIL] Target deletion failed; download not started.");
            return false;
        }
        if (!listDownloadTarget(fileName) || state.targetExists) {
            ModemAT_printLine("[FAIL] Target deletion unconfirmed; download not started.");
            return false;
        }
        ModemAT_printLine("[STATE] Previous target removed; absence confirmed.");
    } else {
        ModemAT_printLine("[STATE] Target is absent; deletion skipped.");
    }
    unsigned long freeBytes;
    if (!query("AT+FSLS?", "+FSLS:", state.config->queryTimeoutMs) ||
        !decimalValue(&freeBytes)) {
        return false;
    }
    if (freeBytes < expectedBytes + FILE_SPACE_MARGIN_BYTES) {
        ModemAT_printLine("[FAIL] Insufficient modem file space including the reserve margin.");
        return false;
    }
    ModemAT_printLine("[STATE] New target name and available storage confirmed.");
    return true;
}
static bool verifyDownloadedFile(const char *fileName, unsigned long expectedBytes,
    const char *pattern) {
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+FSFS=\"%s\"", fileName);
    unsigned long size;
    if (!fits(written, sizeof(command)) || !query(command, "+FSFS:", state.config->queryTimeoutMs) ||
        !decimalValue(&size) || size != expectedBytes) {
        ModemAT_printLine("[FAIL] Stored file size does not match the expected download size.");
        return false;
    }
    const size_t patternSize = pattern == NULL ? 0 : strlen(pattern);
    for (unsigned long offset = 0; offset < expectedBytes;) {
        size_t count = expectedBytes - offset > FILE_READ_BYTES ?
            FILE_READ_BYTES : (size_t)(expectedBytes - offset);
        written = offset == 0 ?
            snprintf(command, sizeof(command), "AT+FSRF=\"%s\",0,%u", fileName,
                (unsigned int)count) :
            snprintf(command, sizeof(command), "AT+FSRF=\"%s\",1,%u,%lu", fileName,
                (unsigned int)count, offset);
        if (!fits(written, sizeof(command))) {
            return false;
        }
        state.fileReadActive = true;
        state.fileReadHeader = false;
        state.fileReadExpected = count;
        state.fileReadCount = 0;
        state.fileReadRemaining = 0;
        bool read = query(command, NULL, state.config->queryTimeoutMs);
        state.fileReadActive = false;
        if (!read || !state.fileReadHeader || state.fileReadRemaining != 0 ||
            state.fileReadCount != count) {
            fail("[FAIL] File readback incomplete; remaining commands blocked.");
            return false;
        }
        for (size_t index = 0; index < count; ++index) {
            if (patternSize > 0 &&
                state.fileData[index] != (uint8_t)pattern[(offset + index) % patternSize]) {
                ModemAT_printLine("[FAIL] Stored content does not match the expected pattern.");
                return false;
            }
        }
        if (offset == 0) {
            char preview[FILE_PREVIEW_BYTES * 3 + 1];
            size_t previewSize = count < FILE_PREVIEW_BYTES ? count : FILE_PREVIEW_BYTES;
            for (size_t index = 0; index < previewSize; ++index) {
                snprintf(preview + index * 3, sizeof(preview) - index * 3, "%02X ",
                    (unsigned int)state.fileData[index]);
            }
            preview[previewSize * 3] = '\0';
            ModemAT_printLine("[FILE PREVIEW HEX] First stored bytes:");
            ModemAT_printLine(preview);
        }
        offset += count;
    }
    ModemAT_printLine(patternSize > 0 ?
        "[VERIFY PASS] Entire file read back; size and content pattern match." :
        "[VERIFY PASS] File size and full readback confirmed; no source-content comparison.");
    return true;
}
bool ModemHttp_download(const char *path, const char *fileName, unsigned long expectedBytes,
    const char *expectedPattern, unsigned long timeoutMs, bool removeExisting) {
    if (!state.initialized || !state.networkReady || state.blocked || state.tls != NULL ||
        state.mayBeOpen || !validText(path, 100) || path[0] != '/' ||
        !validText(fileName, 50) || strpbrk(fileName, ",/\\") != NULL ||
        !strcmp(fileName, ".") || !strcmp(fileName, "..") ||
        expectedBytes == 0 || expectedBytes > 524288 ||
        (expectedPattern != NULL && expectedPattern[0] == '\0') || timeoutMs == 0) {
        return false;
    }
    if (!downloadPreflight(fileName, expectedBytes, removeExisting)) {
        return false;
    }
    char command[COMMAND_BYTES];
    int written = snprintf(command, sizeof(command), "AT+HTTPPARA=url,\"%s%s\"",
        state.config->host, path);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+HTTPPARA=port,%u", state.config->port);
    if (!fits(written, sizeof(command)) || !query(command, NULL, state.config->queryTimeoutMs)) {
        return false;
    }
    char action[COMMAND_BYTES];
    written = snprintf(action, sizeof(action), "AT+FILEHTTPACTION=0,0,%lu,\"%s\"",
        expectedBytes, fileName);
    if (!fits(written, sizeof(action))) {
        return false;
    }
    state.closed = false;
    state.mayBeOpen = true;
    state.fileStatusSeen = false;
    state.fileStatusOk = false;
    state.fileErrorCode = 0;
    if (!query("AT+HTTPSETUP", NULL, state.config->setupTimeoutMs) || state.closed ||
        !start(action, NULL, true)) {
        return false;
    }
    unsigned long startTime = millis();
    while ((unsigned long)(millis() - startTime) < timeoutMs) {
        pump();
        if (state.fileStatusSeen && !state.fileStatusOk) {
            ModemAT_printLine(state.fileStatusLine);
            ModemAT_printLine(state.fileErrorCode == 1416 ?
                "[FAIL] Download RANGE was not satisfied (1416)." :
                "[FAIL] Modem reported a file-download failure.");
            ModemAT_printLine("[STATE] RX boundary unresolved; no cleanup, retry or deletion.");
            return false;
        }
        if (state.blocked || state.finalError) {
            return false;
        }
        if (state.fileStatusSeen && state.finalOk) {
            ModemAT_printLine(state.fileStatusLine);
            if (!state.fileStatusOk || state.downloadedBytes != expectedBytes) {
                ModemAT_printLine("[FAIL] Download result/byte count does not match.");
                return false;
            }
            ModemAT_printLine("[DOWNLOAD PASS] File transfer completed; checking stored data.");
            return ModemHttp_close() && verifyDownloadedFile(fileName, expectedBytes,
                expectedPattern);
        }
        delay(1);
    }
    fail("[FAIL] Download completion timeout; no automatic retry or deletion.");
    return false;
}
unsigned int ModemHttp_status() {
    return state.status;
}
const char *ModemHttp_body() {
    return state.body;
}
void ModemHttp_printResponse() {
    if (!state.complete) {
        ModemAT_printLine("[HTTP] Complete response unavailable.");
        return;
    }
    char log[96];
    snprintf(log, sizeof(log), "[HTTP STATUS] %u | body bytes=%lu", state.status,
        (unsigned long)state.bodySize);
    ModemAT_printLine(log);
    ModemAT_printLine("[HTTP HEADERS]");
    ModemAT_printLine(state.headers);
    ModemAT_printLine("[HTTP BODY]");
    ModemAT_printLine(state.body);
}
bool ModemHttp_close() {
    if (!state.initialized || state.blocked) {
        return false;
    }
    if (!state.mayBeOpen) {
        return true;
    }
    waitIdle(state.config->closeTimeoutMs);
    if (state.closed) {
        return true;
    }
    if (state.phase != HTTP_CONTROL) {
        return false;
    }
    char command[32];
    int written = snprintf(command, sizeof(command), "AT+%sCLOSE", protocolName());
    if (!fits(written, sizeof(command)) ||
        !query(command, NULL, state.config->closeTimeoutMs)) {
        return false;
    }
    state.mayBeOpen = false;
    state.closed = true;
    return true;
}
bool ModemHttp_releaseIp() {
    if (!state.initialized || state.blocked || state.mayBeOpen) {
        return false;
    }
    if (!state.networkOwned) {
        return true;
    }
    if (!query("AT+XIIC=0", NULL, state.config->linkTimeoutMs) ||
        !query("AT+XIIC?", "+XIIC:", state.config->queryTimeoutMs)) {
        return false;
    }
    unsigned int link;
    char ip[48];
    if (sscanf(state.value, " %u,%47s", &link, ip) != 2 || link != 0 || strcmp(ip, "0.0.0.0")) {
        return false;
    }
    state.networkOwned = false;
    state.networkReady = false;
    return true;
}
void ModemHttp_pollIdle() {
    if (state.initialized) {
        pump();
    } else {
        ModemAT_pollIdle();
    }
}
