#ifndef UCXAPP_DEMO_H
#define UCXAPP_DEMO_H

/* Demo material for running the examples on a virtual module (see ucxapp_demo.c). */

#include <stddef.h>

#include "ucxapp.h"

/* On a virtual module: demo values for unset ${VAR}s and the demo files the examples read (made when missing). */
void ucxapp_demo_prepare(ucxapp_t *app);
/* On a virtual module: the second COM port of an example line (COM20) becomes virt:pc. */
void ucxapp_demo_line(ucxapp_t *app, char *line, size_t size);

#endif
