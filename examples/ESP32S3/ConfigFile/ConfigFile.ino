#include <Arduino.h>
#include <ModemAT.h>
#include <ModemPower.h>

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char CONFIG_FILE[] = "config.ini";
static const char TEMP_FILE[] = "config.tmp";
static const char BACKUP_FILE[] = "config.bak";
static const unsigned long FILE_TIMEOUT_MS = 10000;
static const size_t CONFIG_CAPACITY = 256;

struct Configuration {
    unsigned int version;
    char deviceId[33];
    unsigned int intervalSec;
};
static Configuration active = {1, "N706_Client_01", 10};
static bool ready = false;
static bool dirty = true;
static bool exists[3] = {false, false, false};
static bool listingValid = true;
static unsigned long lastTickMs = 0;
static unsigned long tickCount = 0;

static bool sameConfig(const Configuration &a, const Configuration &b) {
    return a.version == b.version && a.intervalSec == b.intervalSec &&
        strcmp(a.deviceId, b.deviceId) == 0;
}

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
        text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
    return text;
}

static bool parseConfig(char *text, Configuration *result) {
    Configuration candidate = {};
    unsigned int seen = 0;
    char *line = text;
    while (line != NULL) {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        char *key = trim(line);
        if (*key != '\0' && *key != '#') {
            char *equals = strchr(key, '=');
            if (equals == NULL) {
                ModemAT_printLine("[CONFIG ERROR] Expected key=value.");
                return false;
            }
            *equals = '\0';
            key = trim(key);
            char *value = trim(equals + 1);
            unsigned int bit = 0;
            if (strcmp(key, "device_id") == 0) {
                bit = 2;
                size_t length = strlen(value);
                if (length == 0 || length > 32) {
                    ModemAT_printLine("[CONFIG ERROR] device_id must contain 1..32 characters.");
                    return false;
                }
                for (size_t i = 0; i < length; ++i) {
                    char c = value[i];
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-')) {
                        ModemAT_printLine("[CONFIG ERROR] device_id allows letters, digits, _ and - only.");
                        return false;
                    }
                }
                strcpy(candidate.deviceId, value);
            } else if (strcmp(key, "version") == 0 || strcmp(key, "sample_interval_sec") == 0) {
                if (*value == '\0') {
                    ModemAT_printLine("[CONFIG ERROR] Numeric value is empty.");
                    return false;
                }
                for (const char *p = value; *p != '\0'; ++p) {
                    if (*p < '0' || *p > '9') {
                        ModemAT_printLine("[CONFIG ERROR] Numeric values require decimal digits.");
                        return false;
                    }
                }
                if (strlen(value) > 2) {
                    ModemAT_printLine("[CONFIG ERROR] Numeric value is out of range.");
                    return false;
                }
                unsigned long number = strtoul(value, NULL, 10);
                if (strcmp(key, "version") == 0) {
                    bit = 1;
                    if (number != 1) {
                        ModemAT_printLine("[CONFIG ERROR] Unsupported version; expected 1.");
                        return false;
                    }
                    candidate.version = 1;
                } else {
                    bit = 4;
                    if (number != 10 && number != 30 && number != 60) {
                        ModemAT_printLine("[CONFIG ERROR] sample_interval_sec must be 10, 30 or 60.");
                        return false;
                    }
                    candidate.intervalSec = (unsigned int)number;
                }
            } else {
                ModemAT_printLine("[CONFIG ERROR] Unknown key.");
                return false;
            }
            if ((seen & bit) != 0) {
                ModemAT_printLine("[CONFIG ERROR] Duplicate key.");
                return false;
            }
            seen |= bit;
        }
        line = next;
    }
    if (seen != 7) {
        ModemAT_printLine("[CONFIG ERROR] Required key is missing.");
        return false;
    }
    *result = candidate;
    return true;
}

static void showConfig() {
    char line[160];
    ModemAT_printLine("========== CONFIGURATION IN RAM ==========");
    snprintf(line, sizeof(line), "[RAM] version=%u | device_id=%s | sample_interval_sec=%u",
        active.version, active.deviceId, active.intervalSec);
    ModemAT_printLine(line);
    ModemAT_printLine(dirty ? "[STORE] RAM changes are not verified as saved." :
        "[STORE] RAM matches the last verified config.ini read.");
    ModemAT_printLine("==========================================");
}

static void listLine(const char *line) {
    const char *record = line;
    if (strncmp(record, "+FSLIST:", 8) == 0) {
        record += 8;
        while (*record == ' ') {
            ++record;
        }
    }
    const char *comma = strrchr(record, ',');
    if (comma == NULL) {
        return;
    }
    const char *names[] = {CONFIG_FILE, TEMP_FILE, BACKUP_FILE};
    for (size_t i = 0; i < 3; ++i) {
        if ((size_t)(comma - record) == strlen(names[i]) &&
            strncmp(record, names[i], strlen(names[i])) == 0) {
            char *end = NULL;
            strtoul(comma + 1, &end, 10);
            if (end == comma + 1 || *end != '\0') {
                listingValid = false;
            }
            exists[i] = true;
        }
    }
}

static bool listFiles() {
    memset(exists, 0, sizeof(exists));
    listingValid = true;
    ModemATResult result = ModemAT_executeWithHandler("AT+FSLIST?", FILE_TIMEOUT_MS, listLine);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK && listingValid;
}

static bool fileCommand(const char *verb, const char *name, const char *other = NULL) {
    char command[128];
    if (other == NULL) {
        snprintf(command, sizeof(command), "AT+%s=\"%s\"", verb, name);
    } else {
        snprintf(command, sizeof(command), "AT+%s=\"%s\",\"%s\"", verb, name, other);
    }
    ModemATResult result = ModemAT_execute(command, NULL, FILE_TIMEOUT_MS);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK;
}

static bool readConfig(const char *name, Configuration *result, const char *expected = NULL) {
    char command[96], sizeText[32], bytes[CONFIG_CAPACITY];
    snprintf(command, sizeof(command), "AT+FSFS=\"%s\"", name);
    ModemATResult status = ModemAT_execute(command, "+FSFS:", FILE_TIMEOUT_MS);
    ModemAT_reportResult(status);
    if (status != MODEM_AT_OK ||
        ModemAT_copyResponseValue(sizeText, sizeof(sizeText)) != MODEM_AT_VALUE_OK) {
        return false;
    }
    char *end = NULL;
    unsigned long size = strtoul(sizeText, &end, 10);
    if (end == sizeText || *end != '\0' || size == 0 || size >= sizeof(bytes)) {
        ModemAT_printLine("[CONFIG ERROR] File size must be 1..255 bytes.");
        return false;
    }
    snprintf(command, sizeof(command), "AT+FSRF=\"%s\",0,%lu", name, size);
    status = ModemAT_readFile(command, (uint8_t *)bytes, (size_t)size, FILE_TIMEOUT_MS);
    ModemAT_reportResult(status);
    if (status != MODEM_AT_OK) {
        return false;
    }
    bytes[size] = '\0';
    if (memchr(bytes, '\0', size) != NULL ||
        (expected != NULL && (strlen(expected) != size || memcmp(bytes, expected, size) != 0))) {
        ModemAT_printLine("[CONFIG ERROR] File bytes did not match the expected text.");
        return false;
    }
    return parseConfig(bytes, result);
}

static bool saveConfig() {
    ModemAT_printLine("========== SAVE: RAM -> FILE -> VERIFY ==========");
    dirty = true;
    if (!listFiles()) {
        ModemAT_printLine("[SAVE FAIL] File list unavailable; no save attempted.");
        return false;
    }
    bool hadConfig = exists[0];
    if (exists[2]) {
        ModemAT_printLine("[SAVE FAIL] config.bak exists; inspect recovery files before saving.");
        return false;
    }
    if (exists[1] && !fileCommand("FSDF", TEMP_FILE)) {
        return false;
    }
    char payload[CONFIG_CAPACITY], command[128];
    int length = snprintf(payload, sizeof(payload),
        "# CodeZoo configuration\nversion=%u\ndevice_id=%s\nsample_interval_sec=%u\n",
        active.version, active.deviceId, active.intervalSec);
    if (length <= 0 || (size_t)length >= sizeof(payload)) {
        return false;
    }
    snprintf(command, sizeof(command), "AT+FSWF=\"%s\",0,%u,%lu", TEMP_FILE,
        (unsigned int)length, FILE_TIMEOUT_MS);
    ModemATResult status = ModemAT_writeData(command, (const uint8_t *)payload,
        (size_t)length, FILE_TIMEOUT_MS);
    ModemAT_reportResult(status);
    Configuration verified = {};
    if (status != MODEM_AT_OK || !readConfig(TEMP_FILE, &verified, payload) ||
        !sameConfig(active, verified)) {
        ModemAT_printLine("[SAVE FAIL] Temporary file verification failed; original file retained.");
        return false;
    }
    if (hadConfig && !fileCommand("FSRN", CONFIG_FILE, BACKUP_FILE)) {
        return false;
    }
    if (!fileCommand("FSRN", TEMP_FILE, CONFIG_FILE)) {
        ModemAT_printLine("[SAVE FAIL] Commit failed; inspect config.bak and config.tmp.");
        return false;
    }
    if (!readConfig(CONFIG_FILE, &verified, payload) || !sameConfig(active, verified)) {
        ModemAT_printLine("[SAVE FAIL] Final verification failed; backup retained.");
        return false;
    }
    dirty = false;
    ModemAT_printLine("[SAVE PASS] config.ini read-back matches RAM and serialized bytes.");
    if (hadConfig && !fileCommand("FSDF", BACKUP_FILE)) {
        ModemAT_printLine("[STORE WARNING] Saved file verified, but backup cleanup failed.");
    }
    return true;
}

static void reloadConfig() {
    ModemAT_printLine("========== LOAD: FILE -> VALIDATE -> RAM ==========");
    Configuration loaded = {};
    if (readConfig(CONFIG_FILE, &loaded)) {
        active = loaded;
        dirty = false;
        lastTickMs = millis();
        ModemAT_printLine("[LOAD PASS] Valid settings applied from config.ini.");
    } else {
        dirty = true;
        ModemAT_printLine("[LOAD FAIL] RAM unchanged; existing file was not overwritten.");
    }
    showConfig();
}

static void printHelp() {
    ModemAT_printLine("[INFO] ConfigFile / ESP32S3 Dev Module / 115200 baud / SIM not required");
    ModemAT_printLine("[MENU] s show | i interval 10/30/60 | w save+verify | r reload | d defaults | h help");
    ModemAT_printLine("[INFO] Enter one key and wait for Menu ready; CR/LF ignored.");
}

void setup() {
    bool initialized = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    ModemAT_begin(MDMSerial, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!initialized || !ModemPower_powerOn()) {
        ModemAT_printLine("[ERROR] Power-on failed; ConfigFile not started.");
        return;
    }
    printHelp();
    if (!listFiles()) {
        ModemAT_printLine("[BOOT] File list failed; defaults in RAM only. No file created.");
    } else if (exists[0]) {
        reloadConfig();
    } else if (exists[2]) {
        ModemAT_printLine("[BOOT] config.ini missing but config.bak exists; preserve recovery files.");
    } else {
        ModemAT_printLine("[BOOT] config.ini missing; create default settings file.");
        saveConfig();
    }
    ready = true;
    lastTickMs = millis();
    showConfig();
    ModemAT_printLine("[STATE] Menu ready");
}

void loop() {
    ModemAT_pollIdle();
    if (!ready) {
        return;
    }
    if (Serial.available() > 0) {
        int key = Serial.read();
        if (key == '\r' || key == '\n') {
            return;
        }
        if (key == 's') {
            showConfig();
        } else if (key == 'i') {
            active.intervalSec = active.intervalSec == 10 ? 30 : active.intervalSec == 30 ? 60 : 10;
            dirty = true;
            lastTickMs = millis();
            ModemAT_printLine("[RAM CHANGE] Interval changed; press w to persist.");
            showConfig();
        } else if (key == 'w') {
            saveConfig();
            showConfig();
        } else if (key == 'r') {
            reloadConfig();
        } else if (key == 'd') {
            active.version = 1;
            strcpy(active.deviceId, "N706_Client_01");
            active.intervalSec = 10;
            dirty = true;
            lastTickMs = millis();
            ModemAT_printLine("[RAM DEFAULTS] Defaults applied in RAM only; press w to persist.");
            showConfig();
        } else if (key == 'h') {
            printHelp();
        } else {
            ModemAT_printLine("[INPUT] Unknown menu key; enter h for help.");
        }
        int queued = Serial.available();
        while (queued-- > 0) {
            Serial.read();
        }
        ModemAT_printLine("[STATE] Menu ready");
    }
    if ((unsigned long)(millis() - lastTickMs) >= active.intervalSec * 1000UL) {
        lastTickMs = millis();
        char line[128];
        snprintf(line, sizeof(line), "[TICK] device_id=%s | count=%lu | interval=%us | saved=%s",
            active.deviceId, ++tickCount, active.intervalSec, dirty ? "no" : "verified");
        ModemAT_printLine(line);
    }
}
