/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HW_TEST_H_
#define HW_TEST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

enum result {
	PASS,
	WARN,
	FAIL,
	SKIP,
};

struct hw_test {
	const char *name;
	const char *desc;
	enum result (*run)(void);
};

/* One-line detail shown next to the verdict in the summary. */
void note(const char *fmt, ...);

/* Waits for a press (value 1) of `code`, or any key/switch event if code < 0.
 * Returns the event value, or -EAGAIN on timeout.
 */
int key_wait(int code, k_timeout_t timeout);
void key_flush(void);

/* True while 'hw run' (all) is running; tests that would end the session
 * (power off) only run when picked by name.
 */
extern bool hw_running_all;

enum audio_mode {
	AUDIO_SILENT,
	AUDIO_TONE,     /* 1 kHz, -12 dBFS */
	AUDIO_LOOPBACK, /* mic straight to the output */
};

struct audio_level {
	uint32_t blocks;
	uint32_t peak;
	uint32_t changes;   /* adjacent RX words that differ: 0 = no data */
	int rms_dbfs_x10;   /* broadband mic level */
	int tone_dbfs_x10;  /* mic level in the 1 kHz bin */
};

/* I2S + TAC5211 (+ R1 amp) up, output silent. Volume in half-dB steps. */
int hw_audio_start(int volume_half_db);
void hw_audio_stop(void);
void hw_audio_mode(enum audio_mode m);
void hw_audio_measure(int ms, struct audio_level *out);

/* Bluetooth: enable once, then idle (nothing on air), busy (fast
 * advertising + continuous scan, the FEM switching all the time) or
 * normal (the HWTEST advertisement).
 */
int radio_enable(void);
void radio_idle(void);
int radio_busy(void);
int radio_advertise(void);

extern const struct hw_test hw_tests[];
extern const size_t hw_tests_count;

enum result test_radio(void);
enum result test_rf_audio(void);

/* Held while tests run; the charge LED leaves the LED alone meanwhile. */
extern struct k_mutex hw_run_lock;

/* USB -> BQ24075 -> battery. Monitor prints USB, /CHG, battery mV, % and
 * %/h once per second.
 */
enum result test_charging(void);
void hw_battery_monitor(int seconds);

#endif /* HW_TEST_H_ */
