# SPDX-License-Identifier: MPL-2.0
#
# This file carries text from open62541 (https://github.com/open62541/open62541,
# MPL-2.0) and the WIZnet modification of it. Both are covered by MPL-2.0.
#
# Patch: refuse new monitored items while free heap is below a reserve.
#
# Applied after retransmit_byte_cap.cmake, the same way (string REPLACE on the
# generated amalgamation, FATAL_ERROR on a missing anchor). Written against
# open62541 10022ffbc (v1.5.4-224).
#
# Why (measured 2026-11-03, opcua_node, W6300 with 270 KB of heap held back):
#   The count caps (maxMonitoredItems) do not know how big an item is. 70
#   Floats100 items ran the heap dry; the next Publish failed to allocate and
#   open62541 closed every SecureChannel with BadOutOfMemory, live clients
#   included. New channels were refused the same way until the abandoned
#   subscriptions expired about 120 s later.
#
# Policy (OPCUA_MI_HEAP_RESERVE, bytes, 0 = off):
#   CreateMonitoredItems answers BadOutOfMemory for an item requested while
#   free heap (heap span minus malloc's in-use bytes, the Diag.FreeHeap value)
#   is below the reserve. Items already running are not touched, so the
#   clients that hold them keep their data. The reserve is what is left for
#   the transient peaks a steady load does not show: handshakes after a link
#   outage took 37-48 KB below the steady FreeHeap with four RSA clients
#   (2026-11-03).
#   Items the server creates for itself (localMon) are not limited.

set(_wiz_mr_old [=[
static void
Operation_CreateMonitoredItem(UA_Server *server, UA_Session *session,
                              struct createMonContext *cmc,
                              const UA_MonitoredItemCreateRequest *request,
                              UA_MonitoredItemCreateResult *result) {
    UA_LOCK_ASSERT(&server->serviceMutex);

    /* Check available capacity */
    if(!cmc->localMon &&
       (((server->config.maxMonitoredItems != 0) &&
         (server->monitoredItemsSize >= server->config.maxMonitoredItems)) ||
        ((server->config.maxMonitoredItemsPerSubscription != 0) &&
         (cmc->sub->monitoredItemsSize >= server->config.maxMonitoredItemsPerSubscription)))) {
        result->statusCode = UA_STATUSCODE_BADTOOMANYMONITOREDITEMS;
        return;
    }
]=])
set(_wiz_mr_new [=[
/* WIZnet patch (port/open62541/patches/mi_heap_reserve.cmake) */
static size_t wizMiHeapRefused = 0;

size_t UA_Server_wizMiHeapRefused(void) {
    return wizMiHeapRefused;
}

static size_t
wizFreeHeapNow(void) {
    extern char __StackLimit, __bss_end__;
    struct mallinfo m = mallinfo();
    return (size_t)(&__StackLimit - &__bss_end__) - (size_t)m.uordblks;
}

static void
Operation_CreateMonitoredItem(UA_Server *server, UA_Session *session,
                              struct createMonContext *cmc,
                              const UA_MonitoredItemCreateRequest *request,
                              UA_MonitoredItemCreateResult *result) {
    UA_LOCK_ASSERT(&server->serviceMutex);

    /* Check available capacity */
    if(!cmc->localMon &&
       (((server->config.maxMonitoredItems != 0) &&
         (server->monitoredItemsSize >= server->config.maxMonitoredItems)) ||
        ((server->config.maxMonitoredItemsPerSubscription != 0) &&
         (cmc->sub->monitoredItemsSize >= server->config.maxMonitoredItemsPerSubscription)))) {
        result->statusCode = UA_STATUSCODE_BADTOOMANYMONITOREDITEMS;
        return;
    }

    /* WIZnet patch (mi_heap_reserve.cmake): keep the reserve for the clients
     * already running. */
    if(!cmc->localMon && OPCUA_MI_HEAP_RESERVE > 0 &&
       wizFreeHeapNow() < (size_t)OPCUA_MI_HEAP_RESERVE) {
        wizMiHeapRefused++;
        result->statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
]=])

string(FIND "${OPEN62541_WIZNET_PATCHED_SOURCE}" "${_wiz_mr_old}" _wiz_at)
if(_wiz_at EQUAL -1)
    message(FATAL_ERROR
        "mi_heap_reserve: anchor not found. "
        "open62541 changed; redo the patch against the new source.")
endif()
string(REPLACE "${_wiz_mr_old}" "${_wiz_mr_new}"
       OPEN62541_WIZNET_PATCHED_SOURCE "${OPEN62541_WIZNET_PATCHED_SOURCE}")
