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

void note(const char *fmt, ...);

/* code < 0 matches any event; returns the event value or -EAGAIN */
int key_wait(int code, k_timeout_t timeout);
void key_flush(void);

/* Set during 'hw run'; power-off tests only run when named */
extern bool hw_running_all;

enum audio_mode {
	AUDIO_SILENT,
	AUDIO_TONE,     /* 1 kHz, -12 dBFS */
	AUDIO_LOOPBACK,
};

struct audio_level {
	uint32_t blocks;
	uint32_t peak;
	uint32_t changes;   /* 0 = no data on SDIN */
	int rms_dbfs_x10;
	int tone_dbfs_x10;  /* 1 kHz bin */
};

int hw_audio_start(int volume_half_db);
void hw_audio_stop(void);
void hw_audio_mode(enum audio_mode m);
void hw_audio_measure(int ms, struct audio_level *out);

int radio_enable(void);
void radio_idle(void);
int radio_busy(void);
int radio_advertise(void);

extern const struct hw_test hw_tests[];
extern const size_t hw_tests_count;

enum result test_radio(void);
enum result test_rf_audio(void);

/* Held while tests run; the charge LED indicator backs off meanwhile */
extern struct k_mutex hw_run_lock;

enum result test_charging(void);
void hw_battery_monitor(int seconds);

#endif /* HW_TEST_H_ */
