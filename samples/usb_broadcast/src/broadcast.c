/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Each sent left SDU releases the next frame pair, so encoding runs on the BIG clock */

#include <lc3.h>
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
#include <zephyr/sys/atomic.h>

#include "usb_broadcast.h"

LOG_MODULE_REGISTER(broadcast, LOG_LEVEL_INF);

BUILD_ASSERT(IN_RANGE(sizeof(CONFIG_BROADCAST_NAME) - 1, BT_AUDIO_BROADCAST_NAME_LEN_MIN,
		      BT_AUDIO_BROADCAST_NAME_LEN_MAX),
	     "Invalid broadcast name length");

#define CHANNELS     2
#define TX_IN_FLIGHT 2

BUILD_ASSERT(CONFIG_BT_ISO_TX_BUF_COUNT >= CHANNELS * (TX_IN_FLIGHT + 1),
	     "Need a spare ISO TX buffer per channel");

static struct bt_bap_lc3_preset preset = BT_BAP_LC3_BROADCAST_PRESET_48_4_1(
	BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT, BT_AUDIO_CONTEXT_TYPE_MEDIA);

static uint8_t bis_left[] = {BT_AUDIO_CODEC_DATA(BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
						 BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_LEFT))};
static uint8_t bis_right[] = {BT_AUDIO_CODEC_DATA(
	BT_AUDIO_CODEC_CFG_CHAN_ALLOC, BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_RIGHT))};

NET_BUF_POOL_FIXED_DEFINE(tx_pool, CONFIG_BT_ISO_TX_BUF_COUNT,
			  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct bt_bap_stream streams[CHANNELS];
static struct bt_bap_broadcast_source *source;
static atomic_t started;
static atomic_t streaming;
static atomic_t in_flight;
static uint16_t seq_num;

static lc3_encoder_mem_48k_t lc3_mem[CHANNELS];
static lc3_encoder_t lc3_enc[CHANNELS];

static K_SEM_DEFINE(sem_tx, 0, 1);
static K_SEM_DEFINE(sem_started, 0, 1);

static int frame_send(void)
{
	const int octets = preset.qos.sdu;
	uint32_t pcm[FRAME_SAMPLES];
	struct net_buf *bufs[CHANNELS] = {0};
	int ret = 0;

	for (size_t ch = 0; ch < CHANNELS; ch++) {
		bufs[ch] = net_buf_alloc(&tx_pool, K_NO_WAIT);
		if (bufs[ch] == NULL) {
			ret = -ENOMEM;
		}
	}

	usb_frame_get(pcm);

	for (size_t ch = 0; ch < CHANNELS && ret == 0; ch++) {
		net_buf_reserve(bufs[ch], BT_ISO_CHAN_SEND_RESERVE);
		if (lc3_encode(lc3_enc[ch], LC3_PCM_FORMAT_S16, (const int16_t *)pcm + ch, 2,
			       octets, net_buf_add(bufs[ch], octets)) != 0) {
			ret = -EIO;
		}
	}

	if (ret) {
		for (size_t ch = 0; ch < CHANNELS; ch++) {
			if (bufs[ch] != NULL) {
				net_buf_unref(bufs[ch]);
			}
		}
		return ret;
	}

	atomic_inc(&in_flight);

	for (size_t ch = 0; ch < CHANNELS; ch++) {
		int err = bt_bap_stream_send(&streams[ch], bufs[ch], seq_num);

		if (err) {
			net_buf_unref(bufs[ch]);
			if (ch == 0) {
				atomic_dec(&in_flight);
			}
			ret = err;
		}
	}

	seq_num++;
	return ret;
}

static void encoder_thread(void *p1, void *p2, void *p3)
{
	uint32_t errors = 0;

	while (true) {
		/* Timeout retries after a failed send */
		(void)k_sem_take(&sem_tx, K_MSEC(FRAME_MS));

		while (atomic_get(&streaming) && atomic_get(&in_flight) < TX_IN_FLIGHT) {
			int err = frame_send();

			if (err) {
				if ((errors++ % 100U) == 0U) {
					LOG_WRN("SDU send failed (%d), %u so far", err, errors);
				}
				break;
			}
		}
	}
}

/* Below the Bluetooth threads */
K_THREAD_DEFINE(encoder_tid, 6144, encoder_thread, NULL, NULL, NULL, 10, 0, 0);

static void stream_started_cb(struct bt_bap_stream *s)
{
	LOG_INF("BIS %s started", s == &streams[0] ? "left" : "right");

	if (atomic_inc(&started) + 1 < CHANNELS) {
		return;
	}

	seq_num = 0;
	atomic_set(&in_flight, 0);
	atomic_set(&streaming, 1);
	k_sem_give(&sem_tx);
	k_sem_give(&sem_started);
}

static void stream_stopped_cb(struct bt_bap_stream *s, uint8_t reason)
{
	atomic_dec(&started);
	atomic_set(&streaming, 0);
	LOG_WRN("BIS %s stopped (0x%02x)", s == &streams[0] ? "left" : "right", reason);
}

static void stream_sent_cb(struct bt_bap_stream *s)
{
	if (s == &streams[0]) {
		atomic_dec(&in_flight);
		k_sem_give(&sem_tx);
	}
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started_cb,
	.stopped = stream_stopped_cb,
	.sent = stream_sent_cb,
};

static int create_source(void)
{
	struct bt_bap_broadcast_source_stream_param stream_params[CHANNELS] = {
		{.stream = &streams[0], .data = bis_left, .data_len = sizeof(bis_left)},
		{.stream = &streams[1], .data = bis_right, .data_len = sizeof(bis_right)},
	};
	struct bt_bap_broadcast_source_subgroup_param subgroup_param = {
		.params_count = CHANNELS,
		.params = stream_params,
		.codec_cfg = &preset.codec_cfg,
	};
	struct bt_bap_broadcast_source_param param = {
		.params_count = 1,
		.params = &subgroup_param,
		.qos = &preset.qos,
		.encryption = false,
		.packing = BT_ISO_PACKING_SEQUENTIAL,
	};

	for (size_t ch = 0; ch < CHANNELS; ch++) {
		bt_bap_stream_cb_register(&streams[ch], &stream_ops);
	}

	return bt_bap_broadcast_source_create(&param, &source);
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

	net_buf_simple_add_le16(&id_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&id_buf, broadcast_id);
	ext_ad[0] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, id_buf.data, id_buf.len);

	err = bt_pbp_get_announcement(NULL, 0, BT_PBP_ANNOUNCEMENT_FEATURE_HIGH_QUALITY, &pba_buf);
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

	err = bt_bap_broadcast_source_get_base(source, &base_buf);
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

	err = bt_bap_broadcast_source_start(source, adv);
	if (err) {
		LOG_ERR("broadcast_source_start failed (%d)", err);
		return err;
	}

	LOG_INF("Advertising \"%s\", broadcast ID 0x%06x, LC3 48 kHz stereo, %u octets",
		CONFIG_BROADCAST_NAME, broadcast_id, preset.qos.sdu);
	return 0;
}

int broadcast_start(void)
{
	int err;

	for (size_t ch = 0; ch < CHANNELS; ch++) {
		lc3_enc[ch] = lc3_setup_encoder(FRAME_MS * USEC_PER_MSEC, PCM_RATE_HZ, PCM_RATE_HZ,
						&lc3_mem[ch]);
		if (lc3_enc[ch] == NULL) {
			LOG_ERR("LC3 encoder setup failed");
			return -EINVAL;
		}
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
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

	return k_sem_take(&sem_started, K_SECONDS(5));
}
