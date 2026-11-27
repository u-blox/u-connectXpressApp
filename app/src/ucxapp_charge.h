/* The charge ledger: what a battery-powered device takes, counted by kind where it is taken.
 *
 * Portable and bounded (a counter a kind, no platform, no shell), so the firmware of a device can keep it and the
 * app can show it. The unit is the microampere second (uAs), which is a microcoulomb: what ppk.measure reports
 * as charge. power.* shows the ledger and says how long a cell lasts at its rate (ucxapp_cmds_power.c). */
#ifndef UCXAPP_CHARGE_H
#define UCXAPP_CHARGE_H

#include <stdint.h>

typedef enum {
    UCXAPP_CHARGE_SLEEP,       /* asleep: the floor everything else stands on */
    UCXAPP_CHARGE_CPU,         /* awake and computing */
    UCXAPP_CHARGE_SENSORS,
    UCXAPP_CHARGE_BLE,         /* advertising, scanning, a link */
    UCXAPP_CHARGE_RANGING,     /* Channel Sounding */
    UCXAPP_CHARGE_WIFI,
    UCXAPP_CHARGE_GNSS,
    UCXAPP_CHARGE_CELL,        /* a cellular modem */
    UCXAPP_CHARGE_STORAGE,     /* writing flash */
    UCXAPP_CHARGE_CONSOLE,     /* a console that keeps the device awake */
    UCXAPP_CHARGE_KINDS
} ucxappCharge_t;

/* Empties the ledger; nowMs is the time it counts from. */
void ucxapp_charge_reset(uint64_t nowMs);
void ucxapp_charge_spend(ucxappCharge_t kind, uint32_t uAs);
uint64_t ucxapp_charge_spent(ucxappCharge_t kind);
uint64_t ucxapp_charge_total(void);
uint64_t ucxapp_charge_since_ms(void);
const char *ucxapp_charge_name(ucxappCharge_t kind);
/* the kind of a name, UCXAPP_CHARGE_KINDS when there is none */
ucxappCharge_t ucxapp_charge_kind(const char *name);
/* the mean current in uA when uAs were taken in ms; 0 for no time */
double ucxapp_charge_mean_ua(uint64_t uAs, uint64_t ms);
/* how long a cell of mAh lasts at a mean current, in years; 0 where it cannot be said */
double ucxapp_charge_years(double mAh, double meanUa);

#endif
