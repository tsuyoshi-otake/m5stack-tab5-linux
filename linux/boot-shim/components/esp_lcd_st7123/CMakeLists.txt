# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#
# Vendored from the M5Stack Tab5 UserDemo (Apache-2.0, Espressif).  Only the
# ST7123 panel driver and the BSP's ST7123 vendor init data are carried; the
# ST7121 and ILI9881C paths of the stock BSP are not needed for the measured
# unit, whose stock log reports ST7123 touch firmware 1.80.1.16.
#
# The component compiles into nothing unless the shim is built for Tab5, so
# the proven M1/M2/C68 shim images keep their exact dependency set.
if("$ENV{BOOT_PROFILE}" STREQUAL "tab5")
    idf_component_register(
        SRCS "esp_lcd_st7123.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_lcd driver
    )
else()
    idf_component_register()
endif()
