/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/printk.h>

#include "hw_test.h"

#define SCAN_MS  4000
#define MAX_SEEN 32

/* Fastest legacy advertising (20 ms) plus a scan window that never closes:
 * the nRF21540 switches between PA and LNA all the time, like a stream.
 */
#define BUSY_ADV_PARAM  BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY, 0x0020, 0x0020, NULL)
#define BUSY_SCAN_PARAM BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_PASSIVE, BT_LE_SCAN_OPT_NONE, 0x0010, 0x0010)

static bt_addr_le_t seen[MAX_SEEN];
static size_t seen_count;
static int8_t best_rssi;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *buf)
{
	ARG_UNUSED(type);
	ARG_UNUSED(buf);

	if (seen_count == 0U || rssi > best_rssi) {
		best_rssi = rssi;
	}

	for (size_t i = 0; i < seen_count; i++) {
		if (bt_addr_le_eq(&seen[i], addr)) {
			return;
		}
	}

	if (seen_count < MAX_SEEN) {
		bt_addr_le_copy(&seen[seen_count++], addr);
	}
}

int radio_enable(void)
{
	static bool enabled;
	int err;

	if (enabled) {
		return 0;
	}

	err = bt_enable(NULL);
	if (err == 0) {
		enabled = true;
	}

	return err;
}

void radio_idle(void)
{
	(void)bt_le_adv_stop();
	(void)bt_le_scan_stop();
}

int radio_advertise(void)
{
	radio_idle();

	return bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);
}

int radio_busy(void)
{
	int err;

	radio_idle();

	err = bt_le_adv_start(BUSY_ADV_PARAM, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		return err;
	}

	seen_count = 0U;
	return bt_le_scan_start(BUSY_SCAN_PARAM, scan_cb);
}

/* Proves the net core boots, IPC works and the radio (through the nRF21540)
 * both receives (scan) and transmits (advertising, check with a phone).
 */
enum result test_radio(void)
{
	int err;

	err = radio_enable();
	if (err) {
		note("bt_enable failed (%d): net core / IPC down", err);
		return FAIL;
	}

	radio_idle();
	seen_count = 0U;
	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_cb);
	if (err) {
		note("scan start failed (%d)", err);
		return FAIL;
	}
	k_msleep(SCAN_MS);
	bt_le_scan_stop();

	err = radio_advertise();
	if (err) {
		note("advertising failed (%d)", err);
		return FAIL;
	}

	if (seen_count == 0U) {
		note("no advertisers heard in %d s; advertising '%s'", SCAN_MS / 1000,
		     CONFIG_BT_DEVICE_NAME);
		return WARN;
	}

	note("%zu devices heard, best RSSI %d dBm; advertising '%s'", seen_count, best_rssi,
	     CONFIG_BT_DEVICE_NAME);
	return PASS;
}

/* FEM at 20 dBm coupling into the mic/amp: measure, then listen. */
#define RF_LOOP_VOLUME (-40) /* -20 dB: avoids acoustic feedback on R1 */
#define RF_LISTEN_TIME K_SECONDS(15)

/* Tenths of a dB as "-12.3"; works for -0.x too. */
static const char *db(int x10, char buf[16])
{
	snprintf(buf, 16, "%s%d.%d", x10 < 0 ? "-" : "", abs(x10) / 10, abs(x10) % 10);
	return buf;
}

enum result test_rf_audio(void)
{
	char b1[16];
	char b2[16];
	char b3[16];
	struct audio_level quiet;
	struct audio_level busy;
	struct audio_level quiet2;
	enum result r = PASS;
	int delta;
	int err;

	err = radio_enable();
	if (err) {
		note("bt_enable failed (%d)", err);
		return FAIL;
	}

	err = hw_audio_start(RF_LOOP_VOLUME);
	if (err) {
		note("audio start failed (%d)", err);
		return FAIL;
	}

	printk("    Keep quiet for 6 s: mic noise with radio off, on, off ...\n");
	radio_idle();
	hw_audio_measure(2000, &quiet);

	err = radio_busy();
	if (err) {
		note("radio busy mode failed (%d)", err);
		hw_audio_stop();
		radio_idle();
		return FAIL;
	}
	hw_audio_measure(2000, &busy);

	/* Idle again: use the quieter idle window so drift can't hide noise. */
	radio_idle();
	hw_audio_measure(2000, &quiet2);
	quiet.rms_dbfs_x10 = MIN(quiet.rms_dbfs_x10, quiet2.rms_dbfs_x10);

	err = radio_busy();
	if (err) {
		note("radio busy mode failed (%d)", err);
		hw_audio_stop();
		radio_idle();
		return FAIL;
	}

	delta = busy.rms_dbfs_x10 - quiet.rms_dbfs_x10;
	if (delta >= 30) {
		r = WARN;
	}

	printk("    Mic noise radio off %s dBFS, radio on %s dBFS\n", db(quiet.rms_dbfs_x10, b1),
	       db(busy.rms_dbfs_x10, b2));
	printk("    LISTEN %d s: mic -> speaker/headphones while the radio transmits at\n"
	       "    20 dBm. Talk, then stay silent and listen for buzz/hum.\n"
	       "    (Use headphones on R1 if it howls.) Press any button to stop early.\n",
	       (int)(k_ticks_to_ms_floor32(RF_LISTEN_TIME.ticks) / 1000U));
	hw_audio_mode(AUDIO_LOOPBACK);
	key_flush();
	(void)key_wait(-1, RF_LISTEN_TIME);

	hw_audio_stop();
	(void)radio_advertise();

	note("mic noise %s%s dB with radio busy (%s -> %s dBFS); listened", delta >= 0 ? "+" : "",
	     db(delta, b3), db(quiet.rms_dbfs_x10, b1), db(busy.rms_dbfs_x10, b2));
	return r;
}
