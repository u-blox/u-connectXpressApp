# Using ucxapp from your own application

ucxapp owns the module's UART. Your application does not send AT commands to the module; it asks ucxapp. The
module, its URCs, the secure store and the trace (`at.trace`) then stay in one place, and the same commands work
in the shell, in scripts, in the browser and in firmware.

| Your application | Best way | Why |
|---|---|---|
| Linux or Windows program, an action now and then (setup, test step, CI) | `ucxapp <port> --exec "..."` | No integration code; exit code 1 at the first failing command |
| Linux program that runs all the time and needs events | a bridge on TCP: `hw.bridge tcps`, `matter.bridge tcps` | Lines in, `OK`/`ERR` lines out, `EVT` lines pushed when something changes |
| A browser or another machine | the web UI's `/api` (`web.start`) | HTTPS with a token, the same commands |
| Firmware on an STM32 board (FreeRTOS or Zephyr) | build into the ucxapp firmware and call `ucxapp_cmd_exec()` | No second UART and no parsing |

## 1. Linux or Windows: one-shot commands

```sh
./app/bin/ucxapp /dev/ttyUSB0 --exec "wifi.profile.connect ubx" --exec "net.ping 8.8.8.8" || echo failed
```

From any language, run that as a child process and check the exit code.
[ucxapp_exec.py](../examples/integration/ucxapp_exec.py) does it for Python. Put
`app.wait <ms>` between two steps that need time (a node or bridge reacting).

## 2. Linux: a long-running link with events

Start ucxapp once (a service, tmux, or your own program) and open a bridge:

```text
hw.bridge tcps 7550        # the board's pins: gpio.read 3, gpio.write 3 1, ...
matter.bridge tcps         # the Matter node on port 7540: set light on, get; EVT lines for every change
```

Connect to the port, send one command per line and read one reply line per command. `EVT ...` lines arrive in
between whenever an input or an endpoint changes, so read lines in one place and sort them: `EVT` lines to
your event handler, the rest as the reply to the command you sent.
[bridge_client.py](../examples/integration/bridge_client.py) does that in Python with a reader thread.
`nc localhost 7540` is enough to try it, and
`hw.exec <line>` in the shell runs one `hw.bridge` line.

## 3. STM32: inside your firmware

Add your source to the ucxapp firmware (`app/stm32_freertos/CMakeLists.txt` or `app/zephyr/CMakeLists.txt`)
and run commands from your own task. Their output goes through `ucxapp_print`, so it shows on the console as if
typed:

```c
#include "ucxapp_cmd.h"
#include "ucxapp_hw.h"

static void myTask(void *arg)
{
    ucxapp_t *app = arg;   /* the app handle of ucxapp_main_freertos.c */
    char line[] = "wifi.profile.connect ubx";   /* cut up in place: always a writable copy */

    if (ucxapp_cmd_exec(app, line) != UCXAPP_OK) {
        (void)ucxapp_hw_led(true);   /* the board's user LED as a fault light */
    }
    for (;;) {
        if (ucxapp_hw_button() == 1) {   /* the board's user button */
            char ping[] = "net.ping 8.8.8.8";

            (void)ucxapp_cmd_exec(app, ping);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

The same on Zephyr, as a thread (`ucxapp_main_zephyr.c` holds the app handle):

```c
#include <zephyr/kernel.h>
#include "ucxapp_cmd.h"
#include "ucxapp_hw.h"

extern ucxapp_t *gMyApp;   /* set from ucxapp_main_zephyr.c before the thread starts */

static void myThread(void *p1, void *p2, void *p3)
{
    char line[] = "wifi.profile.connect ubx";

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    if (ucxapp_cmd_exec(gMyApp, line) != UCXAPP_OK) {
        (void)ucxapp_hw_led(true);
    }
    for (;;) {
        if (ucxapp_hw_button() == 1) {
            char ping[] = "net.ping 8.8.8.8";

            (void)ucxapp_cmd_exec(gMyApp, ping);
        }
        k_msleep(50);
    }
}

K_THREAD_DEFINE(myThreadId, 4096, myThread, NULL, NULL, NULL, 7, 0, 1000);
```

On Zephyr the button and LED need `CONFIG_GPIO=y` (set in `app/zephyr/boards/<board>.conf` for h753, f767,
f439 and f407).

Rules:
- One command at a time: the console runs commands too, so don't type while your task runs one.
- Keep data paths (sockets, bridges) on ucxapp's commands rather than calling u-connectClient beside them, so
  the URCs keep one owner.
- `ucxapp_hw_button()` and `ucxapp_hw_led()` use the button and LED of the board profile
  (`app/boards/<board>.json`); `hw.bridge` and `hw.profile.load` are PC-only.
