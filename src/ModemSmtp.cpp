#include "ModemSmtp.h"
#include "ModemSocket.h"
#include "ModemAT.h"
#include <stdio.h>
#include <string.h>

static const size_t MESSAGE_BUFFER_BYTES = 513;
static const size_t REPLY_LINE_BYTES = 512;
static const size_t RX_BUFFER_BYTES = 4096;
static const size_t MAX_REPLY_BYTES = 8192;
static const size_t MAX_ACCOUNT_BYTES = 100;
static const size_t MAX_PASSWORD_BYTES = 64;
static char message[MESSAGE_BUFFER_BYTES];
static char command[MESSAGE_BUFFER_BYTES];
static uint8_t rxBuffer[RX_BUFFER_BYTES];
static size_t rxOffset = 0;
static size_t rxSize = 0;
static char replyLine[REPLY_LINE_BYTES];
static bool loginAdvertised = false;
// A CR after a complete reply may be the first half of an empty CRLF line.
static bool pendingPaddingCr = false;

static bool asciiText(const char *text, bool newlines) {
    if (text == NULL) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (newlines && (c == '\r' || c == '\n')) {
            continue;
        }
        if (c < 32 || c > 126) {
            return false;
        }
    }
    return true;
}

static bool mailbox(const char *text) {
    return asciiText(text, false) && strlen(text) <= MAX_ACCOUNT_BYTES &&
        strchr(text, '@') != NULL && strpbrk(text, " <>\"\\,;:") == NULL;
}

static bool appendMessage(char c, size_t *used) {
    if (*used + 1 >= sizeof(message)) {
        return false;
    }
    message[(*used)++] = c;
    message[*used] = '\0';
    return true;
}

static bool buildMessage(const ModemSmtpConfig *config) {
    int count = snprintf(message, sizeof(message),
        "From: <%s>\r\nTo: <%s>\r\nSubject: %s\r\n"
        "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=us-ascii\r\n"
        "Content-Transfer-Encoding: 7bit\r\n\r\n",
        config->user, config->recipient, config->subject);
    if (count < 0 || (size_t)count >= sizeof(message)) {
        return false;
    }
    size_t used = (size_t)count;
    bool lineStart = true;
    for (size_t i = 0; config->body[i] != '\0'; ++i) {
        char c = config->body[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && config->body[i + 1] == '\n') {
                ++i;
            }
            if (!appendMessage('\r', &used) || !appendMessage('\n', &used)) {
                return false;
            }
            lineStart = true;
        } else {
            if (lineStart && c == '.' && !appendMessage('.', &used)) {
                return false;
            }
            if (!appendMessage(c, &used)) {
                return false;
            }
            lineStart = false;
        }
    }
    if (!lineStart && (!appendMessage('\r', &used) || !appendMessage('\n', &used))) {
        return false;
    }
    return appendMessage('.', &used) && appendMessage('\r', &used) &&
        appendMessage('\n', &used);
}

bool ModemSmtp_validate(const ModemSmtpConfig *config) {
    return config != NULL && mailbox(config->user) && mailbox(config->recipient) &&
        strstr(config->user, "YOUR_") == NULL &&
        strstr(config->recipient, "YOUR_") == NULL &&
        asciiText(config->appPassword, false) && config->appPassword[0] != '\0' &&
        strlen(config->appPassword) <= MAX_PASSWORD_BYTES &&
        strstr(config->appPassword, "YOUR_") == NULL &&
        strchr(config->appPassword, ' ') == NULL &&
        asciiText(config->ehloName, false) && config->ehloName[0] != '\0' &&
        strlen(config->ehloName) <= MAX_ACCOUNT_BYTES &&
        strpbrk(config->ehloName, " \t") == NULL &&
        asciiText(config->subject, false) && asciiText(config->body, true) &&
        config->replyTimeoutMs > 0 && buildMessage(config);
}

static bool encodeLogin(const char *text) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen(text);
    size_t used = 0;
    if (((length + 2) / 3) * 4 + 3 > sizeof(command)) {
        return false;
    }
    for (size_t i = 0; i < length; i += 3) {
        unsigned long value = (unsigned char)text[i];
        value <<= 16;
        if (i + 1 < length) {
            value |= (unsigned long)(unsigned char)text[i + 1] << 8;
        }
        if (i + 2 < length) {
            value |= (unsigned char)text[i + 2];
        }
        command[used++] = alphabet[(value >> 18) & 63];
        command[used++] = alphabet[(value >> 12) & 63];
        command[used++] = i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        command[used++] = i + 2 < length ? alphabet[value & 63] : '=';
    }
    command[used++] = '\r';
    command[used++] = '\n';
    command[used] = '\0';
    return true;
}

static bool readReply(unsigned int expected, unsigned int alternate,
    unsigned long timeoutMs, bool inspectAuth = false) {
    unsigned long start = millis();
    size_t used = 0;
    size_t total = 0;
    unsigned int multilineCode = 0;
    bool sawCr = pendingPaddingCr;
    pendingPaddingCr = false;
    while ((unsigned long)(millis() - start) < timeoutMs) {
        if (rxOffset == rxSize) {
            unsigned long elapsed = millis() - start;
            if (elapsed >= timeoutMs ||
                !ModemSocket_recv(rxBuffer, sizeof(rxBuffer), &rxSize, timeoutMs - elapsed)) {
                return false;
            }
            rxOffset = 0;
            if (rxSize == 0) {
                continue;
            }
        }
        char c = (char)rxBuffer[rxOffset++];
        if (++total > MAX_REPLY_BYTES) {
            return false;
        }
        if (c == '\r' && !sawCr) {
            sawCr = true;
            continue;
        }
        if (c == '\n' && sawCr) {
            replyLine[used] = '\0';
            if (used == 0 && multilineCode == 0) {
                sawCr = false;
                continue;
            }
            if (used < 4 || replyLine[0] < '2' || replyLine[0] > '5' ||
                replyLine[1] < '0' || replyLine[1] > '9' ||
                replyLine[2] < '0' || replyLine[2] > '9' ||
                (replyLine[3] != '-' && replyLine[3] != ' ')) {
                return false;
            }
            unsigned int code = (replyLine[0] - '0') * 100 +
                (replyLine[1] - '0') * 10 + replyLine[2] - '0';
            if (multilineCode != 0 && multilineCode != code) {
                return false;
            }
            if (inspectAuth && !strncmp(replyLine + 4, "AUTH ", 5)) {
                const char *method = replyLine + 9;
                while (*method != '\0') {
                    while (*method == ' ') {
                        ++method;
                    }
                    const char *end = strchr(method, ' ');
                    size_t length = end != NULL ? (size_t)(end - method) : strlen(method);
                    if (length == 5 && !strncmp(method, "LOGIN", 5)) {
                        loginAdvertised = true;
                    }
                    method += length;
                }
            }
            char log[48];
            snprintf(log, sizeof(log), "[SMTP RX] %u%c", code, replyLine[3]);
            ModemAT_printLine(log);
            if (replyLine[3] == ' ') {
                return code == expected || (alternate != 0 && code == alternate);
            }
            multilineCode = code;
            used = 0;
            sawCr = false;
        } else {
            if (sawCr || c == '\n' || (unsigned char)c < 32 ||
                (unsigned char)c > 126 || used + 1 >= sizeof(replyLine)) {
                return false;
            }
            replyLine[used++] = c;
        }
    }
    return false;
}


static bool replyBoundaryReady() {
    size_t ignored = 0;
    while (rxOffset + 1 < rxSize && rxBuffer[rxOffset] == '\r' &&
        rxBuffer[rxOffset + 1] == '\n') {
        rxOffset += 2;
        ignored += 2;
    }
    if (ignored > 0) {
        ModemAT_printLine("[SMTP] Ignored empty CRLF lines between replies.");
    }
    if (rxSize - rxOffset == 1 && rxBuffer[rxOffset] == '\r') {
        // Preserve the parser state; the next byte must be LF, even in another recv().
        pendingPaddingCr = true;
        ++rxOffset;
        ModemAT_printLine("[SMTP] Trailing CR retained; next reply read must complete CRLF.");
    }
    if (rxOffset != rxSize) {
        char log[112];
        snprintf(log, sizeof(log),
            "[SMTP FAIL] Unexpected pending reply bytes=%lu; next request not sent.",
            (unsigned long)(rxSize - rxOffset));
        ModemAT_printLine(log);
        return false;
    }
    return true;
}

static bool exchange(const char *payload, const char *label, unsigned int expected,
    unsigned long timeoutMs, unsigned int alternate = 0, bool inspectAuth = false) {
    // Retained unsolicited application bytes must not be mistaken for a new reply.
    if (!replyBoundaryReady()) {
        return false;
    }
    ModemAT_printLine(label);
    if (!ModemSocket_send(payload)) {
        ModemAT_printLine("[SMTP FAIL] Modem socket send was not confirmed.");
        return false;
    }
    if (!readReply(expected, alternate, timeoutMs, inspectAuth)) {
        ModemAT_printLine("[SMTP FAIL] Expected server reply was not confirmed.");
        return false;
    }
    return true;
}

ModemSmtpResult ModemSmtp_sendMail(const ModemSmtpConfig *config) {
    if (!ModemSmtp_validate(config)) {
        ModemAT_printLine("[FAIL] Invalid mail settings or message exceeds 512 bytes.");
        return MODEM_SMTP_FAILED;
    }
    rxOffset = 0;
    rxSize = 0;
    loginAdvertised = false;
    pendingPaddingCr = false;
    unsigned long timeout = config->replyTimeoutMs;
    ModemAT_printLine("[SMTP] Waiting for the server greeting within the reply timeout.");
    if (!readReply(220, 0, timeout)) {
        ModemAT_printLine("[SMTP FAIL] Server greeting was not confirmed.");
        return MODEM_SMTP_FAILED;
    }
    if (!replyBoundaryReady()) {
        return MODEM_SMTP_FAILED;
    }
    int count = snprintf(command, sizeof(command), "EHLO %s\r\n", config->ehloName);
    if (count < 0 || (size_t)count >= sizeof(command) ||
        !exchange(command, "[SMTP TX] EHLO", 250, timeout, 0, true) || !loginAdvertised ||
        !exchange("AUTH LOGIN\r\n", "[AUTH] AUTH LOGIN", 334, timeout) ||
        !encodeLogin(config->user) ||
        !exchange(command, "[AUTH] Username hidden", 334, timeout) ||
        !encodeLogin(config->appPassword) ||
        !exchange(command, "[AUTH] App password hidden", 235, timeout)) {
        memset(command, 0, sizeof(command));
        return MODEM_SMTP_FAILED;
    }
    memset(command, 0, sizeof(command));
    ModemAT_printLine("[AUTH] Authentication confirmed.");
    count = snprintf(command, sizeof(command), "MAIL FROM:<%s>\r\n", config->user);
    if (count < 0 || (size_t)count >= sizeof(command) ||
        !exchange(command, "[SMTP TX] MAIL FROM", 250, timeout)) {
        return MODEM_SMTP_FAILED;
    }
    count = snprintf(command, sizeof(command), "RCPT TO:<%s>\r\n", config->recipient);
    if (count < 0 || (size_t)count >= sizeof(command) ||
        !exchange(command, "[SMTP TX] RCPT TO", 250, timeout, 251) ||
        !exchange("DATA\r\n", "[SMTP TX] DATA", 354, timeout)) {
        return MODEM_SMTP_FAILED;
    }
    ModemAT_printLine("[MAIL] Sending message once; no automatic retry.");
    if (!replyBoundaryReady() || !ModemSocket_send(message) || !readReply(250, 0, timeout)) {
        ModemAT_printLine("[MAIL] Acceptance not confirmed; inspect mailbox before retrying.");
        return MODEM_SMTP_ACCEPTANCE_UNKNOWN;
    }
    ModemAT_printLine("[MAIL] Server accepted the message; inbox delivery is not confirmed.");
    ModemAT_printLine("[SMTP TX] QUIT");
    bool quitSent = replyBoundaryReady() && ModemSocket_send("QUIT\r\n");
    if (!quitSent || !readReply(221, 0, timeout)) {
        if (ModemSocket_remoteCloseConfirmed()) {
            ModemAT_printLine("[MAIL] Accepted; peer closed the socket; SMTP 221 not observed.");
            return MODEM_SMTP_ACCEPTED_PEER_CLOSED;
        }
        if (quitSent && ModemSocket_confirmClosed()) {
            ModemAT_printLine("[MAIL] Accepted; socket absence verified; SMTP 221 not observed.");
            return MODEM_SMTP_ACCEPTED_PEER_CLOSED;
        }
        return MODEM_SMTP_ACCEPTED_QUIT_FAILED;
    }
    return MODEM_SMTP_COMPLETE;
}
