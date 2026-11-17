# Security policies in the generated amalgamation.
#
# The generator decides this: with UA_ENABLE_ENCRYPTION=MBEDTLS the produced
# header defines UA_ENABLE_ENCRYPTION and UA_ENABLE_ENCRYPTION_MBEDTLS, and
# without it the same lines are #undef. The crypto plugin sources are
# concatenated either way (open62541 CMakeLists:1188 includes them whenever
# UA_ENABLE_AMALGAMATION is on), so the difference on disk is about 29 KB of
# source and two defines - but those defines are what compile the policies in.
#
# -DOPCUA_ENABLE_ENCRYPTION=ON at configure time. The two settings get separate
# amalgamation directories: the generation step is skipped when the output
# already exists, so sharing one directory would silently serve whichever was
# generated first.
option(OPCUA_ENABLE_ENCRYPTION
       "Generate the open62541 amalgamation with mbedTLS security policies" OFF)

if(OPCUA_ENABLE_ENCRYPTION)
    set(_ua_amalg_suffix "-enc")
else()
    set(_ua_amalg_suffix "")
endif()

set(OPEN62541_WIZNET_AMALGAMATION_DIR
    "${CMAKE_BINARY_DIR}/open62541_pico_amalgamation${_ua_amalg_suffix}"
    CACHE PATH "Directory containing generated open62541.c/open62541.h" FORCE)

set(OPEN62541_WIZNET_C "${OPEN62541_WIZNET_AMALGAMATION_DIR}/open62541.c")
set(OPEN62541_WIZNET_H "${OPEN62541_WIZNET_AMALGAMATION_DIR}/open62541.h")
set(OPEN62541_WIZNET_PATCHED_C
    "${CMAKE_CURRENT_BINARY_DIR}/open62541_wiznet_minimal_ns0.c")

if(NOT EXISTS "${OPEN62541_WIZNET_C}" OR NOT EXISTS "${OPEN62541_WIZNET_H}")
    message(STATUS "open62541 amalgamation not found — generating automatically...")

    set(_ua_enc_args "")
    if(OPCUA_ENABLE_ENCRYPTION)
        # find_package(MbedTLS) insists on three library files before it will
        # report success, even though the amalgamation target only concatenates
        # sources and links nothing. The real libraries are built later in this
        # same tree and do not exist yet at configure time, so empty stand-ins
        # are created purely to satisfy the search. Nothing reads them.
        set(_mbed_stub "${CMAKE_BINARY_DIR}/amalgamation_mbedtls_stub")
        file(MAKE_DIRECTORY "${_mbed_stub}")
        foreach(_lib mbedtls mbedx509 mbedcrypto)
            if(NOT EXISTS "${_mbed_stub}/lib${_lib}.a")
                file(TOUCH "${_mbed_stub}/lib${_lib}.a")
            endif()
        endforeach()

        list(APPEND _ua_enc_args
            -DUA_ENABLE_ENCRYPTION=MBEDTLS
            -DMBEDTLS_INCLUDE_DIRS=${CMAKE_SOURCE_DIR}/libraries/mbedtls/include
            -DMBEDTLS_LIBRARY=${_mbed_stub}/libmbedtls.a
            -DMBEDX509_LIBRARY=${_mbed_stub}/libmbedx509.a
            -DMBEDCRYPTO_LIBRARY=${_mbed_stub}/libmbedcrypto.a
        )
        message(STATUS "open62541 amalgamation: security policies ENABLED (mbedTLS)")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR}/libraries/open62541
            -B ${OPEN62541_WIZNET_AMALGAMATION_DIR}
            -DUA_ARCHITECTURE=none
            -DUA_ENABLE_AMALGAMATION=ON
            -DUA_NAMESPACE_ZERO=MINIMAL
            -DUA_ENABLE_METHODCALLS=ON
            -DUA_ENABLE_SUBSCRIPTIONS=ON
            -DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF
            -DUA_ENABLE_HISTORIZING=OFF
            -DUA_ENABLE_PUBSUB=OFF
            -DUA_ENABLE_DISCOVERY=OFF
            -DUA_ENABLE_NODEMANAGEMENT=ON
            -DUA_MULTITHREADING=0
            -DUA_BUILD_EXAMPLES=OFF
            -DUA_BUILD_UNIT_TESTS=OFF
            ${_ua_enc_args}
        RESULT_VARIABLE _cfg_result
    )
    if(NOT _cfg_result EQUAL 0)
        message(FATAL_ERROR "open62541 amalgamation configure failed (exit ${_cfg_result})")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            --build ${OPEN62541_WIZNET_AMALGAMATION_DIR}
            --target open62541-amalgamation
            --config Release
        RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR "open62541 amalgamation build failed (exit ${_build_result})")
    endif()

    message(STATUS "open62541 amalgamation generated successfully.")
endif()

file(READ "${OPEN62541_WIZNET_C}" OPEN62541_WIZNET_SOURCE)
set(OPEN62541_WIZNET_PATCHED_SOURCE "${OPEN62541_WIZNET_SOURCE}")
# Minimal NS0 startup path; see the patch file.
include(${CMAKE_CURRENT_SOURCE_DIR}/patches/minimal_ns0.cmake)

# Subscription KeepAlive/Lifetime capped by time. The limits are a policy, so
# they are cache options; the defaults suit a small server that must get its
# timers back from clients that vanished. See the patch file for the why.
set(OPCUA_SUB_MAX_KEEPALIVE_MS 40000 CACHE STRING
    "Longest subscription KeepAlive time, and the largest publishing interval (ms)")
set(OPCUA_SUB_MAX_LIFETIME_MS 120000 CACHE STRING
    "Longest a subscription outlives its last Publish request (ms); >= 3 x keepalive")
math(EXPR _wiz_ka3 "3 * ${OPCUA_SUB_MAX_KEEPALIVE_MS}")
if(OPCUA_SUB_MAX_LIFETIME_MS LESS _wiz_ka3)
    message(FATAL_ERROR "OPCUA_SUB_MAX_LIFETIME_MS must be at least 3 x OPCUA_SUB_MAX_KEEPALIVE_MS")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/patches/subscription_time_limits.cmake)
# Retransmission queue capped in encoded bytes per session (0 = no byte cap).
set(OPCUA_RETRANSMIT_MAX_BYTES 16384 CACHE STRING
    "Most encoded bytes one session keeps for Republish; oldest evicted first")
# Platform defaults. The RP2040 (264 KB SRAM) has about a third of the RP2350's
# free heap: four sessions at a 12 KB retransmission cap would hold 48 KB of it
# after a few link outages, and 240 items over four clients already left a
# 24 KB low-water mark (2026-11-02).
if(PICO_PLATFORM MATCHES "rp2040")
    set(_wiz_rt_heap_default 4096)
    set(_wiz_mi_default 200)
    set(_wiz_mi_reserve_default 32768)
else()
    set(_wiz_rt_heap_default 12288)
    set(_wiz_mi_default 400)
    set(_wiz_mi_reserve_default 49152)
endif()
set(OPCUA_RETRANSMIT_MAX_HEAP ${_wiz_rt_heap_default} CACHE STRING
    "Most heap (estimated) one session's Republish queue holds; oldest evicted first")
include(${CMAKE_CURRENT_SOURCE_DIR}/patches/retransmit_byte_cap.cmake)
# Server-wide monitored items. Also sizes the event loop's timer table: an item
# whose sampling interval differs from its publishing interval takes a timer.
set(OPCUA_MAX_MONITORED_ITEMS ${_wiz_mi_default} CACHE STRING
    "Most monitored items on the server; also sizes the event loop timer table")
# New monitored items refused below this free heap; see the patch file.
set(OPCUA_MI_HEAP_RESERVE ${_wiz_mi_reserve_default} CACHE STRING
    "Free heap below which CreateMonitoredItems answers BadOutOfMemory (0 = off)")
include(${CMAKE_CURRENT_SOURCE_DIR}/patches/mi_heap_reserve.cmake)

file(WRITE "${OPEN62541_WIZNET_PATCHED_C}"
     "${OPEN62541_WIZNET_PATCHED_SOURCE}")

add_library(OPEN62541_WIZNET STATIC
    ${OPEN62541_WIZNET_PATCHED_C}
    open62541_wiznet_eventloop.c
)

target_include_directories(OPEN62541_WIZNET PUBLIC
    ${OPEN62541_WIZNET_AMALGAMATION_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(OPEN62541_WIZNET PUBLIC
    UA_ARCHITECTURE_LWIP
    UA_LOGLEVEL=300
    # 이벤트루프 진단. 멀티소켓 브링업(2026-09-03)에 썼다.
    # 소켓 배정·수신 크기·연결 개폐를 [el] 접두어로 찍는다.
    # 문제 추적이 필요하면 1 로 올린다.
    WIZ_EL_DEBUG=0
    OPCUA_SUB_MAX_KEEPALIVE_MS=${OPCUA_SUB_MAX_KEEPALIVE_MS}
    OPCUA_SUB_MAX_LIFETIME_MS=${OPCUA_SUB_MAX_LIFETIME_MS}
    OPCUA_RETRANSMIT_MAX_BYTES=${OPCUA_RETRANSMIT_MAX_BYTES}
    OPCUA_RETRANSMIT_MAX_HEAP=${OPCUA_RETRANSMIT_MAX_HEAP}
    OPCUA_MAX_MONITORED_ITEMS=${OPCUA_MAX_MONITORED_ITEMS}u
    OPCUA_MI_HEAP_RESERVE=${OPCUA_MI_HEAP_RESERVE}
)

target_link_libraries(OPEN62541_WIZNET PUBLIC
    pico_stdlib
    ETHERNET_FILES
    IOLIBRARY_FILES
)

# With the policies compiled in, the amalgamation calls into mbedTLS directly.
#
# WIZ_ENTROPY_FILES is deliberately NOT linked here. It is an INTERFACE library
# that hands its source to whoever links it, so linking it both here and in the
# executable would compile wiz_entropy.c twice into the same image. The
# executable is the one that links it.
if(OPCUA_ENABLE_ENCRYPTION)
    target_link_libraries(OPEN62541_WIZNET PUBLIC
        mbedcrypto
        mbedx509
    )
endif()
