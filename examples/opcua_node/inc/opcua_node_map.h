/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OPCUA_NODE_MAP_H
#define OPCUA_NODE_MAP_H

/*
 * Numeric NodeId allocation for the dynamically generated address space.
 *
 * Layout (namespace = urn:wiznet:opcua-node):
 *   Objects
 *     <DeviceName>                        ns=1;i=OPCUA_ID_ROOT
 *       Device                            ns=1;i=OPCUA_ID_DEVICE
 *         DeviceName/IPAddress/FwVersion/Uptime_s/FreeHeap/
 *         RebootCount/RebootReason/LinkStatus/RawFrame/
 *         Usb / Uart / Modbus diagnostics  ns=1;s="Diag.*"
 *       Channels                          ns=1;i=OPCUA_ID_CHANNELS
 *         <channel name>                  ns=1;s="Channels/<name>"  (DataSource)
 *           EngineeringUnits              ns=1;i=OPCUA_ID_PROP_BASE + idx*4 + 0
 *           EURange                       ns=1;i=OPCUA_ID_PROP_BASE + idx*4 + 1
 *
 * Channel read-callback node_context = channel index (0..count-1).
 * Diagnostic read-callback node_context = OPCUA_DIAG_CTX_BASE + kind.
 */

#define OPCUA_NODE_NS_URI       "urn:wiznet:opcua-node"

#define OPCUA_ID_ROOT           1000u
#define OPCUA_ID_DEVICE         1001u
#define OPCUA_ID_CHANNELS       1002u

#define OPCUA_ID_DIAG_BASE      3000u   /* diagnostic variable numeric ids     */
#define OPCUA_ID_PROP_BASE      5000u   /* per-channel property numeric ids     */

/* node_context tag separating diagnostics from channel indices. Channels are
 * always < OPCUA_MAX_CHANNELS (32), so 0x1000 is a safe, non-overlapping base. */
#define OPCUA_DIAG_CTX_BASE     0x1000u

/* Diagnostic kinds (offset from OPCUA_DIAG_CTX_BASE in node_context). */
typedef enum {
    OPCUA_DIAG_DEVICE_NAME = 0,
    OPCUA_DIAG_IP,
    OPCUA_DIAG_FW_VERSION,
    OPCUA_DIAG_UPTIME_S,
    OPCUA_DIAG_FREE_HEAP,
    OPCUA_DIAG_REBOOT_COUNT,
    OPCUA_DIAG_REBOOT_REASON,
    OPCUA_DIAG_LINK_STATUS,
    OPCUA_DIAG_RAW_FRAME,
    OPCUA_DIAG_USB_FRAMES,
    OPCUA_DIAG_USB_ERRORS,
    OPCUA_DIAG_UART_FRAMES,
    OPCUA_DIAG_UART_ERRORS,
    OPCUA_DIAG_MODBUS_POLLS,
    OPCUA_DIAG_MODBUS_TIMEOUTS,
    /* 시각원(SNTP/RTC) 동기화 상태. 이 값이 Unsynchronised 인 동안에는
     * 채널 값에 sourceTimestamp 를 붙이지 않는다 — 붙이면 1970년이 나간다. */
    OPCUA_DIAG_TIME_SYNC,
    /* Soak-test diagnostics: heap low-water mark, which image is running,
     * and the session/channel/socket state a leak would show up in. */
    OPCUA_DIAG_MIN_FREE_HEAP,
    OPCUA_DIAG_BUILD_ID,
    OPCUA_DIAG_SESSIONS,
    OPCUA_DIAG_CHANNELS,
    OPCUA_DIAG_SOCKETS_BUSY,
    OPCUA_DIAG_HANDSHAKE_REFUSED,
    OPCUA_DIAG_CLOSE_CUT,
    OPCUA_DIAG_CHIP_DROPPED,
    /* Subscription resources: the event loop's fixed timer table and what
     * holds it. Detached subscriptions of vanished clients count too. */
    OPCUA_DIAG_TIMER_SLOTS_USED,
    OPCUA_DIAG_TIMER_SLOTS_PEAK,
    OPCUA_DIAG_SUBSCRIPTIONS,
    OPCUA_DIAG_MONITORED_ITEMS,
    /* Retransmission queues (now and highest, sampled every second) and the
     * socket commands the chip did not finish ("not done after" lines). */
    OPCUA_DIAG_RETRANSMIT_ENTRIES,
    OPCUA_DIAG_RETRANSMIT_ENTRIES_PEAK,
    OPCUA_DIAG_RETRANSMIT_BYTES,
    OPCUA_DIAG_RETRANSMIT_BYTES_PEAK,
    OPCUA_DIAG_RETRANSMIT_HEAP,          /* heap the kept messages took      */
    OPCUA_DIAG_RETRANSMIT_HEAP_PEAK,
    OPCUA_DIAG_RETRANSMIT_SESSION_BYTES, /* fullest session, encoded bytes   */
    OPCUA_DIAG_RETRANSMIT_SESSION_ENTRIES,
    OPCUA_DIAG_RETRANSMIT_EVICTIONS,     /* caps: evicted + not kept         */
    OPCUA_DIAG_RETRANSMIT_SESSION_HEAP,  /* fullest session, estimated heap  */
    OPCUA_DIAG_RETRANSMIT_FREED_EST,     /* freed entries: estimated heap    */
    OPCUA_DIAG_RETRANSMIT_FREED_MEAS,    /* freed entries: measured heap     */
    OPCUA_DIAG_CMD_TIMEOUTS,
    OPCUA_DIAG__COUNT
} OpcUaDiagKind;

#endif /* OPCUA_NODE_MAP_H */
