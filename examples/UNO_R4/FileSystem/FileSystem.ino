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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned long QUERY_TIMEOUT_MS = 5000;
static const unsigned long WRITE_TIMEOUT_MS = 10000;
static const char SAMPLE_FILE[] = "sample.txt";
static const char ARCHIVE_FILE[] = "archive.txt";
static bool sampleExists = false;
static bool archiveExists = false;
static bool listInvalid = false;

static void listLine(const char *line) {
    const char *record = line;
    if (!strncmp(record, "+FSLIST:", 8)) {
        record += 8;
        while (*record == ' ') {
            ++record;
        }
    }
    const char *comma = strrchr(record, ',');
    if (comma == NULL) {
        return;
    }
    size_t length = (size_t)(comma - record);
    bool sample = length == strlen(SAMPLE_FILE) && !strncmp(record, SAMPLE_FILE, length);
    bool archive = length == strlen(ARCHIVE_FILE) && !strncmp(record, ARCHIVE_FILE, length);
    if (!sample && !archive) {
        return;
    }
    char *end;
    strtoul(comma + 1, &end, 10);
    if (end == comma + 1 || *end != '\0') {
        listInvalid = true;
        return;
    }
    sampleExists = sampleExists || sample;
    archiveExists = archiveExists || archive;
}

static bool listFiles() {
    sampleExists = false;
    archiveExists = false;
    listInvalid = false;
    ModemATResult result = ModemAT_executeWithHandler(
        "AT+FSLIST?", QUERY_TIMEOUT_MS, listLine);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK && !listInvalid;
}

static bool command(const char *text) {
    ModemATResult result = ModemAT_execute(text, NULL, QUERY_TIMEOUT_MS);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK;
}

static bool validFileName(const char *name) {
    size_t length = strlen(name);
    return length > 0 && length <= 50 && strpbrk(name, "\"\r\n") == NULL;
}

static bool deleteFile(const char *name) {
    char text[96];
    snprintf(text, sizeof(text), "AT+FSDF=\"%s\"", name);
    return command(text);
}

static bool renameFile(const char *source, const char *destination) {
    char text[128];
    snprintf(text, sizeof(text), "AT+FSRN=\"%s\",\"%s\"", source, destination);
    return command(text);
}

static bool query(const char *text, const char *prefix, char *value, size_t capacity) {
    ModemATResult result = ModemAT_execute(text, prefix, QUERY_TIMEOUT_MS);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK &&
        ModemAT_copyResponseValue(value, capacity) == MODEM_AT_VALUE_OK;
}

static bool checkSize(const char *name, unsigned long expected) {
    char text[96];
    char value[64];
    snprintf(text, sizeof(text), "AT+FSFS=\"%s\"", name);
    if (!query(text, "+FSFS:", value, sizeof(value))) {
        return false;
    }
    char *end;
    unsigned long actual = strtoul(value, &end, 10);
    return end != value && *end == '\0' && actual == expected;
}

static bool readData(const char *name, int mode, unsigned int size,
    unsigned int position, const char *expected) {
    char text[96];
    char value[64];
    if (mode == 0) {
        snprintf(text, sizeof(text), "AT+FSRF=\"%s\",0,%u", name, size);
    } else {
        snprintf(text, sizeof(text), "AT+FSRF=\"%s\",1,%u,%u", name, size, position);
    }
    if (!query(text, "+FSRF:", value, sizeof(value))) {
        return false;
    }
    char *end;
    unsigned long length = strtoul(value, &end, 10);
    if (end == value || *end != ',' || length != size) {
        return false;
    }
    return strlen(end + 1) == size && !strcmp(end + 1, expected);
}

static bool writeData(const char *name, int mode, const char *payload) {
    char text[96];
    char log[96];
    size_t length = strlen(payload);
    snprintf(text, sizeof(text), "AT+FSWF=\"%s\",%d,%u,%lu",
        name, mode, (unsigned int)length, WRITE_TIMEOUT_MS);
    snprintf(log, sizeof(log), "[PAYLOAD] %s (%u bytes; no CR/LF)",
        payload, (unsigned int)length);
    ModemAT_printLine(log);
    ModemATResult result = ModemAT_writeData(text, (const uint8_t *)payload,
        length, WRITE_TIMEOUT_MS);
    ModemAT_reportResult(result);
    return result == MODEM_AT_OK;
}

static bool runFileTest() {
    ModemAT_printLine("[STEP 1] Remove only the two configured test files.");
    if (!listFiles()) {
        return false;
    }
    bool removeSample = sampleExists;
    bool removeArchive = archiveExists;
    if (removeSample && !deleteFile(SAMPLE_FILE)) {
        return false;
    }
    if (removeArchive && !deleteFile(ARCHIVE_FILE)) {
        return false;
    }
    if (!listFiles() || sampleExists || archiveExists) {
        return false;
    }
    ModemAT_printLine("[STEP 2] Check free space before writing.");
    char value[64];
    if (!query("AT+FSLS?", "+FSLS:", value, sizeof(value))) {
        return false;
    }
    char *end;
    unsigned long freeBytes = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || freeBytes < 8) {
        ModemAT_printLine("[ERROR] Free space unavailable or below 8 bytes.");
        return false;
    }
    ModemAT_printLine("[STEP 3] Write HELLO and verify list, size and content.");
    if (!writeData(SAMPLE_FILE, 0, "HELLO") || !listFiles() || !sampleExists || archiveExists ||
        !checkSize(SAMPLE_FILE, 5) || !readData(SAMPLE_FILE, 0, 5, 0, "HELLO")) {
        return false;
    }
    ModemAT_printLine("[STEP 4] Append 123 and verify HELLO123.");
    if (!writeData(SAMPLE_FILE, 1, "123") || !checkSize(SAMPLE_FILE, 8) ||
        !readData(SAMPLE_FILE, 0, 8, 0, "HELLO123")) {
        return false;
    }
    ModemAT_printLine("[STEP 5] Read HELL and O123 using a zero-based byte offset.");
    if (!readData(SAMPLE_FILE, 0, 4, 0, "HELL") ||
        !readData(SAMPLE_FILE, 1, 4, 4, "O123")) {
        return false;
    }
    ModemAT_printLine("[STEP 6] Rename and verify the configured archive file.");
    if (!renameFile(SAMPLE_FILE, ARCHIVE_FILE) || !listFiles() ||
        sampleExists || !archiveExists || !checkSize(ARCHIVE_FILE, 8) ||
        !readData(ARCHIVE_FILE, 0, 8, 0, "HELLO123")) {
        return false;
    }
    ModemAT_printLine("[STEP 7] Delete archive file; verify both configured names are absent.");
    if (!deleteFile(ARCHIVE_FILE) || !listFiles() ||
        sampleExists || archiveExists) {
        return false;
    }
    return true;
}

void setup() {
    bool ready = ModemPower_begin(&POWER_CONFIG);
    Serial.begin(PC_BAUD);
    Serial1.begin(MODEM_BAUD);
    ModemAT_begin(Serial1, Serial, QUIET_MS);
    unsigned long start = millis();
    while (!Serial && (unsigned long)(millis() - start) < SERIAL_WAIT_MS) {
        delay(1);
    }
    if (!ready || !ModemPower_powerOn()) {
        ModemAT_printLine("[FAIL] Modem readiness check failed.");
        return;
    }
    ModemAT_printLine("[INFO] FileSystem / UNO R4 / 115200 baud.");
    if (!validFileName(SAMPLE_FILE) || !validFileName(ARCHIVE_FILE) ||
        !strcmp(SAMPLE_FILE, ARCHIVE_FILE)) {
        ModemAT_printLine("[FAIL] Names must differ: 1-50 bytes, without quotes or CR/LF.");
        return;
    }
    char notice[160];
    snprintf(notice, sizeof(notice), "[INFO] Existing %s and %s WILL be deleted.",
        SAMPLE_FILE, ARCHIVE_FILE);
    ModemAT_printLine(notice);
    bool passed = runFileTest();
    ModemAT_printLine(passed ? "[PASS] File write, append, read, rename and delete verified."
        : "[FAIL] Test stopped. Inspect the last STEP, TX and RX; files may remain.");
    ModemAT_printLine("[DONE] One run only. Reset to run again; no automatic cleanup on failure.");
}

void loop() {
    ModemAT_pollIdle();
}
