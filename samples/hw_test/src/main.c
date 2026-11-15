/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#include "hw_test.h"

struct key_evt {
	uint16_t code;
	int32_t value;
};

K_MSGQ_DEFINE(key_q, sizeof(struct key_evt), 16, 4);
K_MUTEX_DEFINE(hw_run_lock);

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static const char *const verdicts[] = { "PASS", "WARN", "FAIL", "SKIP" };
static enum result results[32];
static char notes[32][96];
static size_t current;

bool hw_running_all;

void note(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(notes[current], sizeof(notes[current]), fmt, ap);
	va_end(ap);
	printk("    %s\n", notes[current]);
}

static void input_cb(struct input_event *evt, void *user_data)
{
	struct key_evt k = { .code = evt->code, .value = evt->value };

	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_KEY) {
		(void)k_msgq_put(&key_q, &k, K_NO_WAIT);
	}
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

void key_flush(void)
{
	k_msgq_purge(&key_q);
}

int key_wait(int code, k_timeout_t timeout)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);
	struct key_evt k;

	while (k_msgq_get(&key_q, &k, sys_timepoint_timeout(end)) == 0) {
		if (code < 0) {
			return k.value;
		}
		if (k.code == code && k.value) {
			return 1;
		}
	}

	return -EAGAIN;
}

static enum result run_one(size_t i)
{
	const struct hw_test *t = &hw_tests[i];

	current = i;
	notes[i][0] = '\0';
	printk("\n[%zu/%zu] %s: %s\n", i + 1, hw_tests_count, t->name, t->desc);
	key_flush();
	results[i] = t->run();
	printk("  -> %s\n", verdicts[results[i]]);

	return results[i];
}

static void summary(size_t first, size_t last)
{
	size_t n[4] = { 0 };

	printk("\n==== %s hardware test summary ====\n", CONFIG_BOARD);
	for (size_t i = first; i <= last; i++) {
		n[results[i]]++;
		printk("  %-4s  %-12s %s\n", verdicts[results[i]], hw_tests[i].name, notes[i]);
	}
	printk("==== %zu pass, %zu warn, %zu fail, %zu skip ====\n", n[PASS], n[WARN], n[FAIL],
	       n[SKIP]);
	printk("Re-run: 'hw run' (all) or 'hw run <name>'; list: 'hw list'\n");

	gpio_pin_set_dt(&led_r, n[FAIL] > 0);
	gpio_pin_set_dt(&led_g, n[FAIL] == 0);
}

static void run_all(void)
{
	k_mutex_lock(&hw_run_lock, K_FOREVER);
	hw_running_all = true;
	for (size_t i = 0; i < hw_tests_count; i++) {
		run_one(i);
	}
	hw_running_all = false;
	summary(0, hw_tests_count - 1);
	k_mutex_unlock(&hw_run_lock);
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < hw_tests_count; i++) {
		shell_print(sh, "  %-12s %s", hw_tests[i].name, hw_tests[i].desc);
	}

	return 0;
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		run_all();
		return 0;
	}

	for (size_t i = 0; i < hw_tests_count; i++) {
		if (strcmp(argv[1], hw_tests[i].name) == 0) {
			k_mutex_lock(&hw_run_lock, K_FOREVER);
			run_one(i);
			summary(i, i);
			k_mutex_unlock(&hw_run_lock);
			return 0;
		}
	}

	shell_error(sh, "Unknown test '%s', see 'hw list'", argv[1]);
	return -EINVAL;
}

#if DT_NODE_EXISTS(DT_ALIAS(charge_status))
static int cmd_battery(const struct shell *sh, size_t argc, char **argv)
{
	int seconds = (argc > 1) ? atoi(argv[1]) : 30;

	ARG_UNUSED(sh);

	k_mutex_lock(&hw_run_lock, K_FOREVER);
	hw_battery_monitor(CLAMP(seconds, 1, 3600));
	k_mutex_unlock(&hw_run_lock);

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(hw_cmds,
	SHELL_CMD(list, NULL, "List tests", cmd_list),
	SHELL_CMD_ARG(run, NULL, "Run all tests, or one: run <name>", cmd_run, 1, 1),
#if DT_NODE_EXISTS(DT_ALIAS(charge_status))
	SHELL_CMD_ARG(battery, NULL, "USB, charger and gauge once per second: battery [s]",
		      cmd_battery, 1, 1),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(hw, &hw_cmds, "Troposound hardware test", NULL);

int main(void)
{
	if (hw_tests_count > ARRAY_SIZE(results)) {
		printk("Test table larger than the result table\n");
		return 0;
	}

	printk("\n*** Troposound %s hardware test, %zu tests ***\n", CONFIG_BOARD, hw_tests_count);
	printk("Interactive steps wait up to 10 s; follow the prompts.\n");
	k_msleep(1000);

	run_all();

	return 0;
}
