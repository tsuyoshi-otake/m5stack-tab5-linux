// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 board bootstrap, called by the boot shim only when the shim
 * is built with ES_BOARD=tab5 (CMake: BOARD_TAB5).  See
 * board-contract.json for the hardware sources behind every GPIO and
 * expander bit used here.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Framebuffer handoff for patch_display_window() in main.c. */
struct tab5_fb {
	uint32_t pa;
	uint32_t bytes;
	bool ok;
};

/*
 * Program the two PI4IOE5V6408 expanders to a safe boot state
 * (speaker/touch/camera/USB off, C6 rail on), route SDMMC slot 1 through
 * the GPIO matrix onto Tab5's CLK=12 CMD=13 D0-3={11,10,9,8}, pulse C6
 * reset on GPIO15, and keep the backlight off.  Never returns on a
 * contract violation that would risk the board; logs and continues past
 * non-fatal failures (e.g. display) so the serial recovery path stays.
 */
void tab5_board_init(struct tab5_fb *fb);

/* #56 deep-sleep hook: clear expander 0x44 P0 (the C6 rail) over the
 * internal I2C bus; the Tab5 equivalent of the Stamp GPIO42 latch. */
void tab5_c6_power_off(void);
