#include "ModemSocket.h"
#include "ModemAT.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t CONTROL_LINE_BYTES = 192;
static const size_t RECEIVE_BUFFER_BYTES = 4096;
static const size_t COMMAND_BUFFER_BYTES = 256;
static const size_t READ_TRACE_BYTES = 128;
static const size_t TRACE_ROW_BYTES = 16;
static const unsigned long CONTROL_QUIET_MS = 100;
static const unsigned int MIN_CLOCK_YEAR = 24;
static const unsigned long MAX_FRAME_BYTES = 4096;

struct SocketState {
    const ModemSocketConfig *config;
    HardwareSerial *uart;
    char line[CONTROL_LINE_BYTES];
    size_t lineLength;
    char value[CONTROL_LINE_BYTES];
    const char *prefix;
    uint8_t received[RECEIVE_BUFFER_BYTES];
    size_t receivedLength;
    unsigned int receiveFrames;
    bool linkOwned;
    unsigned long frameRemaining;
    bool ignoreFrame;
    bool blocked;
    bool finalOk;
    bool finalError;
    bool prompt;
    bool setupOk;
    bool setupError;
    bool sendOk;
    bool sendError;
    bool closeOk;
    bool remoteClosed;
    bool socketOpen;
    bool bufferedReceive;
    bool receivePending;
    bool readActive;
    bool readHeader;
    bool readSocketClosed;
    bool listingSockets;
    bool targetListed;
    bool closeLogPending;
    bool remoteCloseLogPending;
    char closeLine[CONTROL_LINE_BYTES];
    size_t readLimit;
    uint8_t readTrace[READ_TRACE_BYTES];
    size_t readTraceCount;
};

static SocketState state = {};
static bool socketQuietMode = false;
static unsigned long greetingOnOpenTimeoutMs = 0;

// Queue diagnostics while receiving; never print from the byte/line parser.
static const size_t DEFERRED_LOG_BYTES = 2048;
static char deferredLog[DEFERRED_LOG_BYTES];
static size_t deferredLogLength = 0;
static bool deferredLogOverflow = false;
static bool receiveWaitActive = false;
static bool parsingInput = false;
static bool diagnosticDumpActive = false;

static void flushSocketLogs() {
    if (deferredLogLength > 0) {
        deferredLog[deferredLogLength] = '\0';
        ModemAT_printLine(deferredLog);
        deferredLogLength = 0;
    }
    if (deferredLogOverflow) {
        ModemAT_printLine("[DIAG] Deferred socket log truncated; receive data unchanged.");
        deferredLogOverflow = false;
    }
}

static void socketLog(const char *text) {
    if (diagnosticDumpActive || (!receiveWaitActive && !parsingInput &&
        state.lineLength == 0 && state.frameRemaining == 0)) {
        flushSocketLogs();
        ModemAT_printLine(text);
        return;
    }
    size_t length = strlen(text);
    if (length + 2 > sizeof(deferredLog) - deferredLogLength) {
        deferredLogOverflow = true;
        return;
    }
    memcpy(deferredLog + deferredLogLength, text, length);
    deferredLogLength += length;
    deferredLog[deferredLogLength++] = '\n';
    deferredLog[deferredLogLength] = '\0';
}


static const size_t GREETING_TRACE_BYTES = 512;
static bool greetingDiagnostics = false;
static uint8_t greetingTrace[GREETING_TRACE_BYTES];
static unsigned long greetingRawBytes = 0;
static unsigned long greetingLastRxMs = 0;
static unsigned long greetingReadStartBytes = 0;
static char greetingLastControl[CONTROL_LINE_BYTES];
static const size_t GREETING_CONTROL_TRACE_BYTES = 512;
static char greetingControlTrace[GREETING_CONTROL_TRACE_BYTES];
static size_t greetingControlLength = 0;
static bool greetingControlTruncated = false;

static void printGreetingDiagnostics(const char *reason) {
    if (!greetingDiagnostics || receiveWaitActive) {
        return;
    }
    // Freeze before printing. This API is disabled before any application send.
    greetingDiagnostics = false;
    diagnosticDumpActive = true;
    socketLog(reason);
    char log[192];
    snprintf(log, sizeof(log),
        "[GREETING DIAG] rawSinceOpen=%lu, rawDuringRead=%lu, payload=%lu, frames=%u",
        greetingRawBytes, greetingRawBytes - greetingReadStartBytes,
        (unsigned long)state.receivedLength, state.receiveFrames);
    socketLog(log);
    snprintf(log, sizeof(log),
        "[GREETING DIAG] socketOpen=%u, remoteClosed=%u, blocked=%u, finalError=%u, overflow=%u",
        state.socketOpen ? 1U : 0U, state.remoteClosed ? 1U : 0U,
        state.blocked ? 1U : 0U, state.finalError ? 1U : 0U,
        ModemAT_rawOverflowed() ? 1U : 0U);
    socketLog(log);
    snprintf(log, sizeof(log),
        "[GREETING DIAG] partialControl=%lu, frameRemaining=%lu, ignoredFrame=%u",
        (unsigned long)state.lineLength, state.frameRemaining, state.ignoreFrame ? 1U : 0U);
    socketLog(log);
    if (greetingRawBytes > 0) {
        snprintf(log, sizeof(log), "[GREETING DIAG] Last raw byte %lu ms ago.",
            millis() - greetingLastRxMs);
        socketLog(log);
    }
    if (greetingLastControl[0] != '\0') {
        socketLog("[GREETING LAST CONTROL]");
        socketLog(greetingLastControl);
    }
    socketLog("[GREETING CONTROL LINES]");
    socketLog(greetingControlLength > 0 ? greetingControlTrace : "(none)");
    if (greetingControlTruncated) {
        socketLog("[GREETING DIAG] Control trace truncated.");
    }
    if (state.lineLength > 0) {
        socketLog("[GREETING PARTIAL HEX]");
        for (size_t i = 0; i < state.lineLength; i += TRACE_ROW_BYTES) {
            size_t used = 0;
            for (size_t j = i; j < state.lineLength && j < i + TRACE_ROW_BYTES; ++j) {
                int written = snprintf(log + used, sizeof(log) - used, "%02X ",
                    (unsigned int)(uint8_t)state.line[j]);
                if (written < 0 || (size_t)written >= sizeof(log) - used) {
                    break;
                }
                used += (size_t)written;
            }
            socketLog(log);
        }
    }
    size_t count = greetingRawBytes < GREETING_TRACE_BYTES ?
        (size_t)greetingRawBytes : GREETING_TRACE_BYTES;
    unsigned long first = greetingRawBytes - count;
    socketLog("[GREETING WIRE HEX] Last pre-authentication modem bytes:");
    for (size_t offset = 0; offset < count; offset += TRACE_ROW_BYTES) {
        size_t used = 0;
        for (size_t i = offset; i < count && i < offset + TRACE_ROW_BYTES; ++i) {
            int written = snprintf(log + used, sizeof(log) - used, "%02X ",
                (unsigned int)greetingTrace[(first + i) % GREETING_TRACE_BYTES]);
            if (written < 0 || (size_t)written >= sizeof(log) - used) {
                break;
            }
            used += (size_t)written;
        }
        socketLog(log);
    }
    diagnosticDumpActive = false;
}


// Capture only modem control lines during SETUP, never SMTP payload or credentials.
static const size_t SETUP_TRACE_BYTES = 1024;
static char setupTrace[SETUP_TRACE_BYTES];
static size_t setupTraceLength = 0;
static bool setupTraceActive = false;
static bool setupTraceTruncated = false;

static void captureSetupLine(const char *line) {
    if (!setupTraceActive) {
        return;
    }
    size_t length = strlen(line);
    if (setupTraceLength + length + 2 > sizeof(setupTrace)) {
        setupTraceTruncated = true;
        return;
    }
    memcpy(setupTrace + setupTraceLength, line, length);
    setupTraceLength += length;
    setupTrace[setupTraceLength++] = '\n';
    setupTrace[setupTraceLength] = '\0';
}

static void printSetupDiagnostics(const char *reason, unsigned long elapsedMs) {
    setupTraceActive = false;
    if (socketQuietMode && state.finalOk && state.setupOk && !state.blocked &&
        !state.finalError && !state.setupError) {
        return;
    }
    socketLog(reason);
    char log[160];
    snprintf(log, sizeof(log),
        "[SETUP DIAG] elapsed=%lu ms, finalOK=%u, finalError=%u, setupOK=%u, setupError=%u",
        elapsedMs, state.finalOk ? 1U : 0U, state.finalError ? 1U : 0U,
        state.setupOk ? 1U : 0U, state.setupError ? 1U : 0U);
    socketLog(log);
    snprintf(log, sizeof(log),
        "[SETUP DIAG] blocked=%u, remoteClosed=%u, partialBytes=%lu, frameRemaining=%lu",
        state.blocked ? 1U : 0U, state.remoteClosed ? 1U : 0U,
        (unsigned long)state.lineLength, state.frameRemaining);
    socketLog(log);
    socketLog("[SETUP RX] Captured modem control lines:");
    if (setupTraceLength > 0) {
        socketLog(setupTrace);
    } else {
        socketLog("[SETUP RX] No complete control line captured.");
    }
    if (state.lineLength > 0) {
        socketLog("[SETUP RX PARTIAL] Unterminated control bytes (HEX):");
        for (size_t offset = 0; offset < state.lineLength; offset += TRACE_ROW_BYTES) {
            size_t used = 0;
            for (size_t i = offset; i < state.lineLength && i < offset + TRACE_ROW_BYTES; ++i) {
                int written = snprintf(log + used, sizeof(log) - used, "%02X ",
                    (unsigned int)(uint8_t)state.line[i]);
                if (written < 0 || (size_t)written >= sizeof(log) - used) {
                    break;
                }
                used += (size_t)written;
            }
            socketLog(log);
        }
    }
    if (setupTraceTruncated) {
        socketLog("[SETUP DIAG] Control trace truncated; parser state unchanged.");
    }
}


static const char *protocolName() {
    switch (state.config->protocol) {
        case MODEM_SOCKET_TCP:
            return "TCP";
        case MODEM_SOCKET_UDP:
            return "UDP";
        default:
            return "SSLTCP";
    }
}

static bool validSetting(const char *text, size_t maximum) {
    return text != NULL && strlen(text) > 0 && strlen(text) <= maximum &&
        strpbrk(text, "\"\r\n") == NULL;
}

static void failBoundary() {
    state.blocked = true;
}

static bool closeResultMatches(const char *value, const char *expected) {
    size_t index = 0;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        char character = value[i];
        if (character == ' ' || character == '\t') {
            continue;
        }
        if (character >= 'a' && character <= 'z') {
            character -= 'a' - 'A';
        }
        if (expected[index] == '\0' || character != expected[index]) {
            return false;
        }
        ++index;
    }
    return expected[index] == '\0';
}

static void controlLine() {
    state.line[state.lineLength] = '\0';
    const char *line = state.line;
    if (greetingDiagnostics) {
        strcpy(greetingLastControl, line);
        size_t length = strlen(line);
        if (length + 2 <= sizeof(greetingControlTrace) - greetingControlLength) {
            memcpy(greetingControlTrace + greetingControlLength, line, length);
            greetingControlLength += length;
            greetingControlTrace[greetingControlLength++] = '\n';
            greetingControlTrace[greetingControlLength] = '\0';
        } else {
            greetingControlTruncated = true;
        }
    }
    captureSetupLine(line);
    if (!strcmp(line, "OK")) {
        state.finalOk = true;
    } else if (!strcmp(line, "ERROR") || !strncmp(line, "+CME ERROR", 10) ||
        !strncmp(line, "+CMS ERROR", 10)) {
        state.finalError = true;
    }
    if (state.prefix != NULL && !strncmp(line, state.prefix, strlen(state.prefix))) {
        const char *value = line + strlen(state.prefix);
        while (*value == ' ') {
            ++value;
        }
        if (strlen(value) >= sizeof(state.value)) {
            failBoundary();
        } else {
            strcpy(state.value, value);
        }
    }
    char readPrefix[40];
    snprintf(readPrefix, sizeof(readPrefix), "+%sREAD:", protocolName());
    if (state.readActive && !strncmp(line, readPrefix, strlen(readPrefix))) {
        const char *reason = line + strlen(readPrefix);
        while (*reason == ' ') {
            ++reason;
        }
        unsigned int readId;
        unsigned long readLength;
        char extra;
        if (sscanf(reason, "%u,%lu%c", &readId, &readLength, &extra) == 2 &&
            readId == state.config->socketId && readLength == 0 && !state.readHeader) {
            state.readHeader = true;
        }
        if (!strcmp(reason, "SOCKET ID OPEN FAILED") &&
            state.config->protocol == MODEM_SOCKET_TLS && !state.readHeader &&
            state.receivedLength == 0 && state.frameRemaining == 0) {
            // Defer classification until the whole pump has processed any close URC.
            state.readSocketClosed = true;
            state.lineLength = 0;
            return;
        }
        if (*reason < '0' || *reason > '9') {
            socketLog("[READ ERROR] Modem reported:");
            socketLog(line);
            state.finalError = true;
        }
    }
    char prefix[40];
    unsigned int id;
    if (state.bufferedReceive) {
        snprintf(prefix, sizeof(prefix), "+%sRECV:", protocolName());
        if (!strncmp(line, prefix, strlen(prefix))) {
            char extra;
            if (sscanf(line + strlen(prefix), " %u%c", &id, &extra) != 1) {
                failBoundary();
            } else if (id == state.config->socketId) {
                state.receivePending = true;
                if (!socketQuietMode) {
                    socketLog("[URC] Socket data available; READ deferred to recv().");
                }
            }
        }
    }
    char result[64];
    if (state.listingSockets && !strncmp(line, "+SSLTCPSETUP:", 13)) {
        unsigned int port;
        unsigned int mode;
        char host[101];
        char extra;
        if (sscanf(line + 13, " %u,%100[^,],%u,%u%c", &id, host, &port, &mode,
            &extra) != 4 || id > 5 || port == 0 || port > 65535 || mode > 1) {
            state.finalError = true;
        } else if (id == state.config->socketId) {
            state.targetListed = true;
        }
        state.lineLength = 0;
        return;
    }
    snprintf(prefix, sizeof(prefix), "+%sSETUP:", protocolName());
    if (!strncmp(line, prefix, strlen(prefix))) {
        if (sscanf(line + strlen(prefix), " %u,%63[^\r\n]", &id, result) == 2 &&
            id == state.config->socketId) {
            state.setupOk = !strcmp(result, "OK");
            state.setupError = !state.setupOk;
            state.socketOpen = state.setupOk;
        } else {
            state.setupError = true;
        }
    }
    snprintf(prefix, sizeof(prefix), "+%sSEND:", protocolName());
    if (!strncmp(line, prefix, strlen(prefix))) {
        if (sscanf(line + strlen(prefix), " %u,%63[^\r\n]", &id, result) == 2 &&
            id == state.config->socketId) {
            if (state.config->protocol == MODEM_SOCKET_TLS) {
                state.sendOk = !strcmp(result, "OK");
            } else {
                char *end;
                unsigned long count = strtoul(result, &end, 10);
                state.sendOk = end != result && *end == '\0' &&
                    count == strlen(state.config->payload);
            }
            state.sendError = !state.sendOk;
        } else {
            state.sendError = true;
        }
    }
    snprintf(prefix, sizeof(prefix), "+%sCLOSE:", protocolName());
    if (!strncmp(line, prefix, strlen(prefix))) {
        strcpy(state.closeLine, line);
        state.closeLogPending = true;
        if (state.config->protocol == MODEM_SOCKET_TLS) {
            const char *value = line + strlen(prefix);
            while (*value == ' ') {
                ++value;
            }
            char *end;
            unsigned long closeId = strtoul(value, &end, 10);
            while (*end == ' ') {
                ++end;
            }
            if (*value < '0' || *value > '9' || end == value ||
                (*end != '\0' && *end != ',')) {
                state.finalError = true;
            } else if (closeId == state.config->socketId) {
                const char *description = *end == ',' ? end + 1 : end;
                while (*description == ' ') {
                    ++description;
                }
                // The complete socket ID identifies the URC; its text is diagnostic only.
                if (!strncmp(description, "ERROR", 5) ||
                    !strncmp(description, "FAIL", 4)) {
                    state.finalError = true;
                } else {
                    bool remote = !closeResultMatches(description, "OK");
                    state.closeOk = true;
                    state.socketOpen = false;
                    state.remoteClosed = state.remoteClosed || remote;
                    state.remoteCloseLogPending = state.remoteCloseLogPending || remote;
                }
            }
        } else if (sscanf(line + strlen(prefix), " %u , %63[^\r\n]", &id, result) == 2) {
            if (id == state.config->socketId) {
                bool remote = closeResultMatches(result, "LINKCLOSED");
                if (remote || closeResultMatches(result, "OK")) {
                    state.closeOk = true;
                    state.socketOpen = false;
                    // Keep the peer-close event latched until a new socket is opened.
                    state.remoteClosed = state.remoteClosed || remote;
                    state.remoteCloseLogPending = state.remoteCloseLogPending || remote;
                } else {
                    state.finalError = true;
                }
            }
        } else {
            state.finalError = true;
        }
    }
    if (!strcmp(line, "GPRS DISCONNECTION")) {
        state.finalError = true;
    }
    state.lineLength = 0;
}

static void receiveByte(uint8_t byte) {
    if (state.readActive) {
        state.readTrace[state.readTraceCount % READ_TRACE_BYTES] = byte;
        ++state.readTraceCount;
    }
    if (state.frameRemaining > 0) {
        --state.frameRemaining;
        if (!state.ignoreFrame) {
            if (state.receivedLength >= sizeof(state.received)) {
                failBoundary();
            } else {
                state.received[state.receivedLength++] = byte;
            }
        }
        return;
    }
    if (byte == '>' && state.lineLength == 0) {
        state.prompt = true;
        return;
    }
    if ((byte == ' ' || byte == '\t') && state.lineLength == 0) {
        return;
    }
    if (byte == '\r' || byte == '\n') {
        if (state.lineLength > 0) {
            controlLine();
        }
        return;
    }
    if (state.lineLength + 1 >= sizeof(state.line)) {
        failBoundary();
        return;
    }
    state.line[state.lineLength++] = (char)byte;
    state.line[state.lineLength] = '\0';
    if (byte == ',') {
        char prefix[40];
        snprintf(prefix, sizeof(prefix), state.bufferedReceive ? "+%sREAD:" : "+%sRECV:",
            protocolName());
        if (!strncmp(state.line, prefix, strlen(prefix))) {
            unsigned int id;
            unsigned long length;
            int consumed = 0;
            if (sscanf(state.line + strlen(prefix), " %u,%lu,%n",
                &id, &length, &consumed) == 2 && consumed > 0 &&
                (size_t)consumed + strlen(prefix) == state.lineLength) {
                if (state.bufferedReceive &&
                    (!state.readActive || state.readHeader ||
                        id != state.config->socketId || length > state.readLimit)) {
                    failBoundary();
                    return;
                }
                if (state.bufferedReceive) {
                    state.readHeader = true;
                }
                if (length > MAX_FRAME_BYTES) {
                    failBoundary();
                    return;
                }
                if (id == state.config->socketId) {
                    ++state.receiveFrames;
                }
                state.frameRemaining = length;
                state.ignoreFrame = id != state.config->socketId;
                state.lineLength = 0;
            }
        }
    }
}

static void pump() {
    if (ModemAT_rawOverflowed()) {
        failBoundary();
        return;
    }
    while (!state.blocked && ModemAT_rawAvailable() > 0) {
        int value = ModemAT_rawRead();
        if (value < 0) {
            break;
        }
        if (greetingDiagnostics) {
            greetingTrace[greetingRawBytes % GREETING_TRACE_BYTES] = (uint8_t)value;
            ++greetingRawBytes;
            greetingLastRxMs = millis();
        }
        parsingInput = true;
        receiveByte((uint8_t)value);
        parsingInput = false;
    }
    // Drain available UART input before potentially blocking diagnostic output.
    if (state.closeLogPending) {
        state.closeLogPending = false;
        socketLog("[CLOSE RX]");
        socketLog(state.closeLine);
    }
    if (state.remoteCloseLogPending) {
        state.remoteCloseLogPending = false;
        socketLog("[URC] Remote socket closure confirmed.");
    }
    if (!receiveWaitActive && state.lineLength == 0 && state.frameRemaining == 0) {
        flushSocketLogs();
    }
}

void ModemSocket_pollIdle() {
    if (state.config != NULL && ModemAT_isRawMode()) {
        pump();
    } else {
        ModemAT_pollIdle();
    }
}

static void waitIdle(unsigned long durationMs) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < durationMs) {
        pump();
        delay(1);
    }
}

static bool sendBytes(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        pump();
        if (state.blocked || state.uart->write(data[i]) != 1) {
            failBoundary();
            return false;
        }
        state.uart->flush();
    }
    return true;
}

static bool startCommand(const char *command, const char *prefix, bool requireOpen = false) {
    waitIdle(CONTROL_QUIET_MS);
    if (state.blocked || state.frameRemaining > 0 || state.lineLength > 0) {
        failBoundary();
        return false;
    }
    if (requireOpen && !state.socketOpen) {
        return true;
    }
    state.prefix = prefix;
    state.value[0] = '\0';
    state.finalOk = false;
    state.finalError = false;
    state.prompt = false;
    if (!socketQuietMode) {
        socketLog("[TX command]");
        socketLog(command);
    }
    return sendBytes((const uint8_t *)command, strlen(command)) &&
        sendBytes((const uint8_t *)"\r\n", 2);
}

static bool query(const char *command, const char *prefix, unsigned long timeoutMs,
    bool lineOnly = false) {
    if (!startCommand(command, prefix)) {
        return false;
    }
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < timeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        bool dataReady = prefix == NULL || state.value[0] != '\0';
        if (dataReady && (state.finalOk || lineOnly)) {
            waitIdle(CONTROL_QUIET_MS);
            if (prefix != NULL && !socketQuietMode) {
                socketLog(state.value);
            }
            return !state.blocked && !state.finalError;
        }
        delay(1);
    }
    failBoundary();
    socketLog("[ERROR] Command timeout; no further commands will be sent.");
    return false;
}

static bool commandFits(int written, size_t capacity) {
    if (written < 0 || (size_t)written >= capacity) {
        socketLog("[ERROR] Command buffer capacity exceeded.");
        return false;
    }
    return true;
}

static bool prepareNetwork() {
    const ModemSocketConfig *config = state.config;
    char command[COMMAND_BUFFER_BYTES];
    if (!query("AT+CPIN?", "+CPIN:", config->queryTimeoutMs) ||
        strcmp(state.value, "READY")) {
        return false;
    }
    int written = snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
        config->apn);
    if (!commandFits(written, sizeof(command)) ||
        !query(command, NULL, config->queryTimeoutMs)) {
        return false;
    }
    unsigned long start = millis();
    bool registered = false;
    while ((unsigned long)(millis() - start) < config->registerTimeoutMs) {
        if (!query("AT+CEREG?", "+CEREG:", config->queryTimeoutMs)) {
            return false;
        }
        unsigned int mode;
        unsigned int status;
        if (sscanf(state.value, "%u,%u", &mode, &status) != 2) {
            return false;
        }
        if (status == 1 || status == 5) {
            registered = true;
            break;
        }
        if (status == 3) {
            return false;
        }
        waitIdle(config->pollIntervalMs);
    }
    if (!registered || !query("AT+XIIC=1", NULL, config->linkTimeoutMs)) {
        return false;
    }
    state.linkOwned = true;
    start = millis();
    bool active = false;
    while ((unsigned long)(millis() - start) < config->linkTimeoutMs) {
        if (!query("AT+XIIC?", "+XIIC:", config->queryTimeoutMs)) {
            return false;
        }
        unsigned int link;
        unsigned int a;
        unsigned int b;
        unsigned int c;
        unsigned int d;
        if (sscanf(state.value, "%u,%u.%u.%u.%u", &link, &a, &b, &c, &d) != 5) {
            return false;
        }
        if (link == 1 && a <= 255 && b <= 255 && c <= 255 && d <= 255 &&
            (a || b || c || d)) {
            active = true;
            break;
        }
        waitIdle(config->pollIntervalMs);
    }
    return active && query("AT+RECVMODE=1", NULL, config->queryTimeoutMs);
}

static bool configureTls() {
    const ModemSocketConfig *config = state.config;
    if (!query("AT+CCLK?", "+CCLK:", config->queryTimeoutMs)) {
        return false;
    }
    unsigned int year;
    unsigned int month;
    unsigned int day;
    if (sscanf(state.value, "\"%u/%u/%u", &year, &month, &day) != 3 ||
        year < MIN_CLOCK_YEAR || year > 99 || month < 1 || month > 12 || day < 1 || day > 31) {
        socketLog("[ERROR] Set the modem clock before TLS; clock was not changed.");
        return false;
    }
    char command[COMMAND_BUFFER_BYTES];
    int written = snprintf(command, sizeof(command), "AT+CERTCHECK=\"%s\"", config->caFile);
    if (!commandFits(written, sizeof(command)) ||
        !query(command, "+CERTCHECK:", config->queryTimeoutMs, true)) {
        return false;
    }
    char expected[CONTROL_LINE_BYTES];
    written = snprintf(expected, sizeof(expected), "%s,OK", config->caFile);
    if (!commandFits(written, sizeof(expected)) || strcmp(state.value, expected)) {
        socketLog("[ERROR] Required CA certificate is not registered.");
        return false;
    }
    written = snprintf(command, sizeof(command), "AT+SSLTCPCFGA=3,1,\"%s\",\"\",\"\"",
        config->caFile);
    return commandFits(written, sizeof(command)) &&
        query(command, NULL, config->queryTimeoutMs);
}

static bool openSocket() {
    setupTraceLength = 0;
    setupTrace[0] = '\0';
    setupTraceTruncated = false;
    setupTraceActive = true;
    unsigned long start = millis();
    state.setupOk = false;
    state.setupError = false;
    state.remoteClosed = false;
    state.closeOk = false;
    char command[COMMAND_BUFFER_BYTES];
    const ModemSocketConfig *config = state.config;
    int written = snprintf(command, sizeof(command), "AT+%sSETUP=%u,\"%s\",%u%s",
        protocolName(), config->socketId, config->host, config->port,
        config->protocol == MODEM_SOCKET_TLS ? ",0" : "");
    if (!commandFits(written, sizeof(command)) || !startCommand(command, NULL)) {
        printSetupDiagnostics("[SETUP FAIL] Command could not be started.", millis() - start);
        return false;
    }
    start = millis();
    bool setupConfirmed = false;
    unsigned long greetingStart = 0;
    for (;;) {
        pump();
        if (state.blocked || state.finalError || state.setupError || state.remoteClosed) {
            printSetupDiagnostics("[SETUP FAIL] Modem error or unresolved RX boundary.",
                millis() - start);
            return false;
        }
        if (state.finalOk && state.setupOk) {
            if (!setupConfirmed) {
                setupConfirmed = true;
                greetingStart = millis();
            }
            if (state.lineLength == 0 && state.frameRemaining == 0 &&
                (greetingOnOpenTimeoutMs == 0 || state.receivePending)) {
                printSetupDiagnostics("[SETUP PASS] Command OK and asynchronous setup OK.",
                    millis() - start);
                if (greetingOnOpenTimeoutMs > 0) {
                    socketLog("[STATE] Setup and first receive notification captured together.");
                }
                if (!socketQuietMode) {
                    socketLog("[STATE] Socket setup confirmed by asynchronous result.");
                }
                return true;
            }
        }
        if (setupConfirmed && greetingOnOpenTimeoutMs > 0) {
            if ((unsigned long)(millis() - greetingStart) >= greetingOnOpenTimeoutMs) {
                socketLog("[GREETING TIMEOUT] TLS connected; first receive notification missing.");
                if (state.lineLength > 0 || state.frameRemaining > 0) {
                    failBoundary();
                }
                return false;
            }
        } else if ((unsigned long)(millis() - start) >= config->socketTimeoutMs) {
            failBoundary();
            printSetupDiagnostics("[SETUP TIMEOUT] Required final OK/setup result not confirmed.",
                millis() - start);
            return false;
        }
        delay(1);
    }
}

static bool sendPayload() {
    const ModemSocketConfig *config = state.config;
    char command[COMMAND_BUFFER_BYTES];
    size_t size = strlen(config->payload);
    state.sendOk = false;
    state.sendError = false;
    int written = snprintf(command, sizeof(command), "AT+%sSEND=%u,%lu",
        protocolName(), config->socketId, (unsigned long)size);
    if (!commandFits(written, sizeof(command)) || !startCommand(command, NULL)) {
        return false;
    }
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < config->socketTimeoutMs) {
        pump();
        if (state.blocked || state.finalError || state.sendError) {
            return false;
        }
        if (state.prompt) {
            break;
        }
        delay(1);
    }
    if (!state.prompt) {
        failBoundary();
        return false;
    }
    if (!sendBytes((const uint8_t *)config->payload, size)) {
        return false;
    }
    while ((unsigned long)(millis() - start) < config->socketTimeoutMs) {
        pump();
        if (state.blocked || state.finalError || state.sendError) {
            return false;
        }
        if (state.sendOk && (state.finalOk || config->protocol == MODEM_SOCKET_TLS)) {
            return true;
        }
        delay(1);
    }
    failBoundary();
    return false;
}

static bool httpComplete() {
    if (state.receivedLength >= sizeof(state.received)) {
        failBoundary();
        return false;
    }
    state.received[state.receivedLength] = '\0';
    const char *response = (const char *)state.received;
    const char *end = strstr(response, "\r\n\r\n");
    if (end == NULL) {
        return false;
    }
    const char *length = strstr(response, "\r\nContent-Length:");
    if (length == NULL || length > end) {
        return false;
    }
    length += strlen("\r\nContent-Length:");
    char *numberEnd;
    unsigned long expected = strtoul(length, &numberEnd, 10);
    if (numberEnd == length || strncmp(numberEnd, "\r\n", 2)) {
        return false;
    }
    size_t headers = (size_t)(end + 4 - response);
    return expected > 0 && expected <= sizeof(state.received) - headers &&
        state.receivedLength >= headers + expected &&
        (!strncmp(response, "HTTP/1.1 200 ", 13) || !strncmp(response, "HTTP/1.0 200 ", 13));
}

static bool receiveResponse() {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < state.config->receiveTimeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        if (state.config->protocol == MODEM_SOCKET_TLS) {
            if (state.frameRemaining == 0 && httpComplete()) {
                socketLog("[HTTP] 200 response and Content-Length body confirmed.");
                return true;
            }
        } else {
            size_t expected = strlen(state.config->payload);
            if (state.receivedLength >= expected && state.frameRemaining == 0) {
                bool framingOk = state.config->protocol != MODEM_SOCKET_UDP ||
                    state.receiveFrames == 1;
                return framingOk && state.receivedLength == expected &&
                    !memcmp(state.received, state.config->payload, expected);
            }
        }
        if (state.remoteClosed) {
            return false;
        }
        delay(1);
    }
    if (state.frameRemaining > 0 || state.lineLength > 0) {
        failBoundary();
    }
    socketLog("[ERROR] Expected response not completed within the receive timeout.");
    return false;
}

static bool closeSocket() {
    if (!state.socketOpen) {
        return true;
    }
    state.closeOk = false;
    char command[COMMAND_BUFFER_BYTES];
    int written = snprintf(command, sizeof(command), "AT+%sCLOSE=%u",
        protocolName(), state.config->socketId);
    if (!commandFits(written, sizeof(command)) || !startCommand(command, NULL, true)) {
        return false;
    }
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < state.config->socketTimeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        if (state.closeOk) {
            waitIdle(CONTROL_QUIET_MS);
            return !state.blocked;
        }
        delay(1);
    }
    failBoundary();
    return false;
}

bool ModemSocket_run(const ModemSocketConfig *config, HardwareSerial &uart) {
    if (config == NULL ||
        (config->protocol != MODEM_SOCKET_TCP && config->protocol != MODEM_SOCKET_UDP &&
            config->protocol != MODEM_SOCKET_TLS) || !validSetting(config->apn, 100) ||
        !validSetting(config->host, 100) || config->payload == NULL ||
        strlen(config->payload) == 0 || strlen(config->payload) > 512 ||
        config->port == 0 || config->socketId > 5 || config->queryTimeoutMs == 0 ||
        config->registerTimeoutMs == 0 || config->linkTimeoutMs == 0 ||
        config->socketTimeoutMs == 0 || config->receiveTimeoutMs == 0) {
        return false;
    }
    if (config->testCount == 0) {
        socketLog("[DONE] TEST_COUNT is zero; no network test was started.");
        return true;
    }
    if (config->protocol == MODEM_SOCKET_TLS && !validSetting(config->caFile, 50)) {
        return false;
    }
    state = {};
    state.config = config;
    state.uart = &uart;
    if (!ModemAT_enterRawMode()) {
        return false;
    }
    socketLog("[WARNING] Tests consume SIM data. No automatic retry or reconnection.");
    bool success = prepareNetwork();
    if (success && config->protocol == MODEM_SOCKET_TLS) {
        success = configureTls();
    }
    for (unsigned int attempt = 0; success && attempt < config->testCount; ++attempt) {
        char log[96];
        snprintf(log, sizeof(log), "[TEST] %u/%u; payload %lu bytes",
            attempt + 1, config->testCount, (unsigned long)strlen(config->payload));
        socketLog(log);
        state.receivedLength = 0;
        state.receiveFrames = 0;
        success = openSocket() && sendPayload() && receiveResponse();
        if (!state.blocked) {
            bool closed = closeSocket();
            success = success && closed;
        }
        if (!success && !state.blocked && state.receivedLength > 0 &&
            state.receivedLength < sizeof(state.received)) {
            state.received[state.receivedLength] = '\0';
            socketLog("[RX DATA] Response did not satisfy the example checks:");
            socketLog((const char *)state.received);
        }
        if (success) {
            snprintf(log, sizeof(log), "[PASS] Response verified; received %lu bytes.",
                (unsigned long)state.receivedLength);
            socketLog(log);
            if (state.receivedLength < sizeof(state.received)) {
                state.received[state.receivedLength] = '\0';
                socketLog("[RX DATA] Verified application response:");
                socketLog((const char *)state.received);
            }
            if (attempt + 1 < config->testCount) {
                waitIdle(config->testIntervalMs);
            }
        }
    }
    if (state.linkOwned && !state.blocked && !state.socketOpen) {
        bool released = query("AT+XIIC=0", NULL, config->linkTimeoutMs);
        success = success && released;
    }
    if (state.blocked) {
        socketLog("[ERROR] Unresolved response; cleanup commands were not sent.");
    }
    socketLog(success ? "[DONE] Finite test finished. Reset to run again."
        : "[FAIL] Test stopped; inspect settings and responses. No automatic retry.");
    return success;
}

static ModemSocketConfig tcpConfig = {};
static bool tcpApiActive = false;
static bool tcpLinkReady = false;
static bool tcpTlsReady = false;
static bool tcpCloseConfirmed = false;
static bool tcpSendConfirmed = false;
static size_t socketReadChunkBytes = 0;
static unsigned long closeUrcWaitMs = 0;

static bool tcpApiReady() {
    return tcpApiActive && state.config == &tcpConfig && !state.blocked;
}

bool ModemSocket_begin(const ModemSocketConfig *config, HardwareSerial &uart) {
    tcpApiActive = false;
    socketQuietMode = false;
    greetingOnOpenTimeoutMs = 0;
    receiveWaitActive = false;
    parsingInput = false;
    deferredLogLength = 0;
    deferredLogOverflow = false;
    greetingDiagnostics = false;
    tcpTlsReady = false;
    closeUrcWaitMs = 0;
    tcpLinkReady = false;
    tcpCloseConfirmed = false;
    tcpSendConfirmed = false;
    if (config == NULL ||
        (config->protocol != MODEM_SOCKET_TCP && config->protocol != MODEM_SOCKET_UDP &&
            config->protocol != MODEM_SOCKET_TLS) ||
        (config->protocol == MODEM_SOCKET_TLS && !validSetting(config->caFile, 50)) ||
        !validSetting(config->apn, 100) || !validSetting(config->host, 100) ||
        config->port == 0 || config->socketId > 5 || config->queryTimeoutMs == 0 ||
        config->registerTimeoutMs == 0 || config->linkTimeoutMs == 0 ||
        config->socketTimeoutMs == 0 || config->receiveTimeoutMs == 0 ||
        config->pollIntervalMs == 0) {
        socketLog("[ERROR] Invalid socket settings.");
        return false;
    }
    if (!ModemAT_enterRawMode() || !ModemAT_setRawLogBuffering(true)) {
        socketLog("[ERROR] Socket API requires a clean command boundary.");
        return false;
    }
    tcpConfig = *config;
    state = {};
    state.config = &tcpConfig;
    state.uart = &uart;
    tcpApiActive = true;
    return true;
}

// Return 0 for a confirmed inactive link, 1 for a valid active link, -1 for invalid data.
static int tcpLinkState() {
    unsigned int link;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    char extra;
    if (sscanf(state.value, "%u,%u.%u.%u.%u%c", &link, &a, &b, &c, &d, &extra) != 5 ||
        a > 255 || b > 255 || c > 255 || d > 255 || link > 1) {
        return -1;
    }
    if (link == 0 || !(a || b || c || d)) {
        return 0;
    }
    return 1;
}

static bool tcpWaitRegistration() {
    unsigned long start = millis();
    unsigned long elapsed = millis() - start;
    while (elapsed < tcpConfig.registerTimeoutMs) {
        unsigned long remaining = tcpConfig.registerTimeoutMs - elapsed;
        unsigned long timeout = remaining < tcpConfig.queryTimeoutMs ?
            remaining : tcpConfig.queryTimeoutMs;
        if (!query("AT+CEREG?", "+CEREG:", timeout)) {
            return false;
        }
        unsigned int mode;
        unsigned int status;
        if (sscanf(state.value, "%u,%u", &mode, &status) != 2) {
            return false;
        }
        if (status == 1 || status == 5) {
            socketLog("[STATE] Network registration confirmed.");
            return true;
        }
        if (status == 3) {
            socketLog("[ERROR] Network registration denied.");
            return false;
        }
        elapsed = millis() - start;
        if (elapsed >= tcpConfig.registerTimeoutMs) {
            break;
        }
        remaining = tcpConfig.registerTimeoutMs - elapsed;
        waitIdle(remaining < tcpConfig.pollIntervalMs ? remaining : tcpConfig.pollIntervalMs);
        elapsed = millis() - start;
    }
    socketLog("[ERROR] Network registration wait expired.");
    return false;
}

static bool tcpWaitIp(bool active) {
    unsigned long start = millis();
    unsigned long elapsed = millis() - start;
    while (elapsed < tcpConfig.linkTimeoutMs) {
        unsigned long remaining = tcpConfig.linkTimeoutMs - elapsed;
        unsigned long timeout = remaining < tcpConfig.queryTimeoutMs ?
            remaining : tcpConfig.queryTimeoutMs;
        if (!query("AT+XIIC?", "+XIIC:", timeout)) {
            return false;
        }
        int link = tcpLinkState();
        if (link < 0) {
            return false;
        }
        if (active && link == 1) {
            return true;
        }
        unsigned int flag;
        unsigned int a;
        unsigned int b;
        unsigned int c;
        unsigned int d;
        if (!active && link == 0 &&
            sscanf(state.value, "%u,%u.%u.%u.%u", &flag, &a, &b, &c, &d) == 5 &&
            flag == 0 && !(a || b || c || d)) {
            return true;
        }
        elapsed = millis() - start;
        if (elapsed >= tcpConfig.linkTimeoutMs) {
            break;
        }
        remaining = tcpConfig.linkTimeoutMs - elapsed;
        waitIdle(remaining < tcpConfig.pollIntervalMs ? remaining : tcpConfig.pollIntervalMs);
        elapsed = millis() - start;
    }
    return false;
}

bool ModemSocket_prepareNetwork(bool deferTlsConfiguration) {
    if (!tcpApiReady() || state.socketOpen) {
        return false;
    }
    tcpLinkReady = false;
    tcpTlsReady = false;
    tcpCloseConfirmed = false;
    tcpSendConfirmed = false;
    if (!query("AT+CPIN?", "+CPIN:", tcpConfig.queryTimeoutMs) ||
        strcmp(state.value, "READY")) {
        return false;
    }
    char command[COMMAND_BUFFER_BYTES];
    int written = snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"",
        tcpConfig.apn);
    if (!commandFits(written, sizeof(command)) ||
        !query(command, NULL, tcpConfig.queryTimeoutMs) || !tcpWaitRegistration()) {
        return false;
    }
    if (!query("AT+XIIC?", "+XIIC:", tcpConfig.queryTimeoutMs)) {
        return false;
    }
    int link = tcpLinkState();
    if (link < 0) {
        return false;
    }
    if (link == 0) {
        if (!query("AT+XIIC=1", NULL, tcpConfig.linkTimeoutMs) || !tcpWaitIp(true)) {
            return false;
        }
    } else {
        socketLog("[STATE] Existing IP is valid; XIIC activation skipped.");
    }
    char log[CONTROL_LINE_BYTES];
    snprintf(log, sizeof(log), "[STATE] Data link connected, IP: %s",
        strchr(state.value, ',') + 1);
    socketLog(log);
    const char *receiveMode = tcpConfig.protocol == MODEM_SOCKET_TLS &&
        !state.bufferedReceive ? "AT+RECVMODE=1" : "AT+RECVMODE=0";
    if (!query(receiveMode, NULL, tcpConfig.queryTimeoutMs)) {
        return false;
    }
    tcpLinkReady = true;
    if (tcpConfig.protocol == MODEM_SOCKET_TLS && !deferTlsConfiguration) {
        return ModemSocket_configureTls();
    }
    return true;
}


bool ModemSocket_configureTls() {
    tcpTlsReady = false;
    if (!tcpApiReady() || !tcpLinkReady || state.socketOpen ||
        tcpConfig.protocol != MODEM_SOCKET_TLS) {
        return false;
    }
    tcpTlsReady = configureTls();
    return tcpTlsReady;
}

bool ModemSocket_executeControl(const char *command, const char *prefix,
    char *value, size_t capacity, unsigned long timeoutMs) {
    if (!tcpApiReady() || !tcpLinkReady || state.socketOpen || command == NULL ||
        timeoutMs == 0 || strlen(command) >= COMMAND_BUFFER_BYTES ||
        (prefix != NULL && (value == NULL || capacity == 0))) {
        return false;
    }
    if (!query(command, prefix, timeoutMs)) {
        return false;
    }
    if (prefix != NULL) {
        if (strlen(state.value) >= capacity) {
            return false;
        }
        strcpy(value, state.value);
    }
    return true;
}

bool ModemSocket_executeControlWait(const char *command, const char *prefix,
    char *value, size_t capacity, unsigned long commandTimeoutMs,
    unsigned long resultTimeoutMs) {
    if (!tcpApiReady() || !tcpLinkReady || state.socketOpen || command == NULL ||
        strlen(command) >= COMMAND_BUFFER_BYTES || prefix == NULL || value == NULL ||
        capacity == 0 || commandTimeoutMs == 0 || resultTimeoutMs == 0) {
        return false;
    }
    value[0] = '\0';
    if (!startCommand(command, prefix)) {
        return false;
    }
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < resultTimeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        if (state.finalOk && state.value[0] != '\0') {
            waitIdle(CONTROL_QUIET_MS);
            if (state.blocked || state.finalError || strlen(state.value) >= capacity) {
                return false;
            }
            strcpy(value, state.value);
            socketLog(value);
            return true;
        }
        if (!state.finalOk && (unsigned long)(millis() - start) >= commandTimeoutMs) {
            break;
        }
        delay(1);
    }
    failBoundary();
    socketLog("[ERROR] Time control response unresolved; further commands blocked.");
    return false;
}



bool ModemSocket_useBufferedReceive(size_t readChunkBytes) {
    if (!tcpApiReady() || tcpLinkReady || state.socketOpen ||
        tcpConfig.protocol != MODEM_SOCKET_TLS || readChunkBytes == 0 ||
        readChunkBytes > 2048) {
        return false;
    }
    state.bufferedReceive = true;
    socketReadChunkBytes = readChunkBytes;
    return true;
}

bool ModemSocket_setQuietMode(bool enabled) {
    if (!tcpApiReady()) {
        return false;
    }
    socketQuietMode = enabled;
    return true;
}

bool ModemSocket_enableGreetingDiagnostics() {
    if (!tcpApiReady() || state.socketOpen || tcpConfig.protocol != MODEM_SOCKET_TLS) {
        return false;
    }
    greetingRawBytes = 0;
    greetingReadStartBytes = 0;
    greetingLastRxMs = 0;
    greetingLastControl[0] = '\0';
    greetingControlLength = 0;
    greetingControlTrace[0] = '\0';
    greetingControlTruncated = false;
    greetingDiagnostics = true;
    return true;
}

bool ModemSocket_waitForGreetingOnOpen(unsigned long timeoutMs) {
    if (!tcpApiReady() || state.socketOpen || tcpConfig.protocol != MODEM_SOCKET_TLS ||
        !state.bufferedReceive || timeoutMs == 0) {
        return false;
    }
    greetingOnOpenTimeoutMs = timeoutMs;
    return true;
}

bool ModemSocket_open() {
    if (!tcpApiReady() || !tcpLinkReady || state.socketOpen ||
        (tcpConfig.protocol == MODEM_SOCKET_TLS && !tcpTlsReady)) {
        return false;
    }
    state.receivedLength = 0;
    state.receiveFrames = 0;
    state.receivePending = false;
    tcpCloseConfirmed = false;
    tcpSendConfirmed = false;
    receiveWaitActive = true;
    bool opened = openSocket();
    receiveWaitActive = false;
    flushSocketLogs();
    if (!opened) {
        printGreetingDiagnostics("[GREETING FAIL] Open/greeting wait ended; captured diagnostics follow.");
        flushSocketLogs();
    }
    return opened;
}

bool ModemSocket_send(const char *payload) {
    // Authentication and message bytes must never enter this raw diagnostic trace.
    greetingDiagnostics = false;
    if (!tcpApiReady() || !state.socketOpen || tcpSendConfirmed || payload == NULL ||
        strlen(payload) == 0 || strlen(payload) > 512) {
        return false;
    }
    tcpConfig.payload = payload;
    tcpSendConfirmed = sendPayload();
    return tcpSendConfirmed;
}

static bool receiveDirectData(uint8_t *data, size_t capacity, size_t *receivedSize,
    unsigned long timeoutMs) {
    greetingReadStartBytes = greetingRawBytes;
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < timeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            printGreetingDiagnostics("[GREETING FAIL] RX blocked or modem error.");
            return false;
        }
        if (state.receivedLength > 0 && state.frameRemaining == 0) {
            if (state.receivedLength > capacity) {
                socketLog("[ERROR] Receive buffer too small; data retained.");
                return false;
            }
            memcpy(data, state.received, state.receivedLength);
            *receivedSize = state.receivedLength;
            state.receivedLength = 0;
            state.receiveFrames = 0;
            tcpSendConfirmed = false;
            return true;
        }
        if (state.remoteClosed) {
            printGreetingDiagnostics("[GREETING FAIL] Peer closed before complete greeting.");
            return false;
        }
        delay(1);
    }
    if (state.frameRemaining > 0 || state.lineLength > 0) {
        failBoundary();
    }
    printGreetingDiagnostics("[GREETING TIMEOUT] Inspect raw RX and framing counters.");
    socketLog("[ERROR] Direct receive timeout.");
    return false;
}

static void printReadDiagnostics() {
    char log[CONTROL_LINE_BYTES];
    snprintf(log, sizeof(log),
        "[READ DIAG] header=%u, data=%lu, remaining=%lu, finalOK=%u, error=%u, RX=%lu",
        state.readHeader ? 1U : 0U, (unsigned long)state.receivedLength,
        state.frameRemaining, state.finalOk ? 1U : 0U, state.finalError ? 1U : 0U,
        (unsigned long)state.readTraceCount);
    socketLog(log);
    size_t count = state.readTraceCount < READ_TRACE_BYTES ?
        state.readTraceCount : READ_TRACE_BYTES;
    size_t start = state.readTraceCount - count;
    for (size_t offset = 0; offset < count; offset += TRACE_ROW_BYTES) {
        size_t used = 0;
        log[0] = '\0';
        for (size_t i = 0; i < TRACE_ROW_BYTES && offset + i < count; ++i) {
            int written = snprintf(log + used, sizeof(log) - used, "%02X ",
                (unsigned int)state.readTrace[(start + offset + i) % READ_TRACE_BYTES]);
            if (written < 0 || (size_t)written >= sizeof(log) - used) {
                break;
            }
            used += (size_t)written;
        }
        socketLog("[READ RX HEX]");
        socketLog(log);
    }
}

static bool receiveSocketData(uint8_t *data, size_t capacity, size_t *receivedSize,
    unsigned long timeoutMs) {
    if (receivedSize != NULL) {
        *receivedSize = 0;
    }
    if (!tcpApiReady() || data == NULL || receivedSize == NULL || capacity == 0 ||
        (!state.socketOpen && !state.remoteClosed)) {
        return false;
    }
    if (timeoutMs == 0 || timeoutMs > tcpConfig.receiveTimeoutMs) {
        timeoutMs = tcpConfig.receiveTimeoutMs;
    }
    if (!state.bufferedReceive) {
        return receiveDirectData(data, capacity, receivedSize, timeoutMs);
    }
    greetingReadStartBytes = greetingRawBytes;
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < timeoutMs) {
        pump();
        if (state.blocked || state.finalError || (!state.socketOpen && !state.receivePending)) {
            return false;
        }
        if (state.receivePending) {
            break;
        }
        delay(1);
    }
    if (!state.receivePending) {
        socketLog("[ERROR] Receive notification timeout; READ not sent.");
        return false;
    }
    size_t limit = capacity < socketReadChunkBytes ? capacity : socketReadChunkBytes;
    char command[COMMAND_BUFFER_BYTES];
    int written = snprintf(command, sizeof(command), "AT+%sREAD=%u,%lu",
        protocolName(), tcpConfig.socketId, (unsigned long)limit);
    if (!commandFits(written, sizeof(command))) {
        return false;
    }
    // A notification during this READ remains pending for the next recv() call.
    state.receivePending = false;
    state.receivedLength = 0;
    state.receiveFrames = 0;
    state.readHeader = false;
    state.readSocketClosed = false;
    state.readTraceCount = 0;
    state.readActive = true;
    state.readLimit = limit;
    if (!startCommand(command, NULL, true)) {
        state.readActive = false;
        return false;
    }
    if (!state.socketOpen) {
        state.readActive = false;
        state.receivePending = false;
        socketLog("[STATE] Peer closed before READ; READ skipped.");
        return false;
    }
    start = millis();
    while ((unsigned long)(millis() - start) < tcpConfig.queryTimeoutMs) {
        pump();
        if (state.readSocketClosed && !state.blocked && !state.finalError &&
            state.frameRemaining == 0 && state.lineLength == 0) {
            state.readActive = false;
            state.receivePending = false;
            if (state.remoteClosed && state.closeOk && !state.socketOpen) {
                socketLog("[STATE] Peer closure raced with READ; closure confirmed.");
                return false;
            }
            state.finalError = true;
            socketLog("[READ ERROR] Socket unavailable without confirmed peer closure.");
            printReadDiagnostics();
            return false;
        }
        if (state.blocked || state.finalError) {
            printReadDiagnostics();
            if (state.frameRemaining > 0 || state.lineLength > 0) {
                failBoundary();
            }
            state.readActive = false;
            return false;
        }
        if (state.readHeader && state.frameRemaining == 0 && state.finalOk) {
            state.readActive = false;
            memcpy(data, state.received, state.receivedLength);
            *receivedSize = state.receivedLength;
            char log[96];
            snprintf(log, sizeof(log), "[READ] %lu bytes; final OK confirmed.",
                (unsigned long)state.receivedLength);
            if (!socketQuietMode) {
                socketLog(log);
            }
            // Drain full chunks, then wait for a new notification after a short read.
            state.receivePending = state.receivePending || state.receivedLength == limit;
            state.receivedLength = 0;
            tcpSendConfirmed = false;
            return *receivedSize > 0 || tcpConfig.protocol == MODEM_SOCKET_TLS;
        }
        delay(1);
    }
    printReadDiagnostics();
    state.readActive = false;
    failBoundary();
    socketLog("[ERROR] READ response incomplete; further commands blocked.");
    return false;
}

bool ModemSocket_recv(uint8_t *data, size_t capacity, size_t *receivedSize,
    unsigned long timeoutMs) {
    // The bounded receive operation owns UART parsing until completion or failure.
    receiveWaitActive = true;
    bool received = receiveSocketData(data, capacity, receivedSize, timeoutMs);
    receiveWaitActive = false;
    flushSocketLogs();
    if (!received) {
        printGreetingDiagnostics("[GREETING FAIL] Receive did not complete; captured diagnostics follow.");
        flushSocketLogs();
    }
    return received;
}

static bool confirmTlsSocketAbsent() {
    state.listingSockets = true;
    state.targetListed = false;
    if (!startCommand("AT+SSLTCPSETUP?", NULL)) {
        state.listingSockets = false;
        return false;
    }
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < tcpConfig.queryTimeoutMs) {
        pump();
        if (state.blocked || state.finalError) {
            state.listingSockets = false;
            return false;
        }
        if (state.finalOk) {
            state.listingSockets = false;
            if (state.targetListed) {
                socketLog("[ERROR] TLS socket is still listed as connected.");
                return false;
            }
            state.socketOpen = false;
            state.closeOk = true;
            socketLog("[STATE] TLS socket absent from connection list; closed.");
            return true;
        }
        delay(1);
    }
    state.listingSockets = false;
    failBoundary();
    return false;
}


bool ModemSocket_remoteCloseConfirmed() {
    return tcpApiReady() && !state.finalError && state.remoteClosed && state.closeOk &&
        !state.socketOpen && state.frameRemaining == 0 && state.lineLength == 0;
}

bool ModemSocket_confirmClosed() {
    if (!tcpApiReady() || tcpConfig.protocol != MODEM_SOCKET_TLS ||
        state.readActive || state.frameRemaining != 0 || state.lineLength != 0 ||
        state.receivedLength != 0 ||
        (state.finalError && !state.readSocketClosed)) {
        return false;
    }
    // A complete SOCKET ID OPEN FAILED line permits a status query, not a resend.
    if (!confirmTlsSocketAbsent()) {
        return false;
    }
    tcpCloseConfirmed = true;
    tcpSendConfirmed = false;
    state.receivePending = false;
    state.readSocketClosed = false;
    return true;
}

bool ModemSocket_waitForClose() {
    if (!tcpApiReady() || tcpConfig.protocol != MODEM_SOCKET_TLS) {
        return false;
    }
    if (tcpCloseConfirmed && state.closeOk && !state.socketOpen) {
        socketLog("[STATE] Socket closure already confirmed; Socket Close skipped.");
        return true;
    }
    char log[96];
    snprintf(log, sizeof(log), "[STATE] Waiting up to %lu ms for Link Closed URC.",
        closeUrcWaitMs);
    socketLog(log);
    unsigned long start = millis();
    do {
        pump();
        if (state.blocked || state.finalError) {
            return false;
        }
        if (state.remoteClosed && state.closeOk && !state.socketOpen) {
            tcpCloseConfirmed = true;
            tcpSendConfirmed = false;
            socketLog("[STATE] Link Closed confirmed; Socket Close call skipped.");
            return true;
        }
        delay(1);
    } while ((unsigned long)(millis() - start) < closeUrcWaitMs);
    socketLog("[STATE] No Link Closed URC within wait; Socket Close is required.");
    return false;
}

bool ModemSocket_close() {
    if (!tcpApiReady() || (!state.socketOpen && !state.closeOk)) {
        return false;
    }
    tcpCloseConfirmed = closeSocket();
    if (!tcpCloseConfirmed && tcpConfig.protocol == MODEM_SOCKET_TLS && !state.blocked) {
        tcpCloseConfirmed = (state.closeOk && !state.socketOpen) || confirmTlsSocketAbsent();
    }
    if (tcpCloseConfirmed) {
        tcpSendConfirmed = false;
    }
    return tcpCloseConfirmed;
}

bool ModemSocket_releaseIp() {
    if (!tcpApiReady() || !tcpCloseConfirmed || state.socketOpen) {
        return false;
    }
    if (!query("AT+XIIC=0", NULL, tcpConfig.linkTimeoutMs) || !tcpWaitIp(false)) {
        return false;
    }
    tcpLinkReady = false;
    tcpCloseConfirmed = false;
    socketLog("[STATE] Data link released; XIIC state 0 and IP 0.0.0.0 confirmed.");
    return true;
}

void ModemSocket_wait(unsigned long durationMs) {
    if (tcpApiReady()) {
        waitIdle(durationMs);
    }
}

bool ModemSocket_begin(const ModemSocketTcpConfig *config, HardwareSerial &uart) {
    if (config == NULL || config->readChunkBytes == 0 || config->readChunkBytes > 2048) {
        return false;
    }
    const ModemSocketConfig internalConfig = {
        MODEM_SOCKET_TCP,
        config->apn,
        config->host,
        config->port,
        config->socketId,
        NULL,
        NULL,
        NULL,
        0,
        config->queryTimeoutMs,
        config->registerTimeoutMs,
        config->linkTimeoutMs,
        config->socketTimeoutMs,
        config->receiveTimeoutMs,
        config->pollIntervalMs,
        0
    };
    if (!ModemSocket_begin(&internalConfig, uart)) {
        return false;
    }
    state.bufferedReceive = true;
    socketReadChunkBytes = config->readChunkBytes;
    return true;
}

bool ModemSocket_begin(const ModemSocketUdpConfig *config, HardwareSerial &uart) {
    if (config == NULL || config->readChunkBytes == 0 || config->readChunkBytes > 2048) {
        return false;
    }
    const ModemSocketConfig internalConfig = {
        MODEM_SOCKET_UDP,
        config->apn,
        config->host,
        config->port,
        config->socketId,
        NULL,
        NULL,
        NULL,
        0,
        config->queryTimeoutMs,
        config->registerTimeoutMs,
        config->linkTimeoutMs,
        config->socketTimeoutMs,
        config->receiveTimeoutMs,
        config->pollIntervalMs,
        0
    };
    if (!ModemSocket_begin(&internalConfig, uart)) {
        return false;
    }
    state.bufferedReceive = true;
    socketReadChunkBytes = config->readChunkBytes;
    return true;
}

bool ModemSocket_begin(const ModemSocketTlsConfig *config, HardwareSerial &uart) {
    if (config == NULL) {
        return false;
    }
    const ModemSocketConfig internalConfig = {
        MODEM_SOCKET_TLS,
        config->apn,
        config->host,
        config->port,
        config->socketId,
        NULL,
        config->caFile,
        NULL,
        0,
        config->queryTimeoutMs,
        config->registerTimeoutMs,
        config->linkTimeoutMs,
        config->socketTimeoutMs,
        config->receiveTimeoutMs,
        config->pollIntervalMs,
        0
    };
    if (!ModemSocket_begin(&internalConfig, uart)) {
        return false;
    }
    state.bufferedReceive = false;
    closeUrcWaitMs = config->closeUrcWaitMs;
    return true;
}
