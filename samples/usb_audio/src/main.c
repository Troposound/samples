/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB Audio Class 2 soundcard on the TAC5211.
 *
 * Playback: USB OUT (48 kHz, 16-bit stereo) -> play ring -> I2S TX -> codec.
 * Capture:  codec ADC ch1 -> I2S RX left slot -> mic ring -> USB IN (mono).
 *
 * The I2S runs from the 12.288 MHz audio PLL, not from the USB SOF, so both
 * directions are asynchronous. The IN side sends 47/48/49 samples per frame
 * depending on the mic ring fill, so its rate tracks the I2S clock; with
 * implicit feedback the host paces OUT from it (the nRF5340 USBD has only one
 * isochronous IN endpoint, so there is no room for explicit feedback).
 *
 * Both rings are single-producer/single-consumer. The I2S ISR is one side of
 * each; the USB stack thread is the other.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/audio/codec.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include <troposound/audio_i2s.h>
#include <troposound/headset.h>
#include <troposound/volume.h>

LOG_MODULE_REGISTER(usb_audio, LOG_LEVEL_INF);

#define OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(out_terminal))
#define IN_TERMINAL_ID  UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))

#define FRAMES_PER_MS       (AUDIO_I2S_SAMPLE_RATE_HZ / 1000)
#define STEREO_FRAME_BYTES  (2 * sizeof(int16_t))
/* Async full-speed: up to one extra frame per USB frame. */
#define PLAY_MAX_BYTES      ((FRAMES_PER_MS + 1) * STEREO_FRAME_BYTES)
#define MIC_MAX_BYTES       ((FRAMES_PER_MS + 1) * sizeof(int16_t))

/* Ring sizes are powers of two; counters run free and wrap naturally. */
#define PLAY_RING_FRAMES    512U
#define MIC_RING_SAMPLES    512U
/* Wait for this much host audio before feeding the I2S. One I2S block is
 * drained at a time, so the fill swings by FRAMES_PER_MS around the mean.
 */
#define PLAY_PRIME_FRAMES   (3 * FRAMES_PER_MS)
#define MIC_TARGET_SAMPLES  (2 * FRAMES_PER_MS)

/* Encoder start-up volume; the R1 knob sets it from its position. */
#define VOLUME_START_HALF_DB (-20)


BUILD_ASSERT(AUDIO_I2S_BLOCK_WORDS == FRAMES_PER_MS,
	     "Set CONFIG_TROPOSOUND_AUDIO_I2S_BLOCK_WORDS=48 (1 ms blocks)");

/* Play ring: one uint32 per stereo frame, left in the low half-word. That is
 * also the USB byte order (L, R little-endian) and the I2S word layout.
 */
static uint32_t play_ring[PLAY_RING_FRAMES];
static atomic_t play_head; /* written by USB */
static atomic_t play_tail; /* written by I2S ISR */

static int16_t mic_ring[MIC_RING_SAMPLES];
static atomic_t mic_head; /* written by I2S ISR */
static atomic_t mic_tail; /* written by USB */

static atomic_t play_enabled;
static atomic_t mic_enabled;
static atomic_t underruns;
static bool play_primed; /* I2S ISR only */

static uint32_t i2s_tx[2][AUDIO_I2S_BLOCK_WORDS];
static uint32_t i2s_rx[2][AUDIO_I2S_BLOCK_WORDS];
static uint8_t i2s_next; /* I2S ISR only */

K_MEM_SLAB_DEFINE_STATIC(play_slab, ROUND_UP(PLAY_MAX_BYTES, UDC_BUF_GRANULARITY), 4,
			 UDC_BUF_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(mic_slab, ROUND_UP(MIC_MAX_BYTES, UDC_BUF_GRANULARITY), 4,
			 UDC_BUF_ALIGN);

static const struct device *const uac2_dev = DEVICE_DT_GET(DT_NODELABEL(uac2_headset));
static const struct device *const codec_dev = DEVICE_DT_GET_ONE(ti_tac5211);
static const struct gpio_dt_spec on_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_APP_USB_VID,
		   CONFIG_APP_USB_PID);
USBD_DESC_LANG_DEFINE(desc_lang);
USBD_DESC_MANUFACTURER_DEFINE(desc_mfr, "Troposound");
USBD_DESC_PRODUCT_DEFINE(desc_product, CONFIG_APP_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(desc_serial);
USBD_CONFIGURATION_DEFINE(fs_config, 0, 100, NULL);

static uint32_t play_fill(void)
{
	return (uint32_t)atomic_get(&play_head) - (uint32_t)atomic_get(&play_tail);
}

static uint32_t mic_fill(void)
{
	return (uint32_t)atomic_get(&mic_head) - (uint32_t)atomic_get(&mic_tail);
}

/* ---- I2S side (ISR) ---- */

static void fill_tx(uint32_t *tx)
{
	uint32_t tail = (uint32_t)atomic_get(&play_tail);
	uint32_t avail;

	if (!atomic_get(&play_enabled)) {
		/* Consumer-side flush, so the next stream starts empty. */
		atomic_set(&play_tail, atomic_get(&play_head));
		play_primed = false;
		memset(tx, 0, AUDIO_I2S_BLOCK_WORDS * sizeof(uint32_t));
		return;
	}

	avail = (uint32_t)atomic_get(&play_head) - tail;

	if (!play_primed) {
		if (avail < PLAY_PRIME_FRAMES) {
			memset(tx, 0, AUDIO_I2S_BLOCK_WORDS * sizeof(uint32_t));
			return;
		}
		play_primed = true;
	}

	if (avail < AUDIO_I2S_BLOCK_WORDS) {
		/* Underrun: play what is there, pad, and re-prime. */
		atomic_inc(&underruns);
		play_primed = false;
	}

	uint32_t n = MIN(avail, AUDIO_I2S_BLOCK_WORDS);

	for (uint32_t i = 0; i < n; i++) {
		tx[i] = play_ring[(tail + i) & (PLAY_RING_FRAMES - 1)];
	}
	memset(&tx[n], 0, (AUDIO_I2S_BLOCK_WORDS - n) * sizeof(uint32_t));
	atomic_set(&play_tail, tail + n);
}

static void push_mic(uint32_t *rx)
{
	uint32_t head = (uint32_t)atomic_get(&mic_head);
	uint32_t space = MIC_RING_SAMPLES - (head - (uint32_t)atomic_get(&mic_tail));
	uint32_t n = MIN(space, AUDIO_I2S_BLOCK_WORDS);

	/* ADC channel 1 sits in the left slot (low half-word). */
	headset_mic_gain((int16_t *)rx, n, 2);
	for (uint32_t i = 0; i < n; i++) {
		mic_ring[(head + i) & (MIC_RING_SAMPLES - 1)] = (int16_t)(rx[i] & 0xFFFF);
	}
	atomic_set(&mic_head, head + n);
}

static void i2s_cb(uint32_t *released_rx, const uint32_t *released_tx)
{
	ARG_UNUSED(released_tx);

	if (released_rx != NULL) {
		push_mic(released_rx);
	}

	fill_tx(i2s_tx[i2s_next]);
	(void)audio_i2s_set_next_buf(i2s_tx[i2s_next], i2s_rx[i2s_next]);
	i2s_next ^= 1U;
}

/* ---- USB side (USBD thread) ---- */

static void terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
			       bool microframes, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);

	if (terminal == OUT_TERMINAL_ID) {
		atomic_set(&play_enabled, enabled);
		headset_speaker_set(enabled);
		LOG_INF("Playback stream %s", enabled ? "started" : "stopped");
	} else if (terminal == IN_TERMINAL_ID) {
		/* Consumer-side flush: start capture with fresh samples. */
		atomic_set(&mic_tail, atomic_get(&mic_head));
		atomic_set(&mic_enabled, enabled);
		LOG_INF("Capture stream %s", enabled ? "started" : "stopped");
	}
}

static void *get_recv_buf(const struct device *dev, uint8_t terminal, uint16_t size,
			  void *user_data)
{
	void *buf;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (terminal != OUT_TERMINAL_ID || size > PLAY_MAX_BYTES) {
		return NULL;
	}

	if (k_mem_slab_alloc(&play_slab, &buf, K_NO_WAIT) != 0) {
		return NULL;
	}

	return buf;
}

static void data_recv_cb(const struct device *dev, uint8_t terminal, void *buf, uint16_t size,
			 void *user_data)
{
	const uint32_t *frames = buf;
	uint32_t head = (uint32_t)atomic_get(&play_head);
	uint32_t space = PLAY_RING_FRAMES - (head - (uint32_t)atomic_get(&play_tail));
	uint32_t n = MIN(size / STEREO_FRAME_BYTES, space);

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (atomic_get(&play_enabled)) {
		for (uint32_t i = 0; i < n; i++) {
			play_ring[(head + i) & (PLAY_RING_FRAMES - 1)] = frames[i];
		}
		atomic_set(&play_head, head + n);
	}

	k_mem_slab_free(&play_slab, buf);
}

static void sof_cb(const struct device *dev, void *user_data)
{
	uint32_t fill = mic_fill();
	uint32_t tail = (uint32_t)atomic_get(&mic_tail);
	uint32_t n = FRAMES_PER_MS;
	uint32_t avail;
	int16_t *buf;

	ARG_UNUSED(user_data);

	if (!atomic_get(&mic_enabled)) {
		return;
	}

	/* Hold the ring around its target: one sample more or less per ms. */
	if (fill > MIC_TARGET_SAMPLES + FRAMES_PER_MS / 2) {
		n++;
	} else if (fill < MIC_TARGET_SAMPLES - FRAMES_PER_MS / 2) {
		n--;
	}

	if (k_mem_slab_alloc(&mic_slab, (void **)&buf, K_NO_WAIT) != 0) {
		return;
	}

	avail = MIN(fill, n);
	for (uint32_t i = 0; i < avail; i++) {
		buf[i] = mic_ring[(tail + i) & (MIC_RING_SAMPLES - 1)];
	}
	memset(&buf[avail], 0, (n - avail) * sizeof(int16_t));
	atomic_set(&mic_tail, tail + avail);

	if (usbd_uac2_send(dev, IN_TERMINAL_ID, buf, n * sizeof(int16_t)) != 0) {
		k_mem_slab_free(&mic_slab, buf);
	}
}

static void buf_release_cb(const struct device *dev, uint8_t terminal, void *buf, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (terminal == IN_TERMINAL_ID) {
		k_mem_slab_free(&mic_slab, buf);
	}
}

static const struct uac2_ops uac2_ops = {
	.sof_cb = sof_cb,
	.terminal_update_cb = terminal_update_cb,
	.get_recv_buf = get_recv_buf,
	.data_recv_cb = data_recv_cb,
	.buf_release_cb = buf_release_cb,
};

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	LOG_INF("USB: %s", usbd_msg_type_string(msg->type));

	if (!usbd_can_detect_vbus(ctx)) {
		return;
	}

	if (msg->type == USBD_MSG_VBUS_READY) {
		if (usbd_enable(ctx) != 0) {
			LOG_ERR("usbd_enable failed");
		}
	} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
		if (usbd_disable(ctx) != 0) {
			LOG_ERR("usbd_disable failed");
		}
	}
}

static int usb_init(void)
{
	int ret;

	usbd_uac2_set_ops(uac2_dev, &uac2_ops, NULL);

	ret = usbd_add_descriptor(&usbd_ctx, &desc_lang);
	ret = ret ? ret : usbd_add_descriptor(&usbd_ctx, &desc_mfr);
	ret = ret ? ret : usbd_add_descriptor(&usbd_ctx, &desc_product);
	ret = ret ? ret : usbd_add_descriptor(&usbd_ctx, &desc_serial);
	ret = ret ? ret : usbd_add_configuration(&usbd_ctx, USBD_SPEED_FS, &fs_config);
	ret = ret ? ret : usbd_register_all_classes(&usbd_ctx, USBD_SPEED_FS, 1, NULL);
	if (ret) {
		LOG_ERR("USB descriptor/class setup failed: %d", ret);
		return ret;
	}

	/* UAC2 uses an Interface Association Descriptor. */
	usbd_device_set_code_triple(&usbd_ctx, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	ret = usbd_msg_register_cb(&usbd_ctx, usbd_msg_cb);
	ret = ret ? ret : usbd_init(&usbd_ctx);
	if (ret) {
		LOG_ERR("USB init failed: %d", ret);
		return ret;
	}

	if (!usbd_can_detect_vbus(&usbd_ctx)) {
		ret = usbd_enable(&usbd_ctx);
	}

	return ret;
}

/* ---- Audio bring-up ---- */

static int audio_init(void)
{
	struct audio_codec_cfg cfg = {
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE,
		.mclk_freq = 0,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = 2,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			.frame_clk_freq = AUDIO_I2S_SAMPLE_RATE_HZ,
		},
	};
	int ret;

	if (!device_is_ready(codec_dev)) {
		LOG_ERR("TAC5211 not ready");
		return -ENODEV;
	}

	ret = audio_i2s_init();
	if (ret) {
		LOG_ERR("I2S init failed: %d", ret);
		return ret;
	}

	audio_i2s_callback_set(i2s_cb);
	ret = audio_i2s_start(i2s_tx[0], i2s_rx[0]);
	if (ret) {
		LOG_ERR("I2S start failed: %d", ret);
		return ret;
	}
	i2s_next = 1U;

	/* The codec auto-detects its clocks from BCLK/FSYNC, so the I2S must
	 * already be running.
	 */
	ret = audio_codec_configure(codec_dev, &cfg);
	if (ret) {
		LOG_ERR("Codec configure failed: %d", ret);
		return ret;
	}

	audio_codec_start_output(codec_dev);

	ret = headset_start(codec_dev);
	if (ret) {
		return ret;
	}

	return volume_start(codec_dev, VOLUME_START_HALF_DB);
}

int main(void)
{
	int ret;

	LOG_INF("USB audio on %s", CONFIG_BOARD_TARGET);

	ret = audio_init();
	if (ret) {
		return ret;
	}

	ret = usb_init();
	if (ret) {
		return ret;
	}

	(void)gpio_pin_configure_dt(&on_led, GPIO_OUTPUT_ACTIVE);

	while (true) {
		uint32_t under, late;

		k_sleep(K_SECONDS(1));

		under = (uint32_t)atomic_set(&underruns, 0);
		late = audio_i2s_late_count_reset();

		if (atomic_get(&play_enabled) || atomic_get(&mic_enabled)) {
			LOG_INF("play=%d mic=%d underruns=%u late=%u play_fill=%u mic_fill=%u",
				(int)atomic_get(&play_enabled), (int)atomic_get(&mic_enabled), under,
				late, play_fill(), mic_fill());
		}
	}
}
