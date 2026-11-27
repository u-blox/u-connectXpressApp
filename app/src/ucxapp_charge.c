/* The charge ledger (ucxapp_charge.h). Portable, bounded: a counter a kind. */
#include "ucxapp_charge.h"

#include <string.h>

static uint64_t gSpent[UCXAPP_CHARGE_KINDS];
static uint64_t gSinceMs;

static const char *const kName[UCXAPP_CHARGE_KINDS] = {
    "sleep", "cpu", "sensors", "ble", "ranging", "wifi", "gnss", "cell", "storage", "console",
};

void ucxapp_charge_reset(uint64_t nowMs)
{
    memset(gSpent, 0, sizeof(gSpent));
    gSinceMs = nowMs;
}

void ucxapp_charge_spend(ucxappCharge_t kind, uint32_t uAs)
{
    if ((unsigned)kind < UCXAPP_CHARGE_KINDS) {
        gSpent[kind] += uAs;
    }
}

uint64_t ucxapp_charge_spent(ucxappCharge_t kind)
{
    return (unsigned)kind < UCXAPP_CHARGE_KINDS ? gSpent[kind] : 0;
}

uint64_t ucxapp_charge_total(void)
{
    uint64_t sum = 0;

    for (int k = 0; k < UCXAPP_CHARGE_KINDS; k++) {
        sum += gSpent[k];
    }
    return sum;
}

uint64_t ucxapp_charge_since_ms(void)
{
    return gSinceMs;
}

const char *ucxapp_charge_name(ucxappCharge_t kind)
{
    return (unsigned)kind < UCXAPP_CHARGE_KINDS ? kName[kind] : "?";
}

ucxappCharge_t ucxapp_charge_kind(const char *name)
{
    for (int k = 0; k < UCXAPP_CHARGE_KINDS; k++) {
        if (strcmp(name, kName[k]) == 0) {
            return (ucxappCharge_t)k;
        }
    }
    return UCXAPP_CHARGE_KINDS;
}

double ucxapp_charge_mean_ua(uint64_t uAs, uint64_t ms)
{
    return ms == 0 ? 0.0 : (double)uAs * 1000.0 / (double)ms;
}

double ucxapp_charge_years(double mAh, double meanUa)
{
    if (mAh <= 0.0 || meanUa <= 0.0) {
        return 0.0;
    }
    return mAh * 1000.0 / meanUa / (24.0 * 365.25);
}
