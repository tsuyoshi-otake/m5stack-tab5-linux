// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#pragma once

#include "tab5_board.h"

/*
 * Bring up the MIPI-DSI ST7123 panel exactly as the stock BSP does
 * (LDO_VO3 2.5 V, 2 lanes at 1000 Mbps, DPI RGB565 720x1280 @ 70 MHz,
 * ST7123 vendor init data), fill the PSRAM framebuffer with a boot
 * banner, run the DW-GDMA scanout handoff self-test, enable the LEDC
 * backlight, and leave the DPI/GDMA stream running across the Linux
 * jump.
 *
 * On success *fb carries the framebuffer physical address and size so
 * main.c can patch the DTB simple-framebuffer and reserved-memory nodes.
 * On any failure *fb->ok stays false and Linux boots headless.
 */
void tab5_display_init(struct tab5_fb *fb);

/* Serial status line for main.c to emit immediately before the jump:
 * GDMA channel id, LLI address and live state the kernel DSI-scanout
 * driver must pick up. */
void tab5_display_status(void);
