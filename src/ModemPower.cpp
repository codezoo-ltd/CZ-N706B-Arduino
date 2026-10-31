#include "ModemPower.h"
#include "ModemAT.h"
#include <stdio.h>

static const unsigned int BOOT_MINIMUM_AT_PROBES = 10;

static ModemPowerConfig gConfig;
static bool initialized = false;
static bool ldoEnabled = false;
static bool uncertainPower = false;
static bool knownOff = false;

static bool validConfig(const ModemPowerConfig *config) {
    if (config == NULL || config->onOffPin == config->ldoEnablePin) {
        return false;
    }
    if (config->powerOnHighMs == 0 || config->powerOffHighMs == 0 ||
        config->keyReleaseDelayMs == 0 || config->powerOnRxDrainTimeoutMs == 0 ||
        config->powerOnRxDrainQuietMs == 0 || config->atTimeoutMs == 0 ||
        config->bootTimeoutMs == 0 || config->atRetryMs == 0 ||
        config->offConfirmMs == 0 || config->offSilenceMs == 0 ||
        config->offMinimumProbes == 0) {
        return false;
    }
    return true;
}

static void powerPassiveDelay(unsigned long durationMs) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < durationMs) {
        delay(1);
    }
}

static void powerReceiveDelay(unsigned long durationMs) {
    unsigned long start = millis();
    while ((unsigned long)(millis() - start) < durationMs) {
        ModemAT_poll();
        delay(1);
    }
}

static void powerPassivePulse(unsigned long highMs) {
    digitalWrite(gConfig.onOffPin, HIGH);
    powerPassiveDelay(highMs);
    digitalWrite(gConfig.onOffPin, LOW);
    powerPassiveDelay(gConfig.keyReleaseDelayMs);
}

static void powerReceivePulse(unsigned long highMs) {
    digitalWrite(gConfig.onOffPin, HIGH);
    powerReceiveDelay(highMs);
    digitalWrite(gConfig.onOffPin, LOW);
    powerReceiveDelay(gConfig.keyReleaseDelayMs);
}

static ModemATResult observeAt(unsigned long durationMs, bool stopOnOk) {
    return ModemAT_observeAt(
        durationMs,
        gConfig.atRetryMs,
        stopOnOk ? 0 : gConfig.offMinimumProbes,
        gConfig.offSilenceMs,
        stopOnOk);
}

static void printPulseState(const char *state, unsigned long highMs) {
    char line[128];
    snprintf(
        line,
        sizeof(line),
        "[STATE] %s: ON_OFF HIGH %lu ms, LOW, release wait %lu ms%s",
        state,
        highMs,
        gConfig.keyReleaseDelayMs,
        highMs == gConfig.powerOnHighMs ? " (one pulse)" : "");
    ModemAT_printLine(line);
}

bool ModemPower_begin(const ModemPowerConfig *config) {
    initialized = false;
    ldoEnabled = false;
    uncertainPower = false;
    knownOff = false;
    if (!validConfig(config)) {
        return false;
    }
    gConfig = *config;
    digitalWrite(gConfig.onOffPin, LOW);
    pinMode(gConfig.onOffPin, OUTPUT);
    digitalWrite(gConfig.ldoEnablePin, LOW);
    pinMode(gConfig.ldoEnablePin, OUTPUT);
    initialized = true;
    return true;
}

bool ModemPower_isInitialized(void) {
    return initialized;
}

bool ModemPower_powerOn(void) {
    if (!initialized || !ModemAT_isInitialized()) {
        ModemAT_printLine("[STATE] Power API not initialized");
        return false;
    }
    if (uncertainPower && ldoEnabled && !knownOff) {
        ModemAT_printLine("[STATE] Power state unresolved: rechecking AT silence before ON pulse");
        ModemATResult recheck = observeAt(gConfig.offConfirmMs, false);
        if (recheck == MODEM_AT_NO_RESPONSE) {
            uncertainPower = false;
            knownOff = true;
            ModemAT_printLine("[STATE] Shutdown confirmed by absence of valid AT responses");
        } else {
            ModemAT_printLine(
                "[STATE] OFF remains unconfirmed: RX or AT response persists; ON pulse not sent");
            if (recheck == MODEM_AT_OVERFLOW || recheck == MODEM_AT_BLOCKED) {
                ModemAT_reportResult(recheck);
            }
            return false;
        }
    }

    if (!ldoEnabled || knownOff) {
        if (!ldoEnabled) {
            digitalWrite(gConfig.ldoEnablePin, HIGH);
            ldoEnabled = true;
            powerPassiveDelay(gConfig.ldoStartDelayMs);
        }

        powerPassivePulse(gConfig.powerOnHighMs);

        ModemATResult prepared = ModemAT_prepareAfterPowerOn(
            gConfig.powerOnRxDrainTimeoutMs,
            gConfig.powerOnRxDrainQuietMs);
        if (prepared != MODEM_AT_OK) {
            uncertainPower = true;
            knownOff = false;
            ModemAT_printLine("[STATE] RX preparation after power-on did not reach a quiet boundary");
            ModemAT_reportResult(prepared);
            return false;
        }

        printPulseState("ON pulse completed", gConfig.powerOnHighMs);
        ModemATResult result = ModemAT_observeAt(
            gConfig.bootTimeoutMs,
            gConfig.atRetryMs,
            BOOT_MINIMUM_AT_PROBES,
            gConfig.offSilenceMs,
            true);
        ModemAT_reportResult(result);
        if (result == MODEM_AT_OK && ModemAT_commandBoundaryReady()) {
            uncertainPower = false;
            knownOff = false;
            return true;
        }

        uncertainPower = true;
        knownOff = false;
        ModemAT_printLine("[STATE] Boot AT not confirmed; no repeated ON_OFF pulse. Inspect hardware and RX.");
        return false;
    }

    ModemATResult probe = observeAt(gConfig.atTimeoutMs, true);
    ModemAT_reportResult(probe);
    if (probe == MODEM_AT_OK) {
        uncertainPower = false;
        return ModemAT_commandBoundaryReady();
    }
    if (probe != MODEM_AT_NO_RESPONSE || uncertainPower) {
        ModemAT_printLine("[STATE] Power unknown or RX active: no blind/repeated ON_OFF pulse");
        return false;
    }

    ModemAT_printLine("[STATE] No confirmed OFF state: no blind/repeated ON_OFF pulse");
    return false;
}

ModemPowerOffResult ModemPower_powerOff(void) {
    if (!initialized || !ModemAT_isInitialized()) {
        ModemAT_printLine("[STATE] Power API not initialized");
        return MODEM_POWER_OFF_REJECTED_NOT_INITIALIZED;
    }
    if (!ldoEnabled) {
        ModemAT_printLine("[STATE] LDO has not been enabled");
        return MODEM_POWER_OFF_REJECTED_LDO_DISABLED;
    }
    if (uncertainPower) {
        ModemAT_printLine("[STATE] OFF refused: power state unknown; no blind ON_OFF toggle");
        return MODEM_POWER_OFF_REJECTED_POWER_UNKNOWN;
    }

    ModemATResult probe = observeAt(gConfig.atTimeoutMs, true);
    ModemAT_reportResult(probe);
    if (probe != MODEM_AT_OK || !ModemAT_commandBoundaryReady()) {
        ModemAT_printLine("[STATE] OFF refused: AT communication not confirmed");
        return MODEM_POWER_OFF_REJECTED_AT_NOT_READY;
    }

    printPulseState("OFF", gConfig.powerOffHighMs);
    powerReceivePulse(gConfig.powerOffHighMs);

    ModemATResult result = observeAt(gConfig.offConfirmMs, false);
    if (result == MODEM_AT_NO_RESPONSE) {
        uncertainPower = false;
        knownOff = true;
        ModemAT_printLine("[STATE] Shutdown confirmed by absence of valid AT responses");
        ModemAT_printLine(
            "[STATE] LDO_EN stays HIGH. ON_OFF LOW only releases the pulse; PWRMON is not connected, so physical power OFF is not confirmed.");
        return MODEM_POWER_OFF_AT_SILENT;
    }

    uncertainPower = true;
    knownOff = false;
    ModemAT_printLine(
        "[STATE] Shutdown unconfirmed: OK/RX persists, partial RX, or receive error");
    if (result == MODEM_AT_OVERFLOW || result == MODEM_AT_BLOCKED) {
        ModemAT_reportResult(result);
    }
    ModemAT_printLine(
        "[STATE] LDO_EN stays HIGH. ON_OFF LOW only releases the pulse; PWRMON is not connected, so physical power OFF is not confirmed.");
    return MODEM_POWER_OFF_UNCONFIRMED;
}
