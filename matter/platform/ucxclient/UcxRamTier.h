/*
 *    Copyright (c) 2026 u-blox AG
 *
 *    RAM-budget tier of the Matter node's large static buffers (the UDP TX queue so far), one per
 *    u-connectXpressApp target, so every buffer is sized from the same choice.
 *
 *      TIGHT   NUCLEO-F439ZI (256 KB RAM), STM32F4DISCOVERY (192 KB): 8 TX packets.
 *      MEDIUM  NUCLEO-H753ZI (1 MB), NUCLEO-F767ZI (512 KB): 32 TX packets.
 *      LARGE   Windows, Linux, WebAssembly: 64 TX packets.
 *
 *    The other boards of the app (F401, F446, F103, G071) have 512 KB of flash or less, too little
 *    for the Matter SDK: they get no Matter node.
 */
#pragma once

#if defined(STM32F439xx) || defined(STM32F407xx)
#define UCX_RAM_TIER_TIGHT   1
#elif defined(STM32H753xx) || defined(STM32F767xx)
#define UCX_RAM_TIER_MEDIUM  1
#else
#define UCX_RAM_TIER_LARGE   1
#endif
