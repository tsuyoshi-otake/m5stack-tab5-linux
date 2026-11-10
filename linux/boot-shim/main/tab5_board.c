// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 board bootstrap for the Linux boot shim (BOOT_PROFILE=tab5).
 *
 * Replaces the Stamp-AddOn path (m2_sdmmc.c): Tab5 wires the on-board
 * ESP32-C6 to SDMMC slot 1 through the GPIO matrix on
 *   CLK=GPIO12 CMD=GPIO13 DAT0..3=GPIO{11,10,9,8}
 * and C6 reset/EN to GPIO15 (bank 0).  C6 power comes from IO-expander
 * 0x44 P0 (WLAN_PWR_EN), not from an always-on rail, so the expander must
 * be programmed before the reset pulse.
 *
 * Safety contract (board-contract.json): the speaker amp (0x43 P1),
 * touch (P5), camera (P6), USB-A 5 V (0x44 P3) and EXT5V (0x43 P2) stay
 * OFF at boot.  The stock image repeatedly tripped the brownout detector
 * on plain USB power with speaker + backlight + C6 all up
 * (live-log.txt BOD resets), so Linux keeps the same conservative state.
 * The LCD reset line (0x43 P4) is released via the BSP's open-drain /
 * input-pull-up sequence so the ST7123 can be initialised; the
 * backlight (GPIO22) is driven low until Linux or a user asks for it.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "tab5_board.h"
#include "tab5_display.h"

static const char *TAG = "tab5-board";

#define HP_SYS_CLKRST_BASE 0x500e6000u
#define HP_SOC_CLK_CTRL1 (HP_SYS_CLKRST_BASE + 0x18u)
#define HP_PERI_CLK_CTRL01 (HP_SYS_CLKRST_BASE + 0x34u)
#define HP_PERI_CLK_CTRL02 (HP_SYS_CLKRST_BASE + 0x38u)
#define LP_CLKRST_BASE 0x50111000u
#define LP_SDMMC_RESET (LP_CLKRST_BASE + 0x4cu)

#define GPIO_BASE 0x500e0000u
#define GPIO_OUT_W1TS (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC (GPIO_BASE + 0x0cu)
#define GPIO_ENABLE_W1TS (GPIO_BASE + 0x24u) /* bank 0; bank 1 is +0x30 as in m2_sdmmc.c */
#define GPIO_FUNC_IN_SEL_BASE (GPIO_BASE + 0x158u)
#define GPIO_FUNC_OUT_SEL_BASE (GPIO_BASE + 0x558u)
#define IO_MUX_BASE 0x500e1000u

#define SDMMC_SYS_CLK_EN (1u << 14)
#define SDIO_HS_MODE (1u << 22)
#define SDIO_LS_CLK_SRC_SEL (1u << 23)
#define SDIO_LS_CLK_EN (1u << 24)
#define SDIO_EDGE_CFG_UPDATE (1u << 8)
#define SDIO_EDGE_L_SHIFT 9
#define SDIO_EDGE_H_SHIFT 13
#define SDIO_EDGE_N_SHIFT 17
#define SDIO_SLF_EDGE_SEL_SHIFT 21
#define SDIO_DRV_EDGE_SEL_SHIFT 23
#define SDIO_SAM_EDGE_SEL_SHIFT 25
#define SDIO_SLF_CLK_EN (1u << 27)
#define SDIO_DRV_CLK_EN (1u << 28)
#define SDIO_SAM_CLK_EN (1u << 29)
#define SDMMC_RESET_EN (1u << 28)

/* Same board-proven readiness window as the Stamp bootstrap. */
#define C6_RESET_ASSERT_US 10000u
#define C6_RESET_INACTIVE_US 10000u
#define C6_RESET_READY_DELAY_US 1500000u
#define C6_POWER_UP_DELAY_US 20000u

#define IOMUX_FUN_PD (1u << 7)
#define IOMUX_FUN_PU (1u << 8)
#define IOMUX_FUN_IE (1u << 9)
#define IOMUX_FUN_DRV_MASK (3u << 10)
#define IOMUX_MCU_SEL_MASK (7u << 12)
#define PIN_FUNC_GPIO 1u

/* Internal I2C bus: both PI4IOE5V6408 expanders, the codec, mic ADC,
 * RTC, touch and the power monitor share it (board-contract.json). */
#define TAB5_I2C_PORT 0
#define TAB5_I2C_SDA_GPIO 31
#define TAB5_I2C_SCL_GPIO 32
#define TAB5_I2C_HZ 400000u

/* PI4IOE5V6408 registers (esp_io_expander_pi4ioe5v6408.c). */
#define PI4IO_REG_IO_DIR 0x03u /* 1=input (default 0xff) */
#define PI4IO_REG_OUT_SET 0x05u
#define PI4IO_REG_OUT_H_IM 0x07u /* 1=open-drain/high-Z (default 0xff) */
#define PI4IO_REG_PULL_EN 0x0Bu
#define PI4IO_REG_PULL_SEL 0x0Du /* 1=pull-up */

#define EXPANDER0_ADDR 0x43u
#define EXPANDER1_ADDR 0x44u

/* 0x43 pins: P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST, P5 TP_RST, P6 CAM_RST.
 * 0x44 pins: P0 WLAN_PWR_EN, P3 USB5V_EN, P4 PWROFF(keep 0), P5 nCHG_QC_EN,
 * P7 CHG_EN (stock parity per board-contract.json). */
#define EXP0_OUT_OFF 0x00u
#define EXP1_OUT_BOOT 0x89u /* P0|P3|P7 = WLAN_PWR,USB5V,CHG_EN; QC_EN=0 */

static inline volatile uint32_t *reg32(uint32_t addr)
{
	return (volatile uint32_t *)(uintptr_t)addr;
}

static inline void set_bits(uint32_t addr, uint32_t mask)
{
	*reg32(addr) |= mask;
}

static inline void clear_set_bits(uint32_t addr, uint32_t clear, uint32_t set)
{
	uint32_t value = *reg32(addr);
	*reg32(addr) = (value & ~clear) | set;
}

/* GPIO0..25 sit at gpio*4 in the P4 IO_MUX map; GPIO26 moves to 0x6c
 * (the gap at 0x68 that m2_sdmmc.c documents for the Stamp pins 42..48).
 * All Tab5 SDIO pins are <= 15, so the plain gpio*4 mapping applies. */
static void configure_iomux_gpio(uint32_t gpio, bool pullup)
{
	uint32_t clear = IOMUX_FUN_PD | IOMUX_FUN_PU | IOMUX_FUN_IE |
			 IOMUX_FUN_DRV_MASK | IOMUX_MCU_SEL_MASK;
	uint32_t set = IOMUX_FUN_IE | (3u << 10) | (PIN_FUNC_GPIO << 12);
	if (pullup)
		set |= IOMUX_FUN_PU;
	clear_set_bits(IO_MUX_BASE + (gpio * 4u), clear, set);
}

static void route_output(uint32_t gpio, uint32_t signal)
{
	/* OEN_SEL=0 delegates output-enable to the SDMMC peripheral. */
	clear_set_bits(GPIO_FUNC_OUT_SEL_BASE + (gpio * 4u),
			       0x1ffu | (1u << 10) | (1u << 11), signal & 0x1ffu);
}

static void route_input(uint32_t signal, uint32_t gpio)
{
	/* SIG_IN_SEL=1 selects the GPIO matrix rather than the direct bypass. */
	*reg32(GPIO_FUNC_IN_SEL_BASE + (signal * 4u)) = (gpio & 0x3fu) | (1u << 7);
}

static void pulse_c6_reset(void)
{
	const uint32_t bit = 1u << 15u; /* GPIO15: C6 EN/reset, bank 0 */

	configure_iomux_gpio(15u, false);
	*reg32(GPIO_ENABLE_W1TS) = bit;
	*reg32(GPIO_OUT_W1TS) = bit;
	esp_rom_delay_us(C6_RESET_ASSERT_US);
	*reg32(GPIO_OUT_W1TC) = bit;
	esp_rom_delay_us(C6_RESET_INACTIVE_US);
	*reg32(GPIO_OUT_W1TS) = bit;
	esp_rom_delay_us(C6_RESET_READY_DELAY_US);
	ESP_LOGI(TAG, "C6 reset GPIO15 released");
}

static void enable_sdmmc_clock(void)
{
	set_bits(HP_SOC_CLK_CTRL1, SDMMC_SYS_CLK_EN);
	set_bits(LP_SDMMC_RESET, SDMMC_RESET_EN);
	clear_set_bits(LP_SDMMC_RESET, SDMMC_RESET_EN, 0);

	/* PLL_F160M / host divider 4 gives a conservative 40 MHz LS clock. */
	clear_set_bits(HP_PERI_CLK_CTRL01,
			       SDIO_LS_CLK_SRC_SEL | SDIO_HS_MODE, SDIO_LS_CLK_EN);

	uint32_t clear = (0xfu << SDIO_EDGE_L_SHIFT) |
			 (0xfu << SDIO_EDGE_H_SHIFT) |
			 (0xfu << SDIO_EDGE_N_SHIFT) |
			 (0x3u << SDIO_SLF_EDGE_SEL_SHIFT) |
			 (0x3u << SDIO_DRV_EDGE_SEL_SHIFT) |
			 (0x3u << SDIO_SAM_EDGE_SEL_SHIFT);
	uint32_t set = (3u << SDIO_EDGE_L_SHIFT) |
			 (1u << SDIO_EDGE_H_SHIFT) |
			 (3u << SDIO_EDGE_N_SHIFT) |
			 (1u << SDIO_DRV_EDGE_SEL_SHIFT) |
			 SDIO_SLF_CLK_EN | SDIO_DRV_CLK_EN | SDIO_SAM_CLK_EN;
	clear_set_bits(HP_PERI_CLK_CTRL02, clear, set);
	set_bits(HP_PERI_CLK_CTRL02, SDIO_EDGE_CFG_UPDATE);
	clear_set_bits(HP_PERI_CLK_CTRL02, SDIO_EDGE_CFG_UPDATE, 0);
}

/* --- I2C expanders --------------------------------------------------- */

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exp0;
static i2c_master_dev_handle_t s_exp1;

static esp_err_t expander_write(i2c_master_dev_handle_t dev, uint8_t reg,
				uint8_t value)
{
	uint8_t buf[2] = { reg, value };
	return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

/*
 * Write the safe boot state to both expanders.  Direction register
 * defaults to all-input after reset, so programming OUT_SET first and
 * IO_DIR last avoids any glitch on the rails we switch on.
 *
 * 0x43 (EXPANDER0): all eight pins become push-pull outputs with level 0
 *   except P4 LCD_RST, which stays as an input with pull-up enabled so
 *   the panel comes out of reset (the BSP drives the same open-drain
 *   sequence: set_dir INPUT + set_pullupdown UP + output mode OPEN_DRAIN).
 * 0x44 (EXPANDER1): all outputs, levels 0xA9 -> C6 rail on (P0), USB-A
 *   5 V on (P3, stock parity), nCHG_QC_EN 0, CHG_EN 1 (stock parity),
 *   PWROFF 0 (never pulsed at boot).
 */
static esp_err_t expanders_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = TAB5_I2C_PORT,
		.scl_io_num = TAB5_I2C_SCL_GPIO,
		.sda_io_num = TAB5_I2C_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
			    "i2c bus failed");

	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = EXPANDER0_ADDR,
		.scl_speed_hz = TAB5_I2C_HZ,
	};
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_exp0),
			    TAG, "expander 0x43 add failed");
	dev_cfg.device_address = EXPANDER1_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_exp1),
			    TAG, "expander 0x44 add failed");

	/* Expander 0x43: outputs default low (speaker/touch/camera/EXT5V off).
	 * LCD_RST (P4): input with pull-up + open-drain -> panel out of reset. */
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_OUT_SET, EXP0_OUT_OFF),
			    TAG, "exp0 out_set");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_OUT_H_IM, 0x10u),
			    TAG, "exp0 out_h_im"); /* P4 high-Z/open-drain */
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_PULL_SEL, 0xffu),
			    TAG, "exp0 pull_sel");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_PULL_EN, 0x10u),
			    TAG, "exp0 pull_en"); /* P4 pulled up */
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_IO_DIR, 0x90u),
			    TAG, "exp0 io_dir"); /* P4 LCD_RST + P7 hp-detect inputs */

	/* Expander 0x44: C6 rail on; USB-A 5 V and CHG_EN at stock parity,
	 * nCHG_QC_EN=0 (QC enabled per contract), PWROFF=0. */
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_OUT_SET, EXP1_OUT_BOOT),
			    TAG, "exp1 out_set");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_OUT_H_IM, 0x00u),
			    TAG, "exp1 out_h_im");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_IO_DIR, 0x46u),
			    TAG, "exp1 io_dir"); /* P1,P2,P6 inputs */

	esp_rom_delay_us(C6_POWER_UP_DELAY_US);
	ESP_LOGI(TAG, "expanders: C6 rail on, amp/touch/camera off, LCD released");
	return ESP_OK;
}

void tab5_board_init(struct tab5_fb *fb)
{
	fb->ok = false;
	fb->pa = 0;
	fb->bytes = 0;

	/* Backlight off before anything else touches the display rails. */
	gpio_config_t bl = {
		.pin_bit_mask = 1ull << 22,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&bl);
	gpio_set_level(GPIO_NUM_22, 0);

	if (expanders_init() != ESP_OK) {
		ESP_LOGE(TAG, "expander init failed; leaving rails at reset defaults "
			 "(C6 unpowered, panel in reset) and continuing headless");
	} else {
		enable_sdmmc_clock();

		/* Slot 1 via the matrix: CLK=12 CMD=13 DAT0..3={11,10,9,8}. */
		const uint32_t pins[] = { 12u, 13u, 11u, 10u, 9u, 8u };
		for (uint32_t i = 0; i < 6u; ++i)
			configure_iomux_gpio(pins[i], i != 0u);

		route_output(12u, 0u); /* SD_CARD_CCLK_2_PAD_OUT_IDX */
		route_output(13u, 1u); /* SD_CARD_CCMD_2_PAD_OUT_IDX */
		route_output(11u, 2u);
		route_output(10u, 3u);
		route_output(9u, 4u);
		route_output(8u, 5u);
		route_input(1u, 13u); /* CMD */
		route_input(2u, 11u); /* DAT0 */
		route_input(3u, 10u); /* DAT1 / SDIO interrupt */
		route_input(4u, 9u);  /* DAT2 */
		route_input(5u, 8u);  /* DAT3 */
		/* Same slot-1 card-int tie-off as the Stamp path. */
		route_input(129u, 0x3fu);

		pulse_c6_reset();
		ESP_LOGI(TAG, "Slot 1 SDIO ready on GPIO8-13; C6 reset done");

		tab5_display_init(fb);
	}
}
