/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <stdlib.h>

#include <troposound/volume.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(volume, LOG_LEVEL_INF);

#define USER_NODE   DT_PATH(zephyr_user)
#define HAS_KNOB    DT_NODE_HAS_PROP(USER_NODE, io_channels)
#define HAS_ENCODER DT_NODE_EXISTS(DT_ALIAS(qdec0))

#define VOLUME_MIN  (-120) /* -60 dB */
#define VOLUME_MAX  12     /* +6 dB */
#define POLL_PERIOD K_MSEC(50)

static const struct device *codec;
static int volume = INT_MIN;

static void volume_set(int half_db)
{
	half_db = CLAMP(half_db, VOLUME_MIN, VOLUME_MAX);
	if (half_db == volume) {
		return;
	}

	volume = half_db;
	audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL,
				 (audio_property_value_t){.vol = half_db});
	audio_codec_apply_properties(codec);
	LOG_INF("Volume %s%d.%d dB", half_db < 0 ? "-" : "", abs(half_db) / 2,
		(abs(half_db) % 2) * 5);
}

#if HAS_KNOB
/* Audio taper over the knob's travel, in permille of full scale. */
static const struct {
	int16_t permille;
	int16_t half_db;
} taper[] = {
	{ 0, VOLUME_MIN }, { 5, -92 },   { 10, -80 },  { 20, -68 },  { 50, -52 },
	{ 100, -40 },      { 200, -28 }, { 365, -18 }, { 500, -12 }, { 700, -6 },
	{ 880, -2 },       { 950, 0 },   { 1000, VOLUME_MAX },
};

static const struct adc_dt_spec knob = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, volume_pot);
static struct adc_sequence knob_seq;
static int16_t knob_raw;

static int knob_init(void)
{
	int err;

	if (!adc_is_ready_dt(&knob)) {
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&knob);
	if (err) {
		return err;
	}

	adc_sequence_init_dt(&knob, &knob_seq);
	knob_seq.buffer = &knob_raw;
	knob_seq.buffer_size = sizeof(knob_raw);

	return 0;
}

static int knob_half_db(int permille)
{
	for (size_t i = 1; i < ARRAY_SIZE(taper); i++) {
		if (permille <= taper[i].permille) {
			int span = taper[i].permille - taper[i - 1].permille;
			int step = taper[i].half_db - taper[i - 1].half_db;

			return taper[i - 1].half_db +
			       DIV_ROUND_CLOSEST((permille - taper[i - 1].permille) * step, span);
		}
	}

	return VOLUME_MAX;
}

static void knob_poll(void)
{
	const int full = BIT(knob.resolution) - 1;
	int half_db;

	if (adc_read_dt(&knob, &knob_seq) != 0) {
		return;
	}

	half_db = knob_half_db(DIV_ROUND_CLOSEST(CLAMP(knob_raw, 0, full) * 1000, full));

	/* 1 dB dead band so ADC noise doesn't flip between two steps. */
	if (volume == INT_MIN || abs(half_db - volume) >= 2) {
		volume_set(half_db);
	}
}
#endif /* HAS_KNOB */

#if HAS_ENCODER
#define DEG_PER_DETENT 3

static const struct device *const qdec = DEVICE_DT_GET(DT_ALIAS(qdec0));
static int qdec_deg;

static void encoder_poll(void)
{
	struct sensor_value rot;
	int detents;

	if (sensor_sample_fetch(qdec) != 0 ||
	    sensor_channel_get(qdec, SENSOR_CHAN_ROTATION, &rot) != 0) {
		return;
	}

	qdec_deg += rot.val1;
	detents = qdec_deg / DEG_PER_DETENT;
	qdec_deg -= detents * DEG_PER_DETENT;

	if (detents != 0) {
		volume_set(volume + detents * 2);
	}
}
#endif /* HAS_ENCODER */

static void poll_handler(struct k_work *work)
{
#if HAS_KNOB
	knob_poll();
#endif
#if HAS_ENCODER
	encoder_poll();
#endif

	k_work_reschedule(k_work_delayable_from_work(work), POLL_PERIOD);
}

static K_WORK_DELAYABLE_DEFINE(poll_work, poll_handler);

int volume_start(const struct device *dev, int initial_half_db)
{
	codec = dev;

#if HAS_KNOB
	int err = knob_init();

	if (err) {
		LOG_ERR("Volume knob not ready (%d)", err);
		return err;
	}
	ARG_UNUSED(initial_half_db);
#elif HAS_ENCODER
	if (!device_is_ready(qdec)) {
		LOG_ERR("Rotary encoder not ready");
		return -ENODEV;
	}
	(void)sensor_sample_fetch(qdec); /* drop counts from before start */
	volume_set(initial_half_db);
#else
	volume_set(initial_half_db);
#endif

	k_work_reschedule(&poll_work, K_NO_WAIT);

	return 0;
}
