/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB Audio Class 2 speaker: 48 kHz 16-bit stereo OUT, scaled by the volume
 * control into a ring that the LC3 encoders drain one 10 ms frame per BIG
 * event.
 *
 * The BIG clocks the audio out, so the explicit feedback steers the host to
 * its rate: the ring level right after each frame is taken (the low point of
 * the sawtooth) is held at FB_TARGET.
 *
 * The ring is single-producer/single-consumer: the USB stack thread writes,
 * the encoder thread reads.
 */

#include <math.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include "usb_broadcast.h"

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

#define OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(out_terminal))

#define FRAMES_PER_MS      (PCM_RATE_HZ / 1000)
#define STEREO_FRAME_BYTES (2 * sizeof(int16_t))
/* Async full-speed: up to one extra frame per USB frame. */
#define RECV_MAX_BYTES     ((FRAMES_PER_MS + 1) * STEREO_FRAME_BYTES)

/* Power of two; counters run free and wrap naturally. */
#define RING_FRAMES 2048U

/* Q10.14 samples per USB frame. One LSB is 1.27 ppm. */
#define FB_FRAC_BITS  14
#define FB_NOMINAL    (FRAMES_PER_MS << FB_FRAC_BITS)
#define FB_TARGET     (5 * FRAMES_PER_MS)
#define FB_WARMUP     20 /* frames after priming before the level counts */
#define FB_EWMA_SHIFT 8  /* 256 ms at one update per SOF */
#define FB_GAIN       4  /* LSB per sample of error: about 4 s to settle */
#define FB_MAX_DEV    2048

#define GAIN_SHIFT 14
#define GAIN_UNITY BIT(GAIN_SHIFT)

BUILD_ASSERT(FRAME_SAMPLES + FB_TARGET < RING_FRAMES / 2, "Ring too small");

/* One word per stereo frame, left in the low half-word as on the wire. */
static uint32_t ring[RING_FRAMES];
static atomic_t ring_head; /* written by USB */
static atomic_t ring_tail; /* written by the encoder */

static atomic_t stream_on;
static atomic_t gain_q14 = ATOMIC_INIT(GAIN_UNITY);
static atomic_t trough = ATOMIC_INIT(-1); /* -1 until primed and settled */
static atomic_t fb_dev;
static atomic_t underruns;
static atomic_t overruns;

static bool primed;     /* encoder only */
static int settle;      /* encoder only */
static int32_t err_acc; /* USB only: filtered error << FB_EWMA_SHIFT */

K_MEM_SLAB_DEFINE_STATIC(recv_slab, ROUND_UP(RECV_MAX_BYTES, UDC_BUF_GRANULARITY), 4,
			 UDC_BUF_ALIGN);

static const struct device *const uac2_dev = DEVICE_DT_GET(DT_NODELABEL(uac2_speaker));

USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_APP_USB_VID,
		   CONFIG_APP_USB_PID);
USBD_DESC_LANG_DEFINE(desc_lang);
USBD_DESC_MANUFACTURER_DEFINE(desc_mfr, "Troposound");
USBD_DESC_PRODUCT_DEFINE(desc_product, CONFIG_APP_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(desc_serial);
USBD_CONFIGURATION_DEFINE(fs_config, 0, 100, NULL);

void usb_volume_set(int half_db)
{
	atomic_set(&gain_q14, lroundf(GAIN_UNITY * powf(10.0f, half_db / 40.0f)));
}

/* ---- Encoder side ---- */

void usb_frame_get(uint32_t *pcm)
{
	uint32_t head = (uint32_t)atomic_get(&ring_head);
	uint32_t tail = (uint32_t)atomic_get(&ring_tail);
	uint32_t avail = head - tail;

	if (!atomic_get(&stream_on)) {
		/* Consumer-side flush, so the next stream starts empty. */
		atomic_set(&ring_tail, head);
		primed = false;
	} else if (!primed && avail >= FRAME_SAMPLES + FB_TARGET) {
		primed = true;
		settle = FB_WARMUP;
	} else if (primed && avail < FRAME_SAMPLES) {
		atomic_inc(&underruns);
		primed = false;
	}

	if (!primed) {
		atomic_set(&trough, -1);
		memset(pcm, 0, FRAME_SAMPLES * sizeof(uint32_t));
		return;
	}

	for (uint32_t i = 0; i < FRAME_SAMPLES; i++) {
		pcm[i] = ring[(tail + i) & (RING_FRAMES - 1)];
	}
	atomic_set(&ring_tail, tail + FRAME_SAMPLES);

	if (settle > 0) {
		settle--;
	}
	atomic_set(&trough, settle > 0 ? -1 : (atomic_val_t)(avail - FRAME_SAMPLES));
}

/* ---- USB side (USBD thread) ---- */

static void sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
}

static void terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
			       bool microframes, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);

	if (terminal == OUT_TERMINAL_ID) {
		atomic_set(&stream_on, enabled);
		LOG_INF("Host stream %s", enabled ? "started" : "stopped");
	}
}

static void *get_recv_buf(const struct device *dev, uint8_t terminal, uint16_t size,
			  void *user_data)
{
	void *buf;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (terminal != OUT_TERMINAL_ID || size > RECV_MAX_BYTES) {
		return NULL;
	}

	if (k_mem_slab_alloc(&recv_slab, &buf, K_NO_WAIT) != 0) {
		return NULL;
	}

	return buf;
}

static uint16_t scale(int16_t sample, int32_t gain)
{
	int32_t v = ((int32_t)sample * gain) >> GAIN_SHIFT;

	return (uint16_t)CLAMP(v, INT16_MIN, INT16_MAX);
}

static void data_recv_cb(const struct device *dev, uint8_t terminal, void *buf, uint16_t size,
			 void *user_data)
{
	const int16_t *pcm = buf;
	const int32_t gain = (int32_t)atomic_get(&gain_q14);
	uint32_t head = (uint32_t)atomic_get(&ring_head);
	uint32_t space = RING_FRAMES - (head - (uint32_t)atomic_get(&ring_tail));
	uint32_t n = size / STEREO_FRAME_BYTES;

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (atomic_get(&stream_on)) {
		if (n > space) {
			atomic_inc(&overruns);
			n = space;
		}

		for (uint32_t i = 0; i < n; i++) {
			ring[(head + i) & (RING_FRAMES - 1)] =
				scale(pcm[2 * i], gain) |
				((uint32_t)scale(pcm[2 * i + 1], gain) << 16);
		}
		atomic_set(&ring_head, head + n);
	}

	k_mem_slab_free(&recv_slab, buf);
}

static uint32_t feedback_cb(const struct device *dev, uint8_t terminal, void *user_data)
{
	int32_t fill = (int32_t)atomic_get(&trough);
	int32_t dev_lsb;

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (fill < 0) {
		err_acc = 0;
		atomic_set(&fb_dev, 0);
		return FB_NOMINAL;
	}

	err_acc += (fill - FB_TARGET) - (err_acc >> FB_EWMA_SHIFT);

	/* Above target the host is ahead of the BIG, so ask it for less. */
	dev_lsb = CLAMP(-(err_acc >> FB_EWMA_SHIFT) * FB_GAIN, -FB_MAX_DEV, FB_MAX_DEV);
	atomic_set(&fb_dev, dev_lsb);

	return FB_NOMINAL + dev_lsb;
}

static const struct uac2_ops uac2_ops = {
	.sof_cb = sof_cb,
	.terminal_update_cb = terminal_update_cb,
	.get_recv_buf = get_recv_buf,
	.data_recv_cb = data_recv_cb,
	.feedback_cb = feedback_cb,
};

void usb_stats_log(void)
{
	if (!atomic_get(&stream_on)) {
		return;
	}

	LOG_INF("fill=%d feedback=%+d ppm underruns=%u overruns=%u", (int)atomic_get(&trough),
		(int)(atomic_get(&fb_dev) * 1000000LL / FB_NOMINAL),
		(uint32_t)atomic_set(&underruns, 0), (uint32_t)atomic_set(&overruns, 0));
}

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

int usb_start(void)
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
