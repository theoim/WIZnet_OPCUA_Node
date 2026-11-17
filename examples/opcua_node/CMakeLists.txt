set(TARGET_NAME opcua_node)

# 두 OPC UA 예제가 동시에 활성화되면 같은 바이너리 디렉터리를 두 번
# 쓰게 되어 CMake 가 중단된다. 먼저 처리된 쪽이 타깃을 만들므로 가드한다.
if(NOT TARGET OPEN62541_WIZNET)
    add_subdirectory(${CMAKE_SOURCE_DIR}/port/open62541
        ${CMAKE_BINARY_DIR}/port/open62541)
endif()

add_executable(${TARGET_NAME}
    src/main.c
    src/opcua_server.c
    src/opcua_tcp_probe.c
    src/wiznet_network.c
    src/usb_stdio_link.c
    src/data_table.c
    src/opcua_settings.c
    src/opcua_security.c
    src/sntp_client.c
    src/reset_cause.c
    src/uart_link.c
)

# The certificates are generated, not committed: they hold private keys.
# tools/make_opcua_certs.sh writes src/opcua_certs.c and inc/opcua_certs.h, and
# both are git-ignored. Only the encryption build needs them.
if(OPCUA_ENABLE_ENCRYPTION)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/opcua_certs.c")
        message(FATAL_ERROR
            "opcua_certs.c is missing. Run tools/make_opcua_certs.sh first - "
            "the certificates are generated rather than committed because they "
            "contain private keys.")
    endif()
    target_sources(${TARGET_NAME} PRIVATE src/opcua_certs.c)

    # mbedtls_hardware_poll() has to be in this image. WIZ_ENTROPY_FILES is an
    # INTERFACE library, so linking it compiles wiz_entropy.c into this
    # executable rather than leaving the symbol in an archive the linker has
    # already walked past. It is linked here and nowhere else in this image.
    target_link_libraries(${TARGET_NAME} WIZ_ENTROPY_FILES)
endif()

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

# Test image: nodes of each DataType for tools/opcua_heap_datatypes.py.
option(OPCUA_DT_TEST_NODES "Add Test.* nodes of each DataType" OFF)
if(OPCUA_DT_TEST_NODES)
    target_compile_definitions(${TARGET_NAME} PRIVATE OPCUA_DT_TEST_NODES=1)
endif()

# Handshake timing markers - a measurement build, off by default.
# GP2 is high while an asymmetric OPN is delivered (the loop stall a new client
# or a renewal causes), GP3 during each private-key operation, and the console
# prints one "[hs]" line per OPN. With encryption the mbedTLS entry points that
# open62541 calls are wrapped to time the private and public key work.
# tools/opcua_handshake_impact.py reads the console lines.
option(OPCUA_HS_MARKERS "GPIO + console markers around OPN handshakes" OFF)
if(OPCUA_HS_MARKERS)
    target_sources(${TARGET_NAME} PRIVATE src/hs_marker.c)
    target_compile_definitions(${TARGET_NAME} PRIVATE OPCUA_HS_MARKERS=1)
    if(OPCUA_ENABLE_ENCRYPTION)
        target_compile_definitions(${TARGET_NAME} PRIVATE OPCUA_HS_WRAP_CRYPTO=1)
        target_link_options(${TARGET_NAME} PRIVATE
            -Wl,--wrap=mbedtls_pk_sign
            -Wl,--wrap=mbedtls_pk_decrypt
            -Wl,--wrap=mbedtls_rsa_rsaes_oaep_decrypt
            -Wl,--wrap=mbedtls_ecdh_compute_shared
            -Wl,--wrap=mbedtls_pk_verify
            -Wl,--wrap=mbedtls_rsa_rsaes_oaep_encrypt
        )
    endif()
endif()

# Stamp the image so the serial log says which build it is. Inferring that from
# behaviour has already cost one wrong conclusion.
execute_process(
        COMMAND git describe --always --dirty --abbrev=7
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE OPCUA_BUILD_GIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        )
if(NOT OPCUA_BUILD_GIT)
    set(OPCUA_BUILD_GIT "no-git")
endif()
string(TIMESTAMP OPCUA_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

target_compile_definitions(${TARGET_NAME} PRIVATE
        OPCUA_BUILD_GIT="${OPCUA_BUILD_GIT}"
        OPCUA_BUILD_UTC="${OPCUA_BUILD_UTC}"
        )

target_link_libraries(${TARGET_NAME}
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_flash
    hardware_sync
    hardware_watchdog
    ETHERNET_FILES
    IOLIBRARY_FILES
    PORT_COMMON_FILES
    TIMER_FILES
    OPEN62541_WIZNET
)

# memory_budget.md 의 채널 스윕(3/8/16/32)을 소스 수정 없이 재빌드하기 위한 값이다.
# 플래시에 저장된 설정이 없을 때 사용하는 기본 채널 수를 덮어쓴다.
set(OPCUA_DEFAULT_CHANNEL_COUNT 3 CACHE STRING
    "Default channel count when flash holds no settings blob (RAM sweep)")

# Non-zero: last octet of IP and MAC, for a second test board on the same LAN.
set(OPCUA_NET_TEST_HOST 0 CACHE STRING
    "Last IP and MAC octet of a second test board (0 = keep the defaults)")

# Test images only: bytes taken from the heap at boot, so the out-of-memory
# paths can be reached with the load one PC can generate.
set(OPCUA_HEAP_BALLAST 0 CACHE STRING
    "Heap held back at boot for out-of-memory tests (0 = none)")

# PICO_MALLOC_PANIC=0: a failed malloc returns NULL instead of halting the
# board. open62541 checks every allocation and answers BadOutOfMemory, so
# one oversized request fails alone instead of resetting every client.
target_compile_definitions(${TARGET_NAME} PRIVATE
    OPCUA_ENABLE=1
    OPCUA_TCP_PROBE_ENABLE=0
    OPCUA_DEFAULT_CHANNEL_COUNT=${OPCUA_DEFAULT_CHANNEL_COUNT}u
    OPCUA_NET_TEST_HOST=${OPCUA_NET_TEST_HOST}
    OPCUA_HEAP_BALLAST=${OPCUA_HEAP_BALLAST}u
    PICO_MALLOC_PANIC=0
)

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
