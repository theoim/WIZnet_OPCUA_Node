/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "opcua_server.h"
#include "opcua_security.h"

#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "data_table.h"
#include "opcua_node_map.h"
#include "opcua_tcp_probe.h"
#include "reset_cause.h"
#include "pico/stdlib.h"
#include "wiznet_network.h"
#include "wiznet_eventloop_config.h"

#ifndef OPCUA_ENABLE
#define OPCUA_ENABLE 0
#endif

#ifndef OPCUA_TCP_PROBE_ENABLE
#define OPCUA_TCP_PROBE_ENABLE 0
#endif

static OpcUaServerStatus s_status;
static const opcua_settings_t *s_cfg;
static char s_endpoint[48];

static const char *state_name(OpcUaServerState state) {
    switch(state) {
    case OPCUA_SERVER_STATE_DISABLED: return "DISABLED";
    case OPCUA_SERVER_STATE_READY:    return "READY";
    case OPCUA_SERVER_STATE_RUNNING:  return "RUNNING";
    case OPCUA_SERVER_STATE_ERROR:    return "ERROR";
    default:                          return "UNKNOWN";
    }
}

static void build_endpoint_url(const opcua_settings_t *cfg) {
    snprintf(s_endpoint, sizeof(s_endpoint), "opc.tcp://%u.%u.%u.%u:%u",
             cfg->ip[0], cfg->ip[1], cfg->ip[2], cfg->ip[3],
             (unsigned)WIZNET_OPCUA_TCP_PORT);
}

#if OPCUA_ENABLE
#include "open62541.h"
#include "open62541_wiznet_patches.h"

static UA_Server *s_server;

/* Retransmission queue usage, refreshed once a second by opcua_server_poll()
 * and on every read, with the highest values seen. */
static UA_WizRetransmitUsage s_rt;
static size_t s_rt_entries_peak, s_rt_bytes_peak, s_rt_heap_peak;
static uint32_t s_rt_sampled_ms;

static void sample_retransmit(void) {
    UA_Server_wizRetransmitUsage(s_server, &s_rt);
    if(s_rt.entries > s_rt_entries_peak)
        s_rt_entries_peak = s_rt.entries;
    if(s_rt.bytes > s_rt_bytes_peak)
        s_rt_bytes_peak = s_rt.bytes;
    if(s_rt.heap > s_rt_heap_peak)
        s_rt_heap_peak = s_rt.heap;
}
static bool s_server_started;
static bool s_address_space_ready;
static UA_StatusCode s_address_space_status;
static UA_UInt16 s_ns;

/* ── free-heap estimate (leak-trend for A-5 soak) ─────────────────────
 * heap_total = linker heap span; free = total - allocated (newlib mallinfo). */
extern char __StackLimit;
extern char __bss_end__;
static UA_UInt32 free_heap(void) {
    struct mallinfo m = mallinfo();
    uint32_t total = (uint32_t)(&__StackLimit - &__bss_end__);
    return (UA_UInt32)(total - (uint32_t)m.uordblks);
}

/* Heap that malloc has never claimed: total minus the peak arena. The heap
 * only grows through sbrk, so this catches a transient peak inside one
 * handshake that a sampled FreeHeap misses. It is a lower bound of the worst
 * FreeHeap: free chunks inside the arena count as used here. Running out of
 * it is what makes malloc fail.
 *
 * newlib's own high-water mark, read directly: mallinfo().usmblks is not it
 * in this build (it gave a "minimum" above the current FreeHeap). */
extern unsigned long __malloc_max_sbrked_mem;
static UA_UInt32 min_free_heap(void) {
    uint32_t total = (uint32_t)(&__StackLimit - &__bss_end__);
    return (UA_UInt32)(total - (uint32_t)__malloc_max_sbrked_mem);
}

static UA_StatusCode copy_string_to_config(UA_String *dst, const char *text) {
    UA_String src = UA_STRING((char *)text);
    UA_String_clear(dst);
    return UA_String_copy(&src, dst);
}

static UA_StatusCode set_server_url(UA_ServerConfig *config, const char *url) {
    UA_Array_delete(config->serverUrls, config->serverUrlsSize,
                    &UA_TYPES[UA_TYPES_STRING]);
    config->serverUrls = NULL;
    config->serverUrlsSize = 0u;

    config->serverUrls =
        (UA_String *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_STRING]);
    if(!config->serverUrls)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    UA_String ep = UA_STRING((char *)url);
    UA_StatusCode res = UA_String_copy(&ep, &config->serverUrls[0]);
    if(res != UA_STATUSCODE_GOOD) {
        UA_Array_delete(config->serverUrls, 1u, &UA_TYPES[UA_TYPES_STRING]);
        config->serverUrls = NULL;
        return res;
    }
    config->serverUrlsSize = 1u;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode configure_server_identity(UA_ServerConfig *config) {
    UA_StatusCode res = copy_string_to_config(
        &config->applicationDescription.applicationUri, OPCUA_APPLICATION_URI);
    if(res != UA_STATUSCODE_GOOD) return res;
    res = copy_string_to_config(&config->applicationDescription.productUri,
                                OPCUA_PRODUCT_URI);
    if(res != UA_STATUSCODE_GOOD) return res;

    UA_LocalizedText_clear(&config->applicationDescription.applicationName);
    config->applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en", "WIZnet OPC UA Node");
    config->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;

    res = copy_string_to_config(&config->buildInfo.manufacturerName, "WIZnet");
    if(res != UA_STATUSCODE_GOOD) return res;
    res = copy_string_to_config(&config->buildInfo.productName,
                                "WIZnet OPC UA Node (RP2350/W6300)");
    if(res != UA_STATUSCODE_GOOD) return res;
    res = copy_string_to_config(&config->buildInfo.softwareVersion, OPCUA_FW_VERSION);
    if(res != UA_STATUSCODE_GOOD) return res;
    config->buildInfo.buildDate = UA_DateTime_now();

    return set_server_url(config, s_endpoint);
}

/* ── value helpers ────────────────────────────────────────────────── */

/* 시각원이 붙기 전에는 UA_DateTime_now() 가 Unix epoch + 부팅 경과시간,
 * 즉 1970년이다. 그 값을 sourceTimestamp 로 내보내면 클라이언트는 그것을
 * 정상 시각으로 받아 히스토리안·알람 상관분석에 쓴다. 그래서 동기화 전에는
 * 타임스탬프를 아예 붙이지 않는다 — 없는 것이 틀린 것보다 낫다.
 *
 * 동기화 여부는 Device/TimeSync 진단 노드로 노출한다. */
static UA_Boolean timestamp_allowed(void) {
    return wiz_clock_is_synced() ? true : false;
}

static UA_StatusCode emit_scalar(UA_DataValue *value, const void *data,
                                 const UA_DataType *type, UA_Boolean ts) {
    UA_StatusCode res = UA_Variant_setScalarCopy(&value->value, data, type);
    if(res != UA_STATUSCODE_GOOD) {
        value->hasStatus = true;
        value->status = res;
        return UA_STATUSCODE_GOOD;
    }
    value->hasValue = true;
    if(ts && timestamp_allowed()) {
        value->hasSourceTimestamp = true;
        value->sourceTimestamp = UA_DateTime_now();
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode emit_string(UA_DataValue *value, const char *s,
                                 UA_Boolean ts) {
    UA_String str = UA_STRING((char *)s);
    return emit_scalar(value, &str, &UA_TYPES[UA_TYPES_STRING], ts);
}

static UA_StatusCode read_channel(uint16_t idx, UA_DataValue *value,
                                  UA_Boolean ts) {
    channel_value_t v;
    if(!data_table_get(idx, &v)) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
        return UA_STATUSCODE_GOOD;
    }
    /* 아직 한 번도 기록되지 않은 채널. data_table 은 open62541 헤더에
     * 의존하지 않으므로 UA status code 는 여기서 붙인다 — 그쪽에 심으면
     * 드라이버 계층이 OPC UA 를 알게 된다.
     *
     * 판정은 update_ms == 0 이다. data_table_clear() 가 0 으로 밀고,
     * 어떤 쓰기 경로든 now_ms() 로 갱신한다. 부팅 첫 1 ms 안에 값이
     * 들어오면 한 번 놓칠 수 있으나, 그 경우 다음 갱신에서 정상화된다. */
    if(v.update_ms == 0u) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADWAITINGFORINITIALDATA;
        return UA_STATUSCODE_GOOD;
    }
    if(v.status != UA_STATUSCODE_GOOD) {
        value->hasStatus = true;
        value->status = v.status;   /* e.g. Bad_CommunicationError */
        return UA_STATUSCODE_GOOD;
    }

    switch(s_cfg->channels[idx].datatype) {
    case OPCUA_DT_FLOAT: {
        UA_Float f = (UA_Float)v.value;
        return emit_scalar(value, &f, &UA_TYPES[UA_TYPES_FLOAT], ts);
    }
    case OPCUA_DT_INT32: {
        UA_Int32 i = (UA_Int32)(v.value < 0.0 ? v.value - 0.5 : v.value + 0.5);
        return emit_scalar(value, &i, &UA_TYPES[UA_TYPES_INT32], ts);
    }
    case OPCUA_DT_BOOL: {
        UA_Boolean b = v.bool_value;
        return emit_scalar(value, &b, &UA_TYPES[UA_TYPES_BOOLEAN], ts);
    }
    case OPCUA_DT_STRING:
    default:
        return emit_string(value, v.str, ts);
    }
}

static UA_StatusCode read_diag(uint16_t kind, UA_DataValue *value,
                               UA_Boolean ts) {
    diag_counters_t d;
    data_table_diag_snapshot(&d);

    switch(kind) {
    case OPCUA_DIAG_DEVICE_NAME:
        return emit_string(value, s_cfg->device_name, ts);
    case OPCUA_DIAG_IP: {
        WiznetNetworkStatus n;
        wiznet_network_get_status(&n);
        char ip[16];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", n.ip[0], n.ip[1], n.ip[2],
                 n.ip[3]);
        return emit_string(value, ip, ts);
    }
    case OPCUA_DIAG_FW_VERSION:
        return emit_string(value, OPCUA_FW_VERSION, ts);
    case OPCUA_DIAG_UPTIME_S: {
        UA_UInt32 up = (UA_UInt32)(to_ms_since_boot(get_absolute_time()) / 1000u);
        return emit_scalar(value, &up, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_FREE_HEAP: {
        UA_UInt32 fh = free_heap();
        return emit_scalar(value, &fh, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_MIN_FREE_HEAP: {
        UA_UInt32 mf = min_free_heap();
        return emit_scalar(value, &mf, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_BUILD_ID:
        /* The same stamp as the boot banner, so a test can refuse to judge a
         * board that is not running the image it was written against. */
        return emit_string(value, OPCUA_BUILD_GIT " " OPCUA_BUILD_UTC, ts);
    case OPCUA_DIAG_SESSIONS:
    case OPCUA_DIAG_CHANNELS: {
        UA_ServerStatistics st = UA_Server_getStatistics(s_server);
        UA_UInt32 n = (UA_UInt32)(kind == OPCUA_DIAG_SESSIONS
                                      ? st.ss.currentSessionCount
                                      : st.scs.currentChannelCount);
        return emit_scalar(value, &n, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_SOCKETS_BUSY:
    case OPCUA_DIAG_HANDSHAKE_REFUSED:
    case OPCUA_DIAG_CLOSE_CUT:
    case OPCUA_DIAG_CHIP_DROPPED: {
        UA_EventLoop_LWIP_Stats es;
        UA_EventLoop_LWIP_getStats(&es);
        UA_UInt32 n = kind == OPCUA_DIAG_SOCKETS_BUSY      ? es.socketsBusy
                    : kind == OPCUA_DIAG_HANDSHAKE_REFUSED ? es.handshakeRefused
                    : kind == OPCUA_DIAG_CLOSE_CUT         ? es.closeCut
                                                           : es.chipDropped;
        return emit_scalar(value, &n, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_TIMER_SLOTS_USED:
    case OPCUA_DIAG_TIMER_SLOTS_PEAK: {
        UA_EventLoop_LWIP_Stats es;
        UA_EventLoop_LWIP_getStats(&es);
        UA_UInt32 n = kind == OPCUA_DIAG_TIMER_SLOTS_USED ? es.timerSlotsUsed
                                                          : es.timerSlotsPeak;
        return emit_scalar(value, &n, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_SUBSCRIPTIONS:
    case OPCUA_DIAG_MONITORED_ITEMS: {
        UA_UInt32 n = (UA_UInt32)(kind == OPCUA_DIAG_SUBSCRIPTIONS
                                      ? UA_Server_wizSubscriptionCount(s_server)
                                      : UA_Server_wizMonitoredItemCount(s_server));
        return emit_scalar(value, &n, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_RETRANSMIT_ENTRIES:
    case OPCUA_DIAG_RETRANSMIT_ENTRIES_PEAK:
    case OPCUA_DIAG_RETRANSMIT_BYTES:
    case OPCUA_DIAG_RETRANSMIT_BYTES_PEAK:
    case OPCUA_DIAG_RETRANSMIT_HEAP:
    case OPCUA_DIAG_RETRANSMIT_HEAP_PEAK:
    case OPCUA_DIAG_RETRANSMIT_SESSION_BYTES:
    case OPCUA_DIAG_RETRANSMIT_SESSION_ENTRIES:
    case OPCUA_DIAG_RETRANSMIT_EVICTIONS:
    case OPCUA_DIAG_RETRANSMIT_SESSION_HEAP:
    case OPCUA_DIAG_RETRANSMIT_FREED_EST:
    case OPCUA_DIAG_RETRANSMIT_FREED_MEAS: {
        sample_retransmit();
        size_t v = 0;
        switch(kind) {
        case OPCUA_DIAG_RETRANSMIT_ENTRIES:         v = s_rt.entries; break;
        case OPCUA_DIAG_RETRANSMIT_ENTRIES_PEAK:    v = s_rt_entries_peak; break;
        case OPCUA_DIAG_RETRANSMIT_BYTES:           v = s_rt.bytes; break;
        case OPCUA_DIAG_RETRANSMIT_BYTES_PEAK:      v = s_rt_bytes_peak; break;
        case OPCUA_DIAG_RETRANSMIT_HEAP:            v = s_rt.heap; break;
        case OPCUA_DIAG_RETRANSMIT_HEAP_PEAK:       v = s_rt_heap_peak; break;
        case OPCUA_DIAG_RETRANSMIT_SESSION_BYTES:   v = s_rt.sessionMaxBytes; break;
        case OPCUA_DIAG_RETRANSMIT_SESSION_ENTRIES: v = s_rt.sessionMaxEntries; break;
        case OPCUA_DIAG_RETRANSMIT_SESSION_HEAP:    v = s_rt.sessionMaxHeap; break;
        case OPCUA_DIAG_RETRANSMIT_FREED_EST:       v = s_rt.freedEstimate; break;
        case OPCUA_DIAG_RETRANSMIT_FREED_MEAS:      v = s_rt.freedMeasured; break;
        default:                                    v = s_rt.evictions + s_rt.notKept;
        }
        UA_UInt32 n = (UA_UInt32)v;
        return emit_scalar(value, &n, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_CMD_TIMEOUTS: {
        UA_EventLoop_LWIP_Stats es;
        UA_EventLoop_LWIP_getStats(&es);
        UA_UInt32 n = es.cmdTimeouts;
        return emit_scalar(value, &n, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_REBOOT_COUNT: {
        UA_UInt32 rc = s_cfg->reboot_count;
        return emit_scalar(value, &rc, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_REBOOT_REASON:
        /* This boot's cause. RebootCount is the persisted number of unplanned
         * resets (watchdog, brown-out, glitch); see reset_cause.h. */
        return emit_string(value, reset_cause_name(reset_cause_this_boot()), ts);
    case OPCUA_DIAG_LINK_STATUS:
        return emit_string(value,
                           wiznet_network_is_ready() ? "up" : "down", ts);
    case OPCUA_DIAG_RAW_FRAME:
        return emit_string(value, d.last_raw, ts);
    case OPCUA_DIAG_USB_FRAMES:
        return emit_scalar(value, &d.usb_frame_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_USB_ERRORS:
        return emit_scalar(value, &d.usb_error_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_UART_FRAMES:
        return emit_scalar(value, &d.uart_frame_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_UART_ERRORS:
        return emit_scalar(value, &d.uart_error_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_MODBUS_POLLS:
        return emit_scalar(value, &d.modbus_poll_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_MODBUS_TIMEOUTS:
        return emit_scalar(value, &d.modbus_timeout_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_TIME_SYNC:
        /* 시각원이 없으면 채널 값에 sourceTimestamp 가 붙지 않는다.
         * 클라이언트가 그 이유를 알 수 있도록 상태를 노출한다.
         * SNTP 를 붙이면 "Synchronised" 로 바뀌고, 동기화를 잃은 상태를
         * 구분하게 되면 "Lost" 를 추가한다. */
        return emit_string(value,
                           wiz_clock_is_synced() ? "Synchronised"
                                                 : "Unsynchronised", ts);
    default:
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
        return UA_STATUSCODE_GOOD;
    }
}

#ifdef OPCUA_DT_TEST_NODES
/* Test image only: values of every shape the retransmission heap estimate has
 * to walk, changing on each read so that each sample makes a notification.
 * tools/opcua_heap_datatypes.py compares estimate and measurement per type. */
#define OPCUA_DT_CTX_BASE 0x2000u
#define OPCUA_DT_MAX_LEN  200u

typedef enum {
    DT_FLOAT, DT_DOUBLE, DT_STRING_8, DT_STRING_16, DT_STRING_200,
    DT_BYTES_64, DT_FLOATS_10, DT_FLOATS_100, DT_STRINGS_10, DT_COUNT
} OpcUaDtKind;

static const char *const s_dt_names[DT_COUNT] = {
    "Float", "Double", "String8", "String16", "String200",
    "Bytes64", "Floats10", "Floats100", "Strings10"
};

static UA_StatusCode read_dt(uint16_t kind, UA_DataValue *value) {
    static uint32_t counter;
    static char text[OPCUA_DT_MAX_LEN];
    uint32_t n = ++counter;
    UA_Float floats[100];
    UA_String strings[10];
    UA_String str;
    UA_StatusCode res;

    memset(text, 'a' + (int)(n % 26u), sizeof(text));
    str.data = (UA_Byte *)text;
    for(size_t i = 0; i < 100u; i++)
        floats[i] = (UA_Float)(n + i);
    for(size_t i = 0; i < 10u; i++) {
        strings[i].data = (UA_Byte *)text;
        strings[i].length = 8u + i;
    }

    switch(kind) {
    case DT_FLOAT: {
        UA_Float f = (UA_Float)n;
        res = UA_Variant_setScalarCopy(&value->value, &f, &UA_TYPES[UA_TYPES_FLOAT]);
        break;
    }
    case DT_DOUBLE: {
        UA_Double d = (UA_Double)n;
        res = UA_Variant_setScalarCopy(&value->value, &d, &UA_TYPES[UA_TYPES_DOUBLE]);
        break;
    }
    case DT_STRING_8:
    case DT_STRING_16:
    case DT_STRING_200:
        str.length = kind == DT_STRING_8 ? 8u : kind == DT_STRING_16 ? 16u : 200u;
        res = UA_Variant_setScalarCopy(&value->value, &str, &UA_TYPES[UA_TYPES_STRING]);
        break;
    case DT_BYTES_64:
        str.length = 64u;
        res = UA_Variant_setScalarCopy(&value->value, &str, &UA_TYPES[UA_TYPES_BYTESTRING]);
        break;
    case DT_FLOATS_10:
        res = UA_Variant_setArrayCopy(&value->value, floats, 10u, &UA_TYPES[UA_TYPES_FLOAT]);
        break;
    case DT_FLOATS_100:
        res = UA_Variant_setArrayCopy(&value->value, floats, 100u, &UA_TYPES[UA_TYPES_FLOAT]);
        break;
    default:
        res = UA_Variant_setArrayCopy(&value->value, strings, 10u, &UA_TYPES[UA_TYPES_STRING]);
        break;
    }
    if(res != UA_STATUSCODE_GOOD) {
        value->hasStatus = true;
        value->status = res;
    } else {
        value->hasValue = true;
    }
    return UA_STATUSCODE_GOOD;
}
#endif /* OPCUA_DT_TEST_NODES */

static UA_StatusCode opcua_read_value(UA_Server *server,
                                      const UA_NodeId *session_id,
                                      void *session_context,
                                      const UA_NodeId *node_id,
                                      void *node_context,
                                      UA_Boolean include_source_timestamp,
                                      const UA_NumericRange *range,
                                      UA_DataValue *value) {
    (void)server; (void)session_id; (void)session_context; (void)node_id;

    if(range) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

    uintptr_t ctx = (uintptr_t)node_context;
#ifdef OPCUA_DT_TEST_NODES
    if(ctx >= OPCUA_DT_CTX_BASE)
        return read_dt((uint16_t)(ctx - OPCUA_DT_CTX_BASE), value);
#endif
    if(ctx >= OPCUA_DIAG_CTX_BASE)
        return read_diag((uint16_t)(ctx - OPCUA_DIAG_CTX_BASE), value,
                         include_source_timestamp);
    return read_channel((uint16_t)ctx, value, include_source_timestamp);
}

/* ── node builders ────────────────────────────────────────────────── */
static UA_StatusCode add_object(UA_UInt32 numeric_id, UA_NodeId parent,
                                UA_NodeId ref, const char *name) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)name);
    return UA_Server_addObjectNode(
        s_server, UA_NODEID_NUMERIC(s_ns, numeric_id), parent, ref,
        UA_QUALIFIEDNAME(s_ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, NULL, NULL);
}

static UA_StatusCode add_datasource_var(UA_NodeId node_id, UA_NodeId parent,
                                        const char *name, UA_UInt32 ns0_type,
                                        uintptr_t context) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)name);
    attr.dataType = UA_NODEID_NUMERIC(0, ns0_type);
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;

    UA_CallbackValueSource src;
    memset(&src, 0, sizeof(src));
    src.read = opcua_read_value;

    return UA_Server_addDataSourceVariableNode(
        s_server, node_id, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(s_ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, src,
        (void *)context, NULL);
}

static UA_StatusCode add_string_property(UA_UInt32 numeric_id,
                                         UA_NodeId parent, const char *name,
                                         const char *text) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)name);
    attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRING);
    attr.valueRank = UA_VALUERANK_SCALAR;
    UA_String s = UA_STRING((char *)text);
    UA_Variant_setScalar(&attr.value, &s, &UA_TYPES[UA_TYPES_STRING]);
    return UA_Server_addVariableNode(
        s_server, UA_NODEID_NUMERIC(s_ns, numeric_id), parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(s_ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
}

static UA_StatusCode add_range_property(UA_UInt32 numeric_id, UA_NodeId parent,
                                        double low, double high) {
    /* EURange as a Double[2] property. Full AnalogItem/EUInformation is
     * deferred (see docs/node_map.md) to keep MINIMAL NS0 dependency-free. */
    UA_Double range[2] = {low, high};
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", "EURange");
    attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_DOUBLE);
    attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
    /* valueRank 가 양수면 arrayDimensions 의 원소 수가 그 값과 같아야 한다.
     * 이걸 비워 두면 AddNode 가 BadTypeMismatch 로 거절한다 —
     * "The number of ArrayDimensions is not equal to the (positive) ValueRank".
     * 채널 노드 생성이 먼저 실패하던 동안에는 이 코드에 도달하지 못해
     * 결함이 드러나지 않았다 (2026-09-10 실기에서 확인). */
    UA_UInt32 dims[1] = {2};
    attr.arrayDimensions = dims;
    attr.arrayDimensionsSize = 1;
    UA_Variant_setArray(&attr.value, range, 2, &UA_TYPES[UA_TYPES_DOUBLE]);
    return UA_Server_addVariableNode(
        s_server, UA_NODEID_NUMERIC(s_ns, numeric_id), parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(s_ns, "EURange"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
}

static UA_UInt32 datatype_ns0(uint8_t dt) {
    switch(dt) {
    case OPCUA_DT_INT32:  return UA_NS0ID_INT32;
    case OPCUA_DT_BOOL:   return UA_NS0ID_BOOLEAN;
    case OPCUA_DT_STRING: return UA_NS0ID_STRING;
    case OPCUA_DT_FLOAT:
    default:              return UA_NS0ID_FLOAT;
    }
}

static void add_diag_var(UA_NodeId device, OpcUaDiagKind kind,
                         const char *name, UA_UInt32 ns0_type) {
    char nid[40];
    snprintf(nid, sizeof(nid), "Diag.%s", name);
    add_datasource_var(UA_NODEID_STRING(s_ns, nid), device, name, ns0_type,
                       OPCUA_DIAG_CTX_BASE + (uintptr_t)kind);
}

static UA_StatusCode add_address_space(void) {
    if(s_ns == 0)
        s_ns = UA_Server_addNamespace(s_server, OPCUA_NODE_NS_URI);

    UA_NodeId objects = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId root    = UA_NODEID_NUMERIC(s_ns, OPCUA_ID_ROOT);
    UA_NodeId device  = UA_NODEID_NUMERIC(s_ns, OPCUA_ID_DEVICE);
    UA_NodeId chans   = UA_NODEID_NUMERIC(s_ns, OPCUA_ID_CHANNELS);

    UA_StatusCode res = add_object(OPCUA_ID_ROOT, objects,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                   s_cfg->device_name);
    if(res != UA_STATUSCODE_GOOD) return res;

    res = add_object(OPCUA_ID_DEVICE, root,
                     UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), "Device");
    if(res != UA_STATUSCODE_GOOD) return res;

    res = add_object(OPCUA_ID_CHANNELS, root,
                     UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), "Channels");
    if(res != UA_STATUSCODE_GOOD) return res;

    /* Device diagnostics. */
    add_diag_var(device, OPCUA_DIAG_DEVICE_NAME,    "DeviceName",       UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_IP,             "IPAddress",        UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_FW_VERSION,     "FwVersion",        UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_UPTIME_S,       "Uptime_s",         UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_FREE_HEAP,      "FreeHeap",         UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_REBOOT_COUNT,   "RebootCount",      UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_REBOOT_REASON,  "RebootReason",     UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_LINK_STATUS,    "LinkStatus",       UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_RAW_FRAME,      "RawFrame",         UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_USB_FRAMES,     "UsbFrameCount",    UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_USB_ERRORS,     "UsbErrorCount",    UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_UART_FRAMES,    "UartFrameCount",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_UART_ERRORS,    "UartErrorCount",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_MODBUS_POLLS,   "ModbusPollCount",  UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_MODBUS_TIMEOUTS,"ModbusTimeoutCount",UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_TIME_SYNC,      "TimeSync",         UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_MIN_FREE_HEAP,  "MinFreeHeap",      UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_BUILD_ID,       "BuildId",          UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_SESSIONS,       "SessionCount",     UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_CHANNELS,       "ChannelCount",     UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_SOCKETS_BUSY,   "SocketsBusy",      UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_HANDSHAKE_REFUSED, "HandshakeRefused", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_CLOSE_CUT,      "CloseCut",         UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_CHIP_DROPPED,   "ChipDropped",      UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_TIMER_SLOTS_USED, "TimerSlotsUsed", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_TIMER_SLOTS_PEAK, "TimerSlotsPeak", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_SUBSCRIPTIONS,  "SubscriptionCount", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_MONITORED_ITEMS,"MonitoredItemCount",UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_ENTRIES,      "RetransmitEntries",     UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_ENTRIES_PEAK, "RetransmitEntriesPeak", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_BYTES,        "RetransmitBytes",       UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_BYTES_PEAK,   "RetransmitBytesPeak",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_HEAP,         "RetransmitHeap",        UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_HEAP_PEAK,    "RetransmitHeapPeak",    UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_SESSION_BYTES,   "RetransmitSessionMaxBytes",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_SESSION_ENTRIES, "RetransmitSessionMaxEntries", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_EVICTIONS,    "RetransmitEvictions",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_SESSION_HEAP, "RetransmitSessionMaxHeap", UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_FREED_EST,    "RetransmitFreedEstimate",  UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_RETRANSMIT_FREED_MEAS,   "RetransmitFreedMeasured",  UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_CMD_TIMEOUTS,            "CmdTimeouts",           UA_NS0ID_UINT32);

#ifdef OPCUA_DT_TEST_NODES
    for(uint16_t i = 0; i < DT_COUNT; i++) {
        char nid[32];
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_CallbackValueSource src;
        snprintf(nid, sizeof(nid), "Test.%s", s_dt_names[i]);
        attr.displayName = UA_LOCALIZEDTEXT("en", (char *)s_dt_names[i]);
        attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);
        attr.valueRank = UA_VALUERANK_ANY;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ;
        attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;
        memset(&src, 0, sizeof(src));
        src.read = opcua_read_value;
        res = UA_Server_addDataSourceVariableNode(
            s_server, UA_NODEID_STRING(s_ns, nid), device,
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
            UA_QUALIFIEDNAME(s_ns, (char *)s_dt_names[i]),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, src,
            (void *)(uintptr_t)(OPCUA_DT_CTX_BASE + i), NULL);
        if(res != UA_STATUSCODE_GOOD)
            printf("[OPC UA] test node '%s' add failed: %s\r\n", s_dt_names[i],
                   UA_StatusCode_name(res));
    }
#endif

    /* Channels. */
    for(uint16_t i = 0; i < s_cfg->channel_count; i++) {
        const channel_def_t *c = &s_cfg->channels[i];
        if(!c->enabled)
            continue;

        char nid[64];
        snprintf(nid, sizeof(nid), "Channels/%s", c->name);
        UA_NodeId var_id = UA_NODEID_STRING(s_ns, nid);

        res = add_datasource_var(var_id, chans, c->name,
                                 datatype_ns0(c->datatype), (uintptr_t)i);
        if(res != UA_STATUSCODE_GOOD) {
            printf("[OPC UA] channel '%s' add failed: %s\r\n", c->name,
                   UA_StatusCode_name(res));
            continue;
        }
        if(c->unit[0] != '\0')
            add_string_property(OPCUA_ID_PROP_BASE + (UA_UInt32)i * 4u + 0u,
                                var_id, "EngineeringUnits", c->unit);
        add_range_property(OPCUA_ID_PROP_BASE + (UA_UInt32)i * 4u + 1u,
                           var_id, c->eu_low, c->eu_high);
    }
    return UA_STATUSCODE_GOOD;
}
#endif /* OPCUA_ENABLE */

int opcua_server_init(const opcua_settings_t *cfg) {
    s_cfg = cfg;
    build_endpoint_url(cfg);
    s_status.endpoint_url = s_endpoint;
    s_status.application_uri = OPCUA_APPLICATION_URI;
    s_status.channel_count = cfg->channel_count;

#if OPCUA_ENABLE
    s_address_space_ready = false;
    s_address_space_status = UA_STATUSCODE_GOOD;

    UA_ServerConfig config;
    memset(&config, 0, sizeof(config));

    /* 트랜스포트가 동시에 LISTEN 할 하드웨어 소켓 수.
     * 반드시 서버 생성 전에 지정한다. */
    UA_EventLoop_LWIP_setSocketCount(OPCUA_MAX_SOCKETS);

    UA_StatusCode res = UA_ServerConfig_setMinimalCustomBuffer(
        &config, WIZNET_OPCUA_TCP_PORT, NULL, 8192u, 8192u);

    /* 리소스 상한. open62541 기본값은 세션 100 / 보안채널 100 이라
     * 소켓 4개짜리 장치에 맞지 않는다. 트랜스포트 수와 어긋나면 TCP 는
     * 못 붙는데 서버는 여유가 있다고 판단하는 불일치가 생긴다.
     *
     * 한도 초과 시 동작이 항목마다 다르다. maxSessions 를 넘으면
     * BadTooManySessions 로 깔끔하게 거부되지만, maxSecureChannels 를
     * 넘으면 세션이 붙지 않은 채널을 먼저 강제 종료한다
     * (purgeFirstChannelWithoutSession). 즉 동작 중인 클라이언트가
     * 쫓겨나지는 않아도 핸드셰이크 중인 접속은 밀려날 수 있다. */
    if(res == UA_STATUSCODE_GOOD) {
        /* 값이 아직 없는 DataSource 노드를 허용한다.
         *
         * 기본값(UA_RULEHANDLING_DEFAULT)은 AddNode 때 타입 검사를 하다가
         * 값이 비어 있으면 setDefaultValue() 로 기본값을 **쓰려고** 한다.
         * 우리 채널은 읽기 전용 DataSource 라 쓰기 콜백이 없으므로 그 쓰기가
         * BadWriteNotSupported 로 실패하고, 그 결과 채널 노드가 아예 생성되지
         * 않는다 (2026-09-10 실기에서 Channel_1..3 전부 실패).
         *
         * 한 번도 기록되지 않은 채널이 Bad_WaitingForInitialData 를 값 없이
         * 반환하는 것은 의도된 동작이므로, 규칙 쪽을 ACCEPT 으로 낮춘다.
         * 런타임 읽기는 그대로 DataSource 콜백을 탄다. */
        config.allowEmptyVariables        = UA_RULEHANDLING_ACCEPT;
        config.maxSecureChannels          = OPCUA_MAX_SOCKETS;
        /* Twice the sockets, on purpose. A session outlives its channel by
         * design - a client whose connection drops may reactivate it on a new
         * one - so when a client vanishes, its socket comes back through
         * keepalive within seconds but its session holds a slot until
         * maxSessionTimeout. With sessions == sockets, four vanished clients
         * locked the server out for ten minutes with every socket free
         * (BadTooManySessions, seen 2026-10-15). Live clients stay bounded by
         * the four sockets either way; the extra slots only absorb orphans,
         * at about 2.4 KB each. open62541 logs a startup warning that
         * channels are fewer than sessions; that is this decision, not a
         * misconfiguration. */
        config.maxSessions                = 2u * OPCUA_MAX_SOCKETS;
        config.maxSessionTimeout          = 10.0 * 60.0 * 1000.0; /* 10분 */
        /* 25, not 100: every message is also kept for Republish until it is
         * acknowledged, and one sent during a link outage never is. At 100
         * notifications a kept message took several KB of heap and four
         * sessions ran the board out after six outages (2026-10-29). */
        config.maxNotificationsPerPublish = 25;
        /* Per-client caps. open62541 leaves these at 0, which means no limit:
         * a client could create subscriptions, monitored items or queued
         * Publish requests until malloc fails. Past a cap the client gets
         * BadTooManySubscriptions / BadTooManyMonitoredItems, and the oldest
         * queued Publish is answered with BadTooManyPublishRequests. */
        /* Publishing interval capped at the longest KeepAlive time, so the
         * time caps patched into open62541 hold at every interval: KeepAlive
         * >= 1 publish, Lifetime >= 3 x KeepAlive <= OPCUA_SUB_MAX_LIFETIME_MS.
         * See port/open62541/patches/subscription_time_limits.cmake. */
        config.publishingIntervalLimits.max = (UA_Double)OPCUA_SUB_MAX_KEEPALIVE_MS;
        config.maxSubscriptionsPerSession       = 4;
        config.maxMonitoredItemsPerSubscription = 100;
        config.maxPublishReqPerSession          = 8;
        /* Server-wide caps. The per-client caps above still multiply out to
         * 1,600 items across four clients, and 400 items took 124 KB of heap
         * (2026-10-29), so that product does not fit. 400 is the load actually
         * measured; subscriptions are four clients at their per-session cap.
         * An abandoned subscription counts until its lifetime ends (at most
         * OPCUA_SUB_MAX_LIFETIME_MS), so a client that vanished and returned
         * at full load can be refused until then. */
        config.maxSubscriptions                 = 4u * OPCUA_MAX_SOCKETS;
        /* 400 on the RP2350, 200 on the RP2040 (264 KB of SRAM: 240 items
         * across four clients left a 24 KB low-water mark, 2026-11-02); set in
         * port/open62541/CMakeLists.txt, which also sizes the timer table
         * from it. Item size is not counted here: below
         * OPCUA_MI_HEAP_RESERVE of free heap new items are refused instead
         * (patches/mi_heap_reserve.cmake). */
        config.maxMonitoredItems                = OPCUA_MAX_MONITORED_ITEMS;
        /* Messages kept for Republish, counted per session (ua_subscription.c,
         * session-wide check). Part 4 asks for at least twice the Publish
         * requests a session may queue: 2 x 8 = 16. A byte cap applies as well
         * (OPCUA_RETRANSMIT_MAX_BYTES, 16 KB encoded per session) and a heap
         * cap (OPCUA_RETRANSMIT_MAX_HEAP, 12 KB estimated per session), oldest
         * evicted first; port/open62541/patches/retransmit_byte_cap.cmake.
         * A kept message holds ~3.6x its encoded size in heap, so the heap
         * cap is the one that binds; it keeps fewer than 16 when messages
         * are large, which trades Part 4's minimum for not running out of
         * heap. Initial test values, not a product specification. */
        config.maxRetransmissionQueueSize = 16;
    }
    if(res == UA_STATUSCODE_GOOD)
        res = configure_server_identity(&config);

    /* Security policies and their endpoints, when the build has them.
     *
     * After the identity is set, because the certificates carry the same
     * ApplicationUri in their subjectAltName and a client checks the two
     * against each other. Before the server is created, because the endpoint
     * list is read out of the config at that moment.
     *
     * A failure here is logged, not fatal: a device that still answers on
     * SecurityPolicy#None can be reached and diagnosed, which a device that
     * refuses to start cannot. */
    if(res == UA_STATUSCODE_GOOD) {
        opcua_security_apply(&config);
        opcua_security_report(&config);
    }

    if(res == UA_STATUSCODE_GOOD) {
        s_server = UA_Server_newWithConfig(&config);
        if(!s_server)
            res = UA_STATUSCODE_BADOUTOFMEMORY;
    }

    bool startup_done = false;
    if(res == UA_STATUSCODE_GOOD) {
        res = UA_Server_run_startup(s_server);
        startup_done = (res == UA_STATUSCODE_GOOD);
    }

    if(res != UA_STATUSCODE_GOOD) {
        if(s_server) {
            if(startup_done)
                UA_Server_run_shutdown(s_server);
            UA_Server_delete(s_server);
            s_server = NULL;
        } else {
            UA_ServerConfig_clear(&config);
        }
        s_status.state = OPCUA_SERVER_STATE_ERROR;
        s_status.network_enabled = false;
        s_status.status_text = UA_StatusCode_name(res);
        printf("[OPC UA] startup failed: %s\r\n", UA_StatusCode_name(res));
        return -1;
    }

    s_address_space_status = add_address_space();
    s_address_space_ready = (s_address_space_status == UA_STATUSCODE_GOOD);
    if(!s_address_space_ready)
        printf("[OPC UA] address space warning: %s\r\n",
               UA_StatusCode_name(s_address_space_status));

    s_server_started = true;
    s_status.state = OPCUA_SERVER_STATE_RUNNING;
    s_status.network_enabled = true;
    s_status.status_text = s_address_space_ready
                               ? "open62541 running; dynamic address space"
                               : "open62541 running; address space incomplete";
    return 0;
#else
#if OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_init();
    s_status.state = OPCUA_SERVER_STATE_RUNNING;
    s_status.network_enabled = true;
    s_status.status_text = "WIZnet TCP probe; open62541 not linked";
    return 0;
#else
    s_status.state = OPCUA_SERVER_STATE_READY;
    s_status.network_enabled = false;
    s_status.status_text = "OPC UA scaffold; open62541/WIZnet transport not linked";
    return 0;
#endif
#endif
}

int opcua_server_reload_nodes(const opcua_settings_t *cfg) {
    s_cfg = cfg;
    s_status.channel_count = cfg->channel_count;
#if OPCUA_ENABLE
    if(!s_server_started || !s_server)
        return -1;
    /* Delete the root subtree, then rebuild from cfg. Namespace persists. */
    UA_Server_deleteNode(s_server, UA_NODEID_NUMERIC(s_ns, OPCUA_ID_ROOT), true);
    s_address_space_status = add_address_space();
    s_address_space_ready = (s_address_space_status == UA_STATUSCODE_GOOD);
    printf("[OPC UA] node reload %s (%u channels)\r\n",
           s_address_space_ready ? "ok" : "partial",
           (unsigned)cfg->channel_count);
    return s_address_space_ready ? 0 : -1;
#else
    return 0;
#endif
}

void opcua_server_poll(void) {
#if OPCUA_ENABLE
    if(s_server_started && s_server) {
        UA_Server_run_iterate(s_server, false);
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if(now - s_rt_sampled_ms >= 1000u) {
            s_rt_sampled_ms = now;
            sample_retransmit();
        }
    }
#elif OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_poll();
#endif
}

void opcua_server_get_status(OpcUaServerStatus *status) {
    if(status)
        *status = s_status;
}

void opcua_server_print_status(void) {
    printf("[OPC UA] state=%s network=%s endpoint=%s channels=%u\r\n",
           state_name(s_status.state),
           s_status.network_enabled ? "on" : "off",
           s_status.endpoint_url ? s_status.endpoint_url : "(none)",
           (unsigned)s_status.channel_count);
    printf("[OPC UA] %s\r\n",
           s_status.status_text ? s_status.status_text : "(no status)");
    struct mallinfo m = mallinfo();
    printf("[heap] total=%lu in_use=%lu arena=%lu peak_arena=%lu "
           "free=%lu min_free=%lu\r\n",
           (unsigned long)(&__StackLimit - &__bss_end__),
           (unsigned long)m.uordblks, (unsigned long)m.arena,
           __malloc_max_sbrked_mem, (unsigned long)free_heap(),
           (unsigned long)min_free_heap());
    UA_EventLoop_LWIP_Stats es;
    UA_EventLoop_LWIP_getStats(&es);
    printf("[sock] busy=%u cmd_timeouts=%lu cmd_max_us=%lu\r\n",
           (unsigned)es.socketsBusy, (unsigned long)es.cmdTimeouts,
           (unsigned long)es.cmdMaxUs);
#if OPCUA_ENABLE
    /* Same values as the Diag.* nodes, here so they can be read while the
     * network is down (LINKDOWN tests poll this over USB). */
    if(s_server) {
        sample_retransmit();
        printf("[subs] subs=%u items=%u heap_refused=%u timers=%u/%u peak=%u "
               "run_max=%luus "
               "rt_entries=%u peak=%u rt_bytes=%u peak=%u rt_heap=%u peak=%u "
               "sess_bytes=%u sess_heap=%u sess_entries=%u evict=%u notkept=%u "
               "freed=%u est=%u meas=%u\r\n",
               (unsigned)UA_Server_wizSubscriptionCount(s_server),
               (unsigned)UA_Server_wizMonitoredItemCount(s_server),
               (unsigned)UA_Server_wizMiHeapRefused(),
               (unsigned)es.timerSlotsUsed, (unsigned)es.timerSlotsMax,
               (unsigned)es.timerSlotsPeak, (unsigned long)es.timerRunMaxUs,
               (unsigned)s_rt.entries, (unsigned)s_rt_entries_peak,
               (unsigned)s_rt.bytes, (unsigned)s_rt_bytes_peak,
               (unsigned)s_rt.heap, (unsigned)s_rt_heap_peak,
               (unsigned)s_rt.sessionMaxBytes, (unsigned)s_rt.sessionMaxHeap,
               (unsigned)s_rt.sessionMaxEntries,
               (unsigned)s_rt.evictions, (unsigned)s_rt.notKept,
               (unsigned)s_rt.freedCount, (unsigned)s_rt.freedEstimate,
               (unsigned)s_rt.freedMeasured);
    }
#endif
#if !OPCUA_ENABLE && OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_print_status();
#endif
}
