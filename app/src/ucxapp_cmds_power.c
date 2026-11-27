/* power.*: the charge ledger (ucxapp_charge.h) shown, and a battery budget made from it. Put in what ppk.measure
 * reported for each thing a device does in a period (its charge in uC is uAs), and power.budget says how long a
 * cell lasts. Firmware that keeps the ledger itself fills it as it runs; power.status then shows what it took. */
#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_charge.h"

#include <stdlib.h>
#include <string.h>

#define CELL_MAH_DEFAULT 220.0   /* a CR2032 */

static bool numberArg(const char *text, double low, double high, double *out)
{
    char *end = NULL;
    double v = strtod(text, &end);

    if (end == text || *end != '\0' || !(v >= low && v <= high)) {
        return false;
    }
    *out = v;
    return true;
}

static void ledgerPrint(void)
{
    for (int k = 0; k < UCXAPP_CHARGE_KINDS; k++) {
        uint64_t uAs = ucxapp_charge_spent((ucxappCharge_t)k);

        if (uAs > 0) {
            ucxapp_print("  %-8s %10.3f mAs\n", ucxapp_charge_name((ucxappCharge_t)k), (double)uAs / 1000.0);
        }
    }
    ucxapp_print("  %-8s %10.3f mAs\n", "total", (double)ucxapp_charge_total() / 1000.0);
}

static void lifePrint(double mAh, double meanUa)
{
    double years = ucxapp_charge_years(mAh, meanUa);

    if (years <= 0.0) {
        ucxapp_print("  nothing was spent: no life to say\n");
    } else if (years >= 1.0) {
        ucxapp_print("  a cell of %.0f mAh lasts %.1f years at %.2f uA\n", mAh, years, meanUa);
    } else {
        ucxapp_print("  a cell of %.0f mAh lasts %.0f days at %.2f uA\n", mAh, years * 365.25, meanUa);
    }
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    double mAh = CELL_MAH_DEFAULT;
    uint64_t ms = (uint64_t)ucxapp_platform_tick_ms() - ucxapp_charge_since_ms();

    (void)app;
    if (argc > 2 || (argc == 2 && !numberArg(argv[1], 1.0, 100000.0, &mAh))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("charge taken in the last %lu s:\n", (unsigned long)(ms / 1000u));
    ledgerPrint();
    lifePrint(mAh, ucxapp_charge_mean_ua(ucxapp_charge_total(), ms));
    return UCXAPP_OK;
}

static int cmdBudget(ucxapp_t *app, int argc, char *argv[])
{
    double period = 0.0;
    double mAh = CELL_MAH_DEFAULT;

    (void)app;
    if (argc < 2 || argc > 3 || !numberArg(argv[1], 1.0, 1e9, &period)
        || (argc == 3 && !numberArg(argv[2], 1.0, 100000.0, &mAh))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("if this is what %.0f s take:\n", period);
    ledgerPrint();
    lifePrint(mAh, ucxapp_charge_mean_ua(ucxapp_charge_total(), (uint64_t)(period * 1000.0)));
    return UCXAPP_OK;
}

static int cmdSpend(ucxapp_t *app, int argc, char *argv[])
{
    ucxappCharge_t kind;
    double uAs = 0.0;

    (void)app;
    if (argc != 3) {
        return UCXAPP_ERR_USAGE;
    }
    kind = ucxapp_charge_kind(argv[1]);
    if (kind == UCXAPP_CHARGE_KINDS || !numberArg(argv[2], 0.0, 4e9, &uAs)) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_charge_spend(kind, (uint32_t)uAs);
    return UCXAPP_OK;
}

static int cmdReset(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_charge_reset(ucxapp_platform_tick_ms());
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gPowerCmds[] = {
    { "power.status", "[cell mAh=220]", "The charge taken since the ledger was emptied, by kind, and how long a cell lasts at that rate", 0, cmdStatus },
    { "power.budget", "<seconds> [cell mAh=220]", "Take what the ledger holds as the cost of a period, and say how long a cell lasts", 0, cmdBudget },
    { "power.spend", "<sleep|cpu|sensors|ble|ranging|wifi|gnss|cell|storage|console> <uAs>", "Put a charge into the ledger, e.g. what ppk.measure reported (uC is uAs)", 0, cmdSpend },
    { "power.reset", "", "Empty the ledger", 0, cmdReset },
};

void ucxapp_register_power_cmds(void)
{
    ucxapp_charge_reset(ucxapp_platform_tick_ms());
    ucxapp_cmd_register(gPowerCmds, sizeof(gPowerCmds) / sizeof(gPowerCmds[0]));
}
