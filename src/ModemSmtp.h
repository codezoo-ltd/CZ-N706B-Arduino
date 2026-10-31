#ifndef CZ_N706B_MODEM_SMTP_H
#define CZ_N706B_MODEM_SMTP_H

#include <Arduino.h>

struct ModemSmtpConfig {
    const char *user;
    const char *appPassword;
    const char *recipient;
    const char *ehloName;
    const char *subject;
    const char *body;
    unsigned long replyTimeoutMs;
};

enum ModemSmtpResult {
    MODEM_SMTP_FAILED,
    MODEM_SMTP_ACCEPTANCE_UNKNOWN,
    MODEM_SMTP_ACCEPTED_QUIT_FAILED,
    MODEM_SMTP_COMPLETE,
    MODEM_SMTP_ACCEPTED_PEER_CLOSED
};

// Validate before powering on. ASCII text only; encoded message is at most 512 bytes.
bool ModemSmtp_validate(const ModemSmtpConfig *config);
// Requires an open ModemSocket TLS connection. Never reconnects or retries mail.
ModemSmtpResult ModemSmtp_sendMail(const ModemSmtpConfig *config);

#endif
