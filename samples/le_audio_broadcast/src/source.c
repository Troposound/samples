/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* LE Audio broadcast source: TAC5211 mic (left I2S slot, 48 kHz) -> LC3 mono 10 ms ->
 * one BIS, advertised as a Public Broadcast (PBP) with a Broadcast_Name.
 */

#include <string.h>

#include <lc3.h>
#include <troposound/audio_i2s.h>
#include <troposound/headset.h>
#include <zephyr/audio/codec.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/audio/pbp.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include "broadcast.h"

LOG_MODULE_REGISTER(broadcast_source, LOG_LEVEL_INF);

BUILD_ASSERT(IN_RANGE(sizeof(CONFIG_BROADCAST_NAME) - 1, BT_AUDIO_BROADCAST_NAME_LEN_MIN,
		      BT_AUDIO_BROADCAST_NAME_LEN_MAX),
	     "Invalid broadcast name length");

#define PCM_RATE_HZ      48000
#define FRAME_US         10000
#define FRAME_SAMPLES    (PCM_RATE_HZ / 100) /* 480 stereo frames per 10 ms at 48 kHz */
#define TX_BUF_COUNT     CONFIG_BT_ISO_TX_BUF_COUNT
#define PCM_BLOCK_COUNT  4

#if defined(CONFIG_BROADCAST_SAMPLE_RATE_48K)
static struct bt_bap_lc3_preset preset = BT_BAP_LC3_BROADCAST_PRESET_48_2_1(
	BT_AUDIO_LOCATION_MONO_AUDIO, BT_AUDIO_CONTEXT_TYPE_MEDIA);
#define LC3_RATE_HZ  48000
#define PBP_FEATURES BT_PBP_ANNOUNCEMENT_FEATURE_HIGH_QUALITY
#else
static struct bt_bap_lc3_preset preset = BT_BAP_LC3_BROADCAST_PRESET_24_2_1(
	BT_AUDIO_LOCATION_MONO_AUDIO, BT_AUDIO_CONTEXT_TYPE_MEDIA);
#define LC3_RATE_HZ  24000
#define PBP_FEATURES BT_PBP_ANNOUNCEMENT_FEATURE_STANDARD_QUALITY
#endif

BUILD_ASSERT(AUDIO_I2S_BLOCK_WORDS == FRAME_SAMPLES, "I2S block must be one 10 ms LC3 frame");

NET_BUF_POOL_FIXED_DEFINE(tx_pool, TX_BUF_COUNT, BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct bt_bap_stream stream;
static struct bt_bap_broadcast_source *broadcast_source;
static volatile bool streaming;
static uint16_t seq_num;

static lc3_encoder_mem_48k_t lc3_mem;
static lc3_encoder_t lc3_enc;

/* Interleaved stereo 16-bit PCM blocks; the encoder reads the left slot. */
static uint32_t pcm_blocks[PCM_BLOCK_COUNT][FRAME_SAMPLES];
K_MSGQ_DEFINE(pcm_q, sizeof(uint32_t *), PCM_BLOCK_COUNT, sizeof(uint32_t *));

static K_SEM_DEFINE(sem_started, 0, 1);

static uint32_t tx_silence[FRAME_SAMPLES]; /* EasyDMA reads RAM only */
static const uint32_t mute_silence[FRAME_SAMPLES]; /* encoded while muted */
static size_t next_rx;

static void i2s_cb(uint32_t *released_rx, const uint32_t *released_tx)
{
	ARG_UNUSED(released_tx);

	if (released_rx != NULL) {
		/* Full: drop the block, the encoder is behind. */
		(void)k_msgq_put(&pcm_q, &released_rx, K_NO_WAIT);
	}

	(void)audio_i2s_set_next_buf(tx_silence, pcm_blocks[next_rx]);
	next_rx = (next_rx + 1) % PCM_BLOCK_COUNT;
}

static int audio_in_start(void)
{
	const struct device *codec = DEVICE_DT_GET(DT_NODELABEL(tac5211));
	struct audio_codec_cfg cfg = {
		.mclk_freq = 0,
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = 2,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			.frame_clk_freq = PCM_RATE_HZ,
		},
	};
	int err;

	if (!device_is_ready(codec)) {
		LOG_ERR("TAC5211 not ready");
		return -ENODEV;
	}

	err = audio_i2s_init();
	if (err) {
		LOG_ERR("I2S init failed (%d)", err);
		return err;
	}

	audio_i2s_callback_set(i2s_cb);

	/* First pair goes to start(), the second is queued from the first callback. */
	next_rx = 1;
	err = audio_i2s_start(tx_silence, pcm_blocks[0]);
	if (err) {
		LOG_ERR("I2S start failed (%d)", err);
		return err;
	}

	/* Codec auto-detects its clocks from BCLK/FSYNC, so configure after start. */
	err = audio_codec_configure(codec, &cfg);
	if (err) {
		LOG_ERR("TAC5211 configure failed (%d)", err);
		return err;
	}

	audio_codec_start_output(codec);
	controls_start(codec);
	return 0;
}

static void encoder_thread(void *p1, void *p2, void *p3)
{
	const int octets = preset.qos.sdu;
	uint8_t sdu[CONFIG_BT_ISO_TX_MTU];
	uint32_t *block;
	uint32_t sent = 0;
	uint32_t dropped = 0;

	while (true) {
		k_msgq_get(&pcm_q, &block, K_FOREVER);

		if (!streaming) {
			continue;
		}

		/* Muted: keep the stream running, send silence. */
		const uint32_t *pcm = controls_muted() ? mute_silence : block;

		/* stride 2 = left slot of each stereo word (mic is on ADC channel 1) */
		headset_mic_gain((int16_t *)block, FRAME_SAMPLES, 2);

		if (lc3_encode(lc3_enc, LC3_PCM_FORMAT_S16, (const int16_t *)pcm, 2, octets,
			       sdu) != 0) {
			LOG_ERR("lc3_encode failed");
			continue;
		}

		struct net_buf *buf = net_buf_alloc(&tx_pool, K_NO_WAIT);

		if (buf == NULL) {
			/* Controller queue full: I2S clock ran ahead of the ISO clock. */
			dropped++;
			continue;
		}

		net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
		net_buf_add_mem(buf, sdu, octets);

		int err = bt_bap_stream_send(&stream, buf, seq_num++);

		if (err) {
			net_buf_unref(buf);
			dropped++;
			continue;
		}

		if ((++sent % 1000U) == 0U) {
			LOG_INF("Sent %u SDUs, dropped %u", sent, dropped);
		}
	}
}

K_THREAD_DEFINE(encoder_tid, 4096, encoder_thread, NULL, NULL, NULL, 5, 0, 0);

static void stream_started_cb(struct bt_bap_stream *s)
{
	seq_num = 0;
	streaming = true;
	led_state_set(LED_STATE_STREAMING);
	LOG_INF("BIS started");
}

static void stream_stopped_cb(struct bt_bap_stream *s, uint8_t reason)
{
	streaming = false;
	led_state_set(LED_STATE_SEARCHING);
	LOG_INF("BIS stopped (0x%02x)", reason);
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started_cb,
	.stopped = stream_stopped_cb,
};

static void source_started_cb(struct bt_bap_broadcast_source *source)
{
	k_sem_give(&sem_started);
}

static struct bt_bap_broadcast_source_cb source_cb = {
	.started = source_started_cb,
};

static int create_source(void)
{
	struct bt_bap_broadcast_source_stream_param stream_param = {
		.stream = &stream,
	};
	struct bt_bap_broadcast_source_subgroup_param subgroup_param = {
		.params_count = 1,
		.params = &stream_param,
		.codec_cfg = &preset.codec_cfg,
	};
	struct bt_bap_broadcast_source_param param = {
		.params_count = 1,
		.params = &subgroup_param,
		.qos = &preset.qos,
		.encryption = false,
		.packing = BT_ISO_PACKING_SEQUENTIAL,
	};

	bt_bap_stream_cb_register(&stream, &stream_ops);

	return bt_bap_broadcast_source_create(&param, &broadcast_source);
}

static int start_advertising(void)
{
	NET_BUF_SIMPLE_DEFINE(id_buf, BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
	NET_BUF_SIMPLE_DEFINE(pba_buf, BT_UUID_SIZE_16 + 2 + 16);
	NET_BUF_SIMPLE_DEFINE(base_buf, 128);
	struct bt_le_ext_adv *adv;
	struct bt_data ext_ad[3];
	struct bt_data per_ad;
	uint32_t broadcast_id;
	int err;

	err = bt_le_ext_adv_create(BT_BAP_ADV_PARAM_BROADCAST_FAST, NULL, &adv);
	if (err) {
		LOG_ERR("ext_adv_create failed (%d)", err);
		return err;
	}

	err = bt_le_per_adv_set_param(adv, BT_BAP_PER_ADV_PARAM_BROADCAST_FAST);
	if (err) {
		LOG_ERR("per_adv_set_param failed (%d)", err);
		return err;
	}

	err = bt_rand(&broadcast_id, BT_AUDIO_BROADCAST_ID_SIZE);
	if (err) {
		return err;
	}
	broadcast_id &= BIT_MASK(24);

	/* Broadcast Audio Announcement (BAP) */
	net_buf_simple_add_le16(&id_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&id_buf, broadcast_id);
	ext_ad[0] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, id_buf.data, id_buf.len);

	/* Public Broadcast Announcement (PBP), no extra metadata */
	err = bt_pbp_get_announcement(NULL, 0, PBP_FEATURES, &pba_buf);
	if (err) {
		LOG_ERR("PBP announcement failed (%d)", err);
		return err;
	}
	ext_ad[1] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, pba_buf.data, pba_buf.len);

	ext_ad[2] = (struct bt_data)BT_DATA(BT_DATA_BROADCAST_NAME, CONFIG_BROADCAST_NAME,
					    sizeof(CONFIG_BROADCAST_NAME) - 1);

	err = bt_le_ext_adv_set_data(adv, ext_ad, ARRAY_SIZE(ext_ad), NULL, 0);
	if (err) {
		LOG_ERR("ext_adv_set_data failed (%d)", err);
		return err;
	}

	err = bt_bap_broadcast_source_get_base(broadcast_source, &base_buf);
	if (err) {
		LOG_ERR("get_base failed (%d)", err);
		return err;
	}

	per_ad = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, base_buf.data, base_buf.len);
	err = bt_le_per_adv_set_data(adv, &per_ad, 1);
	if (err) {
		LOG_ERR("per_adv_set_data failed (%d)", err);
		return err;
	}

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		LOG_ERR("ext_adv_start failed (%d)", err);
		return err;
	}

	err = bt_le_per_adv_start(adv);
	if (err) {
		LOG_ERR("per_adv_start failed (%d)", err);
		return err;
	}

	err = bt_bap_broadcast_source_start(broadcast_source, adv);
	if (err) {
		LOG_ERR("broadcast_source_start failed (%d)", err);
		return err;
	}

	LOG_INF("Advertising \"%s\", broadcast ID 0x%06x, %d Hz LC3, %u octets",
		CONFIG_BROADCAST_NAME, broadcast_id, LC3_RATE_HZ, preset.qos.sdu);
	return 0;
}

int role_run(void)
{
	int err;

	led_state_set(LED_STATE_SEARCHING);

	lc3_enc = lc3_setup_encoder(FRAME_US, LC3_RATE_HZ, PCM_RATE_HZ, &lc3_mem);
	if (lc3_enc == NULL) {
		LOG_ERR("LC3 encoder setup failed");
		return -EINVAL;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = bt_bap_broadcast_source_register_cb(&source_cb);
	if (err) {
		return err;
	}

	err = create_source();
	if (err) {
		LOG_ERR("broadcast_source_create failed (%d)", err);
		return err;
	}

	err = start_advertising();
	if (err) {
		return err;
	}

	k_sem_take(&sem_started, K_FOREVER);

	return audio_in_start();
}
