# SPDX-License-Identifier: MPL-2.0
#
# This file carries text from open62541 (https://github.com/open62541/open62541,
# MPL-2.0) and the WIZnet modification of it. Both are covered by MPL-2.0.
#
# Patch: build open62541's built-in minimal namespace 0.
#
# Applied by port/open62541/CMakeLists.txt to the generated amalgamation, the
# same way as the other patches here: string(REPLACE) on the source text,
# FATAL_ERROR if the text it expects is not found. Written against open62541
# 10022ffbc (v1.5.4-224).
#
# Why: with UA_NAMESPACE_ZERO=MINIMAL the generator emits no NS0 nodes and the
# stock code expects them from an external nodestore. initNS0() creates the
# built-in minimal set (Root, Objects, Server) instead.

string(REPLACE
[=[#ifdef UA_GENERATED_NAMESPACE_ZERO
    /* Standard configuration: generate NS0 nodes at runtime */
    res = initNS0(server);
#else
    /* NONE configuration: NS0 pre-loaded by external nodestore (e.g., ROM).
     * Only connect data sources for dynamic values like ServerTime, ServerStatus, etc. */
    res = initNS0_dataSources(server);
#endif]=]
[=[#ifdef UA_GENERATED_NAMESPACE_ZERO
    /* Standard configuration: generate NS0 nodes at runtime */
    res = initNS0(server);
#else
    /* WIZnet MINIMAL build: create open62541's built-in minimal NS0.
     * This keeps Root/Objects/Server available without REDUCED generated nodes. */
    res = initNS0(server);
#endif]=]
    OPEN62541_WIZNET_PATCHED_SOURCE
    "${OPEN62541_WIZNET_PATCHED_SOURCE}")

if(OPEN62541_WIZNET_PATCHED_SOURCE STREQUAL OPEN62541_WIZNET_SOURCE)
    message(FATAL_ERROR
        "Failed to patch open62541 minimal NS0 startup path. "
        "Check generated open62541.c layout.")
endif()
