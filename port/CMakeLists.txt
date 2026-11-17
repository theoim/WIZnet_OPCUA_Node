# ioLibrary_Driver
add_library(IOLIBRARY_FILES STATIC)

target_sources(IOLIBRARY_FILES PUBLIC
        ${PORT_DIR}/ioLibrary_Driver/src/wizchip_spi.c
        ${PORT_DIR}/ioLibrary_Driver/src/wizchip_gpio_irq.c
        )

if(${BOARD_NAME} STREQUAL W55RP20_EVB_PICO OR ${WIZNET_CHIP} STREQUAL W6300)
pico_generate_pio_header(IOLIBRARY_FILES ${PORT_DIR}/ioLibrary_Driver/src/wizchip_qspi_pio.pio)

target_sources(IOLIBRARY_FILES PUBLIC 
        ${PORT_DIR}/ioLibrary_Driver/src/wizchip_qspi_pio.c
)

target_include_directories(IOLIBRARY_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${PORT_DIR}/ioLibrary_Driver/inc
        ${PORT_DIR}
)
endif()

target_include_directories(IOLIBRARY_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${PORT_DIR}/ioLibrary_Driver/inc
        ${PORT_DIR}
        )

target_link_libraries(IOLIBRARY_FILES PUBLIC
        pico_stdlib
        hardware_pio
        hardware_spi
        hardware_dma
        hardware_clocks
        )

# timer
add_library(TIMER_FILES STATIC)

target_sources(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer/timer.c
        )

target_include_directories(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer
        )

target_link_libraries(TIMER_FILES PRIVATE
        pico_stdlib      
        )


# port_common
add_library(PORT_COMMON_FILES STATIC)

target_include_directories(PORT_COMMON_FILES PUBLIC
${PORT_DIR}
)

target_link_libraries(PORT_COMMON_FILES PUBLIC
pico_stdlib
hardware_clocks
hardware_spi
hardware_dma
hardware_pio
)

# ── WIZ_ENTROPY_FILES: mbedTLS 엔트로피 소스 ─────────────────────
#
# wiz_claw_tls.c 안에 file-static 으로 들어 있던 콜백을 공용으로 뽑은 것이다.
# OPC UA 서버가 cJSON 과 wiz_claw 전체를 끌어오지 않고 쓰려면 분리가 필요했다.
# 하드웨어 RNG 가 없는 부품에서는 wiz_entropy.h 가 빌드를 멈춘다 —
# 쓰는 쪽이 WIZ_ENTROPY_ALLOW_NO_TRNG 로 의식적으로 허용해야 한다.

# INTERFACE 라이브러리인 이유.
#
# mbedcrypto 가 mbedtls_hardware_poll() 을 필요로 하는데 이 코드는 mbedcrypto 의
# 헤더가 필요하다. 의존이 양방향이라 정적 라이브러리로는 링커가 아카이브를 훑는
# 순서에 걸리고, OBJECT 로 만들면 pico_stdlib 같은 SDK INTERFACE 라이브러리가
# 자기 소스를 이 타깃에도 컴파일해 넣어 실행 파일과 중복 정의가 난다.
#
# INTERFACE 는 아무것도 컴파일하지 않고 소비자에게 소스를 넘긴다. 결과적으로
# wiz_entropy.c 가 소비자 안에서 컴파일되므로 심볼은 항상 최종 링크에 있다.
# 단, 한 실행 파일에 두 경로로 들어가면 또 중복이므로 소비자는 하나만 링크한다.
add_library(WIZ_ENTROPY_FILES INTERFACE)

target_sources(WIZ_ENTROPY_FILES INTERFACE
    ${PORT_DIR}/mbedtls/src/wiz_entropy.c
)

target_include_directories(WIZ_ENTROPY_FILES INTERFACE
    ${PORT_DIR}/mbedtls/inc
)

target_link_libraries(WIZ_ENTROPY_FILES INTERFACE
    pico_stdlib
    pico_rand
    mbedcrypto
)
