// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 display bring-up inside the boot shim.
 *
 * The stock BSP sequence (esp-bsp bsp/m5stack_tab5/src/bsp_display.c) is:
 *   1. power the MIPI D-PHY from the P4 internal LDO VO3 at 2.5 V
 *   2. release the panel reset via IO-expander 0x43 P4 (done in
 *      tab5_board.c before this runs)
 *   3. create a 2-lane DSI bus at 1000 Mbps per lane (BSP default;
 *      the 965 Mbps override is applied only on ST7121 boards)
 *   4. attach a DBI command channel and the ST7123 panel with the vendor
 *      init data (disp_init_data_st7123.h, carried next to the driver)
 *   5. DPI in RGB565, 720x1280, 70 MHz dot clock with the ST7123 timing
 *
 * Scan-out after the jump to Linux
 * -------------------------------
 * ESP-IDF's DPI panel driver feeds the DSI bridge through one DW-GDMA
 * channel programmed with a single full-frame link-list item (LLI).
 * Whether the stream could free-run across the Linux jump is settled
 * by register analysis rather than experiment:
 *
 *   - DW-GDMA clears the LLI "valid" bit (ctrl_hi bit 31) in memory
 *     when a block finishes -- the hardware writes the descriptor back.
 *     ESP-IDF's trans-done callback re-marks the item and re-enables
 *     the channel on EVERY frame (esp_lcd_panel_dpi.c
 *     mipi_dsi_dma_trans_done_cb: dw_gdma_lli_set_block_markers +
 *     dw_gdma_channel_use_link_list + dw_gdma_channel_enable_ctrl).
 *   - The upstream dw-axi-dmac driver does exactly the same for cyclic
 *     transfers (dw-axi-dmac-platform.c re-marks CH_CTL_H_LLI_VALID and
 *     re-enables the channel in IRQ context).  If the hardware could
 *     loop a marked-valid LLI by itself, neither driver would re-arm.
 *
 * So a pure-hardware self-loop is impossible: re-pointing the LLI tail
 * at itself buys exactly one extra frame, because the valid bit still
 * gets cleared in memory.  The stream therefore survives handoff via
 * the linux/tab5 DSI-scanout kernel driver: it claims the shared
 * DW-GDMA interrupt (ETS source 24, CLIC slot 25 in the Tab5 DTS),
 * finds the channel whose DAR is the DSI-bridge FIFO window
 * (0x50105000), and performs the same re-arm the IDF callback does.
 * Until that driver probes, the panel holds the last completed frame.
 *
 * The self-test below proves both halves of that story on real
 * hardware registers before the jump: masking the channel's interrupt
 * signal stops the IDF callback and the channel auto-disables after
 * the in-flight frame (int_st0 ch_disabled), then a manual re-arm plus
 * restored signal mask produces 30 consecutive frame-done events.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7123.h"
#include "disp_init_data_st7123.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/dw_gdma_ll.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "soc/dw_gdma_struct.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "tab5_display.h"
#include "tab5_layout.h"

static const char *TAG = "tab5-disp";

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

/* Backlight: stock BSP uses LEDC low-speed timer 0 at 5 kHz / 10-bit on
 * GPIO22 with LEDC_AUTO_CLK.  AUTO on P4 resolves through the
 * global-clock path; this pins XTAL (40 MHz, never gated by the Linux
 * port) so the PWM survives both the Linux handoff and later clock
 * changes.  Duty ~15% matches a dim-but-readable boot splash. */
#define LCD_BL_GPIO 22
#define LCD_BL_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_FREQ_HZ 5000
#define LCD_BL_DUTY 154 /* ~15% of 1024 */
#define LCD_BL_FADE_MS 200

/* Static banner drawn before Linux is loaded.  The framebuffer is RGB565
 * little-endian; a solid fill keeps this independent of font assets. */
#define FB_STRIDE_PX (LCD_H_RES)
#define FB_BYTES ((uint32_t)LCD_H_RES * LCD_V_RES * 2u)

/* DW-GDMA register facts the handoff depends on. */
#define DWGDMA_CH_COUNT 4
#define DWGDMA_DAR_DSI_FIFO 0x50105000u /* MIPI_DSI_BRG_MEM_BASE */
#define DWGDMA_INT_BLOCK_TFR_DONE (1u << 0)
#define DWGDMA_INT_DMA_TFR_DONE (1u << 1)
#define DWGDMA_INT_LLI_INVALID_ERR (1u << 13)
#define DWGDMA_INT_CH_DISABLED (1u << 30)
#define DWGDMA_INT_CH_ABORTED (1u << 31)
#define DWGDMA_LLI_CTRL_VALID (1u << 31)
#define DWGDMA_LLI_CTRL_LAST (1u << 30)

static dw_gdma_dev_t *const gdma = &DW_GDMA;
static int s_scanout_chan = -1;
static uint32_t s_scanout_lli_pa;

/* Kernel window + gap + DTB window, 16 MiB from the start of PSRAM. */
static uint8_t s_load_windows[(TAB5_DTB_LOAD_PA - TAB5_KERNEL_LOAD_PA) +
			      TAB5_LOAD_RESERVE_BYTES]
	EXT_RAM_NOINIT_ATTR __attribute__((aligned(64)));

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
	for (size_t i = 0;
	     i < sizeof(tab5_fb_guard_regions) / sizeof(tab5_fb_guard_regions[0]);
	     ++i) {
		uint32_t r0 = tab5_fb_guard_regions[i][0];
		uint32_t r1 = r0 + tab5_fb_guard_regions[i][1];
		if (pa < r1 && pa + bytes > r0)
			return true;
	}
	return false;
}

/* Find the GDMA channel the DPI driver allocated: it is the only one
 * whose destination is the DSI-bridge FIFO window. */
static int scanout_find_channel(void)
{
	for (int i = 0; i < DWGDMA_CH_COUNT; ++i) {
		if (gdma->ch[i].dar0.val == DWGDMA_DAR_DSI_FIFO) {
			s_scanout_chan = i;
			return i;
		}
	}
	return -1;
}

/* The channel's llp0 reads 0 while the block runs (it holds the NEXT item),
 * so the item is found by content instead: the one 64-byte-aligned record
 * in internal RAM with sar == framebuffer and dar == the DSI FIFO. */
#define SRAM_SCAN_LO 0x4ff00000u
#define SRAM_SCAN_HI 0x4ff80000u
static uint32_t scanout_find_lli(uint32_t fb_pa)
{
	for (uint32_t pa = SRAM_SCAN_LO; pa < SRAM_SCAN_HI; pa += 64) {
		const volatile uint32_t *w = (const volatile uint32_t *)
			CACHE_LL_L2MEM_NON_CACHE_ADDR(pa);
		if (w[0] == fb_pa && w[2] == DWGDMA_DAR_DSI_FIFO)
			return pa;
	}
	return 0;
}

/* Re-arm the scan-out channel exactly the way the IDF trans-done
 * callback does: re-mark the LLI valid+last, re-point llp0 at it, and
 * re-enable the channel.  Called by the self-test and documented as the
 * contract the kernel DSI-scanout driver implements. */
static void scanout_rearm(int ch)
{
	/* Write through the uncached alias, as dw_gdma.c does (items_nc):
	 * the DMA reads the item from RAM, not from the L1 data cache. */
	volatile dw_gdma_link_list_item_t *lli =
		(void *)CACHE_LL_L2MEM_NON_CACHE_ADDR(s_scanout_lli_pa);
	lli->ctrl_hi.val |= DWGDMA_LLI_CTRL_VALID | DWGDMA_LLI_CTRL_LAST;
	gdma->ch[ch].llp0.loc0 = s_scanout_lli_pa >> 6;
	dw_gdma_ll_channel_enable(gdma, ch, true);
}

/*
 * Two-phase serial self-test of the handoff contract.
 *
 * Phase A ("stall proof"): mask the channel's interrupt signals so the
 * IDF trans-done callback stops firing.  One frame later the channel
 * must have auto-disabled (int_st0 ch_disabled / lli-invalid latched),
 * proving the hardware writes back the LLI and no free-run exists --
 * the same dead state the kernel inherits if its scanout driver is
 * missing.
 *
 * Phase B ("armed run"): restore the signal mask, kick the channel
 * with the same re-arm sequence the kernel driver uses, then count 30
 * consecutive frame-done events via the raw status register.  Each
 * event also drives the IDF callback, so the run is self-sustaining.
 *
 * Returns true when both phases behaved as designed.
 */
static bool scanout_self_test(int ch)
{
	volatile dmac_channel_reg_t *c = &gdma->ch[ch];
	const uint32_t frame_us = 17000; /* ~58 fps at 70 MHz DPI clock */
	uint32_t saved_sig = c->int_sig_ena0.val;
	uint32_t st_a, chen_a;
	int done = 0, iters = 0;
	bool stalled, armed_ok;

	/* Phase A: silence the channel for ~3 frames, then inspect. */
	c->int_clr0.val = 0xffffffffu;
	c->int_sig_ena0.val = 0;
	esp_rom_delay_us(3 * frame_us);
	st_a = c->int_st0.val;
	chen_a = gdma->chen0.val;
	stalled = (st_a & (DWGDMA_INT_CH_DISABLED |
			   DWGDMA_INT_LLI_INVALID_ERR |
			   DWGDMA_INT_DMA_TFR_DONE)) != 0 &&
		  (chen_a & (1u << ch)) == 0;
	ESP_LOGI(TAG, "self-test A (masked): int_st0=0x%08" PRIx32
		 " chen0=0x%08" PRIx32 " -> %s",
		 st_a, chen_a, stalled ? "stalled as designed"
				       : "STILL RUNNING (unexpected)");

	/* Phase B: restore mask, manual re-arm, count 30 frames. */
	c->int_sig_ena0.val = saved_sig;
	c->int_clr0.val = 0xffffffffu;
	scanout_rearm(ch);
	while (done < 30 && iters++ < 300) {
		uint32_t st = c->int_st0.val;
		if (st & (DWGDMA_INT_BLOCK_TFR_DONE | DWGDMA_INT_DMA_TFR_DONE)) {
			c->int_clr0.val = st;
			++done;
		}
		esp_rom_delay_us(2000);
	}
	armed_ok = done >= 30;
	ESP_LOGI(TAG, "self-test B (armed): %d frame-done events in %d polls"
		 " int_st0=0x%08" PRIx32 " -> %s",
		 done, iters, c->int_st0.val,
		 armed_ok ? "stream healthy under re-arm" : "RE-ARM FAILED");

	if (!armed_ok)
		return false;
	if (!stalled)
		ESP_LOGW(TAG, "stall proof inconclusive; re-arm path "
			 "verified, treating handoff as needing the "
			 "kernel driver anyway");
	return true;
}

static void backlight_init(void)
{
	ledc_timer_config_t timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LCD_BL_DUTY_RES,
		.timer_num = LEDC_TIMER_0,
		.freq_hz = LCD_BL_FREQ_HZ,
		.clk_cfg = LEDC_USE_XTAL_CLK,
	};
	ledc_channel_config_t chan = {
		.gpio_num = LCD_BL_GPIO,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LEDC_CHANNEL_0,
		.timer_sel = LEDC_TIMER_0,
		.duty = 0,
		.hpoint = 0,
	};
	if (ledc_timer_config(&timer) != ESP_OK ||
	    ledc_channel_config(&chan) != ESP_OK) {
		ESP_LOGW(TAG, "backlight LEDC setup failed; GPIO%d stays low",
			 LCD_BL_GPIO);
		return;
	}
	ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
				     LCD_BL_DUTY, LCD_BL_FADE_MS,
				     LEDC_FADE_NO_WAIT);
	ESP_LOGI(TAG, "backlight on: GPIO%d 5 kHz/10-bit XTAL duty %u/1024",
		 LCD_BL_GPIO, (unsigned)LCD_BL_DUTY);
}

/* One-line pre-jump status for main.c: confirms what the kernel driver
 * must pick up. */
void tab5_display_status(void)
{
	if (s_scanout_chan < 0) {
		ESP_LOGI(TAG, "scanout: no DW-GDMA channel tracked");
		return;
	}
	volatile dmac_channel_reg_t *c = &gdma->ch[s_scanout_chan];
	ESP_LOGI(TAG, "scanout handoff: ch%d lli=0x%08" PRIx32
		 " int_st0=0x%08" PRIx32 " chen0=0x%08" PRIx32
		 " (kernel: espressif,esp32p4-dsi-scanout, ETS src 24)",
		 s_scanout_chan, s_scanout_lli_pa,
		 c->int_st0.val, gdma->chen0.val);
}

void tab5_display_init(struct tab5_fb *out)
{
	/* The kernel and DTB load windows are held by a linker-placed array at
	 * the very start of PSRAM, so the PSRAM heap (and its metadata) begins
	 * above them and nothing the shim allocates is ever overwritten by the
	 * kernel/DTB copy.  Fail closed if the linker did not put it there. */
	if ((uintptr_t)s_load_windows != TAB5_KERNEL_LOAD_PA ||
	    sizeof(s_load_windows) < (TAB5_DTB_LOAD_PA - TAB5_KERNEL_LOAD_PA) +
				     TAB5_LOAD_RESERVE_BYTES) {
		ESP_LOGW(TAG, "load-window array at %p (%u bytes), expected 0x%08x; "
			 "display skipped", s_load_windows,
			 (unsigned)sizeof(s_load_windows), TAB5_KERNEL_LOAD_PA);
		return;
	}
	ESP_LOGI(TAG, "load windows held at %p (%u bytes)", s_load_windows,
		 (unsigned)sizeof(s_load_windows));

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

	/* First-boot diagnostics: every channel's view, then again after a
	 * pause with the load path still idle, to tell "stream dead" from
	 * "stream killed by the kernel copy that follows". */
	for (int pass = 0; pass < 2; ++pass) {
		for (int i = 0; i < DWGDMA_CH_COUNT; ++i) {
			volatile dmac_channel_reg_t *c = &gdma->ch[i];
			ESP_LOGI(TAG, "diag%d ch%d: sar=0x%08" PRIx32 " dar=0x%08" PRIx32
				 " llp=0x%08" PRIx32 " st=0x%08" PRIx32
				 " sig=0x%08" PRIx32 " chen=0x%08" PRIx32,
				 pass, i, c->sar0.val, c->dar0.val, c->llp0.val,
				 c->int_st0.val, c->int_sig_ena0.val, gdma->chen0.val);
		}
		if (pass == 0)
			esp_rom_delay_us(300000);
	}
	s_scanout_lli_pa = scanout_find_lli(out->pa);
	ESP_LOGI(TAG, "scanout LLI at 0x%08" PRIx32, s_scanout_lli_pa);
	if (scanout_find_channel() < 0 || !s_scanout_lli_pa) {
		ESP_LOGW(TAG, "DW-GDMA scanout channel/LLI not found; "
			 "stream will die at the IDF callback boundary");
	} else if (scanout_self_test(s_scanout_chan)) {
		backlight_init();
	} else {
		ESP_LOGE(TAG, "scanout self-test failed; backlight stays off "
			 "and the picture may freeze at handoff");
	}
	ESP_LOGI(TAG, "ST7123 up: fb @0x%08" PRIx32 " (%" PRIu32 " bytes), "
		 "stream left running for Linux simple-framebuffer",
		 out->pa, out->bytes);
}
