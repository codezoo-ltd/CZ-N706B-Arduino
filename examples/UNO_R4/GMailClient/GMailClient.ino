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
static const unsigned long STARTUP_URC_WAIT_MS = 1000;
static const unsigned long STARTUP_BOUNDARY_TIMEOUT_MS = 5000;
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
#include <ModemSmtp.h>
#include <ModemTime.h>

// Replace placeholders locally. Never publish real credentials.
static const char APN[] = "simplio.apn";
static const char SERVER_HOST[] = "smtp.gmail.com";
static const uint16_t SERVER_PORT = 465;
static const uint8_t SOCKET_ID = 0;
static const char CA_FILE[] = "gts_root_r1.pem";
static const char GMAIL_USER[] = "YOUR_GMAIL_ADDRESS@gmail.com";
static const char APP_PASSWORD[] = "YOUR_APP_PASSWORD";
static const char RECIPIENT[] = "YOUR_RECIPIENT@example.com";
static const char EHLO_NAME[] = "n706b.local";
static const char EMAIL_SUBJECT[] = "CZ-N706B Gmail SMTPS test";
static const char EMAIL_BODY[] = "Hello.\nThis message was sent by the CZ-N706B modem.";
static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long REGISTER_TIMEOUT_MS = 180000;
static const unsigned long LINK_TIMEOUT_MS = 60000;
static const unsigned long SOCKET_TIMEOUT_MS = 60000;
static const unsigned long RECEIVE_TIMEOUT_MS = 60000;
static const unsigned long POLL_INTERVAL_MS = 2000;
static const unsigned long CLOSE_URC_WAIT_MS = 5000;
static const unsigned long SMTP_REPLY_TIMEOUT_MS = 60000;
static const size_t SOCKET_READ_CHUNK_BYTES = 32;
static const char TIME_SERVER[] = "time.windows.com";
static const unsigned int TIME_SERVER_TIMEOUT_SECONDS = 10;
// Keep UTC E0 and DST off when applying Update To directly to CCLK.
static const char TIME_ZONE[] = "E0";
static const unsigned int DAYLIGHT_SAVING = 0;
static const bool CLOCK_FIELDS_ARE_UTC = true;
static const unsigned long SYNC_RESULT_TIMEOUT_MS = 20000;

static const ModemTimeConfig TIME_CONFIG = {
    APN,
    TIME_SERVER,
    TIME_SERVER_TIMEOUT_SECONDS,
    TIME_ZONE,
    DAYLIGHT_SAVING,
    CLOCK_FIELDS_ARE_UTC,
    QUERY_TIMEOUT_MS,
    REGISTER_TIMEOUT_MS,
    LINK_TIMEOUT_MS,
    SYNC_RESULT_TIMEOUT_MS,
    POLL_INTERVAL_MS
};

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

static const ModemSmtpConfig MAIL_CONFIG = {
    GMAIL_USER,
    APP_PASSWORD,
    RECIPIENT,
    EHLO_NAME,
    EMAIL_SUBJECT,
    EMAIL_BODY,
    SMTP_REPLY_TIMEOUT_MS
};

static bool waitForStartupNotifications() {
    // PBREADY is an informational startup URC, not an AT command failure.
    // Drain complete lines without discarding partial bytes or unresolved responses.
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < STARTUP_BOUNDARY_TIMEOUT_MS) {
        ModemAT_pollIdle();
        if ((unsigned long)(millis() - start) >= STARTUP_URC_WAIT_MS &&
            ModemAT_commandBoundaryReady()) {
            return true;
        }
        delay(1);
    }
    ModemAT_printLine("[FAIL] Startup response boundary unresolved; ATE0 not sent.");
    return false;
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
    ModemAT_printLine("[INFO] GMailClient / UNO R4 / one message per reset.");
    ModemAT_printLine("[INFO] Stored CA required; UTC time sync and CCLK readback run before TLS.");
    if (!ModemSmtp_validate(&MAIL_CONFIG)) {
        ModemAT_printLine("[FAIL] Set account, app password and recipient; ASCII message <=512 bytes.");
        return;
    }
    if (!powerReady || !ModemPower_powerOn()) {
        ModemAT_printLine("[FAIL] Power/AT readiness failed; mail not started.");
        return;
    }
    if (!waitForStartupNotifications()) {
        return;
    }
    if (ModemAT_execute("ATE0", NULL, AT_TIMEOUT_MS) != MODEM_AT_OK) {
        ModemAT_printLine("[FAIL] Echo disable failed; mail not started.");
        return;
    }
    ModemAT_printLine("[STATE] Modem echo disabled (ATE0).");
    if (!ModemSocket_begin(&SOCKET_CONFIG, Serial1) ||
        !ModemSocket_useBufferedReceive(SOCKET_READ_CHUNK_BYTES) ||
        !ModemSocket_setQuietMode(false)) {
        ModemAT_printLine("[FAIL] TLS API initialization failed; mail not started.");
        return;
    }
    ModemAT_printLine("[PREPARE] Standard SSLTCP SIM, APN, registration and IP sequence.");
    if (!ModemSocket_prepareNetwork(true)) {
        ModemAT_printLine("[FAIL] Standard network preparation failed; TLS/mail not started.");
        return;
    }
    ModemAT_printLine("[TIME] One UTC update, CCLK write and verification on the active IP.");
    if (!ModemTime_beginOnSocket(&TIME_CONFIG) || !ModemTime_syncTime() ||
        !ModemTime_applySyncedClock()) {
        ModemAT_printLine("[FAIL] Time sync/CCLK verification failed; TLS/mail not started.");
        return;
    }
    if (!ModemSocket_configureTls()) {
        ModemAT_printLine("[FAIL] Clock/CA/TLS settings failed; mail not started.");
        return;
    }
    ModemAT_printLine("[TLS] Connecting to Gmail; waiting for setup and first receive notification.");
    if (!ModemSocket_enableGreetingDiagnostics() ||
        !ModemSocket_waitForGreetingOnOpen(SMTP_REPLY_TIMEOUT_MS)) {
        ModemAT_printLine("[FAIL] Greeting diagnostics initialization failed; mail not started.");
        return;
    }
    if (!ModemSocket_open()) {
        ModemAT_printLine("[FAIL] TLS setup or first receive notification not confirmed; authentication not started.");
        ModemAT_printLine("[INFO] Stored CERTCHECK OK does not prove server-chain validation.");
        ModemAT_printLine("[DONE] No retry or cleanup commands; inspect modem/socket/IP state.");
        return;
    }
    ModemAT_printLine("[TLS] Connected. Receive notification -> bounded READ -> SMTP reply.");
    ModemSmtpResult result = ModemSmtp_sendMail(&MAIL_CONFIG);
    if (result != MODEM_SMTP_COMPLETE && result != MODEM_SMTP_ACCEPTED_PEER_CLOSED) {
        if (result == MODEM_SMTP_ACCEPTED_QUIT_FAILED) {
            ModemAT_printLine("[MAIL] Already accepted; QUIT failed. Do not resend automatically.");
        }
        ModemAT_printLine("[DONE] Stopped; inspect mail/socket/IP state. No cleanup or retry.");
        return;
    }
    bool closed = ModemSocket_waitForClose();
    if (!closed) {
        closed = ModemSocket_close();
    }
    if (!closed || !ModemSocket_releaseIp()) {
        ModemAT_printLine("[DONE] Mail accepted; socket/IP cleanup incomplete. Do not resend.");
        return;
    }
    if (result == MODEM_SMTP_ACCEPTED_PEER_CLOSED) {
        ModemAT_printLine("[PASS] Mail accepted; peer closed; IP released. SMTP 221 not observed.");
    } else {
        ModemAT_printLine("[PASS] Mail accepted, SMTP ended and IP released. Check recipient inbox.");
    }
    ModemAT_printLine("[DONE] No automatic repeat. Reset sends another message.");
}

void loop() {
    ModemSocket_pollIdle();
}
