/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/dt-bindings/led/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "broadcast.h"

LOG_MODULE_REGISTER(strip, LOG_LEVEL_INF);

#define STRIP_NODE DT_ALIAS(led_strip)
#define STRIP_LEN  DT_PROP(STRIP_NODE, chain_length)
#define STRIP_SIDE ((STRIP_LEN + 1) / 2)
#define USER_NODE  DT_PATH(zephyr_user)

#define SPI_HZ      DT_PROP(STRIP_NODE, spi_max_frequency)
#define SYMBOL_BITS DT_PROP(STRIP_NODE, bits_per_symbol)
#define SYMBOL_ONE  DT_PROP(STRIP_NODE, spi_one_frame)
#define SYMBOL_ZERO DT_PROP(STRIP_NODE, spi_zero_frame)
/* Lead with the reset: the SPIM drives the first bit early and would stretch it. */
#define RESET_BYTES DIV_ROUND_UP(DT_PROP(STRIP_NODE, reset_delay) * (SPI_HZ / 1000000), 8)
#define TX_BYTES    (RESET_BYTES + DIV_ROUND_UP(STRIP_LEN * 24 * SYMBOL_BITS, 8) + 1)

#define RATE_HZ      (STRIP_FRAME_SAMPLES * 100)
#define IDLE_TIME    K_MSEC(25)
#define RENDER_EVERY 2 /* 50 fps */

#define BANDS     16
#define F_LO_HZ   40.0f
#define F_HI_HZ   16000.0f
#define BASS_HZ   160.0f
#define TREBLE_HZ 5000.0f
#define PI_F      3.14159265f
#define LN2_F     0.69314718f

#define GATE_DB       -60.0f
#define FLOOR_DB      -50.0f
#define BAND_RANGE_DB 30.0f
#define AGC_FALL_DB   0.02f /* per frame: 2 dB/s */
#define MEAN_ALPHA    0.01f /* about 1 s */
#define LEVEL_SPAN_DB 16.0f
#define REL_SPAN_DB   15.0f
#define RELEASE       0.02f /* per frame: full scale in 0.5 s */
#define REL_RELEASE   0.03f
#define PEAK_HOLD     30
#define PEAK_GRAVITY  0.0004f

#define BEAT_ALPHA   0.02f
#define BEAT_MIN_DB  3.0f
#define BEAT_SIGMAS  1.2f
#define BEAT_RISE_DB 3.0f
#define BEAT_GAP     20 /* frames: up to 300 BPM */
#define RISE_FRAMES  3

#define RIPPLES 8

#define MAX_BRIGHT   40
/* H1 VLED: 32 pixels white at 40/255 */
#define POWER_BUDGET (32 * 3 * MAX_BRIGHT)
#define VLED_SETTLE  K_MSEC(20)
#define FAULT_RETRY  K_SECONDS(2)

BUILD_ASSERT(STRIP_LEN >= 1, "Empty LED strip");
BUILD_ASSERT(DT_PROP_LEN(STRIP_NODE, color_mapping) == 3, "RGB strips only");

enum strip_mode {
	MODE_BLOOM,
	MODE_VU,
	MODE_SPECTRUM,
	MODE_STROBE,
	MODE_RIPPLES,
	MODE_OFF,
	MODE_COUNT,
};

struct biquad {
	float b0, a1, a2, z1, z2;
};

struct ripple {
	float r;
	float v;
	float speed;
	uint8_t hue;
};

K_MSGQ_DEFINE(pcm_q, STRIP_FRAME_SAMPLES * sizeof(uint32_t), 2, 4);

static const struct spi_dt_spec bus =
	SPI_DT_SPEC_GET(STRIP_NODE, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));
static const uint8_t color_map[] = DT_PROP(STRIP_NODE, color_mapping);
static const struct gpio_dt_spec vled_en = GPIO_DT_SPEC_GET_OR(USER_NODE, extled_en_gpios, {0});
static const struct gpio_dt_spec vled_fault =
	GPIO_DT_SPEC_GET_OR(USER_NODE, extled_fault_gpios, {0});
static struct gpio_callback fault_cb;
static atomic_t fault;
static bool vled_on;

static atomic_t mode_sel;

static struct biquad bank[BANDS];
static int bass_bands;
static int treble_from;

static bool adapted;
static float band_ref = FLOOR_DB;
static float level_mean;
static float band_mean[BANDS];
static float bass_mean;
static float bass_var;
static float bass_hist[RISE_FRAMES];
static int hist_idx;
static int since_beat;

static float level[2];
static float peak[2];
static float peak_vel[2];
static int peak_hold[2];
static float band[BANDS]; /* against the loudest band */
static float rel[BANDS];  /* against each band's own average */
static float bass;
static float bass_rel;
static float treble;
static float beat_strength;
static bool beat_pending;

static float pos[STRIP_LEN]; /* bin centre: 0 at the middle, 1 at the ends */
static struct led_rgb canvas[STRIP_LEN];
static struct ripple ripples[RIPPLES];

void strip_audio_feed(const uint32_t *pcm)
{
	(void)k_msgq_put(&pcm_q, pcm, K_NO_WAIT);
}

void strip_mode_next(void)
{
	atomic_inc(&mode_sel);
}

static uint32_t rnd(void)
{
	static uint32_t state = 0x2545f491;

	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

/* --- Analysis ------------------------------------------------------------ */

static void bank_init(void)
{
	const float bw = log2f(F_HI_HZ / F_LO_HZ) / (BANDS - 1);

	for (int k = 0; k < BANDS; k++) {
		const float fc = F_LO_HZ * exp2f(k * bw);
		const float w0 = 2.0f * PI_F * fc / RATE_HZ;
		const float alpha = sinf(w0) * sinhf(0.5f * LN2_F * bw * w0 / sinf(w0));
		const float a0 = 1.0f + alpha;

		bank[k] = (struct biquad){
			.b0 = alpha / a0,
			.a1 = -2.0f * cosf(w0) / a0,
			.a2 = (1.0f - alpha) / a0,
		};

		if (fc < BASS_HZ) {
			bass_bands = k + 1;
		}
		if (fc < TREBLE_HZ) {
			treble_from = k + 1;
		}
	}
}

static float to_db(float power)
{
	return 10.0f * log10f(power + 1e-12f);
}

static float clamp01(float x)
{
	return CLAMP(x, 0.0f, 1.0f);
}

static void adapt(float lvl_db, const float *band_db, float bass_db)
{
	if (!adapted) {
		adapted = true;
		level_mean = lvl_db;
		memcpy(band_mean, band_db, sizeof(band_mean));
		bass_mean = bass_db;
		bass_var = BEAT_MIN_DB * BEAT_MIN_DB;
		return;
	}

	level_mean += (lvl_db - level_mean) * MEAN_ALPHA;
	for (int k = 0; k < BANDS; k++) {
		band_mean[k] += (band_db[k] - band_mean[k]) * MEAN_ALPHA;
	}
}

static void peak_update(int c)
{
	if (level[c] >= peak[c]) {
		peak[c] = level[c];
		peak_vel[c] = 0.0f;
		peak_hold[c] = PEAK_HOLD;
	} else if (peak_hold[c] > 0) {
		peak_hold[c]--;
	} else {
		peak_vel[c] += PEAK_GRAVITY;
		peak[c] = MAX(level[c], peak[c] - peak_vel[c]);
	}
}

static void beat_detect(float bass_db, bool silent)
{
	const float dev = bass_db - bass_mean;
	const float rise = bass_db - bass_hist[hist_idx];

	bass_hist[hist_idx] = bass_db;
	hist_idx = (hist_idx + 1) % RISE_FRAMES;

	if (silent) {
		return;
	}

	if (since_beat < BEAT_GAP) {
		since_beat++;
	} else if (dev > MAX(BEAT_MIN_DB, BEAT_SIGMAS * sqrtf(bass_var)) && rise > BEAT_RISE_DB) {
		beat_pending = true;
		beat_strength = CLAMP(dev / 12.0f, 0.3f, 1.0f);
		since_beat = 0;
	}

	bass_mean += dev * BEAT_ALPHA;
	bass_var += (dev * dev - bass_var) * BEAT_ALPHA;
}

static void analyse(const uint32_t *pcm)
{
	float lvl[2] = {0};
	float pwr[BANDS] = {0};
	float lvl_db[2];
	float band_db[BANDS];
	float band_max = -120.0f;
	float bass_pwr = 0.0f;
	float loud;
	float bass_db;
	bool silent;

	if (pcm != NULL) {
		for (size_t n = 0; n < STRIP_FRAME_SAMPLES; n++) {
			const float l = (int16_t)(uint16_t)pcm[n] / 32768.0f;
			const float r = (int16_t)(uint16_t)(pcm[n] >> 16) / 32768.0f;
			const float x = 0.5f * (l + r);

			lvl[0] += l * l;
			lvl[1] += r * r;

			for (int k = 0; k < BANDS; k++) {
				struct biquad *f = &bank[k];
				const float y = f->b0 * x + f->z1;

				f->z1 = f->z2 - f->a1 * y;
				f->z2 = -f->b0 * x - f->a2 * y;
				pwr[k] += y * y;
			}
		}
	}

	for (int c = 0; c < 2; c++) {
		lvl_db[c] = to_db(lvl[c] / STRIP_FRAME_SAMPLES);
	}

	for (int k = 0; k < BANDS; k++) {
		band_db[k] = to_db(pwr[k] / STRIP_FRAME_SAMPLES);
		band_max = MAX(band_max, band_db[k]);
		if (k < bass_bands) {
			bass_pwr += pwr[k] / STRIP_FRAME_SAMPLES;
		}
	}

	loud = MAX(lvl_db[0], lvl_db[1]);
	bass_db = to_db(bass_pwr);
	silent = loud < GATE_DB;

	if (!silent) {
		adapt(loud, band_db, bass_db);
	}

	band_ref = MAX(MAX(band_max, band_ref - AGC_FALL_DB), FLOOR_DB);

	for (int c = 0; c < 2; c++) {
		const float n =
			silent ? 0.0f : clamp01((lvl_db[c] - level_mean) / LEVEL_SPAN_DB + 0.5f);

		level[c] = MAX(n, level[c] - RELEASE);
		peak_update(c);
	}

	bass = 0.0f;
	bass_rel = 0.0f;
	treble = 0.0f;
	for (int k = 0; k < BANDS; k++) {
		const float a = clamp01((band_db[k] - band_ref + BAND_RANGE_DB) / BAND_RANGE_DB);
		const float r =
			silent ? 0.0f : clamp01((band_db[k] - band_mean[k]) / REL_SPAN_DB + 0.4f);

		band[k] = MAX(a, band[k] - RELEASE);
		rel[k] = MAX(r, rel[k] - REL_RELEASE);

		if (k < bass_bands) {
			bass = MAX(bass, band[k]);
			bass_rel = MAX(bass_rel, rel[k]);
		} else if (k >= treble_from) {
			treble = MAX(treble, rel[k]);
		}
	}

	beat_detect(bass_db, silent);
}

/* --- Effects ------------------------------------------------------------- */

static struct led_rgb hsv(uint8_t h, uint8_t v)
{
	uint8_t region = h / 43;
	uint8_t rem = (h - region * 43) * 6;
	uint8_t q = (v * (255 - rem)) >> 8;
	uint8_t t = (v * rem) >> 8;

	switch (region) {
	case 0:
		return (struct led_rgb){.r = v, .g = t, .b = 0};
	case 1:
		return (struct led_rgb){.r = q, .g = v, .b = 0};
	case 2:
		return (struct led_rgb){.r = 0, .g = v, .b = t};
	case 3:
		return (struct led_rgb){.r = 0, .g = q, .b = v};
	case 4:
		return (struct led_rgb){.r = t, .g = 0, .b = v};
	default:
		return (struct led_rgb){.r = v, .g = 0, .b = q};
	}
}

static uint8_t u8(float x)
{
	return (uint8_t)(clamp01(x) * 255.0f);
}

static void blend_max(struct led_rgb *p, struct led_rgb c)
{
	p->r = MAX(p->r, c.r);
	p->g = MAX(p->g, c.g);
	p->b = MAX(p->b, c.b);
}

static void fade(uint8_t keep_of_256)
{
	for (int i = 0; i < STRIP_LEN; i++) {
		canvas[i].r = (canvas[i].r * keep_of_256) >> 8;
		canvas[i].g = (canvas[i].g * keep_of_256) >> 8;
		canvas[i].b = (canvas[i].b * keep_of_256) >> 8;
	}
}

static float band_at(const float *v, float p)
{
	const float b = p * (BANDS - 1);
	const int k = MIN((int)b, BANDS - 2);
	const float f = b - k;

	return v[k] * (1.0f - f) + v[k + 1] * f;
}

static void fx_bloom(uint32_t frame, bool beat)
{
	const float core = clamp01((bass - 0.6f) / 0.4f);

	ARG_UNUSED(frame);
	ARG_UNUSED(beat);

	for (int i = 0; i < STRIP_LEN; i++) {
		const float v = clamp01((bass - pos[i]) / 0.2f);
		const float ice = core * clamp01(1.0f - pos[i] / MAX(bass, 0.01f));

		canvas[i] = (struct led_rgb){
			.r = u8(v * ice * 0.3f),
			.g = u8(v * ice * 0.6f),
			.b = u8(v),
		};
	}
}

static float side_of(const float v[2], int i)
{
	const int side = 2 * i - (STRIP_LEN - 1);

	return side < 0 ? v[0] : side > 0 ? v[1] : MAX(v[0], v[1]);
}

static void fx_vu(uint32_t frame, bool beat)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(beat);

	for (int i = 0; i < STRIP_LEN; i++) {
		const float u = 1.0f - pos[i];
		const int bin = MIN((int)(u * STRIP_SIDE), STRIP_SIDE - 1);
		const float pk = side_of(peak, i);
		const float fill = (side_of(level, i) * STRIP_SIDE) - bin;

		canvas[i] = hsv((uint8_t)(85.0f * (1.0f - u)), u8(fill));

		if (pk * STRIP_SIDE >= 1.0f && bin == MIN((int)(pk * STRIP_SIDE), STRIP_SIDE - 1)) {
			canvas[i] = (struct led_rgb){.r = 255, .g = 255, .b = 255};
		}
	}
}

static void fx_spectrum(uint32_t frame, bool beat)
{
	ARG_UNUSED(beat);

	for (int i = 0; i < STRIP_LEN; i++) {
		const float v = band_at(rel, pos[i]);

		canvas[i] = hsv((uint8_t)(frame / 2 + (uint32_t)(pos[i] * 160.0f)), u8(v * v));
	}
}

static void fx_strobe(uint32_t frame, bool beat)
{
	static uint8_t hue;
	static float flash;
	const float base = 0.15f + 0.25f * MAX(level[0], level[1]);

	if (beat) {
		hue += 40 + rnd() % 64;
		flash = beat_strength;
	}

	for (int i = 0; i < STRIP_LEN; i++) {
		canvas[i] = hsv((uint8_t)((uint32_t)(pos[i] * 128.0f) - frame * 3), u8(base));
		blend_max(&canvas[i], hsv(hue, u8(flash * (1.0f - 0.5f * pos[i]))));
	}

	flash *= 0.85f;
}

static void fx_ripples(uint32_t frame, bool beat)
{
	static uint8_t hue;
	const float width = MAX(0.1f, 1.5f / STRIP_SIDE);
	const float sparkle = clamp01((treble - 0.6f) / 0.4f) * 0.08f;

	ARG_UNUSED(frame);

	fade(200);

	if (beat) {
		struct ripple *slot = &ripples[0];

		for (int n = 0; n < RIPPLES; n++) {
			if (ripples[n].v == 0.0f) {
				slot = &ripples[n];
				break;
			}
			if (ripples[n].r > slot->r) {
				slot = &ripples[n];
			}
		}

		hue += 37;
		*slot = (struct ripple){
			.r = 0.0f,
			.v = 0.5f + 0.5f * beat_strength,
			.speed = 0.03f + 0.04f * beat_strength,
			.hue = hue,
		};
	}

	for (int i = 0; i < STRIP_LEN; i++) {
		blend_max(&canvas[i], hsv(hue, u8(0.35f * bass_rel * (1.0f - pos[i] / 0.3f))));
	}

	for (int n = 0; n < RIPPLES; n++) {
		struct ripple *rp = &ripples[n];

		if (rp->v == 0.0f) {
			continue;
		}

		for (int i = 0; i < STRIP_LEN; i++) {
			const float a = 1.0f - fabsf(pos[i] - rp->r) / width;

			if (a > 0.0f) {
				blend_max(&canvas[i], hsv(rp->hue, u8(a * rp->v)));
			}
		}

		rp->r += rp->speed;
		if (rp->r > 1.0f + width) {
			rp->v = 0.0f;
		}
	}

	for (int i = 0; i < STRIP_LEN; i++) {
		if ((rnd() & UINT16_MAX) < (uint32_t)(sparkle * UINT16_MAX)) {
			canvas[i] = (struct led_rgb){.r = 200, .g = 200, .b = 200};
		}
	}
}

static void (*const effects[])(uint32_t frame, bool beat) = {
	[MODE_BLOOM] = fx_bloom,
	[MODE_VU] = fx_vu,
	[MODE_SPECTRUM] = fx_spectrum,
	[MODE_STROBE] = fx_strobe,
	[MODE_RIPPLES] = fx_ripples,
};

static const char *const mode_names[] = {
	[MODE_BLOOM] = "blue bass bloom",
	[MODE_VU] = "VU meter",
	[MODE_SPECTRUM] = "spectrum",
	[MODE_STROBE] = "beat strobe",
	[MODE_RIPPLES] = "beat ripples",
	[MODE_OFF] = "off",
};

/* --- Output -------------------------------------------------------------- */

static uint8_t channel(const struct led_rgb *p, uint8_t id)
{
	switch (id) {
	case LED_COLOR_ID_RED:
		return p->r;
	case LED_COLOR_ID_GREEN:
		return p->g;
	case LED_COLOR_ID_BLUE:
		return p->b;
	default:
		return 0;
	}
}

static int show(void)
{
	static uint8_t tx[TX_BYTES];
	const struct spi_buf buf = {.buf = tx, .len = sizeof(tx)};
	const struct spi_buf_set set = {.buffers = &buf, .count = 1};
	size_t bit = RESET_BYTES * 8;
	uint32_t sum = 0;
	uint32_t scale = MAX_BRIGHT;

	for (int i = 0; i < STRIP_LEN; i++) {
		sum += canvas[i].r + canvas[i].g + canvas[i].b;
	}

	if (sum * MAX_BRIGHT / 255 > POWER_BUDGET) {
		scale = POWER_BUDGET * 255 / sum;
	}

	memset(tx, 0, sizeof(tx));
	for (int i = 0; i < STRIP_LEN; i++) {
		for (size_t c = 0; c < ARRAY_SIZE(color_map); c++) {
			const uint8_t v = channel(&canvas[i], color_map[c]) * scale / 255;

			for (int b = 7; b >= 0; b--) {
				const uint8_t sym = (v & BIT(b)) ? SYMBOL_ONE : SYMBOL_ZERO;

				for (int s = SYMBOL_BITS - 1; s >= 0; s--, bit++) {
					if (sym & BIT(s)) {
						tx[bit / 8] |= BIT(7 - bit % 8);
					}
				}
			}
		}
	}

	return spi_write_dt(&bus, &set);
}

static void fault_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	atomic_set(&fault, 1);
}

static void vled_set(bool on)
{
	if (vled_en.port == NULL || on == vled_on) {
		return;
	}

	vled_on = on;

	if (!on && vled_fault.port != NULL) {
		(void)gpio_pin_interrupt_configure_dt(&vled_fault, GPIO_INT_DISABLE);
	}

	(void)gpio_pin_set_dt(&vled_en, on);

	if (on && vled_fault.port != NULL) {
		k_sleep(VLED_SETTLE);
		atomic_set(&fault, gpio_pin_get_dt(&vled_fault) > 0);
		(void)gpio_pin_interrupt_configure_dt(&vled_fault, GPIO_INT_EDGE_TO_ACTIVE);
	}
}

static int strip_init(void)
{
	int err;

	if (!spi_is_ready_dt(&bus)) {
		return -ENODEV;
	}

	if (vled_en.port != NULL) {
		if (!gpio_is_ready_dt(&vled_en)) {
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&vled_en, GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	if (vled_fault.port != NULL) {
		if (!gpio_is_ready_dt(&vled_fault)) {
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&vled_fault, GPIO_INPUT);
		if (err) {
			return err;
		}

		gpio_init_callback(&fault_cb, fault_isr, BIT(vled_fault.pin));
		err = gpio_add_callback_dt(&vled_fault, &fault_cb);
		if (err) {
			return err;
		}
	}

	for (int i = 0; i < STRIP_LEN; i++) {
		pos[i] = (abs(2 * i - (STRIP_LEN - 1)) / 2 + 0.5f) / STRIP_SIDE;
	}

	bank_init();

	return 0;
}

static void strip_thread(void *p1, void *p2, void *p3)
{
	static uint32_t pcm[STRIP_FRAME_SAMPLES];
	enum strip_mode cur = MODE_COUNT;
	uint32_t frame = 0;
	int err;

	err = strip_init();
	if (err) {
		LOG_ERR("LED strip init failed (%d)", err);
		return;
	}

	while (true) {
		const enum strip_mode mode = (uint32_t)atomic_get(&mode_sel) % MODE_COUNT;
		const bool got = k_msgq_get(&pcm_q, pcm, IDLE_TIME) == 0;
		bool beat;

		if (mode != cur) {
			cur = mode;
			memset(canvas, 0, sizeof(canvas));
			memset(ripples, 0, sizeof(ripples));
			LOG_INF("LED strip: %s", mode_names[mode]);

			if (mode == MODE_OFF) {
				(void)show();
				vled_set(false);
			}
		}

		if (mode == MODE_OFF) {
			continue;
		}

		analyse(got ? pcm : NULL);

		if (++frame % RENDER_EVERY != 0) {
			continue;
		}

		if (atomic_cas(&fault, 1, 0)) {
			LOG_ERR("VLED overcurrent, retrying");
			vled_set(false);
			k_sleep(FAULT_RETRY);
			continue;
		}

		vled_set(true);

		beat = beat_pending;
		beat_pending = false;
		effects[mode](frame / RENDER_EVERY, beat);

		err = show();
		if (err) {
			LOG_WRN("LED strip update failed (%d)", err);
		}
	}
}

K_THREAD_DEFINE(strip_tid, 2048, strip_thread, NULL, NULL, NULL, 10, 0, 0);
