# SPDX-License-Identifier: MPL-2.0
#
# This file carries text from open62541 (https://github.com/open62541/open62541,
# MPL-2.0) and the WIZnet modification of it. Both are covered by MPL-2.0.
#
# Patch: per-session caps on the retransmission queue - encoded bytes and
# heap - and the counters that check them.
#
# Applied after subscription_time_limits.cmake, the same way (string REPLACE
# on the generated amalgamation, FATAL_ERROR on a missing anchor). Written
# against open62541 10022ffbc (v1.5.4-224).
#
# Why (measured 2026-10-29, opcua_node, 4 clients x 100 monitored items):
#   Responses sent while the link is down are never acknowledged, so they stay
#   in the retransmission queue until the session ends. Limited by count only,
#   the queues took the board out of heap after six outages (two panics).
#   An encoded-byte cap alone was not enough: a kept message holds about 3.6x
#   its encoded size in heap (683 B encoded, ~2.5 KB heap, estimated from the
#   FreeHeap trend), so the count cap was reached first and four full sessions
#   would still hold ~160 KB.
#
# Policy, per session, 0 = off (CMake cache options):
#   OPCUA_RETRANSMIT_MAX_BYTES  encoded bytes (what Republish would resend)
#   OPCUA_RETRANSMIT_MAX_HEAP   heap held by the kept messages
#   - Adding a message that takes the session over either cap evicts that
#     session's oldest messages first, with the existing
#     removeOldestRetransmissionMessageFromSession().
#   - A message over either cap on its own is not kept. The PublishResponse
#     is sent as usual; only Republish of it is lost.
#   - Acknowledged messages are freed by the existing code.
#   The count cap (maxRetransmissionQueueSize) still applies first. With the
#   byte/heap caps a session may keep fewer than Part 4's 2 x Publish
#   requests; that trade is deliberate - see opcua_server.c.
#
# Heap accounting:
#   Estimate - each entry's heap is computed when it is queued by walking the
#   message with the type descriptions, the same walk UA_clear() makes, and
#   counting every allocation as a newlib chunk: request + 4 bytes of header,
#   rounded up to 8, at least 16. The message's memory was allocated earlier
#   (at sampling) and only moved into the message, so measuring malloc growth
#   while building it shows nothing - tried, it read 40 B per message.
#   Measured - when an entry is freed (acknowledged or evicted) the drop of
#   mallinfo().uordblks across its clear+free is the real heap it held.
#   Single-threaded build (UA_MULTITHREADING 0), so nothing else allocates in
#   between. Both are summed over freed entries; their ratio says how far the
#   estimate - which the heap cap uses - can be trusted.

# 1. Entry fields, the heap walker and the counters, ahead of every use.
set(_wiz_rt_old [=[
typedef struct UA_NotificationMessageEntry {
    TAILQ_ENTRY(UA_NotificationMessageEntry) listEntry;
    UA_NotificationMessage message;
} UA_NotificationMessageEntry;
]=])
set(_wiz_rt_new [=[
#include <malloc.h>  /* WIZnet patch: mallinfo() for the measured heap */
typedef struct UA_NotificationMessageEntry {
    TAILQ_ENTRY(UA_NotificationMessageEntry) listEntry;
    UA_NotificationMessage message;
    size_t wizBytes;    /* WIZnet patch: encoded size of message */
    size_t wizHeap;     /* WIZnet patch: estimated heap of message and entry */
} UA_NotificationMessageEntry;

/* WIZnet patch (port/open62541/patches/retransmit_byte_cap.cmake) */
static size_t wizRetransmitEvictions = 0;
static size_t wizRetransmitNotKept = 0;
static size_t wizFreedCount = 0, wizFreedEstimate = 0, wizFreedMeasured = 0;

/* newlib malloc chunk for an n-byte request (32-bit: 4-byte header, 8-byte
 * alignment, 16-byte minimum). */
static size_t
wizChunk(size_t n) {
    if(n == 0)
        return 0;
    size_t c = (n + 4 + 7) & ~(size_t)7;
    return c < 16 ? 16 : c;
}

static size_t wizHeapOf(const void *p, const UA_DataType *type);

static size_t
wizHeapArray(const void *arr, size_t len, const UA_DataType *type) {
    if((uintptr_t)arr <= (uintptr_t)UA_EMPTY_ARRAY_SENTINEL || len == 0)
        return 0;
    size_t h = wizChunk(len * type->memSize);
    if(!type->pointerFree) {
        for(size_t i = 0; i < len; i++)
            h += wizHeapOf((const char*)arr + i * type->memSize, type);
    }
    return h;
}

static size_t
wizHeapString(const UA_String *s) {
    return (uintptr_t)s->data > (uintptr_t)UA_EMPTY_ARRAY_SENTINEL ?
        wizChunk(s->length) : 0;
}

static size_t
wizHeapNodeId(const UA_NodeId *n) {
    return (n->identifierType == UA_NODEIDTYPE_STRING ||
            n->identifierType == UA_NODEIDTYPE_BYTESTRING) ?
        wizHeapString(&n->identifier.string) : 0;
}

/* Heap owned by *p, not counting *p itself. Mirrors the clear functions. */
static size_t
wizHeapOf(const void *p, const UA_DataType *type) {
    switch(type->typeKind) {
    case UA_DATATYPEKIND_STRING:
    case UA_DATATYPEKIND_BYTESTRING:
    case UA_DATATYPEKIND_XMLELEMENT:
        return wizHeapString((const UA_String*)p);
    case UA_DATATYPEKIND_NODEID:
        return wizHeapNodeId((const UA_NodeId*)p);
    case UA_DATATYPEKIND_EXPANDEDNODEID: {
        const UA_ExpandedNodeId *e = (const UA_ExpandedNodeId*)p;
        return wizHeapNodeId(&e->nodeId) + wizHeapString(&e->namespaceUri);
    }
    case UA_DATATYPEKIND_QUALIFIEDNAME:
        return wizHeapString(&((const UA_QualifiedName*)p)->name);
    case UA_DATATYPEKIND_LOCALIZEDTEXT: {
        const UA_LocalizedText *l = (const UA_LocalizedText*)p;
        return wizHeapString(&l->locale) + wizHeapString(&l->text);
    }
    case UA_DATATYPEKIND_EXTENSIONOBJECT: {
        const UA_ExtensionObject *x = (const UA_ExtensionObject*)p;
        if(x->encoding == UA_EXTENSIONOBJECT_DECODED) {
            if(!x->content.decoded.data)
                return 0;
            return wizChunk(x->content.decoded.type->memSize) +
                wizHeapOf(x->content.decoded.data, x->content.decoded.type);
        }
        if(x->encoding <= UA_EXTENSIONOBJECT_ENCODED_XML)
            return wizHeapNodeId(&x->content.encoded.typeId) +
                wizHeapString(&x->content.encoded.body);
        return 0;   /* DECODED_NODELETE: borrowed */
    }
    case UA_DATATYPEKIND_VARIANT: {
        const UA_Variant *v = (const UA_Variant*)p;
        if(v->storageType == UA_VARIANT_DATA_NODELETE)
            return 0;
        size_t h = 0;
        if(v->type)
            h += wizHeapArray(v->data, v->arrayLength ? v->arrayLength : 1, v->type);
        if((uintptr_t)v->arrayDimensions > (uintptr_t)UA_EMPTY_ARRAY_SENTINEL)
            h += wizChunk(v->arrayDimensionsSize * sizeof(UA_UInt32));
        return h;
    }
    case UA_DATATYPEKIND_DATAVALUE:
        return wizHeapOf(&((const UA_DataValue*)p)->value, &UA_TYPES[UA_TYPES_VARIANT]);
    case UA_DATATYPEKIND_DIAGNOSTICINFO: {
        const UA_DiagnosticInfo *d = (const UA_DiagnosticInfo*)p;
        size_t h = wizHeapString(&d->additionalInfo);
        if(d->hasInnerDiagnosticInfo && d->innerDiagnosticInfo)
            h += wizChunk(sizeof(UA_DiagnosticInfo)) +
                wizHeapOf(d->innerDiagnosticInfo, type);
        return h;
    }
    case UA_DATATYPEKIND_STRUCTURE:
    case UA_DATATYPEKIND_OPTSTRUCT: {
        size_t h = 0;
        uintptr_t ptr = (uintptr_t)p;
        for(size_t i = 0; i < type->membersSize; ++i) {
            const UA_DataTypeMember *m = &type->members[i];
            const UA_DataType *mt = m->memberType;
            ptr += m->padding;
            if(!m->isOptional && !m->isArray) {
                h += wizHeapOf((const void*)ptr, mt);
                ptr += mt->memSize;
            } else if(!m->isOptional) {
                size_t len = *(const size_t*)ptr;
                ptr += sizeof(size_t);
                h += wizHeapArray(*(void *const *)ptr, len, mt);
                ptr += sizeof(void*);
            } else if(!m->isArray) {
                h += wizHeapArray(*(void *const *)ptr, 1, mt);
                ptr += sizeof(void*);
            } else {
                size_t len = *(const size_t*)ptr;
                ptr += sizeof(size_t);
                h += wizHeapArray(*(void *const *)ptr, len, mt);
                ptr += sizeof(void*);
            }
        }
        return h;
    }
    default:
        return 0;   /* fixed-size scalars; unions do not occur in messages */
    }
}

/* Clear and free one entry, adding its estimated and measured heap to the
 * counters. */
static void
wizFreeEntry(UA_NotificationMessageEntry *e) {
    size_t before = (size_t)mallinfo().uordblks;
    size_t est = e->wizHeap;
    UA_NotificationMessage_clear(&e->message);
    UA_free(e);
    size_t after = (size_t)mallinfo().uordblks;
    wizFreedCount++;
    wizFreedEstimate += est;
    wizFreedMeasured += before > after ? before - after : 0;
}
]=])

# 2. Evicting: free through wizFreeEntry so the counters see it.
set(_wiz_rt_old2 [=[
    TAILQ_REMOVE(&sub->retransmissionQueue, oldestEntry, listEntry);
    UA_NotificationMessage_clear(&oldestEntry->message);
    UA_free(oldestEntry);
]=])
set(_wiz_rt_new2 [=[
    TAILQ_REMOVE(&sub->retransmissionQueue, oldestEntry, listEntry);
    wizFreeEntry(oldestEntry);  /* WIZnet patch: was clear + free */
]=])

# 3. Acknowledged: the same.
set(_wiz_rt_old3 [=[
    TAILQ_REMOVE(&sub->retransmissionQueue, entry, listEntry);
    --sub->retransmissionQueueSize;
    UA_NotificationMessage_clear(&entry->message);
    UA_free(entry);
]=])
set(_wiz_rt_new3 [=[
    TAILQ_REMOVE(&sub->retransmissionQueue, entry, listEntry);
    --sub->retransmissionQueueSize;
    wizFreeEntry(entry);        /* WIZnet patch: was clear + free */
]=])

# 4. Session totals, ahead of the function that caps them.
set(_wiz_rt_old4 [=[
static void
UA_Subscription_addRetransmissionMessage(UA_Server *server, UA_Subscription *sub,
                                         UA_NotificationMessageEntry *entry) {
]=])
set(_wiz_rt_new4 [=[
/* WIZnet patch: encoded bytes and estimated heap held by one session's
 * retransmission queues. At most maxRetransmissionQueueSize entries. */
static void
wizSessionRetransmit(UA_Session *session, size_t *bytes, size_t *heap) {
    size_t b = 0, h = 0;
    UA_Subscription *s;
    TAILQ_FOREACH(s, &session->subscriptions, sessionListEntry) {
        UA_NotificationMessageEntry *e;
        TAILQ_FOREACH(e, &s->retransmissionQueue, listEntry) {
            b += e->wizBytes;
            h += e->wizHeap;
        }
    }
    *bytes = b;
    *heap = h;
}

static UA_Boolean
wizSessionOverCap(UA_Session *session, const UA_NotificationMessageEntry *add) {
    size_t b, h;
    wizSessionRetransmit(session, &b, &h);
    return (OPCUA_RETRANSMIT_MAX_BYTES > 0 &&
            b + add->wizBytes > (size_t)OPCUA_RETRANSMIT_MAX_BYTES) ||
           (OPCUA_RETRANSMIT_MAX_HEAP > 0 &&
            h + add->wizHeap > (size_t)OPCUA_RETRANSMIT_MAX_HEAP);
}

static void
UA_Subscription_addRetransmissionMessage(UA_Server *server, UA_Subscription *sub,
                                         UA_NotificationMessageEntry *entry) {
]=])

# 5. The caps, where a message joins the queue.
set(_wiz_rt_old5 [=[
    /* Add entry */
    TAILQ_INSERT_TAIL(&sub->retransmissionQueue, entry, listEntry);
    ++sub->retransmissionQueueSize;
]=])
set(_wiz_rt_new5 [=[
    /* WIZnet patch (port/open62541/patches/retransmit_byte_cap.cmake):
     * keep the session's queues within the byte and heap caps. */
    if(session) {
        while(session->totalRetransmissionQueueSize > 0 &&
              wizSessionOverCap(session, entry)) {
            removeOldestRetransmissionMessageFromSession(session);
            wizRetransmitEvictions++;
        }
    }

    /* Add entry */
    TAILQ_INSERT_TAIL(&sub->retransmissionQueue, entry, listEntry);
    ++sub->retransmissionQueueSize;
]=])

# 6. Size the entry before it joins the queue; drop it if it alone is over.
set(_wiz_rt_old6 [=[
            retransmission->message = response->notificationMessage;
            UA_Subscription_addRetransmissionMessage(server, sub, retransmission);
]=])
set(_wiz_rt_new6 [=[
            retransmission->message = response->notificationMessage;
            /* WIZnet patch (retransmit_byte_cap.cmake) */
            retransmission->wizBytes =
                UA_calcSizeBinary(&retransmission->message,
                                  &UA_TYPES[UA_TYPES_NOTIFICATIONMESSAGE], NULL);
            retransmission->wizHeap = wizChunk(sizeof(UA_NotificationMessageEntry)) +
                wizHeapOf(&retransmission->message,
                          &UA_TYPES[UA_TYPES_NOTIFICATIONMESSAGE]);
            if((OPCUA_RETRANSMIT_MAX_BYTES > 0 &&
                retransmission->wizBytes > (size_t)OPCUA_RETRANSMIT_MAX_BYTES) ||
               (OPCUA_RETRANSMIT_MAX_HEAP > 0 &&
                retransmission->wizHeap > (size_t)OPCUA_RETRANSMIT_MAX_HEAP)) {
                /* Too big to keep: send it, but do not queue it. Clearing the
                 * pointer leaves the message with the response, which frees
                 * it after sending. */
                UA_free(retransmission);
                retransmission = NULL;
                wizRetransmitNotKept++;
            } else {
                UA_Subscription_addRetransmissionMessage(server, sub, retransmission);
            }
]=])

foreach(_i "" 2 3 4 5 6)
    string(FIND "${OPEN62541_WIZNET_PATCHED_SOURCE}" "${_wiz_rt_old${_i}}" _wiz_at)
    if(_wiz_at EQUAL -1)
        message(FATAL_ERROR
            "retransmit_byte_cap: anchor ${_i} not found. "
            "open62541 changed; redo the patch against the new source.")
    endif()
    string(REPLACE "${_wiz_rt_old${_i}}" "${_wiz_rt_new${_i}}"
           OPEN62541_WIZNET_PATCHED_SOURCE "${OPEN62541_WIZNET_PATCHED_SOURCE}")
endforeach()

# Usage over all subscriptions, the fullest session, and the accounting check.
# Declared in open62541_wiznet_patches.h. Walks every queued message (at most
# 16 per session); call at a diagnostic rate.
string(APPEND OPEN62541_WIZNET_PATCHED_SOURCE [=[

/* WIZnet patch (port/open62541/patches/retransmit_byte_cap.cmake) */
#ifdef UA_ENABLE_SUBSCRIPTIONS
#ifndef UA_WIZ_RETRANSMIT_USAGE_DEFINED   /* same definition as the header */
#define UA_WIZ_RETRANSMIT_USAGE_DEFINED
typedef struct {
    size_t entries, bytes, heap;              /* all subscriptions            */
    size_t sessionMaxBytes, sessionMaxHeap;   /* the fullest single session   */
    size_t sessionMaxEntries;
    size_t evictions, notKept;                /* since boot, byte/heap caps   */
    size_t freedCount, freedEstimate, freedMeasured; /* accounting check      */
} UA_WizRetransmitUsage;
#endif
void UA_Server_wizRetransmitUsage(UA_Server *server, UA_WizRetransmitUsage *u) {
    memset(u, 0, sizeof(*u));
    UA_Subscription *sub;
    LIST_FOREACH(sub, &server->subscriptions, serverListEntry) {
        UA_NotificationMessageEntry *e;
        TAILQ_FOREACH(e, &sub->retransmissionQueue, listEntry) {
            u->entries++;
            u->bytes += e->wizBytes;
            u->heap += e->wizHeap;
        }
    }
    session_list_entry *se;
    LIST_FOREACH(se, &server->sessions, pointers) {
        size_t b, h;
        wizSessionRetransmit(&se->session, &b, &h);
        if(b > u->sessionMaxBytes)
            u->sessionMaxBytes = b;
        if(h > u->sessionMaxHeap)
            u->sessionMaxHeap = h;
        if(se->session.totalRetransmissionQueueSize > u->sessionMaxEntries)
            u->sessionMaxEntries = se->session.totalRetransmissionQueueSize;
    }
    u->evictions = wizRetransmitEvictions;
    u->notKept = wizRetransmitNotKept;
    u->freedCount = wizFreedCount;
    u->freedEstimate = wizFreedEstimate;
    u->freedMeasured = wizFreedMeasured;
}
#endif
]=])
