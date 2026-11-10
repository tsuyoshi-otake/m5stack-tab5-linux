// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 display bring-up inside the boot shim.
 *
 * The stock BSP sequence (esp-bsp bsp/m5stack_tab5/src/bsp_display.c) is:
 *   1. power the MIPI D-PHY from the P4 internal LDO VO3 at 2.5 V
 *   2. release the panel reset via IO-expander 0x43 P4 (done in
 *      tab5_board.c before this runs)
 *   3. create a 2-lane DSI bus at ~965 Mbps
 *   4. attach a DBI command channel and the ST7123 panel with the vendor
 *      init data (disp_init_data_st7123.h, carried next to the driver)
 *   5. DPI in RGB565, 720x1280, 70 MHz dot clock with the ST7123 timing
 *
 * ESP-IDF's DPI panel driver keeps streaming the framebuffer through
 * DW-GDMA once the stream starts.  Nothing here tears the stream down,
 * so the panel keeps refreshing from the same PSRAM buffer after the
 * shim jumps to Linux.  The kernel picks the buffer up through the
 * simple-framebuffer DTB node that main.c patches with the real address.
 *
 * Known limitation, deliberately left for the first hardware session:
 * whether IDF re-arms the GDMA link list per frame on the vsync ISR has
 * to be measured on the board; if it does, the picture freezes on the
 * last frame after the jump and the LLI tail must be re-pointed to form
 * a self-loop before handoff (tab5_display.c keeps the buffer content
 * correct either way, so the failure mode is a frozen banner, not a
 * crash).
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7123.h"
#include "disp_init_data_st7123.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "hal/cache_hal.h"
#include "tab5_display.h"

static const char *TAG = "tab5-disp";

/* PSRAM windows the shim owns after display init and must never give to
 * the DPI driver's heap framebuffer.  First-fit heap allocation hands
 * out the lowest free address, so reserving the kernel and DTB load
 * windows first pushes the framebuffer above them. */
#define KERNEL_LOAD_PA 0x48000000u
#define DTB_LOAD_PA 0x48800000u
#define RESERVE_BYTES (8u * 1024u * 1024u)

#define LCD_H_RES 720
#define LCD_V_RES 1280
#define LCD_DSI_LANES 2
/* BSP default lane rate; the 965 Mbps override is applied only on
 * ST7121 boards (bsp_display.c lane-bitrate select).  The measured unit
 * reports ST7123, so it takes this default. */
#define LCD_DSI_LANE_MBPS 1000
#define LCD_DPI_MHZ 70
#define LCD_DPHY_LDO_CHAN 3
#define LCD_DPHY_LDO_MV 2500

/* Static banner drawn before Linux is loaded.  The framebuffer is RGB565
 * little-endian; a solid fill keeps this independent of font assets. */
#define FB_STRIDE_PX (LCD_H_RES)
#define FB_BYTES ((uint32_t)LCD_H_RES * LCD_V_RES * 2u)

static void draw_splash(uint16_t *fb)
{
	/* Dark blue field with a green "M5 Tab5 Linux" band: 5 horizontal
	 * stripes at the top of the 720x1280 portrait panel. */
	for (uint32_t i = 0; i < LCD_H_RES * LCD_V_RES; ++i)
		fb[i] = 0x000fu;
	for (uint32_t y = 40; y < 120; ++y) {
		for (uint32_t x = 40; x < LCD_H_RES - 40; ++x)
			fb[y * FB_STRIDE_PX + x] = 0x07e0u;
	}
	for (uint32_t y = 140; y < 170; ++y) {
		for (uint32_t x = 40; x < LCD_H_RES - 40; ++x)
			fb[y * FB_STRIDE_PX + x] = 0xffffu;
	}
	cache_hal_writeback_addr((uint32_t)(uintptr_t)fb, FB_BYTES);
}

static bool fb_collides(uint32_t pa, uint32_t bytes)
{
	/* PSRAM regions the shim fills or Linux reserves; mirrors
	 * reserved-memory and the load addresses in main.c. */
	static const uint32_t regions[][2] = {
		{ 0x48000000u, 0x00800000u }, /* kernel Image window */
		{ 0x48800000u, 0x00800000u }, /* DTB window */
		{ 0x495af000u, 0x00400000u }, /* nommu pool low */
		{ 0x499bf000u, 0x00001000u }, /* boot update request */
		{ 0x499c0000u, 0x00040000u }, /* boot partition staging */
		{ 0x49a00000u, 0x00600000u }, /* nommu userspace pool */
	};
	for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
		uint32_t r0 = regions[i][0], r1 = r0 + regions[i][1];
		if (pa < r1 && pa + bytes > r0)
			return true;
	}
	return false;
}

void tab5_display_init(struct tab5_fb *out)
{
	/* Keep the kernel and DTB load windows out of the heap before the
	 * DPI driver grabs its framebuffer (see comment at the top). */
	void *resv_k = heap_caps_malloc(RESERVE_BYTES,
			MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
	void *resv_d = heap_caps_malloc(RESERVE_BYTES,
			MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
	if (resv_k != (void *)(uintptr_t)KERNEL_LOAD_PA ||
	    resv_d != (void *)(uintptr_t)DTB_LOAD_PA)
		ESP_LOGW(TAG, "load-window reservation landed at %p/%p, "
			 "expecting 0x%08x/0x%08x", resv_k, resv_d,
			 KERNEL_LOAD_PA, DTB_LOAD_PA);
	/* Never freed: the windows stay out of the heap for the rest of
	 * the shim's life, which is all that matters here. */

	esp_ldo_channel_handle_t ldo = NULL;
	esp_lcd_dsi_bus_handle_t dsi = NULL;
	esp_lcd_panel_io_handle_t io = NULL;
	esp_lcd_panel_handle_t panel = NULL;

	esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = LCD_DPHY_LDO_CHAN,
		.voltage_mv = LCD_DPHY_LDO_MV,
	};
	if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) {
		ESP_LOGW(TAG, "DPHY LDO acquire failed; display skipped");
		return;
	}

	esp_lcd_dsi_bus_config_t bus_cfg = {
		.bus_id = 0,
		.num_data_lanes = LCD_DSI_LANES,
		.phy_clk_src = 0, /* BSP: let the driver choose the default */
		.lane_bit_rate_mbps = LCD_DSI_LANE_MBPS,
	};
	if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi) != ESP_OK) {
		ESP_LOGW(TAG, "DSI bus failed; display skipped");
		return;
	}

	esp_lcd_dbi_io_config_t dbi_cfg = {
		.virtual_channel = 0,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
	};
	if (esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io) != ESP_OK) {
		ESP_LOGW(TAG, "DBI io failed; display skipped");
		return;
	}

	const esp_lcd_dpi_panel_config_t dpi_cfg = {
		.virtual_channel = 0,
		.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
		.dpi_clock_freq_mhz = LCD_DPI_MHZ,
		.in_color_format = LCD_COLOR_FMT_RGB565,
		.num_fbs = 1,
		.video_timing = {
			.h_size = LCD_H_RES,
			.v_size = LCD_V_RES,
			.hsync_back_porch = 40,
			.hsync_pulse_width = 2,
			.hsync_front_porch = 40,
			.vsync_back_porch = 8,
			.vsync_pulse_width = 2,
			.vsync_front_porch = 220,
		},
	};
	const st7123_vendor_config_t vendor_cfg = {
		.init_cmds = disp_init_data_st7123,
		.init_cmds_size = sizeof(disp_init_data_st7123) /
				  sizeof(disp_init_data_st7123[0]),
		.mipi_config = {
			.dsi_bus = dsi,
			.dpi_config = &dpi_cfg,
		},
	};
	esp_lcd_panel_dev_config_t panel_cfg = {
		.reset_gpio_num = GPIO_NUM_NC, /* released via expander 0x43 P4 */
		.flags = {
			.reset_active_high = 0,
		},
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
		.vendor_config = (void *)&vendor_cfg,
	};
	if (esp_lcd_new_panel_st7123(io, &panel_cfg, &panel) != ESP_OK) {
		ESP_LOGW(TAG, "ST7123 panel failed; display skipped");
		return;
	}
	if (esp_lcd_panel_reset(panel) != ESP_OK ||
	    esp_lcd_panel_init(panel) != ESP_OK ||
	    esp_lcd_panel_disp_on_off(panel, true) != ESP_OK) {
		ESP_LOGW(TAG, "ST7123 init sequence failed; display skipped");
		return;
	}

	void *fb = NULL;
	if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
		ESP_LOGW(TAG, "framebuffer query failed; display skipped");
		return;
	}
	if (fb_collides((uint32_t)(uintptr_t)fb, FB_BYTES)) {
		ESP_LOGE(TAG, "framebuffer @%p overlaps a Linux load region; "
			 "tearing down the panel and booting headless", fb);
		esp_lcd_panel_del(panel);
		return;
	}
	draw_splash((uint16_t *)fb);

	out->pa = (uint32_t)(uintptr_t)fb;
	out->bytes = FB_BYTES;
	out->ok = true;
	ESP_LOGI(TAG, "ST7123 up: fb @0x%08" PRIx32 " (%" PRIu32 " bytes), "
		 "stream left running for Linux simple-framebuffer",
		 out->pa, out->bytes);
}
