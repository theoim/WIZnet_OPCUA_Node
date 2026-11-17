# SPDX-License-Identifier: MPL-2.0
#
# This file carries text from open62541 (https://github.com/open62541/open62541,
# MPL-2.0) and the WIZnet modification of it. Both are covered by MPL-2.0.
#
# Patch: subscription KeepAlive and Lifetime capped by time, and two counters.
#
# Applied to the generated amalgamation by port/open62541/CMakeLists.txt, the
# same way as the minimal-NS0 patch there: string(REPLACE) on the source text,
# FATAL_ERROR if the text it expects is not found. The open62541 tree under
# libraries/ is never modified. Written against open62541 10022ffbc
# (v1.5.4-224); check the anchor text again when the submodule moves.
#
# Why (measured 2026-10-29, opcua_node, RP2350):
#   open62541 limits a subscription only by counts - lifeTimeCountLimits and
#   keepAliveCountLimits - which mean different times at different publishing
#   intervals. A client that vanished kept its subscription, and every timer
#   its monitored items held, for LifetimeCount x PublishingInterval:
#   10000 x 100 ms = 1000 s with asyncua's defaults. Meanwhile the socket came
#   back in about 30 s, so new clients could connect but CreateSubscription
#   failed with BadOutOfMemory until the old subscription expired at ~17 min.
#
# Policy (OPCUA_SUB_MAX_KEEPALIVE_MS, OPCUA_SUB_MAX_LIFETIME_MS):
#   KeepAlive   <= max keepalive time / publishing interval
#   Lifetime    <= max lifetime time  / publishing interval
#   Lifetime    >= 3 x KeepAlive  (Part 4 rule, kept; applied last by the
#                                  existing open62541 line below the patch)
#   The publishing interval itself is capped at the max keepalive time in
#   opcua_server.c, so KeepAlive >= 1 and 3 x KeepAlive stays within the
#   lifetime cap (40 s x 3 = 120 s). Counts round down, so the time is never
#   exceeded. The revised values reach the client in the Create/Modify
#   response as usual - both services go through setSubscriptionSettings().
#
# This bounds how long a subscription outlives the client that stopped
# sending Publish requests. It does not disconnect live clients.

set(_wiz_sub_old [=[
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.lifeTimeCountLimits,
                               requestedLifetimeCount, subscription->lifeTimeCount);
    if(subscription->lifeTimeCount < 3 * subscription->maxKeepAliveCount)
]=])

set(_wiz_sub_new [=[
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.lifeTimeCountLimits,
                               requestedLifetimeCount, subscription->lifeTimeCount);
    /* WIZnet patch (port/open62541/patches/subscription_time_limits.cmake):
     * KeepAlive and Lifetime capped by time, not only by count. */
    if(subscription->publishingInterval > 0.0) {
        UA_UInt32 kaMax = (UA_UInt32)
            ((UA_Double)OPCUA_SUB_MAX_KEEPALIVE_MS / subscription->publishingInterval);
        if(kaMax < 1)
            kaMax = 1;
        if(subscription->maxKeepAliveCount > kaMax)
            subscription->maxKeepAliveCount = kaMax;
        UA_UInt32 ltMax = (UA_UInt32)
            ((UA_Double)OPCUA_SUB_MAX_LIFETIME_MS / subscription->publishingInterval);
        if(subscription->lifeTimeCount > ltMax)
            subscription->lifeTimeCount = ltMax;
    }
    if(subscription->lifeTimeCount < 3 * subscription->maxKeepAliveCount)
]=])

string(FIND "${OPEN62541_WIZNET_PATCHED_SOURCE}" "${_wiz_sub_old}" _wiz_at)
if(_wiz_at EQUAL -1)
    message(FATAL_ERROR
        "subscription_time_limits: anchor not found in setSubscriptionSettings(). "
        "open62541 changed; redo the patch against the new source.")
endif()
string(REPLACE "${_wiz_sub_old}" "${_wiz_sub_new}"
       OPEN62541_WIZNET_PATCHED_SOURCE "${OPEN62541_WIZNET_PATCHED_SOURCE}")

# Counters for Diag.SubscriptionCount / Diag.MonitoredItemCount. UA_Server is
# opaque outside the amalgamation, so the accessors live inside it. Detached
# subscriptions (session gone, lifetime not yet run out) are still counted:
# that is what the counter is for. Declared in open62541_wiznet_patches.h.
string(APPEND OPEN62541_WIZNET_PATCHED_SOURCE [=[

/* WIZnet patch (port/open62541/patches/subscription_time_limits.cmake) */
#ifdef UA_ENABLE_SUBSCRIPTIONS
size_t UA_Server_wizSubscriptionCount(UA_Server *server) {
    return server->subscriptionsSize;
}
size_t UA_Server_wizMonitoredItemCount(UA_Server *server) {
    return server->monitoredItemsSize;
}
#endif
]=])
