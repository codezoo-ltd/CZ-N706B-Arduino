#ifndef CZ_N706B_MODEM_POWER_H
#define CZ_N706B_MODEM_POWER_H

#include <Arduino.h>

struct ModemPowerConfig {
    uint8_t onOffPin;
    uint8_t ldoEnablePin;
    unsigned long ldoStartDelayMs;
    unsigned long powerOnHighMs;
    unsigned long powerOffHighMs;
    unsigned long keyReleaseDelayMs;
    unsigned long powerOnRxDrainTimeoutMs;
    unsigned long powerOnRxDrainQuietMs;
    unsigned long atTimeoutMs;
    unsigned long bootTimeoutMs;
    unsigned long atRetryMs;
    unsigned long offConfirmMs;
    unsigned long offSilenceMs;
    unsigned int offMinimumProbes;
};

enum ModemPowerOffResult {
    MODEM_POWER_OFF_AT_SILENT,
    MODEM_POWER_OFF_UNCONFIRMED,
    MODEM_POWER_OFF_REJECTED_NOT_INITIALIZED,
    MODEM_POWER_OFF_REJECTED_LDO_DISABLED,
    MODEM_POWER_OFF_REJECTED_POWER_UNKNOWN,
    MODEM_POWER_OFF_REJECTED_AT_NOT_READY
};

bool ModemPower_begin(const ModemPowerConfig *config);
bool ModemPower_isInitialized(void);
bool ModemPower_powerOn(void);
ModemPowerOffResult ModemPower_powerOff(void);

#endif
