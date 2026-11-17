/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/nrf_clock.h>
#include <hal/nrf_usbreg.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/onoff.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include <troposound/audio_i2s.h>

#include "hw_test.h"

#define USER_NODE    DT_PATH(zephyr_user)
#define CODEC_NODE   DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tac5211)
#define GAUGE_NODE   DT_ALIAS(fuel_gauge0)

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};

static enum result test_leds(void)
{
	static const char *const names[] = { "red", "green", "blue" };
	int ans = -EAGAIN;

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i]) ||
		    gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0) {
			note("LED %s: GPIO not ready", names[i]);
			return FAIL;
		}
	}

	printk("    The RGB LED cycles red, green, blue, white. All four correct? [y/n]\n");
	key_flush();
	/* Last step lights all three: white */
	for (size_t step = 0; ans < 0; step = (step + 1) % (ARRAY_SIZE(leds) + 1)) {
		for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
			gpio_pin_set_dt(&leds[i], step == i || step == ARRAY_SIZE(leds));
		}
		ans = yn_wait(K_MSEC(700));
	}
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		gpio_pin_set_dt(&leds[i], 0);
	}

	note("operator: red, green, blue, white %s", ans ? "ok" : "NOT ok");
	return ans ? PASS : FAIL;
}

static enum result test_clocks(void)
{
	struct onoff_manager *hf = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	struct onoff_client cli;
	nrf_clock_lfclk_t lf_src;
	nrf_clock_hfclk_t hf_src = NRF_CLOCK_HFCLK_LOW_ACCURACY;
	bool hf_ok = false;
	int err;

	z_nrf_clock_control_lf_on(CLOCK_CONTROL_NRF_LF_START_STABLE);
	if (!nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_LFCLK, &lf_src) ||
	    lf_src != NRF_CLOCK_LFCLK_XTAL) {
		note("32.768 kHz LFXO not running (src %d)", (int)lf_src);
		return FAIL;
	}

	sys_notify_init_spinwait(&cli.notify);
	err = onoff_request(hf, &cli);
	if (err < 0) {
		note("HFXO request failed (%d)", err);
		return FAIL;
	}

	for (int i = 0; i < 100 && !hf_ok; i++) {
		hf_ok = nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_HFCLK, &hf_src) &&
			hf_src == NRF_CLOCK_HFCLK_HIGH_ACCURACY;
		k_msleep(1);
	}
	onoff_release(hf);

	if (!hf_ok) {
		note("32 MHz HFXO did not start");
		return FAIL;
	}

	note("LFXO 32.768 kHz and HFXO 32 MHz running");
	return PASS;
}

#define MAX17048_REG_VERSION 0x08

static enum result test_i2c(void)
{
	const struct i2c_dt_spec codec = I2C_DT_SPEC_GET(CODEC_NODE);
	const struct i2c_dt_spec gauge = I2C_DT_SPEC_GET(GAUGE_NODE);
	uint8_t page;
	uint8_t ver[2];
	enum result r = PASS;
	char codec_txt[24];
	char gauge_txt[24];

	if (i2c_reg_read_byte_dt(&codec, 0x00, &page) == 0) {
		snprintf(codec_txt, sizeof(codec_txt), "ok (page %u)", page);
	} else {
		snprintf(codec_txt, sizeof(codec_txt), "NO ACK");
		r = FAIL;
	}

	if (i2c_burst_read_dt(&gauge, MAX17048_REG_VERSION, ver, sizeof(ver)) == 0) {
		uint16_t v = sys_get_be16(ver);

		snprintf(gauge_txt, sizeof(gauge_txt), "ok (ver 0x%04x)", v);
		if ((v & 0xFFF0) != 0x0010) {
			r = FAIL;
		}
	} else {
		snprintf(gauge_txt, sizeof(gauge_txt), "NO ACK");
		r = FAIL;
	}

	note("TAC5211@0x%02x %s, MAX17048@0x%02x %s", codec.addr, codec_txt, gauge.addr,
	     gauge_txt);
	return r;
}

static enum result test_fuel_gauge(void)
{
	const struct device *gauge = DEVICE_DT_GET(GAUGE_NODE);
	union fuel_gauge_prop_val volt;
	union fuel_gauge_prop_val soc;

	if (!device_is_ready(gauge)) {
		note("MAX17048 driver not ready");
		return FAIL;
	}

	if (fuel_gauge_get_prop(gauge, FUEL_GAUGE_VOLTAGE, &volt) < 0 ||
	    fuel_gauge_get_prop(gauge, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &soc) < 0) {
		note("property read failed");
		return FAIL;
	}

	note("battery %d mV, %u %%", volt.voltage / 1000, soc.relative_state_of_charge);

	if (volt.voltage < 3000000 || volt.voltage > 4500000) {
		return WARN;
	}

	return PASS;
}

static enum result test_audio(void)
{
	struct audio_level quiet;
	struct audio_level loud;
	uint32_t expect = (2000U * (AUDIO_I2S_SAMPLE_RATE_HZ / 1000U)) / AUDIO_I2S_BLOCK_WORDS;
	uint32_t late;
	bool heard;
	int rise;
	int err;

	err = hw_audio_start(-24); /* -12 dB */
	if (err) {
		note("I2S/TAC5211 start failed (%d)", err);
		return FAIL;
	}

	printk("    Measuring: 2 s silence, then 1 kHz tone on speaker/headphones\n");
	hw_audio_mode(AUDIO_SILENT);
	hw_audio_measure(2000, &quiet);
	hw_audio_mode(AUDIO_TONE);
	hw_audio_measure(2000, &loud);
	late = audio_i2s_late_count_reset();
	key_flush();
	heard = ask_yn("Do you hear a steady 1 kHz tone?");
	hw_audio_stop();

	if (quiet.blocks < expect * 9U / 10U || quiet.blocks > expect * 11U / 10U) {
		note("I2S ran %u blocks in 2 s, expected %u (LRCK not 48 kHz?)", quiet.blocks,
		     expect);
		return FAIL;
	}

	if (quiet.changes == 0U && loud.changes == 0U) {
		note("mic data constant: no capture on SDIN");
		return FAIL;
	}

	/* Acoustic path; headset ear cups may keep the tone from the mic */
	rise = (loud.tone_dbfs_x10 - quiet.tone_dbfs_x10) / 10;
	note("%s; mic %d dBFS; 1 kHz at mic %d -> %d dBFS (%+d dB); late %u",
	     heard ? "tone heard" : "tone NOT heard", quiet.rms_dbfs_x10 / 10,
	     quiet.tone_dbfs_x10 / 10, loud.tone_dbfs_x10 / 10, rise, late);

	if (!heard) {
		return FAIL;
	}

	if (rise < 10) {
		return WARN; /* heard, but the mic did not pick it up */
	}

	return late ? WARN : PASS;
}

#define TAC5211_REG_PAGE        0x00
#define TAC5211_REG_GPO1_CFG0   0x0C
#define TAC5211_REG_GPI_CFG     0x0D
#define TAC5211_REG_GPO_GPI_VAL 0x0E
#define TAC5211_GPO1_GPO_PUSHPULL 0x21 /* GPO1_CFG = GPO, drive low and high */
#define TAC5211_GPI1_EN           BIT(1)
#define TAC5211_GPO1_VAL          BIT(5)
#define TAC5211_GPI1_MON          BIT(1)

static enum result test_codec_gpio(void)
{
	const struct i2c_dt_spec codec = I2C_DT_SPEC_GET(CODEC_NODE);
	const struct gpio_dt_spec gpo1 = GPIO_DT_SPEC_GET(USER_NODE, codec_gpo1_gpios);
	const struct gpio_dt_spec gpi1 = GPIO_DT_SPEC_GET(USER_NODE, codec_gpi1_gpios);
	int gpo_seen[2];
	int gpi_seen[2];
	uint8_t val;
	int err;

	err = i2c_reg_write_byte_dt(&codec, TAC5211_REG_PAGE, 0);
	err = err ? err : i2c_reg_write_byte_dt(&codec, TAC5211_REG_GPO1_CFG0,
						 TAC5211_GPO1_GPO_PUSHPULL);
	err = err ? err : i2c_reg_write_byte_dt(&codec, TAC5211_REG_GPI_CFG, TAC5211_GPI1_EN);
	if (err) {
		note("codec I2C write failed (%d)", err);
		return FAIL;
	}

	/* Pull against the driven level so an open line reads wrong */
	for (int level = 0; level <= 1; level++) {
		err = i2c_reg_update_byte_dt(&codec, TAC5211_REG_GPO_GPI_VAL, TAC5211_GPO1_VAL,
					     level ? TAC5211_GPO1_VAL : 0);
		gpio_pin_configure_dt(&gpo1, GPIO_INPUT | (level ? GPIO_PULL_DOWN : GPIO_PULL_UP));
		k_msleep(2);
		gpo_seen[level] = err ? -1 : gpio_pin_get_dt(&gpo1);
	}

	for (int level = 0; level <= 1; level++) {
		gpio_pin_configure_dt(&gpi1, level ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
		k_msleep(2);
		err = i2c_reg_read_byte_dt(&codec, TAC5211_REG_GPO_GPI_VAL, &val);
		gpi_seen[level] = err ? -1 : !!(val & TAC5211_GPI1_MON);
	}

	(void)i2c_reg_write_byte_dt(&codec, TAC5211_REG_GPO1_CFG0, 0);
	(void)i2c_reg_write_byte_dt(&codec, TAC5211_REG_GPI_CFG, 0);
	(void)i2c_reg_update_byte_dt(&codec, TAC5211_REG_GPO_GPI_VAL, TAC5211_GPO1_VAL, 0);
	gpio_pin_configure_dt(&gpo1, GPIO_INPUT);
	gpio_pin_configure_dt(&gpi1, GPIO_INPUT);

	note("GPO1->P%d.%02d read %d/%d, P%d.%02d->GPI1 read %d/%d (want 0/1)",
	     gpo1.port == DEVICE_DT_GET(DT_NODELABEL(gpio1)), gpo1.pin, gpo_seen[0], gpo_seen[1],
	     gpi1.port == DEVICE_DT_GET(DT_NODELABEL(gpio1)), gpi1.pin, gpi_seen[0], gpi_seen[1]);

	return (gpo_seen[0] == 0 && gpo_seen[1] == 1 && gpi_seen[0] == 0 && gpi_seen[1] == 1)
		       ? PASS
		       : FAIL;
}

USBD_DEVICE_DEFINE(hw_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x1915, 0x5310);
USBD_DESC_LANG_DEFINE(usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "Troposound");
USBD_DESC_PRODUCT_DEFINE(usb_product, CONFIG_BT_DEVICE_NAME);
USBD_CONFIGURATION_DEFINE(usb_fs_config, 0, 100, NULL);

static K_SEM_DEFINE(usb_configured, 0, 1);

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (msg->type == USBD_MSG_CONFIGURATION) {
		k_sem_give(&usb_configured);
	} else if (msg->type == USBD_MSG_VBUS_READY) {
		(void)usbd_enable(ctx);
	} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
		(void)usbd_disable(ctx);
	}
}

static int usb_init_once(void)
{
	static int ret = 1;

	if (ret <= 0) {
		return ret;
	}

	ret = usbd_add_descriptor(&hw_usbd, &usb_lang);
	ret = ret ? ret : usbd_add_descriptor(&hw_usbd, &usb_mfr);
	ret = ret ? ret : usbd_add_descriptor(&hw_usbd, &usb_product);
	ret = ret ? ret : usbd_add_configuration(&hw_usbd, USBD_SPEED_FS, &usb_fs_config);
	ret = ret ? ret : usbd_register_all_classes(&hw_usbd, USBD_SPEED_FS, 1, NULL);
	ret = ret ? ret : usbd_msg_register_cb(&hw_usbd, usb_msg_cb);
	ret = ret ? ret : usbd_init(&hw_usbd);
	if (ret == 0 && nrf_usbreg_status_get(NRF_USBREGULATOR) & NRF_USBREG_STATUS_VBUSDETECT_MASK) {
		/* VBUS was present before the callback was registered */
		(void)usbd_enable(&hw_usbd);
	}

	return ret;
}

static enum result test_usb(void)
{
	bool vbus = nrf_usbreg_status_get(NRF_USBREGULATOR) & NRF_USBREG_STATUS_VBUSDETECT_MASK;
	int err;

	if (!vbus) {
		note("no VBUS: connect the board's USB port and re-run 'hw run usb'");
		return WARN;
	}

	err = usb_init_once();
	if (err) {
		note("USB device stack init failed (%d)", err);
		return FAIL;
	}

	if (k_sem_take(&usb_configured, K_SECONDS(2)) != 0) {
		printk("    Waiting for a host to enumerate the board (n to give up) ...\n");
		key_flush();
		while (k_sem_take(&usb_configured, K_MSEC(100)) != 0) {
			if (said_no()) {
				note("VBUS ok, but no host enumerated the board (D+/D-, cable, hub)");
				return FAIL;
			}
		}
	}
	k_sem_give(&usb_configured); /* stays configured for re-runs */

	note("VBUS ok, enumerated and configured by the host");
	return PASS;
}

struct button {
	uint16_t code;
	const char *label;
};

#define BUTTON_ENTRY(n) { DT_PROP(n, zephyr_code), DT_PROP(n, label) },

static const struct button buttons[] = {
	DT_FOREACH_CHILD(DT_PATH(buttons), BUTTON_ENTRY)
};

static enum result test_buttons(void)
{
	enum result r = PASS;
	char missed[48] = "";

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		bool no = false;

		printk("    Press %s (n if nothing happens) ...\n", buttons[i].label);
		key_flush();
		while (!no && key_wait(buttons[i].code, K_MSEC(20)) < 0) {
			no = said_no();
		}
		if (no) {
			printk("    %s: not detected\n", buttons[i].label);
			strncat(missed, buttons[i].label, sizeof(missed) - strlen(missed) - 2);
			strcat(missed, " ");
			r = FAIL;
		} else {
			printk("    %s: ok\n", buttons[i].label);
		}
	}

	if (r == PASS) {
		note("all %zu buttons pressed", ARRAY_SIZE(buttons));
	} else {
		note("not pressed: %s", missed);
	}

	return r;
}

#if DT_NODE_HAS_PROP(USER_NODE, io_channels)
static enum result test_pot(void)
{
	const struct adc_dt_spec pot = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, volume_pot);
	const int full = BIT(pot.resolution) - 1;
	struct adc_sequence seq = { 0 };
	int16_t raw;
	int lo = INT16_MAX;
	int hi = INT16_MIN;

	if (!adc_is_ready_dt(&pot) || adc_channel_setup_dt(&pot) < 0) {
		note("ADC not ready");
		return FAIL;
	}
	adc_sequence_init_dt(&pot, &seq);
	seq.buffer = &raw;
	seq.buffer_size = sizeof(raw);

	printk("    Turn the volume knob fully down and fully up (n if it does nothing) ...\n");
	key_flush();
	while (!said_no()) {
		if (adc_read_dt(&pot, &seq) < 0) {
			note("ADC read failed");
			return FAIL;
		}
		lo = MIN(lo, MAX(raw, 0));
		hi = MAX(hi, MIN(raw, full));
		if (hi - lo > full * 7 / 10) {
			break;
		}
		k_msleep(20);
	}

	note("pot range %d..%d of %d (%d%%)", lo, hi, full, ((hi - lo) * 100) / full);

	return (hi - lo > full * 7 / 10) ? PASS : FAIL;
}
#endif

#if DT_NODE_EXISTS(DT_ALIAS(jack_detect))
static enum result test_switch(const struct gpio_dt_spec *pin, const char *what,
			       const char *action, enum result on_no)
{
	int before = gpio_pin_get_dt(pin);

	printk("    %s now %s. %s (n if nothing happens) ...\n", what,
	       before ? "ACTIVE" : "inactive", action);
	key_flush();
	while (gpio_pin_get_dt(pin) == before) {
		if (said_no()) {
			note("%s stayed %s", what, before ? "active" : "inactive");
			return on_no;
		}
		k_msleep(20);
	}

	note("%s changed %s -> %s", what, before ? "active" : "inactive",
	     before ? "inactive" : "active");
	return PASS;
}

static enum result test_jack(void)
{
	static const struct gpio_dt_spec pin = GPIO_DT_SPEC_GET(DT_ALIAS(jack_detect), gpios);

	return test_switch(&pin, "Headphone jack", "Plug in or pull out headphones",
			   FAIL);
}
#endif

#if DT_NODE_EXISTS(DT_ALIAS(fuel_gauge_alert))
static enum result test_gauge_alert(void)
{
	static const struct gpio_dt_spec pin =
		GPIO_DT_SPEC_GET(DT_ALIAS(fuel_gauge_alert), gpios);
	int v = gpio_pin_get_dt(&pin);

	if (v < 0) {
		note("alert pin read failed (%d)", v);
		return FAIL;
	}

	note("ALRT pin %s", v ? "ACTIVE (low battery / SoC alert latched)" : "idle");
	return v ? WARN : PASS;
}
#endif

#if DT_NODE_EXISTS(DT_ALIAS(qdec0))
static enum result test_encoder(void)
{
	const struct device *qdec = DEVICE_DT_GET(DT_ALIAS(qdec0));
	struct sensor_value rot;
	bool left = false;
	bool right = false;

	if (!device_is_ready(qdec)) {
		note("QDEC not ready");
		return FAIL;
	}

	(void)sensor_sample_fetch(qdec); /* drop stale count */
	printk("    Turn the encoder left and right (n if nothing happens) ...\n");
	key_flush();
	while (!(left && right) && !said_no()) {
		if (sensor_sample_fetch(qdec) == 0 &&
		    sensor_channel_get(qdec, SENSOR_CHAN_ROTATION, &rot) == 0) {
			if (rot.val1 < 0 && !left) {
				left = true;
				printk("    left: ok\n");
			} else if (rot.val1 > 0 && !right) {
				right = true;
				printk("    right: ok\n");
			}
		}
		k_msleep(20);
	}

	note("left %s, right %s", left ? "ok" : "NOT seen", right ? "ok" : "NOT seen");
	return (left && right) ? PASS : FAIL;
}
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led_strip))
#define STRIP_NODE     DT_ALIAS(led_strip)
/* WS2812 can't report its length; bits past the last LED fall off the end */
#define STRIP_MAX_LEN  256
#define SYMBOL_BITS    DT_PROP(STRIP_NODE, bits_per_symbol)
#define SPI_HZ         DT_PROP(STRIP_NODE, spi_max_frequency)
#define PREAMBLE_BYTES DIV_ROUND_UP(DT_PROP(STRIP_NODE, reset_delay) * (SPI_HZ / 1000000), 8)
#define FRAME_BYTES    (PREAMBLE_BYTES + DIV_ROUND_UP(STRIP_MAX_LEN * 24 * SYMBOL_BITS, 8) + 1)

static uint8_t strip_frame[FRAME_BYTES];

/* Same raw-SPI encoding as the blinky sample */
static int strip_fill(const struct spi_dt_spec *spi, uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t grb[3] = { g, r, b };
	const struct spi_buf buf = { .buf = strip_frame, .len = sizeof(strip_frame) };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };
	size_t bit = PREAMBLE_BYTES * 8;

	memset(strip_frame, 0, sizeof(strip_frame));
	for (int i = 0; i < STRIP_MAX_LEN; i++) {
		for (int c = 0; c < 3; c++) {
			for (int k = 7; k >= 0; k--) {
				uint8_t sym = (grb[c] & BIT(k)) ? DT_PROP(STRIP_NODE, spi_one_frame)
								: DT_PROP(STRIP_NODE, spi_zero_frame);

				for (int s = SYMBOL_BITS - 1; s >= 0; s--, bit++) {
					if (sym & BIT(s)) {
						strip_frame[bit / 8] |= 0x80 >> (bit % 8);
					}
				}
			}
		}
	}

	return spi_write_dt(spi, &tx);
}

static enum result test_led_strip(void)
{
	static const struct spi_dt_spec spi = SPI_DT_SPEC_GET(
		STRIP_NODE, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));
	static const struct gpio_dt_spec en = GPIO_DT_SPEC_GET(USER_NODE, extled_en_gpios);
	static const struct gpio_dt_spec fault = GPIO_DT_SPEC_GET(USER_NODE, extled_fault_gpios);
	/* Dim, so a full-length strip stays within the VLED switch limit */
	static const uint8_t colors[][3] = {
		{ 16, 0, 0 }, { 0, 16, 0 }, { 0, 0, 16 }, { 10, 10, 10 }, { 0, 0, 0 },
	};
	bool faulted = false;
	int ans = -EAGAIN;

	if (!spi_is_ready_dt(&spi) || !gpio_is_ready_dt(&en) || !gpio_is_ready_dt(&fault)) {
		note("SPI/GPIO not ready");
		return FAIL;
	}

	gpio_pin_configure_dt(&fault, GPIO_INPUT);
	gpio_pin_configure_dt(&en, GPIO_OUTPUT_ACTIVE);
	k_msleep(20);

	printk("    The LED strip cycles red, green, blue, white, off.\n"
	       "    All LEDs show every color? [y/n]\n");
	key_flush();
	for (size_t i = 0; ans < 0; i = (i + 1) % ARRAY_SIZE(colors)) {
		if (strip_fill(&spi, colors[i][0], colors[i][1], colors[i][2]) < 0) {
			note("SPI write failed");
			gpio_pin_set_dt(&en, 0);
			return FAIL;
		}
		ans = yn_wait(K_MSEC(700));
		faulted |= gpio_pin_get_dt(&fault) > 0;
	}

	(void)strip_fill(&spi, 0, 0, 0);
	gpio_pin_set_dt(&en, 0);

	if (faulted) {
		note("VLED switch reported FAULT (overcurrent/short)");
		return FAIL;
	}

	note("operator: R/G/B/W on all LEDs %s, VLED no fault", ans ? "ok" : "NOT ok");
	return ans ? PASS : FAIL;
}
#endif

#if DT_NODE_EXISTS(DT_ALIAS(qwiic_i2c))
#define QWIIC_NODE DT_ALIAS(qwiic_i2c)

static enum result test_qwiic(void)
{
	const struct device *bus = DEVICE_DT_GET(QWIIC_NODE);
	const struct gpio_dt_spec scl = GPIO_DT_SPEC_GET(QWIIC_NODE, scl_gpios);
	const struct gpio_dt_spec sda = GPIO_DT_SPEC_GET(QWIIC_NODE, sda_gpios);
	char found[48] = "";
	size_t n = 0;

	if (!device_is_ready(bus)) {
		note("Qwiic I2C driver not ready");
		return FAIL;
	}

	/* On-board 10k pull-ups hold an idle bus high */
	if (gpio_pin_get_dt(&scl) != 1 || gpio_pin_get_dt(&sda) != 1) {
		note("bus not idle high: SCL %d SDA %d (pull-ups, short?)",
		     gpio_pin_get_dt(&scl), gpio_pin_get_dt(&sda));
		return FAIL;
	}

	for (uint16_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t dummy;

		if (i2c_read(bus, &dummy, 1, addr) == 0) {
			size_t len = strlen(found);

			snprintf(found + len, sizeof(found) - len, " 0x%02x", addr);
			n++;
		}
	}

	if (n == 0U) {
		note("lines idle high; no device on the connector (plug one in to test)");
		return WARN;
	}

	note("lines idle high; %zu device(s):%s", n, found);
	return PASS;
}
#endif

#if DT_NODE_HAS_PROP(USER_NODE, power_off_gpios)
static enum result test_power_off(void)
{
	const struct gpio_dt_spec off = GPIO_DT_SPEC_GET(USER_NODE, power_off_gpios);

	if (hw_running_all) {
		note("skipped in 'hw run'; 'hw run power_off' turns the headset off");
		return SKIP;
	}

	printk("    Turning off in 2 s. On battery the headset must go dark;\n"
	       "    press the power button to turn it on again. (On USB only it may stay on.)\n");
	k_msleep(2000);
	gpio_pin_configure_dt(&off, GPIO_OUTPUT_ACTIVE);
	k_msleep(1000);
	gpio_pin_configure_dt(&off, GPIO_INPUT);

	note("still running 1 s after KILL: board is powered from USB, or KILL is not wired");
	return WARN;
}
#endif

const struct hw_test hw_tests[] = {
	{ "leds", "RGB status LED", test_leds },
	{ "clocks", "32 kHz and 32 MHz crystals", test_clocks },
	{ "i2c", "TAC5211 and MAX17048 respond on I2C", test_i2c },
	{ "fuel_gauge", "MAX17048 battery voltage and charge", test_fuel_gauge },
	{ "audio", "I2S + TAC5211: tone to speaker, picked up by mic", test_audio },
	{ "codec_gpio", "TAC5211 GPO1/GPI1 lines to the MCU", test_codec_gpio },
	{ "usb", "USB VBUS and enumeration by the host", test_usb },
	{ "radio", "Net core, BLE scan + advertise, nRF21540 at 20 dBm", test_radio },
#if DT_NODE_EXISTS(DT_ALIAS(led_strip))
	{ "led_strip", "WS2812 strip and VLED switch", test_led_strip },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(qwiic_i2c))
	{ "qwiic", "Qwiic connector pull-ups and device scan", test_qwiic },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(fuel_gauge_alert))
	{ "gauge_alert", "MAX17048 ALRT pin idle", test_gauge_alert },
#endif
	{ "buttons", "Press every button", test_buttons },
#if DT_NODE_EXISTS(DT_ALIAS(qdec0))
	{ "encoder", "Rotary encoder", test_encoder },
#endif
#if DT_NODE_HAS_PROP(USER_NODE, io_channels)
	{ "pot", "Volume potentiometer full range", test_pot },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(jack_detect))
	{ "jack", "Headphone jack detect", test_jack },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(charge_status))
	{ "charging", "USB -> BQ24075 -> battery, /CHG and gauge agree", test_charging },
#endif
	{ "rf_audio", "Radio noise in mic/speaker: measure, then listen", test_rf_audio },
#if DT_NODE_HAS_PROP(USER_NODE, power_off_gpios)
	{ "power_off", "LTC2950 KILL turns the headset off (by name only)", test_power_off },
#endif
};

const size_t hw_tests_count = ARRAY_SIZE(hw_tests);
