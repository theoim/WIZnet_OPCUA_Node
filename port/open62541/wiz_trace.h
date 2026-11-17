/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WIZ_TRACE_H
#define WIZ_TRACE_H

/* Where the loop was when the watchdog fired.
 *
 * A watchdog reset leaves nothing on the console: two LINKDOWN runs of the
 * 12 h soak (2026-10-20) reset the board 11 s and 24 s into the outage with no
 * line printed in between. Each potentially blocking step writes its code here
 * first. The watchdog scratch registers survive a watchdog reset (only a
 * power-on or RUN reset clears them), so the next boot can print the last step
 * entered. Scratch 4-7 belong to the SDK and bootrom; 0-3 are free.
 *
 *   scratch[0]  stage code (WIZ_TR_*), 0 = never marked
 *   scratch[1]  detail: socket number in bits 31..24, step-specific low bits
 *   scratch[2]  ms since boot when the stage was entered
 *   scratch[3]  ms since boot of the last watchdog_update() */

#include <stdint.h>

#include "hardware/structs/watchdog.h"
#include "pico/time.h"

enum {
    WIZ_TR_NONE = 0,
    /* main loop, in call order */
    WIZ_TR_USB = 0x01,
    WIZ_TR_UART = 0x02,
    WIZ_TR_NET = 0x03,
    WIZ_TR_SNTP = 0x04,
    WIZ_TR_OPCUA = 0x05,
    WIZ_TR_SLEEP = 0x06,
    /* eventloop */
    WIZ_TR_TIMERS = 0x10,       /* open62541 timers: publish, renew, sessions */
    WIZ_TR_SEND_FSR = 0x20,     /* wiz_send_all waiting for TX space */
    WIZ_TR_SEND = 0x21,         /* wiz_send(): SENDOK check, copy, SEND */
    WIZ_TR_RECV = 0x30,
    WIZ_TR_RX_CB = 0x31,        /* open62541 processing a received chunk */
    WIZ_TR_RECV_COPY = 0x32,    /* copying the RX buffer out over QSPI */
    WIZ_TR_RECV_CMD = 0x33,     /* RECV command, bounded wait */
    WIZ_TR_DISCON = 0x40,       /* DISCON command */
    WIZ_TR_CLOSE = 0x41,        /* ioLibrary close() */
    WIZ_TR_OPEN = 0x42,         /* ioLibrary socket() / listen() */
};

static inline void wiz_trace(uint32_t stage, uint32_t detail) {
    watchdog_hw->scratch[0] = stage;
    watchdog_hw->scratch[1] = detail;
    watchdog_hw->scratch[2] = to_ms_since_boot(get_absolute_time());
}

static inline void wiz_trace_fed(void) {
    watchdog_hw->scratch[3] = to_ms_since_boot(get_absolute_time());
}

#define WIZ_TR_SOCK(sock, low) (((uint32_t)(sock) << 24) | ((uint32_t)(low) & 0xFFFFFFu))

#endif /* WIZ_TRACE_H */
