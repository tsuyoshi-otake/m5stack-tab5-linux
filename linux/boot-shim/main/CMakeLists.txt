# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only

# Tab5-only requirements stay empty for every other profile, so M1/M2/C68
# keep their exact link set.
set(tab5_requires "")
if("$ENV{BOOT_PROFILE}" STREQUAL "tab5")
    set(tab5_requires esp_lcd esp_lcd_st7123 driver)
endif()

idf_component_register(
    SRCS "main.c" "m2_sdmmc.c" "c68_secondary_entry.S"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${tab5_requires}
)

# Build identification (#22): the git revision is printed at boot.
if(DEFINED ENV{ES_GIT_REV} AND NOT "$ENV{ES_GIT_REV}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GIT_REV="$ENV{ES_GIT_REV}")
endif()

# Keep M1's proven boot path free of SDIO side effects.  The M2 and C68
# network builds opt in explicitly when they build this project.
if("$ENV{BOOT_PROFILE}" STREQUAL "m2" OR
   "$ENV{BOOT_PROFILE}" STREQUAL "c68")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M2_SDIO=1)
endif()
# Tab5 is a different board, not an M-profile: it carries its own SDIO
# route (GPIO8-13 + C6 reset GPIO15), two PI4IOE power expanders on the
# internal I2C bus, and the ST7123 MIPI-DSI panel.  BOARD_TAB5 selects
# tab5_board.c/tab5_display.c in main.c and drops the Stamp OTG and
# GPIO42/43-48 paths entirely.
if("$ENV{BOOT_PROFILE}" STREQUAL "tab5")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_TAB5=1 UP_BOOT=1)
    target_sources(${COMPONENT_LIB} PRIVATE "tab5_board.c" "tab5_display.c")
endif()
if("$ENV{C68_CLEAN_RELEASE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C68_CLEAN_RELEASE=1)
elseif("$ENV{BOOT_PROFILE}" STREQUAL "m1" OR
       "$ENV{BOOT_PROFILE}" STREQUAL "m2")
    # UP Linux has CONFIG_SMP=n; do not compile the historical CPU1 probe
    # whose C33 layout symbols exist only in the C68 experiment.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UP_BOOT=1)
endif()
if("$ENV{TSENS_ORACLE}" STREQUAL "1")
    target_sources(${COMPONENT_LIB} PRIVATE "tsens_oracle.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TSENS_ORACLE=1)
endif()
if("$ENV{CMD53_RETENTION_BB}" STREQUAL "1")
    target_sources(${COMPONENT_LIB} PRIVATE "cmd53_bb.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../cmd53-bb")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CMD53_RETENTION_BB=1)
    if("$ENV{CMD52_MARKER}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CMD52_MARKER=1)
    endif()
    if("$ENV{CMD53_BB_SELFTEST_TORN}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CMD53_BB_SELFTEST_TORN=1)
    elseif("$ENV{CMD53_BB_SELFTEST}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CMD53_BB_SELFTEST=1)
    endif()
endif()
# #56 Deep Sleep.  Added last so its RTC_NOINIT state links after rtc_c,
# whose LP RAM address the CMD53_RETENTION_BB kernel patches take from nm.
target_sources(${COMPONENT_LIB} PRIVATE "deep_sleep.c")
# #57: the LP core program that checks the temperature while asleep.
ulp_embed_binary(lp_monitor "lp/lp_monitor.c" "deep_sleep.c")
