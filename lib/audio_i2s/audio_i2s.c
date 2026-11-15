/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <troposound/audio_i2s.h>

#include <errno.h>

#include <hal/nrf_clock.h>
#include <nrfx_i2s.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define I2S_NODE              DT_NODELABEL(i2s0)
#define HFCLKAUDIO_12_288_MHZ 0x9BA6U

PINCTRL_DT_DEFINE(I2S_NODE);
static nrfx_i2s_t i2s_inst = NRFX_I2S_INSTANCE(DT_REG_ADDR(I2S_NODE));

static audio_i2s_callback_t i2s_callback;
static bool i2s_started;
static atomic_t i2s_late_cnt = ATOMIC_INIT(0);

/* nRF5340 I2S master: stereo, 16-bit, 48 kHz */
static nrfx_i2s_config_t i2s_cfg = {
	.skip_gpio_cfg = true,
	.skip_psel_cfg = true,
	.irq_priority = DT_IRQ(I2S_NODE, priority),
	.mode = NRF_I2S_MODE_MASTER,
	.format = NRF_I2S_FORMAT_I2S,
	.alignment = NRF_I2S_ALIGN_LEFT,
	.sample_width = NRF_I2S_SWIDTH_16BIT,
	.channels = NRF_I2S_CHANNELS_STEREO,
	.clksrc = NRF_I2S_CLKSRC_ACLK,
	.prescalers = {
		.mck_setup = NRF_I2S_MCK_32MDIV2, /* MCLK = 12.288 MHz from ACLK */
		.ratio = NRF_I2S_RATIO_256X,      /* LRCK = 48 kHz (12.288M / 256) */
		.enable_bypass = true,
	},
};

static void i2s_irq_handler(const void *arg)
{
	nrfx_i2s_irq_handler(&i2s_inst);
}

static void i2s_data_handler(nrfx_i2s_buffers_t const *bufs, uint32_t status)
{
	if (!(status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) || !i2s_callback) {
		return;
	}

	/* Late: nrfx reused the in-flight pair. Queue a fresh one to recover. */
	if (bufs == NULL) {
		atomic_inc(&i2s_late_cnt);
		i2s_callback(NULL, NULL);
		return;
	}

	i2s_callback(bufs->p_rx_buffer, bufs->p_tx_buffer);
}

uint32_t audio_i2s_late_count_reset(void)
{
	return (uint32_t)atomic_set(&i2s_late_cnt, 0);
}

int audio_i2s_init(void)
{
	int ret;

	nrf_clock_hfclkaudio_config_set(NRF_CLOCK, HFCLKAUDIO_12_288_MHZ);
	nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKAUDIOSTARTED);
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKAUDIOSTART);

	for (int i = 0; !nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKAUDIOSTARTED); i++) {
		if (i >= 1000) {
			return -ETIMEDOUT;
		}
		k_busy_wait(10);
	}

	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(I2S_NODE), PINCTRL_STATE_DEFAULT);
	if (ret) {
		return ret;
	}

	IRQ_CONNECT(DT_IRQN(I2S_NODE), DT_IRQ(I2S_NODE, priority), i2s_irq_handler, NULL, 0);
	irq_enable(DT_IRQN(I2S_NODE));

	return nrfx_i2s_init(&i2s_inst, &i2s_cfg, i2s_data_handler);
}

void audio_i2s_callback_set(audio_i2s_callback_t callback)
{
	i2s_callback = callback;
}

int audio_i2s_start(const uint32_t *tx_buf, uint32_t *rx_buf)
{
	int ret;

	if (!tx_buf || !rx_buf) {
		return -EINVAL;
	}

	const nrfx_i2s_buffers_t bufs = {
		.p_tx_buffer = tx_buf,
		.p_rx_buffer = rx_buf,
		.buffer_size = AUDIO_I2S_BLOCK_WORDS,
	};

	i2s_started = true;
	ret = nrfx_i2s_start(&i2s_inst, &bufs, 0);
	if (ret) {
		i2s_started = false;
	}

	return ret;
}

int audio_i2s_set_next_buf(const uint32_t *tx_buf, uint32_t *rx_buf)
{
	if (!i2s_started) {
		return -EIO;
	}

	if (!tx_buf || !rx_buf) {
		return -EINVAL;
	}

	const nrfx_i2s_buffers_t bufs = {
		.p_tx_buffer = tx_buf,
		.p_rx_buffer = rx_buf,
		.buffer_size = AUDIO_I2S_BLOCK_WORDS,
	};

	return nrfx_i2s_next_buffers_set(&i2s_inst, &bufs);
}

void audio_i2s_stop(void)
{
	if (i2s_started) {
		nrfx_i2s_stop(&i2s_inst);
		i2s_started = false;
	}
}

void audio_i2s_uninit(void)
{
	audio_i2s_stop();
	nrfx_i2s_uninit(&i2s_inst);
}
