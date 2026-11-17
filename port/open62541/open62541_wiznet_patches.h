/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/* Functions added to the open62541 amalgamation by the patches under
 * port/open62541/patches/. See subscription_time_limits.cmake. */
#ifndef OPEN62541_WIZNET_PATCHES_H
#define OPEN62541_WIZNET_PATCHES_H

#include <stddef.h>
#include "open62541.h"

/* Subscriptions on the server, including ones detached from a session that
 * timed out and not yet expired. */
size_t UA_Server_wizSubscriptionCount(UA_Server *server);

/* Monitored items over all those subscriptions. */
size_t UA_Server_wizMonitoredItemCount(UA_Server *server);

/* Retransmission queues (messages kept for Republish). bytes is the encoded
 * size the byte cap counts; heap is the estimated heap the heap cap counts.
 * freedEstimate / freedMeasured compare that estimate with the real drop of
 * malloc's in-use bytes when entries are freed. See retransmit_byte_cap.cmake.
 * Walks the queues; call at a diagnostic rate. */
#ifndef UA_WIZ_RETRANSMIT_USAGE_DEFINED   /* same definition as the patch */
#define UA_WIZ_RETRANSMIT_USAGE_DEFINED
typedef struct {
    size_t entries, bytes, heap;              /* all subscriptions            */
    size_t sessionMaxBytes, sessionMaxHeap;   /* the fullest single session   */
    size_t sessionMaxEntries;
    size_t evictions, notKept;                /* since boot, byte/heap caps   */
    size_t freedCount, freedEstimate, freedMeasured; /* accounting check      */
} UA_WizRetransmitUsage;
#endif
void UA_Server_wizRetransmitUsage(UA_Server *server, UA_WizRetransmitUsage *u);

/* Monitored items refused since boot because free heap was below
 * OPCUA_MI_HEAP_RESERVE. See mi_heap_reserve.cmake. */
size_t UA_Server_wizMiHeapRefused(void);

#endif /* OPEN62541_WIZNET_PATCHES_H */
