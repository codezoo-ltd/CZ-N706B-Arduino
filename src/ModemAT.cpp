#include "ModemAT.h"
#include <stdio.h>
#include <string.h>

static const size_t MODEM_AT_LINE_SIZE = 192;
static const size_t MODEM_AT_RX_QUEUE_SIZE = 1024;
static const size_t MODEM_AT_TRACE_SIZE = 2048;
static const unsigned long MODEM_AT_PC_LOG_TIMEOUT_MS = 100;
static const unsigned long MODEM_AT_RECOVERY_DRAIN_TIMEOUT_MS = 100;
static const unsigned long MODEM_AT_RECOVERY_QUIET_MS = 5;
static HardwareSerial *modemUart = NULL;
static HardwareSerial *pcUart = NULL;
static unsigned long quietMs = 100;
static char rxLine[MODEM_AT_LINE_SIZE];
static size_t rxLength = 0;
static bool dropping = false;
static bool pending = false;
static bool pendingProbe = false;
static uint8_t rxQueue[MODEM_AT_RX_QUEUE_SIZE], rxTrace[MODEM_AT_TRACE_SIZE];
static size_t rxHead = 0, rxTail = 0, traceLength = 0;
static bool rxQueueOverflow = false, lineOverflow = false;
static bool traceActive = false, traceOverflow = false, pcLogTruncated = false;
static char responseValue[MODEM_AT_LINE_SIZE];
static bool initialized = false;
static bool rawMode = false;
static bool rawLogBuffering = false;
static ModemATLineHandler lineHandler = NULL;
static ModemATLineHandler lineObserver = NULL;
static ModemATLineHandler idleLineHandler = NULL;
static bool promptPadding = false;
static unsigned long rxByteCount = 0;
static unsigned long lastRxByteMs = 0;

static void resetReceiveState(void) {
    lineObserver = NULL;
    idleLineHandler = NULL;
    rxLength = 0;
    dropping = false;
    pending = false;
    pendingProbe = false;
    rxHead = 0;
    rxTail = 0;
    traceLength = 0;
    rxQueueOverflow = false;
    lineOverflow = false;
    traceActive = false;
    traceOverflow = false;
    pcLogTruncated = false;
    responseValue[0] = '\0';
    rawMode = false;
    rawLogBuffering = false;
    promptPadding = false;
}

void ModemAT_begin(HardwareSerial &modem, HardwareSerial &pc, unsigned long quietDelayMs) {
    modemUart = &modem;
    pcUart = &pc;
    quietMs = quietDelayMs;
    resetReceiveState();
    initialized = true;
}

bool ModemAT_isInitialized(void) {
    return initialized && modemUart != NULL && pcUart != NULL;
}

bool ModemAT_resetAfterVerifiedPowerState(void) {
    return ModemAT_prepareAfterPowerOn(
        MODEM_AT_RECOVERY_DRAIN_TIMEOUT_MS,
        MODEM_AT_RECOVERY_QUIET_MS) == MODEM_AT_OK;
}

ModemATResult ModemAT_prepareAfterPowerOn(
    unsigned long drainTimeoutMs,
    unsigned long quietDelayMs) {
    if (!ModemAT_isInitialized() || drainTimeoutMs == 0 || quietDelayMs == 0) {
        return MODEM_AT_BLOCKED;
    }

    resetReceiveState();

    unsigned long start = millis();
    unsigned long lastByte = start;
    while ((unsigned long)(millis() - start) < drainTimeoutMs) {
        bool received = false;
        while (modemUart->available() > 0) {
            if ((unsigned long)(millis() - start) >= drainTimeoutMs) {
                resetReceiveState();
                return MODEM_AT_TIMEOUT;
            }
            int value = modemUart->read();
            if (value < 0) {
                break;
            }
            received = true;
            lastByte = millis();
        }
        if (!received && (unsigned long)(millis() - lastByte) >= quietDelayMs) {
            resetReceiveState();
            return MODEM_AT_OK;
        }
        delay(1);
    }

    resetReceiveState();
    return MODEM_AT_TIMEOUT;
}

// Drain the hardware UART before parsing or writing any PC diagnostic output.
void ModemAT_poll() {
    if (!ModemAT_isInitialized() || rawMode) {
        return;
    }
    while (modemUart->available() > 0) {
        int c = modemUart->read();
        if (c < 0) {
            break;
        }
        ++rxByteCount;
        lastRxByteMs = millis();
        size_t next = (rxHead + 1) % MODEM_AT_RX_QUEUE_SIZE;
        if (next == rxTail) {
            rxQueueOverflow = true;
        } else {
            rxQueue[rxHead] = (uint8_t)c;
            rxHead = next;
        }
    }
}

// Preserve FIFO bytes only; the raw owner remains the sole protocol parser.
static void bufferRawDuringLog() {
    if (!rawMode || !rawLogBuffering) {
        return;
    }
    while (modemUart->available() > 0) {
        size_t next = (rxHead + 1) % MODEM_AT_RX_QUEUE_SIZE;
        if (next == rxTail) {
            rxQueueOverflow = true;
            return;
        }
        int value = modemUart->read();
        if (value < 0) {
            break;
        }
        rxQueue[rxHead] = (uint8_t)value;
        rxHead = next;
        ++rxByteCount;
        lastRxByteMs = millis();
    }
}

bool ModemAT_setRawLogBuffering(bool enabled) {
    if (!ModemAT_isInitialized() || !rawMode) {
        return false;
    }
    rawLogBuffering = enabled;
    return true;
}

bool ModemAT_rawOverflowed(void) {
    return rxQueueOverflow || lineOverflow || traceOverflow;
}

// Diagnostics never wait indefinitely for the PC. Keep draining modemUart while
// sending them. A queue overflow is latched: a lost command boundary is unsafe.
static void pcText(const char *text) {
    if (!ModemAT_isInitialized() || text == NULL) {
        return;
    }
    unsigned long start = millis();
    while (*text != '\0') {
        bufferRawDuringLog();
        ModemAT_poll();
        if (pcUart->availableForWrite() > 0) {
            pcUart->write((uint8_t)*text);
            ++text;
            start = millis();
        } else if ((unsigned long)(millis() - start) >= MODEM_AT_PC_LOG_TIMEOUT_MS) {
            pcLogTruncated = true;
            return;
        } else {
            delay(1);
        }
    }
}

static void pcLine(const char *text) {
    pcText(text);
    pcText("\r\n");
}

// CR/LF are independent delimiters. Keep partial lines across calls, and
// discard an overlong line through its delimiter without printing in the reader.
static int uartReadLine() {
    ModemAT_poll();
    while (rxTail != rxHead) {
        uint8_t c = rxQueue[rxTail];
        rxTail = (rxTail + 1) % MODEM_AT_RX_QUEUE_SIZE;
        if (traceActive) {
            if (traceLength < MODEM_AT_TRACE_SIZE) {
                rxTrace[traceLength++] = c;
            } else {
                traceOverflow = true;
            }
        }
        if (promptPadding && c == ' ') {
            continue;
        }
        promptPadding = false;
        if (c == '\r' || c == '\n') {
            if (dropping) {
                dropping = false;
                rxLength = 0;
                return -1;
            }
            if (rxLength == 0) {
                continue;
            }
            rxLine[rxLength] = '\0';
            rxLength = 0;
            if (lineObserver != NULL) {
                lineObserver(rxLine);
            }
            return 1;
        }
        if (!dropping) {
            if (rxLength < MODEM_AT_LINE_SIZE - 1) {
                rxLine[rxLength++] = (char)c;
            } else {
                dropping = true;
                rxLength = 0;
                lineOverflow = true;
            }
        }
    }
    return 0;
}

static bool finalError(const char *s) {
    return !strcmp(s, "ERROR") || !strncmp(s, "+CME ERROR:", 11) || !strncmp(s, "+CMS ERROR:", 11);
}

static bool terminal(const char *s) {
    return !strcmp(s, "OK") || finalError(s);
}

static bool readyUrc(const char *s) {
    return !strncmp(s, "+PBREADY", 8);
}

static bool readResponseValue(const char *line, const char *prefix) {
    if (prefix == NULL || strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *value = line + strlen(prefix);
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) {
        --length;
    }
    if (length == 0 || length >= sizeof(responseValue)) {
        return false;
    }
    memcpy(responseValue, value, length);
    responseValue[length] = '\0';
    return true;
}

ModemATValueResult ModemAT_copyResponseValue(char *destination, size_t destinationSize) {
    if (destination == NULL || destinationSize == 0) {
        return MODEM_AT_VALUE_BUFFER_TOO_SMALL;
    }
    destination[0] = '\0';
    if (responseValue[0] == '\0') {
        return MODEM_AT_VALUE_NONE;
    }
    size_t length = strlen(responseValue);
    if (length >= destinationSize) {
        return MODEM_AT_VALUE_BUFFER_TOO_SMALL;
    }
    memcpy(destination, responseValue, length + 1);
    return MODEM_AT_VALUE_OK;
}

void ModemAT_pollIdle() {
    if (!ModemAT_isInitialized() || rawMode) {
        return;
    }
    int r;
    while ((r = uartReadLine()) != 0) {
        if (r < 0) {
            pcLine("[STATE] RX LINE BUFFER OVERFLOW: discarded through line end");
            continue;
        }
        if (idleLineHandler != NULL) {
            idleLineHandler(rxLine);
        }
        if (pending) {
            pcText("[RX late] ");
        } else {
            pcText("[RX idle/URC] ");
        }
        pcLine(rxLine);
        if (pending && terminal(rxLine) && !rxQueueOverflow) {
            pending = false;
            pcLine("[STATE] Late final consumed; command boundary recovered");
        }
    }
}

void ModemAT_printLine(const char *text) {
    if (!ModemAT_isInitialized()) {
        return;
    }
    pcLine(text);
}

bool ModemAT_commandBoundaryReady(void) {
    if (!ModemAT_isInitialized()) {
        return false;
    }
    ModemAT_pollIdle();
    return !pending && !dropping && rxLength == 0 && !rxQueueOverflow;
}

static void uartWait(unsigned long duration) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < duration) {
        ModemAT_pollIdle();
        delay(1);
    }
}

// Replay only after the command has completed or expired. This never edits the
// captured bytes; the separate HEX view retains CR/LF and non-printable bytes.
static void printRxTrace(const char *cmd, const char *prefix, bool hex) {
    char text[MODEM_AT_LINE_SIZE];
    size_t length = 0;
    bool truncatedLine = false;
    for (size_t i = 0; i <= traceLength; ++i) {
        uint8_t c = 0;
        if (i < traceLength) {
            c = rxTrace[i];
        }
        if (i == traceLength || c == '\r' || c == '\n') {
            if (length > 0 || truncatedLine) {
                text[length] = '\0';
                if (truncatedLine) {
                    pcText("[RX overlong/partial] ");
                } else if (i == traceLength) {
                    pcText("[RX partial] ");
                } else if (!strcmp(text, cmd)) {
                    pcText("[RX echo] ");
                } else if (terminal(text)) {
                    pcText("[RX final] ");
                } else if (prefix != NULL && !strncmp(text, prefix, strlen(prefix))) {
                    pcText("[RX data] ");
                } else {
                    pcText("[RX URC/other] ");
                }
                pcLine(text);
            }
            length = 0;
            truncatedLine = false;
        } else if (length < sizeof(text) - 1) {
            if (c >= 32 && c <= 126) {
                text[length++] = (char)c;
            } else {
                text[length++] = '.';
            }
        } else {
            truncatedLine = true;
        }
    }
    if (hex) {
        static const char digits[] = "0123456789ABCDEF";
        pcLine("[DIAG] Raw modemUart bytes consumed by parser; HEX, up to 16 bytes per row:");
        for (size_t i = 0; i < traceLength; i += 16) {
            char row[16 * 3 + 1];
            size_t n = 0;
            for (size_t j = i; j < traceLength && j < i + 16; ++j) {
                row[n++] = digits[rxTrace[j] >> 4];
                row[n++] = digits[rxTrace[j] & 15];
                row[n++] = ' ';
            }
            row[n] = '\0';
            pcText("[RX HEX] ");
            pcLine(row);
        }
    }
    if (traceOverflow) {
        pcLine("[STATE] RX TRACE BUFFER OVERFLOW: HEX/text capture truncated");
    }
    if (lineOverflow) {
        pcLine("[STATE] RX LINE BUFFER OVERFLOW: discard through line end");
    }
    if (rxQueueOverflow) {
        pcLine(
            "[STATE] RX QUEUE BUFFER OVERFLOW: boundary unknown; restart from known hardware state");
    }
}

// No PC printing or waiting for PC output occurs in the active receive loop.
static ModemATResult atReceive(const char *cmd, const char *prefix, unsigned long timeout) {
    unsigned long start = millis();
    bool received = false;
    bool payload = (prefix == NULL);
    pending = true;
    pendingProbe = !strcmp(cmd, "AT");

    while ((unsigned long)(millis() - start) < timeout) {
        int r = uartReadLine();
        if (traceLength > 0 || traceOverflow) {
            received = true;
        }
        if (rxQueueOverflow) {
            return MODEM_AT_OVERFLOW;
        }
        if (r == 1) {
            if (!strcmp(rxLine, cmd)) {
                continue;
            }
            if (terminal(rxLine)) {
                pending = false;
                pendingProbe = false;
                if (lineOverflow || traceOverflow) {
                    return MODEM_AT_OVERFLOW;
                }
                if (finalError(rxLine)) {
                    return MODEM_AT_ERROR;
                }
                if (payload) {
                    return MODEM_AT_OK;
                }
                return MODEM_AT_MISSING_DATA;
            }
            if (lineHandler != NULL) {
                lineHandler(rxLine);
            }
            if (readResponseValue(rxLine, prefix)) {
                payload = true;
            }
        }
        // Consume a burst without inter-line delay, even when lines are split.
        ModemAT_poll();
        if (rxHead == rxTail) {
            delay(1);
        }
    }
    if (lineOverflow || traceOverflow || rxQueueOverflow) {
        return MODEM_AT_OVERFLOW;
    }
    if (received) {
        return MODEM_AT_TIMEOUT;
    }
    return MODEM_AT_NO_RESPONSE;
}

ModemATResult ModemAT_execute(const char *cmd, const char *prefix, unsigned long timeout) {
    responseValue[0] = '\0';
    if (!ModemAT_isInitialized() || rawMode || cmd == NULL || timeout == 0) {
        return MODEM_AT_BLOCKED;
    }
    uartWait(quietMs);
    if (rxQueueOverflow) {
        pcLine("[STATE] RX queue overflow is latched; no new command");
        return MODEM_AT_OVERFLOW;
    }
    if (pending || dropping || rxLength) {
        pcLine(
            "[STATE] BLOCKED: unresolved response/partial line; wait for late final or reset test with known power state");
        return MODEM_AT_BLOCKED;
    }
    pcText("[TX] ");
    pcLine(cmd);
    // pcText services incoming UART bytes: recheck before starting a command.
    ModemAT_pollIdle();
    if (rxQueueOverflow || dropping || rxLength) {
        return MODEM_AT_BLOCKED;
    }
    traceLength = 0;
    traceOverflow = false;
    lineOverflow = false;
    responseValue[0] = '\0';
    traceActive = true;
    modemUart->print(cmd);
    modemUart->print("\r\n");

    ModemATResult result = atReceive(cmd, prefix, timeout);
    traceActive = false;
    printRxTrace(cmd, prefix, result != MODEM_AT_OK);
    if (result != MODEM_AT_OK) {
        responseValue[0] = '\0';
    }
    if (pcLogTruncated) {
        pcLogTruncated = false;
        pcLine("[STATE] PC diagnostic output truncated: PC was not ready");
    }
    return result;
}

void ModemAT_reportResult(ModemATResult r) {
    static const char *const labels[] = {
        "OK",
        "MODEM ERROR: final ERROR / CME ERROR / CMS ERROR received",
        "NO RESPONSE",
        "TIMEOUT: received bytes without final",
        "BUFFER OVERFLOW",
        "BLOCKED",
        "Final OK received, but expected response data was not found"};
    pcText("[STATE] ");
    pcLine(labels[(int)r]);
}

ModemATResult ModemAT_observeAt(unsigned long durationMs, unsigned long retryMs,
    unsigned int minimumProbes, unsigned long silenceMs, bool stopOnOk) {
    if (!ModemAT_isInitialized() || rawMode || durationMs == 0 || retryMs == 0) {
        return MODEM_AT_BLOCKED;
    }
    if (rxQueueOverflow || (pending && !pendingProbe)) {
        pcLine("[STATE] Power observation blocked: unresolved non-AT response or RX queue overflow");
        return MODEM_AT_BLOCKED;
    }

    char windowLine[112];
    snprintf(
        windowLine,
        sizeof(windowLine),
        "[TX power window] AT with CR/LF; repeat every %lu ms, observe aggregate RX",
        retryMs);
    pcLine(windowLine);
    traceLength = 0;
    traceOverflow = false;
    lineOverflow = false;
    traceActive = true;
    pending = true;
    pendingProbe = true;

    unsigned long start = millis();
    unsigned long lastProbe = start;
    unsigned long startByteCount = rxByteCount;
    unsigned int probes = 0;
    unsigned int okCount = 0;
    unsigned int errorCount = 0;
    unsigned int readyUrcCount = 0;
    unsigned long lastOkMs = 0;
    unsigned long lastValidResponseMs = start;
    bool firstProbe = true;
    bool ready = false;
    unsigned long observedByteCount = rxByteCount;
    unsigned long lastObservedByteMs = start;

    while (true) {
        unsigned long now = millis();
        bool withinWindow = (unsigned long)(now - start) < durationMs;
        bool needsMinimum = stopOnOk && probes < minimumProbes;
        // Allow one response interval after the last required boot probe.
        bool awaitingLastProbe = stopOnOk && minimumProbes > 0 && !firstProbe &&
            probes == minimumProbes && (unsigned long)(now - lastProbe) < retryMs;
        if (!withinWindow && !needsMinimum && !awaitingLastProbe) {
            break;
        }
        if ((withinWindow || needsMinimum) &&
            (firstProbe || (unsigned long)(now - lastProbe) >= retryMs)) {
            modemUart->print("AT\r\n");
            lastProbe = millis();
            firstProbe = false;
            ++probes;
            if (stopOnOk && minimumProbes > 0) {
                char probeLine[96];
                snprintf(
                    probeLine,
                    sizeof(probeLine),
                    "[TX power probe] AT attempt %u; minimum before timeout %u",
                    probes,
                    minimumProbes);
                pcLine(probeLine);
            }
        }

        int readResult = uartReadLine();
        if (rxByteCount != observedByteCount) {
            observedByteCount = rxByteCount;
            lastObservedByteMs = lastRxByteMs;
        }
        if (rxQueueOverflow || traceOverflow || lineOverflow) {
            break;
        }
        if (readResult == 1) {
            if (!strcmp(rxLine, "OK")) {
                ++okCount;
                lastOkMs = millis();
                lastValidResponseMs = lastOkMs;
                ready = true;
                pending = false;
                pendingProbe = false;
                if (stopOnOk) {
                    break;
                }
            } else if (finalError(rxLine)) {
                ++errorCount;
                lastValidResponseMs = millis();
                pending = false;
                pendingProbe = false;
            } else if (readyUrc(rxLine)) {
                ++readyUrcCount;
            }
        }

        ModemAT_poll();
        if (rxHead == rxTail) {
            delay(1);
        }
    }

    ModemAT_poll();
    traceActive = false;
    ModemATResult result = MODEM_AT_TIMEOUT;
    if (rxQueueOverflow || traceOverflow || lineOverflow) {
        result = MODEM_AT_OVERFLOW;
    } else if (stopOnOk && ready) {
        result = MODEM_AT_OK;
    } else if (stopOnOk && traceLength == 0 && !dropping && rxLength == 0 && rxHead == rxTail) {
        result = MODEM_AT_NO_RESPONSE;
    } else if (
        !stopOnOk && probes >= minimumProbes && errorCount == 0 &&
        readyUrcCount == 0 &&
        (unsigned long)(millis() - lastValidResponseMs) >= silenceMs) {
        result = MODEM_AT_NO_RESPONSE;
    }

    if (stopOnOk) {
        char summary[144];
        snprintf(
            summary,
            sizeof(summary),
            "[DIAG] AT readiness: attempts=%u, minimum=%u, OK=%u, errors=%u, elapsed=%lu ms",
            probes,
            minimumProbes,
            okCount,
            errorCount,
            (unsigned long)(millis() - start));
        pcLine(summary);
    }
    if (!stopOnOk) {
        char summary[224];
        char lastOkText[24];
        const char *partialLineText = "no";
        if (dropping || rxLength > 0) {
            partialLineText = "yes";
        }
        if (okCount == 0) {
            strcpy(lastOkText, "none");
        } else {
            snprintf(
                lastOkText,
                sizeof(lastOkText),
                "%lu ms ago",
                (unsigned long)(millis() - lastOkMs));
        }
        snprintf(
            summary,
            sizeof(summary),
            "[DIAG] Shutdown observation: AT OK=%u, AT error=%u, ready URC=%u, RX bytes=%lu, partial line=%s, last OK=%s, last valid=%lu ms ago, last RX=%lu ms ago",
            okCount,
            errorCount,
            readyUrcCount,
            observedByteCount - startByteCount,
            partialLineText,
            lastOkText,
            (unsigned long)(millis() - lastValidResponseMs),
            (unsigned long)(millis() - lastObservedByteMs));
        pcLine(summary);
    }
    printRxTrace("AT", NULL, !stopOnOk || (result != MODEM_AT_OK && result != MODEM_AT_NO_RESPONSE));
    if (!stopOnOk && result == MODEM_AT_NO_RESPONSE) {
        resetReceiveState();
    }
    return result;
}

bool ModemAT_enterRawMode(void) {
    if (!ModemAT_isInitialized() || rawMode || pending || dropping || rxQueueOverflow ||
        lineOverflow || traceOverflow) {
        return false;
    }
    rawMode = true;
    return true;
}

bool ModemAT_isRawMode(void) {
    return ModemAT_isInitialized() && rawMode;
}

size_t ModemAT_rawAvailable(void) {
    if (!ModemAT_isRawMode()) {
        return 0;
    }
    size_t queued = 0;
    if (rxHead >= rxTail) {
        queued = rxHead - rxTail;
    } else {
        queued = MODEM_AT_RX_QUEUE_SIZE - rxTail + rxHead;
    }
    return (rxLength - 0) + queued + (size_t)modemUart->available();
}

int ModemAT_rawRead(void) {
    if (!ModemAT_isRawMode()) {
        return -1;
    }
    if (rxLength > 0) {
        int value = (uint8_t)rxLine[0];
        memmove(rxLine, rxLine + 1, rxLength - 1);
        --rxLength;
        return value;
    }
    if (rxTail != rxHead) {
        int value = rxQueue[rxTail];
        rxTail = (rxTail + 1) % MODEM_AT_RX_QUEUE_SIZE;
        return value;
    }
    if (modemUart->available() <= 0) {
        return -1;
    }
    int value = modemUart->read();
    if (value >= 0) {
        ++rxByteCount;
        lastRxByteMs = millis();
    }
    return value;
}

ModemATResult ModemAT_executeWithHandler(const char *command, unsigned long timeoutMs,
    ModemATLineHandler handler) {
    if (lineHandler != NULL || handler == NULL) {
        return MODEM_AT_BLOCKED;
    }
    lineHandler = handler;
    ModemATResult result = ModemAT_execute(command, NULL, timeoutMs);
    lineHandler = NULL;
    return result;
}

ModemATResult ModemAT_writeData(const char *command, const uint8_t *data, size_t size,
    unsigned long timeoutMs) {
    if (!ModemAT_isInitialized() || rawMode || command == NULL || data == NULL ||
        size == 0 || timeoutMs == 0) {
        return MODEM_AT_BLOCKED;
    }
    uartWait(quietMs);
    if (!ModemAT_commandBoundaryReady()) {
        return MODEM_AT_BLOCKED;
    }
    pcText("[TX] ");
    pcLine(command);
    ModemAT_pollIdle();
    if (!ModemAT_commandBoundaryReady()) {
        return MODEM_AT_BLOCKED;
    }
    traceLength = 0;
    traceOverflow = false;
    lineOverflow = false;
    responseValue[0] = '\0';
    traceActive = true;
    pending = true;
    pendingProbe = false;
    modemUart->print(command);
    modemUart->print("\r\n");
    unsigned long start = millis();
    bool prompted = false;
    size_t sent = 0;
    ModemATResult result = MODEM_AT_TIMEOUT;
    while ((unsigned long)(millis() - start) < timeoutMs) {
        int readResult = uartReadLine();
        if (lineOverflow || traceOverflow || rxQueueOverflow) {
            result = MODEM_AT_OVERFLOW;
            break;
        }
        if (readResult == 1) {
            if (terminal(rxLine)) {
                pending = false;
                result = finalError(rxLine) ? MODEM_AT_ERROR : MODEM_AT_MISSING_DATA;
                break;
            }
            if (!strcmp(rxLine, ">") || !strcmp(rxLine, "> ")) {
                prompted = true;
            }
        } else if (rxLength > 0 && rxLine[0] == '>') {
            prompted = true;
        }
        if (prompted) {
            rxLength = 0;
            promptPadding = true;
            while (sent < size && (unsigned long)(millis() - start) < timeoutMs) {
                ModemAT_poll();
                // UNO R4 UART inherits availableForWrite() == 0; write directly.
                size_t written = modemUart->write(data[sent]);
                if (written != 1) {
                    break;
                }
                ++sent;
            }
            if (sent == size) {
                unsigned long elapsed = (unsigned long)(millis() - start);
                if (elapsed < timeoutMs) {
                    result = atReceive(command, NULL, timeoutMs - elapsed);
                }
            }
            break;
        }
        delay(1);
    }
    traceActive = false;
    printRxTrace(command, NULL, result != MODEM_AT_OK);
    if (prompted) {
        char line[96];
        snprintf(line, sizeof(line), "[TX DATA] UART accepted %lu/%lu bytes without CR/LF.",
            (unsigned long)sent, (unsigned long)size);
        pcLine(line);
    }
    return result;
}

// FSRF embeds file bytes after +FSRF: <length>, without line escaping.
// A line parser cannot distinguish file newlines from response separators.
ModemATResult ModemAT_readFile(const char *command, uint8_t *data, size_t expectedSize,
    unsigned long timeoutMs) {
    if (!ModemAT_isInitialized() || rawMode || command == NULL || data == NULL ||
        expectedSize == 0 || expectedSize > 512 || timeoutMs == 0) {
        return MODEM_AT_BLOCKED;
    }
    uartWait(quietMs);
    if (!ModemAT_commandBoundaryReady()) {
        return MODEM_AT_BLOCKED;
    }
    pcText("[TX] ");
    pcLine(command);
    if (!ModemAT_commandBoundaryReady()) {
        return MODEM_AT_BLOCKED;
    }
    traceLength = 0;
    traceOverflow = false;
    lineOverflow = false;
    traceActive = true;
    pending = true;
    pendingProbe = false;
    responseValue[0] = '\0';
    modemUart->print(command);
    modemUart->print("\r\n");
    char header[192];
    size_t headerLength = 0;
    size_t received = 0;
    bool payload = false;
    unsigned long start = millis();
    ModemATResult result = MODEM_AT_TIMEOUT;
    while ((unsigned long)(millis() - start) < timeoutMs) {
        ModemAT_poll();
        if (rxQueueOverflow || traceOverflow) {
            result = MODEM_AT_OVERFLOW;
            break;
        }
        if (rxTail == rxHead) {
            delay(1);
            continue;
        }
        uint8_t byte = rxQueue[rxTail];
        rxTail = (rxTail + 1) % MODEM_AT_RX_QUEUE_SIZE;
        if (traceLength >= MODEM_AT_TRACE_SIZE) {
            traceOverflow = true;
            result = MODEM_AT_OVERFLOW;
            break;
        }
        rxTrace[traceLength++] = byte;
        if (payload) {
            data[received++] = byte;
            if (received == expectedSize) {
                unsigned long elapsed = (unsigned long)(millis() - start);
                if (elapsed < timeoutMs) {
                    result = atReceive(command, NULL, timeoutMs - elapsed);
                }
                break;
            }
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            header[headerLength] = '\0';
            if (terminal(header)) {
                pending = false;
                result = finalError(header) ? MODEM_AT_ERROR : MODEM_AT_MISSING_DATA;
                break;
            }
            headerLength = 0;
            continue;
        }
        if (headerLength >= sizeof(header) - 1) {
            result = MODEM_AT_OVERFLOW;
            break;
        }
        header[headerLength++] = (char)byte;
        header[headerLength] = '\0';
        if (byte == ',' && strncmp(header, "+FSRF:", 6) == 0) {
            unsigned long length = 0;
            char extra = 0;
            if (sscanf(header + 6, " %lu,%c", &length, &extra) != 1 ||
                length != expectedSize) {
                result = MODEM_AT_MISSING_DATA;
                break;
            }
            payload = true;
        }
    }
    traceActive = false;
    printRxTrace(command, "+FSRF:", result != MODEM_AT_OK);
    return result;
}

// Observers only inspect lines. They must not print or execute commands.
bool ModemAT_setLineObserver(ModemATLineHandler observer) {
    if (!ModemAT_isInitialized() || rawMode || pending) {
        return false;
    }
    lineObserver = observer;
    return true;
}

ModemATResult ModemAT_probeAtOnce(unsigned long timeoutMs) {
    if (!ModemAT_isInitialized() || rawMode || timeoutMs == 0) {
        return MODEM_AT_BLOCKED;
    }
    ModemAT_pollIdle();
    if (rxQueueOverflow || lineOverflow || traceOverflow || dropping || rxLength != 0 ||
        (pending && !pendingProbe)) {
        return MODEM_AT_BLOCKED;
    }
    // Repeating AT is safe here; do not clear an unresolved service command.
    if (pending && pendingProbe) {
        pending = false;
        pendingProbe = false;
    }
    return ModemAT_execute("AT", NULL, timeoutMs);
}

static const char *awaitedPrefix = NULL;
static char *awaitedValue = NULL;
static size_t awaitedCapacity = 0;
static bool awaitedSeen = false;
static bool awaitedInvalid = false;

static void captureAwaitedLine(const char *line) {
    if (awaitedPrefix == NULL || strncmp(line, awaitedPrefix, strlen(awaitedPrefix))) {
        return;
    }
    const char *value = line + strlen(awaitedPrefix);
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    size_t length = strlen(value);
    if (awaitedSeen || length == 0 || length >= awaitedCapacity) {
        awaitedInvalid = true;
        return;
    }
    memcpy(awaitedValue, value, length + 1);
    awaitedSeen = true;
}

ModemATResult ModemAT_executeWaitUrc(const char *command, unsigned long commandTimeoutMs,
    const char *urcPrefix, char *value, size_t capacity, unsigned long resultTimeoutMs) {
    if (lineHandler != NULL || idleLineHandler != NULL || urcPrefix == NULL ||
        urcPrefix[0] == '\0' || value == NULL || capacity == 0 || resultTimeoutMs == 0) {
        return MODEM_AT_BLOCKED;
    }
    awaitedPrefix = urcPrefix;
    awaitedValue = value;
    awaitedCapacity = capacity;
    awaitedSeen = false;
    awaitedInvalid = false;
    value[0] = '\0';
    lineHandler = captureAwaitedLine;
    unsigned long start = millis();
    ModemATResult result = ModemAT_execute(command, NULL, commandTimeoutMs);
    lineHandler = NULL;
    if (result == MODEM_AT_OK) {
        idleLineHandler = captureAwaitedLine;
        while ((unsigned long)(millis() - start) < resultTimeoutMs) {
            ModemAT_pollIdle();
            if (awaitedSeen || awaitedInvalid || rxQueueOverflow || lineOverflow) {
                break;
            }
            delay(1);
        }
        idleLineHandler = NULL;
        if (awaitedInvalid || rxQueueOverflow || lineOverflow || traceOverflow) {
            result = MODEM_AT_OVERFLOW;
        } else if (!awaitedSeen || !ModemAT_commandBoundaryReady()) {
            result = MODEM_AT_TIMEOUT;
        }
        if (result != MODEM_AT_OK) {
            // A late result is still possible. Do not permit unrelated commands.
            pending = true;
            pendingProbe = false;
        }
    }
    awaitedPrefix = NULL;
    awaitedValue = NULL;
    awaitedCapacity = 0;
    return result;
}
