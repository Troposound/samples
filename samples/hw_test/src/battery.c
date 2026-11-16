/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <hal/nrf_usbreg.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "hw_test.h"

#if DT_NODE_EXISTS(DT_ALIAS(charge_status))

/* Charging is disabled while the debugger connector is plugged in */

#define GAUGE_NODE DT_ALIAS(fuel_gauge0)

#define MAX17048_REG_VCELL 0x02
#define MAX17048_REG_SOC   0x04
#define MAX17048_REG_HIBRT 0x0A
#define MAX17048_REG_CRATE 0x16

#define USB_WAIT_TIME    K_SECONDS(10)
#define CHARGE_SETTLE_MS 3000 /* charger soft start plus gauge samples */
#define CHARGE_RISE_MV   15   /* charge current x cell resistance, minimum */
/* Below the BQ24075 recharge threshold (4.2 V - ~100 mV) */
#define BATT_LOW_MV      4050

struct batt {
	bool vbus;
	bool charging; /* BQ24075 /CHG low */
	int mv;
	int soc_x10;   /* 0.1 % */
	int crate_x10; /* 0.1 %/h, positive while charging */
};

static const struct i2c_dt_spec gauge = I2C_DT_SPEC_GET(GAUGE_NODE);
static const struct gpio_dt_spec chg_pin = GPIO_DT_SPEC_GET(DT_ALIAS(charge_status), gpios);

static int gauge_read(uint8_t reg, uint16_t *val)
{
	uint8_t buf[2];
	int err = i2c_burst_read_dt(&gauge, reg, buf, sizeof(buf));

	if (err) {
		return err;
	}

	*val = sys_get_be16(buf);
	return 0;
}

/* Hibernate samples VCELL every 45 s; active mode every 250 ms */
static int gauge_hibernate_off(void)
{
	uint8_t buf[2] = { 0 };

	return i2c_burst_write_dt(&gauge, MAX17048_REG_HIBRT, buf, sizeof(buf));
}

static int vcell_mv(int *mv)
{
	uint16_t vcell;
	int err = gauge_read(MAX17048_REG_VCELL, &vcell);

	if (err) {
		return err;
	}

	*mv = (int)vcell * 5 / 64; /* 78.125 uV/LSB */
	return 0;
}

static int soc_x10(int *soc)
{
	uint16_t raw;
	int err = gauge_read(MAX17048_REG_SOC, &raw);

	if (err) {
		return err;
	}

	*soc = (int)raw * 10 / 256;
	return 0;
}

static bool vbus_present(void)
{
	return nrf_usbreg_status_get(NRF_USBREGULATOR) & NRF_USBREG_STATUS_VBUSDETECT_MASK;
}

static int batt_read(struct batt *b)
{
	uint16_t crate;
	int sum = 0;
	int err;

	b->vbus = vbus_present();
	b->charging = gpio_pin_get_dt(&chg_pin) == 1;

	for (int i = 0; i < 4; i++) {
		int mv;

		err = vcell_mv(&mv);
		if (err) {
			return err;
		}
		sum += mv;
		k_msleep(250);
	}
	b->mv = sum / 4;

	err = soc_x10(&b->soc_x10);
	err = err ? err : gauge_read(MAX17048_REG_CRATE, &crate);
	if (err) {
		return err;
	}

	b->crate_x10 = (int16_t)crate * 208 / 100; /* 0.208 %/h per LSB */
	return 0;
}

static void batt_print(const char *tag, const struct batt *b)
{
	printk("    %-8s USB %-3s  /CHG %-12s %4d mV  %3d.%d %%  %s%d.%d %%/h\n", tag,
	       b->vbus ? "in" : "out", b->charging ? "charging" : "not charging", b->mv,
	       b->soc_x10 / 10, b->soc_x10 % 10, b->crate_x10 < 0 ? "-" : "+",
	       abs(b->crate_x10) / 10, abs(b->crate_x10) % 10);
}

static bool usb_wait_change(bool from)
{
	k_timepoint_t end = sys_timepoint_calc(USB_WAIT_TIME);

	while (!sys_timepoint_expired(end)) {
		if (vbus_present() != from) {
			return true;
		}
		k_msleep(100);
	}
	return false;
}

/* Charge current shows as a VCELL step when the charger starts */
enum result test_charging(void)
{
	struct batt s[3];
	size_t n = 1;
	int rise = 0;
	bool idle_low = false;

	if (!gpio_is_ready_dt(&chg_pin) || gauge_hibernate_off() != 0 || batt_read(&s[0]) != 0) {
		note("MAX17048 or /CHG pin not accessible");
		return FAIL;
	}
	batt_print("now", &s[0]);

	while (n < ARRAY_SIZE(s)) {
		bool from = s[n - 1].vbus;

		printk("    %s the USB cable (board keeps running on battery) ...\n",
		       from ? "Unplug" : "Plug in");
		if (!usb_wait_change(from)) {
			break;
		}
		k_msleep(CHARGE_SETTLE_MS);
		if (batt_read(&s[n]) != 0) {
			note("MAX17048 read failed");
			return FAIL;
		}
		batt_print(s[n].vbus ? "USB in" : "USB out", &s[n]);
		n++;
	}

	for (size_t i = 0; i < n; i++) {
		if (!s[i].vbus && s[i].charging) {
			note("/CHG says charging without USB power");
			return FAIL;
		}
		if (s[i].vbus && !s[i].charging && s[i].mv < BATT_LOW_MV) {
			idle_low = true;
		}
	}

	for (size_t i = 1; i < n; i++) {
		if (!s[i].vbus || !s[i].charging) {
			continue;
		}
		rise = s[i].mv - s[i - 1].mv;
		if (rise < CHARGE_RISE_MV) {
			note("charging, but battery rose only %d mV: no charge current", rise);
			return FAIL;
		}
	}

	if (rise > 0) {
		note("/CHG follows USB; charging lifts battery +%d mV (%d mV, %d %%)", rise,
		     s[n - 1].mv, s[n - 1].soc_x10 / 10);
		return PASS;
	}

	if (idle_low) {
		note("USB in, %d mV, charger off: debugger attached? Check with the charge LED",
		     s[n - 1].mv);
		return WARN;
	}

	if (n < ARRAY_SIZE(s)) {
		note("USB not %s, charging not proven: %d mV, %d %%, %s",
		     n == 1 ? "unplugged/plugged" : "plugged back", s[n - 1].mv,
		     s[n - 1].soc_x10 / 10, s[n - 1].charging ? "charging" : "not charging");
		return WARN;
	}

	note("battery near full (%d mV, %d %%): charger idle as expected, /CHG ok", s[n - 1].mv,
	     s[n - 1].soc_x10 / 10);
	return PASS;
}

void hw_battery_monitor(int seconds)
{
	struct batt b;

	if (gauge_hibernate_off() != 0) {
		printk("MAX17048 not accessible\n");
		return;
	}
	for (int i = 0; i < seconds; i++) {
		if (batt_read(&b) != 0) {
			printk("MAX17048 read failed\n");
			break;
		}
		batt_print("", &b);
		/* batt_read() takes 1 s */
	}
}

/* R1's charger drives its own charge LED */
#define HW_CHARGE_LED IS_ENABLED(CONFIG_BOARD_R1)

#define TICK_MS      250
#define SETTLE_TICKS (CHARGE_SETTLE_MS / TICK_MS)

enum ind {
	IND_NONE, /* LED owned by the tests */
	IND_BATTERY,
	IND_SETTLING,
	IND_CHARGING,
	IND_FAULT,
	IND_IDLE,
};

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static void led_rgb(bool r, bool g, bool b)
{
	gpio_pin_set_dt(&led_r, r);
	gpio_pin_set_dt(&led_g, g);
	gpio_pin_set_dt(&led_b, b);
}

static void led_show(enum ind ind, uint32_t tick, int soc)
{
	bool flash = (tick % 8U) == 0U;

	switch (ind) {
	case IND_BATTERY:
		led_rgb(flash && soc < 500, flash && soc >= 200, false);
		break;
	case IND_CHARGING:
		led_rgb(false, false, !HW_CHARGE_LED && (tick / 2U) % 2U == 0U);
		break;
	case IND_FAULT:
		led_rgb(tick % 2U == 0U, false, false);
		break;
	case IND_SETTLING:
	case IND_IDLE:
		led_rgb(false, false, !HW_CHARGE_LED);
		break;
	default:
		break;
	}
}

static void charge_led_thread(void *p1, void *p2, void *p3)
{
	enum ind ind = IND_NONE;
	bool vbus = vbus_present();
	bool chg = false;
	bool paused = false;
	int mv[4];
	int base_mv = 0;
	int settle = 0;
	int soc = 0;
	uint32_t tick = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!gpio_is_ready_dt(&chg_pin) || !gpio_is_ready_dt(&led_r) ||
	    gauge_hibernate_off() != 0 || vcell_mv(&mv[0]) != 0) {
		printk("charge LED: gauge or GPIO not ready, disabled\n");
		return;
	}
	for (size_t i = 1; i < ARRAY_SIZE(mv); i++) {
		mv[i] = mv[0];
	}
	gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
	chg = gpio_pin_get_dt(&chg_pin) == 1;

	for (;; k_msleep(TICK_MS), tick++) {
		int before = (mv[0] + mv[1] + mv[2] + mv[3]) / 4;
		bool vbus_now;
		bool chg_now;
		int v;

		/* Tests own the LED and the USB cable */
		if (k_mutex_lock(&hw_run_lock, K_NO_WAIT) != 0) {
			ind = IND_NONE;
			paused = true;
			continue;
		}

		vbus_now = vbus_present();
		chg_now = gpio_pin_get_dt(&chg_pin) == 1;
		if (vcell_mv(&v) == 0) {
			mv[tick % ARRAY_SIZE(mv)] = v;
		}
		(void)soc_x10(&soc);
		k_mutex_unlock(&hw_run_lock);

		if (paused) {
			paused = false;
			vbus = vbus_now;
			chg = chg_now;
			continue;
		}

		if (vbus_now == vbus && chg_now == chg) {
			if (ind == IND_SETTLING && --settle == 0) {
				int rise = (mv[0] + mv[1] + mv[2] + mv[3]) / 4 - base_mv;

				ind = rise >= CHARGE_RISE_MV ? IND_CHARGING : IND_FAULT;
				printk("charge LED: charging, battery %+d mV -> %s\n", rise,
				       ind == IND_CHARGING ? "ok" : "FAULT, no charge current");
			}
		} else if (chg_now && !vbus_now) {
			ind = IND_FAULT;
			printk("charge LED: /CHG active without USB -> FAULT\n");
		} else if (chg_now && !chg) {
			base_mv = before;
			settle = SETTLE_TICKS;
			ind = IND_SETTLING;
		} else if (!chg_now) {
			ind = vbus_now ? IND_IDLE : IND_BATTERY;
			printk("charge LED: USB %s, charger off, %d mV, %d %%\n",
			       vbus_now ? "in" : "out", before, soc / 10);
		}
		vbus = vbus_now;
		chg = chg_now;

		led_show(ind, tick, soc);
	}
}

K_THREAD_DEFINE(charge_led, 1024, charge_led_thread, NULL, NULL, NULL, 10, 0, 0);

#endif /* DT_NODE_EXISTS(DT_ALIAS(charge_status)) */
