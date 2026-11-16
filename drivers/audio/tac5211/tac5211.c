#define DT_DRV_COMPAT ti_tac5211

#include <troposound/tac5211.h>
#include "tac5211_regs.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(tac5211, CONFIG_TAC5211_LOG_LEVEL);

#define TAC5211_SUPPORTED_SAMPLE_RATE_HZ 48000U
#define TAC5211_SUPPORTED_WORD_SIZE_BITS 16U
#define TAC5211_SUPPORTED_MCLK_HZ        12288000U

#define TAC5211_PLAYBACK_VOLUME_MIN_STEPS     (-120) /* -60 dB */
#define TAC5211_PLAYBACK_VOLUME_MAX_STEPS     54     /* +27 dB */
#define TAC5211_PLAYBACK_VOLUME_DEFAULT_STEPS (-24)  /* -12 dB */

#define TAC5211_PLAYBACK_EQ_BAND_COUNT 3

#define TAC5211_PLAYBACK_EQ_GAIN_MIN_TENTH_DB (-120) /* -12 dB */
#define TAC5211_PLAYBACK_EQ_GAIN_MAX_TENTH_DB 120    /* +12 dB */
#define TAC5211_PLAYBACK_EQ_Q_MIN_MILLI       250    /* 0.25 */
#define TAC5211_PLAYBACK_EQ_Q_MAX_MILLI       8000   /* 8.0 */

#define TAC5211_PLAYBACK_EQ_BAND1_FREQ_MIN_HZ 120U
#define TAC5211_PLAYBACK_EQ_BAND1_FREQ_MAX_HZ 250U
#define TAC5211_PLAYBACK_EQ_BAND2_FREQ_MIN_HZ 1000U
#define TAC5211_PLAYBACK_EQ_BAND2_FREQ_MAX_HZ 4000U
#define TAC5211_PLAYBACK_EQ_BAND3_FREQ_MIN_HZ 6000U
#define TAC5211_PLAYBACK_EQ_BAND3_FREQ_MAX_HZ 12000U

#define TAC5211_DAC_DVOL_0_DB 201
#define TAC5211_DAC_DVOL_MUTE 0U
#define TAC5211_PLAYBACK_VOLUME_DEFAULT_REG                                                        \
	(TAC5211_DAC_DVOL_0_DB + TAC5211_PLAYBACK_VOLUME_DEFAULT_STEPS)

#define TAC5211_ADC_DVOL_DEFAULT_REG                                                               \
	(TAC5211_ADC_DVOL_0_DB + CONFIG_TAC5211_ADC_GAIN_HALF_DB)

#define TAC5211_EQ_PI          3.14159265358979323846
#define TAC5211_Q31_SCALE      2147483648.0
#define TAC5211_Q31_MAX_DOUBLE ((double)INT32_MAX / TAC5211_Q31_SCALE)

struct tac5211_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec reset_gpio;
};

struct tac5211_playback_eq_band {
	uint32_t frequency_hz;
	int16_t gain_tenth_db;
	uint16_t q_milli;
};

struct tac5211_playback_eq {
	struct tac5211_playback_eq_band bands[TAC5211_PLAYBACK_EQ_BAND_COUNT];
};

struct tac5211_data {
	struct k_mutex lock;
	int16_t playback_volume_steps;
	struct tac5211_playback_eq playback_eq;
	enum tac5211_playback_eq_preset playback_eq_preset;
	enum tac5211_capture_input capture_input;
	bool playback_eq_enabled;
	bool playback_muted;
	bool configured;
};

struct reg_config {
	uint8_t reg;
	uint8_t val;
};

enum tac5211_eq_shape {
	TAC5211_EQ_LOW_SHELF,
	TAC5211_EQ_PEAK,
	TAC5211_EQ_HIGH_SHELF,
};

struct tac5211_eq_band_def {
	uint32_t min_freq_hz;
	uint32_t max_freq_hz;
	uint32_t default_freq_hz;
	int16_t default_gain_tenth_db;
	uint16_t default_q_milli;
	enum tac5211_eq_shape shape;
};

struct tac5211_biquad_coeffs {
	int32_t n0;
	int32_t n1;
	int32_t n2;
	int32_t d1;
	int32_t d2;
};

struct tac5211_biquad_reg_block {
	uint8_t page;
	uint8_t base_reg;
};

static const struct tac5211_eq_band_def eq_band_defs[TAC5211_PLAYBACK_EQ_BAND_COUNT] = {
	{
		.min_freq_hz = TAC5211_PLAYBACK_EQ_BAND1_FREQ_MIN_HZ,
		.max_freq_hz = TAC5211_PLAYBACK_EQ_BAND1_FREQ_MAX_HZ,
		.default_freq_hz = 120U,
		.default_gain_tenth_db = -20,
		.default_q_milli = 707U,
		.shape = TAC5211_EQ_LOW_SHELF,
	},
	{
		.min_freq_hz = TAC5211_PLAYBACK_EQ_BAND2_FREQ_MIN_HZ,
		.max_freq_hz = TAC5211_PLAYBACK_EQ_BAND2_FREQ_MAX_HZ,
		/* Howl cut: measured speaker-to-mic loop resonances at 1850-3050 Hz */
		.default_freq_hz = 2400U,
		.default_gain_tenth_db = -70,
		.default_q_milli = 700U,
		.shape = TAC5211_EQ_PEAK,
	},
	{
		.min_freq_hz = TAC5211_PLAYBACK_EQ_BAND3_FREQ_MIN_HZ,
		.max_freq_hz = TAC5211_PLAYBACK_EQ_BAND3_FREQ_MAX_HZ,
		.default_freq_hz = 6000U,
		/* Kept low: speaker and mic responses both peak at 3-8 kHz, feeding howl */
		.default_gain_tenth_db = 30,
		.default_q_milli = 707U,
		.shape = TAC5211_EQ_HIGH_SHELF,
	},
};

/* Gains in 0.1 dB, indexed by GATT preset 0-4. Band 2 is the howl cut on every preset. */
#define TAC5211_PLAYBACK_EQ_PRESET_COUNT                                                           \
	((unsigned)TAC5211_PLAYBACK_EQ_PRESET_BASS_BOOST + 1U)

static const int16_t playback_eq_preset_gain[TAC5211_PLAYBACK_EQ_PRESET_COUNT]
					    [TAC5211_PLAYBACK_EQ_BAND_COUNT] = {
	[TAC5211_PLAYBACK_EQ_PRESET_FLAT] = { -20, -70, 30 },
	[TAC5211_PLAYBACK_EQ_PRESET_VOICE] = { -90, -70, -30 },
	[TAC5211_PLAYBACK_EQ_PRESET_BRIGHT] = { -30, -70, 90 },
	[TAC5211_PLAYBACK_EQ_PRESET_WARM] = { 45, -70, -30 },
	[TAC5211_PLAYBACK_EQ_PRESET_BASS_BOOST] = { 90, -70, 0 },
};

/* Datasheet Table 7-48: out ch1 uses BQ1/5/9, out ch2 BQ2/6/10. Addresses per Table 8-210/211. */
#define TAC5211_PLAYBACK_EQ_CHANNEL_COUNT 2U

static const struct tac5211_biquad_reg_block
	playback_biquad_regs[TAC5211_PLAYBACK_EQ_CHANNEL_COUNT][TAC5211_PLAYBACK_EQ_BAND_COUNT] = {
		{
			{TAC5211_PAGE_DAC_BQ_1_TO_6, 0x08},  /* DAC_BQ1: band 1 */
			{TAC5211_PAGE_DAC_BQ_1_TO_6, 0x58},  /* DAC_BQ5: band 2 */
			{TAC5211_PAGE_DAC_BQ_7_TO_12, 0x30}, /* DAC_BQ9: band 3 */
		},
		{
			{TAC5211_PAGE_DAC_BQ_1_TO_6, 0x1C},  /* DAC_BQ2:  band 1 */
			{TAC5211_PAGE_DAC_BQ_1_TO_6, 0x6C},  /* DAC_BQ6:  band 2 */
			{TAC5211_PAGE_DAC_BQ_7_TO_12, 0x44}, /* DAC_BQ10: band 3 */
	},
};

static int reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct tac5211_config *config = dev->config;

	return i2c_reg_write_byte_dt(&config->i2c, reg, val);
}

static int reg_select_page(const struct device *dev, uint8_t page)
{
	return reg_write(dev, TAC5211_REG_PAGE_SEL, page);
}

static int reg_write_page(const struct device *dev, uint8_t page, uint8_t reg, uint8_t val)
{
	int err = reg_select_page(dev, page);

	if (err) {
		return err;
	}

	return reg_write(dev, reg, val);
}

static uint8_t playback_volume_reg(int16_t steps)
{
	return (uint8_t)(TAC5211_DAC_DVOL_0_DB + steps);
}

static int playback_eq_program_locked(const struct device *dev, bool dac_live);

static bool playback_eq_preset_valid(enum tac5211_playback_eq_preset preset)
{
	return (unsigned)preset < TAC5211_PLAYBACK_EQ_PRESET_COUNT;
}

static const char *playback_eq_preset_name(enum tac5211_playback_eq_preset preset)
{
	switch (preset) {
	case TAC5211_PLAYBACK_EQ_PRESET_FLAT:
		return "flat";
	case TAC5211_PLAYBACK_EQ_PRESET_VOICE:
		return "voice";
	case TAC5211_PLAYBACK_EQ_PRESET_BRIGHT:
		return "bright";
	case TAC5211_PLAYBACK_EQ_PRESET_WARM:
		return "warm";
	case TAC5211_PLAYBACK_EQ_PRESET_BASS_BOOST:
		return "bass";
	default:
		return "invalid";
	}
}

static void playback_eq_from_preset(struct tac5211_playback_eq *eq,
				    enum tac5211_playback_eq_preset preset)
{
	for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
		eq->bands[i].frequency_hz = eq_band_defs[i].default_freq_hz;
		eq->bands[i].gain_tenth_db = playback_eq_preset_gain[preset][i];
		eq->bands[i].q_milli = eq_band_defs[i].default_q_milli;
	}
}

static void playback_eq_default(struct tac5211_playback_eq *eq)
{
	playback_eq_from_preset(eq, TAC5211_PLAYBACK_EQ_PRESET_FLAT);
}

static int playback_eq_validate(const struct tac5211_playback_eq *eq)
{
	if (eq == NULL) {
		return -EINVAL;
	}

	for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
		const struct tac5211_playback_eq_band *band = &eq->bands[i];
		const struct tac5211_eq_band_def *def = &eq_band_defs[i];

		if (band->frequency_hz < def->min_freq_hz ||
		    band->frequency_hz > def->max_freq_hz ||
		    band->gain_tenth_db < TAC5211_PLAYBACK_EQ_GAIN_MIN_TENTH_DB ||
		    band->gain_tenth_db > TAC5211_PLAYBACK_EQ_GAIN_MAX_TENTH_DB ||
		    band->q_milli < TAC5211_PLAYBACK_EQ_Q_MIN_MILLI ||
		    band->q_milli > TAC5211_PLAYBACK_EQ_Q_MAX_MILLI) {
			return -EINVAL;
		}
	}

	return 0;
}

static int q31_from_double(double val, int32_t *q31)
{
	long long raw;

	if (!isfinite(val) || val < -1.0 || val > 1.0) {
		return -ERANGE;
	}

	if (val >= TAC5211_Q31_MAX_DOUBLE) {
		*q31 = INT32_MAX;
		return 0;
	}

	if (val <= -1.0) {
		*q31 = INT32_MIN;
		return 0;
	}

	raw = llround(val * TAC5211_Q31_SCALE);
	if (raw > INT32_MAX) {
		*q31 = INT32_MAX;
	} else if (raw < INT32_MIN) {
		*q31 = INT32_MIN;
	} else {
		*q31 = (int32_t)raw;
	}

	return 0;
}

static int playback_biquad_allpass(struct tac5211_biquad_coeffs *coeffs)
{
	coeffs->n0 = INT32_MAX;
	coeffs->n1 = 0;
	coeffs->n2 = 0;
	coeffs->d1 = 0;
	coeffs->d2 = 0;

	return 0;
}

static int playback_biquad_from_band(const struct tac5211_playback_eq_band *band,
				     const struct tac5211_eq_band_def *def,
				     struct tac5211_biquad_coeffs *coeffs)
{
	double gain_db;
	double q;
	double a;
	double omega;
	double sin_omega;
	double cos_omega;
	double alpha;
	double sqrt_a;
	double b0;
	double b1;
	double b2;
	double a0;
	double a1;
	double a2;
	double tac[5];
	double max_num;
	double makeup;
	int err;

	if (band->gain_tenth_db == 0) {
		return playback_biquad_allpass(coeffs);
	}

	gain_db = (double)band->gain_tenth_db / 10.0;
	q = (double)band->q_milli / 1000.0;
	a = pow(10.0, gain_db / 40.0);
	omega = 2.0 * TAC5211_EQ_PI * (double)band->frequency_hz /
		(double)TAC5211_SUPPORTED_SAMPLE_RATE_HZ;
	sin_omega = sin(omega);
	cos_omega = cos(omega);
	alpha = sin_omega / (2.0 * q);
	sqrt_a = sqrt(a);

	switch (def->shape) {
	case TAC5211_EQ_LOW_SHELF:
		b0 = a * ((a + 1.0) - ((a - 1.0) * cos_omega) + (2.0 * sqrt_a * alpha));
		b1 = 2.0 * a * ((a - 1.0) - ((a + 1.0) * cos_omega));
		b2 = a * ((a + 1.0) - ((a - 1.0) * cos_omega) - (2.0 * sqrt_a * alpha));
		a0 = (a + 1.0) + ((a - 1.0) * cos_omega) + (2.0 * sqrt_a * alpha);
		a1 = -2.0 * ((a - 1.0) + ((a + 1.0) * cos_omega));
		a2 = (a + 1.0) + ((a - 1.0) * cos_omega) - (2.0 * sqrt_a * alpha);
		break;
	case TAC5211_EQ_PEAK:
		b0 = 1.0 + (alpha * a);
		b1 = -2.0 * cos_omega;
		b2 = 1.0 - (alpha * a);
		a0 = 1.0 + (alpha / a);
		a1 = -2.0 * cos_omega;
		a2 = 1.0 - (alpha / a);
		break;
	case TAC5211_EQ_HIGH_SHELF:
		b0 = a * ((a + 1.0) + ((a - 1.0) * cos_omega) + (2.0 * sqrt_a * alpha));
		b1 = -2.0 * a * ((a - 1.0) + ((a + 1.0) * cos_omega));
		b2 = a * ((a + 1.0) + ((a - 1.0) * cos_omega) - (2.0 * sqrt_a * alpha));
		a0 = (a + 1.0) - ((a - 1.0) * cos_omega) + (2.0 * sqrt_a * alpha);
		a1 = 2.0 * ((a - 1.0) - ((a + 1.0) * cos_omega));
		a2 = (a + 1.0) - ((a - 1.0) * cos_omega) - (2.0 * sqrt_a * alpha);
		break;
	default:
		return -EINVAL;
	}

	if (a0 == 0.0 || !isfinite(a0)) {
		return -ERANGE;
	}

	b0 /= a0;
	b1 /= a0;
	b2 /= a0;
	a1 /= a0;
	a2 /= a0;

	/* Biquad precedes DVC (Figure 7-69): attenuate boosts so the peak stays <= 0 dBFS */
	makeup = (gain_db > 0.0) ? pow(10.0, -gain_db / 20.0) : 1.0;

	tac[0] = b0 * makeup;
	tac[1] = (b1 / 2.0) * makeup;
	tac[2] = b2 * makeup;
	tac[3] = -a1 / 2.0;
	tac[4] = -a2;

	max_num = MAX(MAX(fabs(tac[0]), fabs(tac[1])), fabs(tac[2]));
	if (max_num > TAC5211_Q31_MAX_DOUBLE) {
		double scale = TAC5211_Q31_MAX_DOUBLE / max_num;

		tac[0] *= scale;
		tac[1] *= scale;
		tac[2] *= scale;
	}

	err = q31_from_double(tac[0], &coeffs->n0);
	if (err == 0) {
		err = q31_from_double(tac[1], &coeffs->n1);
	}
	if (err == 0) {
		err = q31_from_double(tac[2], &coeffs->n2);
	}
	if (err == 0) {
		err = q31_from_double(tac[3], &coeffs->d1);
	}
	if (err == 0) {
		err = q31_from_double(tac[4], &coeffs->d2);
	}

	return err;
}

static int16_t playback_eq_pos_gain_tenth(const int16_t *gains)
{
	int16_t sum = 0;

	for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
		if (gains[i] > 0) {
			sum += gains[i];
		}
	}

	return sum;
}

static int playback_eq_scale_nums(struct tac5211_biquad_coeffs *coeffs, double scale)
{
	double n[3];
	double max_num;
	int err;

	n[0] = ((double)coeffs->n0 / TAC5211_Q31_SCALE) * scale;
	n[1] = ((double)coeffs->n1 / TAC5211_Q31_SCALE) * scale;
	n[2] = ((double)coeffs->n2 / TAC5211_Q31_SCALE) * scale;
	max_num = MAX(MAX(fabs(n[0]), fabs(n[1])), fabs(n[2]));
	if (max_num > TAC5211_Q31_MAX_DOUBLE) {
		double fit = TAC5211_Q31_MAX_DOUBLE / max_num;

		n[0] *= fit;
		n[1] *= fit;
		n[2] *= fit;
	}

	err = q31_from_double(n[0], &coeffs->n0);
	if (err == 0) {
		err = q31_from_double(n[1], &coeffs->n1);
	}
	if (err == 0) {
		err = q31_from_double(n[2], &coeffs->n2);
	}

	return err;
}

/* Restore midband level lost to boost makeup so presets match FLAT loudness */
static int playback_eq_match_flat_loudness(const struct tac5211_playback_eq *eq,
					   struct tac5211_biquad_coeffs *coeffs)
{
	int16_t preset_pos = 0;
	int16_t extra;
	double scale;

	for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
		if (eq->bands[i].gain_tenth_db > 0) {
			preset_pos += eq->bands[i].gain_tenth_db;
		}
	}

	extra = preset_pos - playback_eq_pos_gain_tenth(
			playback_eq_preset_gain[TAC5211_PLAYBACK_EQ_PRESET_FLAT]);
	if (extra == 0) {
		return 0;
	}

	scale = pow(10.0, (double)extra / 200.0);
	if (!isfinite(scale) || scale <= 0.0) {
		return -ERANGE;
	}

	for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
		if (coeffs[i].n0 != INT32_MAX || coeffs[i].n1 != 0 || coeffs[i].n2 != 0) {
			return playback_eq_scale_nums(&coeffs[i], scale);
		}
	}

	return playback_eq_scale_nums(&coeffs[0], scale);
}

static int playback_eq_build_coeffs(const struct tac5211_playback_eq *eq,
				    struct tac5211_biquad_coeffs *coeffs)
{
	int err = playback_eq_validate(eq);

	if (err) {
		return err;
	}

	for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
		err = playback_biquad_from_band(&eq->bands[i], &eq_band_defs[i], &coeffs[i]);
		if (err) {
			return err;
		}
	}

	return playback_eq_match_flat_loudness(eq, coeffs);
}

static int playback_eq_coeffs_from_state(const struct tac5211_data *data,
					 struct tac5211_biquad_coeffs *coeffs)
{
	if (!data->playback_eq_enabled) {
		for (size_t i = 0; i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
			int err = playback_biquad_allpass(&coeffs[i]);

			if (err) {
				return err;
			}
		}

		return 0;
	}

	return playback_eq_build_coeffs(&data->playback_eq, coeffs);
}

static int write_biquad_coeffs(const struct device *dev,
			       const struct tac5211_biquad_reg_block *block,
			       const struct tac5211_biquad_coeffs *coeffs)
{
	const int32_t values[] = {
		coeffs->n0, coeffs->n1, coeffs->n2, coeffs->d1, coeffs->d2,
	};
	uint8_t reg = block->base_reg;
	int err = reg_select_page(dev, block->page);

	if (err) {
		return err;
	}

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		uint32_t val = (uint32_t)values[i];

		for (int shift = 24; shift >= 0; shift -= 8) {
			err = reg_write(dev, reg++, (uint8_t)(val >> shift));
			if (err) {
				return err;
			}
		}
	}

	return 0;
}

static int playback_eq_write_biquads_locked(const struct device *dev,
					    const struct tac5211_biquad_coeffs *coeffs)
{
	int err = 0;
	int page_err;

	for (size_t ch = 0; err == 0 && ch < TAC5211_PLAYBACK_EQ_CHANNEL_COUNT; ch++) {
		for (size_t i = 0; err == 0 && i < TAC5211_PLAYBACK_EQ_BAND_COUNT; i++) {
			err = write_biquad_coeffs(dev, &playback_biquad_regs[ch][i], &coeffs[i]);
		}
	}

	page_err = reg_select_page(dev, TAC5211_PAGE_CONTROL);
	if (err == 0) {
		err = page_err;
	}

	return err;
}

static int playback_eq_apply_coeffs_locked(const struct device *dev,
					   const struct tac5211_biquad_coeffs *coeffs)
{
	int err = reg_write_page(dev, TAC5211_PAGE_CONTROL, TAC5211_REG_DSP_CFG1,
				 TAC5211_DSP_CFG1_DAC_3_BIQUADS);

	if (err == 0) {
		err = playback_eq_write_biquads_locked(dev, coeffs);
	}

	if (err) {
		LOG_ERR("Failed to apply playback EQ coefficients: %d", err);
	}

	return err;
}

static int write_reg_table(const struct device *dev, const struct reg_config *tbl, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		int err = reg_write(dev, tbl[i].reg, tbl[i].val);

		if (err) {
			LOG_ERR("Failed to write reg 0x%02x: %d", tbl[i].reg, err);
			return err;
		}
	}

	return 0;
}

static bool capture_input_is_valid(enum tac5211_capture_input input)
{
	return input == TAC5211_CAPTURE_INPUT_IN1P || input == TAC5211_CAPTURE_INPUT_IN2P;
}

static const char *capture_input_name(enum tac5211_capture_input input)
{
	switch (input) {
	case TAC5211_CAPTURE_INPUT_IN1P:
		return "IN1P";
	case TAC5211_CAPTURE_INPUT_IN2P:
		return "IN2P";
	default:
		return "unknown";
	}
}

static uint8_t capture_input_dyn_pupd_cfg(enum tac5211_capture_input input)
{
	uint8_t cfg = TAC5211_DYN_PUPD_ADC_DYN_PUPD_EN;

	if (input == TAC5211_CAPTURE_INPUT_IN2P) {
		cfg |= TAC5211_DYN_PUPD_ADC_CH_SWAP;
	}

	return cfg;
}

static uint8_t capture_input_dvol(enum tac5211_capture_input input)
{
	int steps = CONFIG_TAC5211_ADC_GAIN_HALF_DB;

	if (input == TAC5211_CAPTURE_INPUT_IN2P) {
		steps += CONFIG_TAC5211_ADC_GAIN_EXT_EXTRA_HALF_DB;
	}

	return TAC5211_ADC_DVOL_0_DB + MIN(steps, TAC5211_ADC_DVOL_MAX_STEPS);
}

static int capture_input_write_locked(const struct device *dev)
{
	struct tac5211_data *data = dev->data;
	int err = reg_write_page(dev, TAC5211_PAGE_CONTROL, TAC5211_REG_DYN_PUPD_CFG,
				 capture_input_dyn_pupd_cfg(data->capture_input));

	if (err) {
		LOG_ERR("Failed to select capture input %s: %d",
			capture_input_name(data->capture_input), err);
		return err;
	}

	err = reg_write_page(dev, TAC5211_PAGE_CONTROL, TAC5211_REG_ADC_CH1_CFG2,
			     capture_input_dvol(data->capture_input));
	if (err) {
		LOG_ERR("Failed to set capture gain for %s: %d",
			capture_input_name(data->capture_input), err);
	}

	return err;
}

static int capture_input_apply_locked(const struct device *dev)
{
	int err;
	int power_err;

	err = reg_write_page(dev, TAC5211_PAGE_CONTROL, TAC5211_REG_PWR_CFG,
			     TAC5211_PWR_PLAYBACK_AND_MICBIAS);
	if (err) {
		LOG_ERR("Failed to power down ADC for capture input switch: %d", err);
		return err;
	}

	k_sleep(K_MSEC(1));

	err = capture_input_write_locked(dev);

	power_err = reg_write_page(dev, TAC5211_PAGE_CONTROL, TAC5211_REG_PWR_CFG,
				   TAC5211_PWR_FULL_DUPLEX);
	if (power_err) {
		LOG_ERR("Failed to power ADC back up after capture input switch: %d", power_err);
	}

	if (err == 0) {
		err = power_err;
	}

	return err;
}

int tac5211_capture_input_set(const struct device *dev, enum tac5211_capture_input input)
{
	struct tac5211_data *data;
	enum tac5211_capture_input old_input;
	int err = 0;

	if (dev == NULL || !capture_input_is_valid(input)) {
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (input == data->capture_input) {
		k_mutex_unlock(&data->lock);
		return 0;
	}

	old_input = data->capture_input;
	data->capture_input = input;

	if (data->configured) {
		err = capture_input_apply_locked(dev);
		if (err) {
			data->capture_input = old_input;
		}
	}

	k_mutex_unlock(&data->lock);

	if (err == 0) {
		LOG_DBG("Capture input set to %s", capture_input_name(input));
	}

	return err;
}

int tac5211_playback_eq_enable(const struct device *dev, bool enable)
{
	struct tac5211_data *data;
	bool old_enable;
	int err = 0;

	if (dev == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (enable == data->playback_eq_enabled) {
		k_mutex_unlock(&data->lock);
		return 0;
	}

	old_enable = data->playback_eq_enabled;
	data->playback_eq_enabled = enable;

	if (data->configured) {
		err = playback_eq_program_locked(dev, true);
		if (err) {
			data->playback_eq_enabled = old_enable;
		}
	}

	k_mutex_unlock(&data->lock);

	if (err == 0) {
		LOG_INF("Playback EQ %s", enable ? "speaker" : "bypass");
	}

	return err;
}

int tac5211_playback_eq_preset_set(const struct device *dev,
				   enum tac5211_playback_eq_preset preset)
{
	struct tac5211_data *data;
	int err = 0;

	if (dev == NULL || !playback_eq_preset_valid(preset)) {
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (preset == data->playback_eq_preset) {
		k_mutex_unlock(&data->lock);
		return 0;
	}

	data->playback_eq_preset = preset;
	playback_eq_from_preset(&data->playback_eq, preset);

	if (data->configured && data->playback_eq_enabled) {
		err = playback_eq_program_locked(dev, true);
	}

	k_mutex_unlock(&data->lock);

	if (err) {
		LOG_ERR("Playback EQ preset %s store ok, program failed: %d",
			playback_eq_preset_name(preset), err);
	} else {
		LOG_INF("Playback EQ preset %s", playback_eq_preset_name(preset));
	}

	return err;
}

/* I2S target; clocks auto-detected from BCLK/FSYNC, no PLL setup */
static const struct reg_config init_config[] = {
	{TAC5211_REG_PAGE_SEL, TAC5211_PAGE_CONTROL},
	{TAC5211_REG_GPIO1_CFG0, TAC5211_PIN_DISABLED},
	{TAC5211_REG_GPIO2_CFG0, TAC5211_PIN_DISABLED},
	{TAC5211_REG_GPO1_CFG0, TAC5211_PIN_DISABLED},
	{TAC5211_REG_INTF_CFG2, TAC5211_INTF_CFG2_PASI_DIN_ENABLE},
	{TAC5211_REG_INTF_CFG4, TAC5211_INTF_CFG4_ANALOG_CH1},
	{TAC5211_REG_ASI_CFG1, TAC5211_ASI_CFG1_SINGLE_DIN_DOUT},
	{TAC5211_REG_PASI_CFG0, TAC5211_PASI_CFG0_I2S_16_BIT},
	{TAC5211_REG_PASI_RX_CH1_CFG, TAC5211_PASI_RX_CH1_LEFT_SLOT0},
	{TAC5211_REG_PASI_RX_CH2_CFG, TAC5211_PASI_RX_CH2_RIGHT_SLOT0},
	{TAC5211_REG_CNT_CLK_CFG0, TAC5211_CNT_CLK_CFG0_DEFAULT},
	{TAC5211_REG_OUT1X_CFG0, TAC5211_OUT1X_DAC_STEREO_SINGLE_ENDED},
	{TAC5211_REG_OUT1X_CFG1, TAC5211_OUT1P_HEADPHONE_0_DB},
	{TAC5211_REG_OUT1X_CFG2, TAC5211_OUT1M_HEADPHONE_0_DB},
	{TAC5211_REG_DAC_CH1A_CFG0, TAC5211_DAC_DVOL_MUTE},
	{TAC5211_REG_DAC_CH1B_CFG0, TAC5211_DAC_DVOL_MUTE},
	{TAC5211_REG_ADC_CH1_CFG0, TAC5211_ADC_CH1_SINGLE_ENDED_10K},
	{TAC5211_REG_ADC_CH1_CFG2, TAC5211_ADC_DVOL_DEFAULT_REG},
	{TAC5211_REG_DSP_CFG1, TAC5211_DSP_CFG1_DAC_3_BIQUADS},
};

static const struct reg_config power_config[] = {
	{TAC5211_REG_PAGE_SEL, TAC5211_PAGE_CONTROL},
	{TAC5211_REG_CH_EN, TAC5211_CH_EN_INPUT1_OUTPUT1_OUTPUT2},
	{TAC5211_REG_PWR_CFG, TAC5211_PWR_FULL_DUPLEX},
};

static int playback_dvol_write_locked(const struct device *dev, uint8_t reg_val)
{
	int err = reg_select_page(dev, TAC5211_PAGE_CONTROL);

	if (err == 0) {
		err = reg_write(dev, TAC5211_REG_DAC_CH1A_CFG0, reg_val);
	}
	if (err == 0) {
		err = reg_write(dev, TAC5211_REG_DAC_CH1B_CFG0, reg_val);
	}

	return err;
}

static int playback_apply_locked(const struct device *dev)
{
	struct tac5211_data *data = dev->data;
	uint8_t reg_val;
	int err;

	if (!data->configured) {
		return 0;
	}

	reg_val = data->playback_muted ? 0 : playback_volume_reg(data->playback_volume_steps);

	err = playback_dvol_write_locked(dev, reg_val);
	if (err) {
		LOG_ERR("Failed to apply playback state: %d", err);
	}

	return err;
}

static int playback_eq_program_locked(const struct device *dev, bool dac_live)
{
	struct tac5211_data *data = dev->data;
	struct tac5211_biquad_coeffs coeffs[TAC5211_PLAYBACK_EQ_BAND_COUNT];
	int err = playback_eq_coeffs_from_state(data, coeffs);

	if (err) {
		return err;
	}

	if (!dac_live) {
		return playback_eq_apply_coeffs_locked(dev, coeffs);
	}

	err = playback_dvol_write_locked(dev, TAC5211_DAC_DVOL_MUTE);
	if (err) {
		LOG_ERR("Failed to mute DAC for EQ update: %d", err);
		return err;
	}

	err = playback_eq_write_biquads_locked(dev, coeffs);
	{
		const int restore_err = playback_apply_locked(dev);

		if (err == 0) {
			err = restore_err;
		}
	}
	if (err) {
		LOG_ERR("Failed to apply playback EQ coefficients: %d", err);
	}

	return err;
}

static int playback_volume_set_locked(const struct device *dev, int16_t steps)
{
	struct tac5211_data *data = dev->data;
	int16_t old_steps = data->playback_volume_steps;
	bool old_muted = data->playback_muted;
	int err;

	steps = CLAMP(steps, TAC5211_PLAYBACK_VOLUME_MIN_STEPS, TAC5211_PLAYBACK_VOLUME_MAX_STEPS);

	data->playback_volume_steps = steps;
	data->playback_muted = false;

	err = playback_apply_locked(dev);
	if (err) {
		data->playback_volume_steps = old_steps;
		data->playback_muted = old_muted;
		return err;
	}

	LOG_DBG("Playback volume set to %d half-dB steps", data->playback_volume_steps);
	return 0;
}

static int playback_mute_set_locked(const struct device *dev, bool mute)
{
	struct tac5211_data *data = dev->data;
	int err;

	if (mute == data->playback_muted) {
		return 0;
	}

	data->playback_muted = mute;

	err = playback_apply_locked(dev);
	if (err) {
		data->playback_muted = !mute;
		return err;
	}

	LOG_DBG("Playback %s", mute ? "muted" : "unmuted");
	return 0;
}

static bool channel_supported(audio_channel_t channel)
{
	return channel == AUDIO_CHANNEL_ALL;
}

static int tac5211_codec_configure(const struct device *dev, struct audio_codec_cfg *codec_cfg)
{
	const struct tac5211_config *config = dev->config;
	struct tac5211_data *data = dev->data;
	enum tac5211_capture_input configured_input = TAC5211_CAPTURE_INPUT_IN1P;
	bool eq_enabled = true;
	struct i2s_config *i2s_cfg;
	int err;

	if (codec_cfg == NULL) {
		return -EINVAL;
	}

	if (codec_cfg->dai_type != AUDIO_DAI_TYPE_I2S) {
		LOG_ERR("Only I2S DAI is supported");
		return -EINVAL;
	}

	if (codec_cfg->dai_route != AUDIO_ROUTE_PLAYBACK_CAPTURE) {
		LOG_ERR("Only playback+capture route is supported");
		return -EINVAL;
	}

	if (codec_cfg->mclk_freq != 0U && codec_cfg->mclk_freq != TAC5211_SUPPORTED_MCLK_HZ) {
		LOG_ERR("Unsupported MCLK %u Hz", codec_cfg->mclk_freq);
		return -EINVAL;
	}

	i2s_cfg = &codec_cfg->dai_cfg.i2s;
	if (i2s_cfg->word_size != TAC5211_SUPPORTED_WORD_SIZE_BITS || i2s_cfg->channels != 2U ||
	    i2s_cfg->frame_clk_freq != TAC5211_SUPPORTED_SAMPLE_RATE_HZ ||
	    (i2s_cfg->format & I2S_FMT_DATA_FORMAT_MASK) != I2S_FMT_DATA_FORMAT_I2S) {
		LOG_ERR("Unsupported I2S config: %u-bit, %u channels, %u Hz, format 0x%02x",
			i2s_cfg->word_size, i2s_cfg->channels, i2s_cfg->frame_clk_freq,
			i2s_cfg->format);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->configured = false;

	if (config->reset_gpio.port != NULL) {
		err = gpio_pin_set_dt(&config->reset_gpio, 1);
		if (err) {
			LOG_ERR("Failed to assert reset GPIO: %d", err);
			k_mutex_unlock(&data->lock);
			return err;
		}
		k_sleep(K_MSEC(1));
		err = gpio_pin_set_dt(&config->reset_gpio, 0);
		if (err) {
			LOG_ERR("Failed to release reset GPIO: %d", err);
			k_mutex_unlock(&data->lock);
			return err;
		}
		k_sleep(K_MSEC(2));
	}

	err = reg_select_page(dev, TAC5211_PAGE_CONTROL);
	if (err == 0) {
		err = reg_write(dev, TAC5211_REG_SW_RESET, 0x01);
	}
	if (err) {
		LOG_ERR("Reset failed: %d", err);
		k_mutex_unlock(&data->lock);
		return err;
	}

	k_sleep(K_MSEC(16)); /* register init time after reset (TI SBAA600: 16 ms) */

	/* Coefficient and channel config must wait 10 ms after wake-up */
	err = reg_write_page(dev, TAC5211_PAGE_CONTROL, TAC5211_REG_DEV_MISC_CFG,
			     TAC5211_DEV_MISC_ACTIVE_WITH_VREF);
	if (err == 0) {
		k_sleep(K_MSEC(10));
		err = write_reg_table(dev, init_config, ARRAY_SIZE(init_config));
	}
	if (err == 0) {
		err = capture_input_write_locked(dev);
	}
	if (err == 0) {
		err = playback_eq_program_locked(dev, false);
	}
	if (err == 0) {
		err = write_reg_table(dev, power_config, ARRAY_SIZE(power_config));
	}
	if (err == 0) {
		data->configured = true;
		err = playback_apply_locked(dev);
		configured_input = data->capture_input;
		eq_enabled = data->playback_eq_enabled;
	}
	k_mutex_unlock(&data->lock);

	if (err == 0) {
		LOG_INF("TAC5211 configured (48 kHz, 16-bit, playback+capture, mic=%s, "
			"mic gain=+%d.%d dB, eq=%s)",
			capture_input_name(configured_input),
			CONFIG_TAC5211_ADC_GAIN_HALF_DB / 2,
			(CONFIG_TAC5211_ADC_GAIN_HALF_DB % 2) * 5,
			eq_enabled ? "speaker" : "bypass");
	}

	return err;
}

static void tac5211_codec_start_output(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static void tac5211_codec_stop_output(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static int tac5211_codec_set_property(const struct device *dev, audio_property_t property,
				      audio_channel_t channel, audio_property_value_t val)
{
	struct tac5211_data *data = dev->data;
	int err;

	if (!channel_supported(channel)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		err = playback_volume_set_locked(dev, (int16_t)val.vol);
		break;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		err = playback_mute_set_locked(dev, val.mute);
		break;
	default:
		err = -ENOTSUP;
		break;
	}

	k_mutex_unlock(&data->lock);
	return err;
}

static int tac5211_codec_apply_properties(const struct device *dev)
{
	struct tac5211_data *data = dev->data;
	int err;

	k_mutex_lock(&data->lock, K_FOREVER);
	err = playback_apply_locked(dev);
	k_mutex_unlock(&data->lock);

	return err;
}

static int tac5211_init(const struct device *dev)
{
	const struct tac5211_config *config = dev->config;
	struct tac5211_data *data = dev->data;
	int err;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	if (config->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset_gpio)) {
			LOG_ERR("Reset GPIO not ready");
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("Failed to configure reset GPIO: %d", err);
			return err;
		}
	}

	k_mutex_init(&data->lock);
	data->playback_volume_steps = TAC5211_PLAYBACK_VOLUME_DEFAULT_STEPS;
	playback_eq_default(&data->playback_eq);
	data->playback_eq_preset = TAC5211_PLAYBACK_EQ_PRESET_FLAT;
	data->capture_input = TAC5211_CAPTURE_INPUT_IN1P;
	data->playback_eq_enabled = true;
	data->playback_muted = false;
	data->configured = false;

	return 0;
}

static const struct audio_codec_api tac5211_codec_api = {
	.configure = tac5211_codec_configure,
	.start_output = tac5211_codec_start_output,
	.stop_output = tac5211_codec_stop_output,
	.set_property = tac5211_codec_set_property,
	.apply_properties = tac5211_codec_apply_properties,
};

#define TAC5211_DEFINE(inst)                                                                       \
	static const struct tac5211_config tac5211_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                    \
	};                                                                                         \
	static struct tac5211_data tac5211_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, tac5211_init, NULL, &tac5211_data_##inst,                      \
			      &tac5211_config_##inst, POST_KERNEL, CONFIG_TAC5211_INIT_PRIORITY,   \
			      &tac5211_codec_api)

DT_INST_FOREACH_STATUS_OKAY(TAC5211_DEFINE)
