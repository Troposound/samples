/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* LE Audio broadcast sink: scan for a Broadcast Audio Announcement, PA sync, read the
 * BASE, BIG sync to the first BIS, decode LC3 to 48 kHz and play it on both
 * TAC5211 headphone channels.
 */

#include <string.h>
#include <strings.h>

#include <lc3.h>
#include <troposound/audio_i2s.h>
#include <troposound/headset.h>
#include <zephyr/audio/codec.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "broadcast.h"

LOG_MODULE_REGISTER(broadcast_sink, LOG_LEVEL_INF);

#define PCM_RATE_HZ    48000
#define FRAME_US       10000
#define FRAME_SAMPLES  (PCM_RATE_HZ / 100)
#define BLOCK_COUNT    4
#define PRIME_BLOCKS   2
#define MAX_SDU        155
#define STEP_TIMEOUT   K_SECONDS(5)
#define NAME_LEN       (BT_AUDIO_BROADCAST_NAME_LEN_MAX + 1)

BUILD_ASSERT(AUDIO_I2S_BLOCK_WORDS == FRAME_SAMPLES, "I2S block must be one 10 ms LC3 frame");

/* --- Audio output ------------------------------------------------------- */

struct lc3_frame {
	uint16_t len;
	bool valid;
	uint8_t data[MAX_SDU];
};

K_MSGQ_DEFINE(frame_q, sizeof(struct lc3_frame), BLOCK_COUNT, 4);
K_MSGQ_DEFINE(free_q, sizeof(uint32_t *), BLOCK_COUNT, sizeof(uint32_t *));
K_MSGQ_DEFINE(ready_q, sizeof(uint32_t *), BLOCK_COUNT, sizeof(uint32_t *));

static uint32_t pcm_blocks[BLOCK_COUNT][FRAME_SAMPLES];
static uint32_t tx_silence[FRAME_SAMPLES]; /* EasyDMA reads RAM only */
static uint32_t rx_scratch[FRAME_SAMPLES]; /* capture is not used by the sink */
static bool primed;

static lc3_decoder_mem_48k_t lc3_mem;
static lc3_decoder_t lc3_dec;

static void i2s_cb(uint32_t *released_rx, const uint32_t *released_tx)
{
	const uint32_t *next = tx_silence;

	ARG_UNUSED(released_rx);

	if (released_tx != NULL && released_tx != tx_silence) {
		uint32_t *blk = (uint32_t *)released_tx;

		(void)k_msgq_put(&free_q, &blk, K_NO_WAIT);
	}

	if (!primed && k_msgq_num_used_get(&ready_q) >= PRIME_BLOCKS) {
		primed = true;
	}

	if (primed) {
		uint32_t *blk;

		if (k_msgq_get(&ready_q, &blk, K_NO_WAIT) == 0) {
			next = blk;
		} else {
			primed = false; /* underrun: refill before playing again */
		}
	}

	(void)audio_i2s_set_next_buf(next, rx_scratch);
}

static void decoder_thread(void *p1, void *p2, void *p3)
{
	struct lc3_frame frame;
	uint32_t *blk;

	while (true) {
		k_msgq_get(&frame_q, &frame, K_FOREVER);

		if (lc3_dec == NULL || k_msgq_get(&free_q, &blk, K_NO_WAIT) != 0) {
			continue; /* not configured, or playback is behind: drop */
		}

		/* NULL input runs packet loss concealment */
		(void)lc3_decode(lc3_dec, frame.valid ? frame.data : NULL, frame.len,
				 LC3_PCM_FORMAT_S16, (int16_t *)blk, 2);

		/* Mono to both slots: left is in the low half-word */
		for (size_t i = 0; i < FRAME_SAMPLES; i++) {
			blk[i] = (blk[i] & 0xFFFFU) | (blk[i] << 16);
		}

		(void)k_msgq_put(&ready_q, &blk, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(decoder_tid, 4096, decoder_thread, NULL, NULL, NULL, 5, 0, 0);

static int audio_out_start(void)
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

	for (size_t i = 0; i < BLOCK_COUNT; i++) {
		uint32_t *blk = pcm_blocks[i];

		(void)k_msgq_put(&free_q, &blk, K_NO_WAIT);
	}

	err = audio_i2s_init();
	if (err) {
		LOG_ERR("I2S init failed (%d)", err);
		return err;
	}

	audio_i2s_callback_set(i2s_cb);

	err = audio_i2s_start(tx_silence, rx_scratch);
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

	audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL,
				 (audio_property_value_t){.mute = false});
	audio_codec_apply_properties(codec);
	audio_codec_start_output(codec);
	controls_start(codec);

	return 0;
}

/* --- Broadcast sink ----------------------------------------------------- */

static K_SEM_DEFINE(sem_found, 0, 1);
static K_SEM_DEFINE(sem_pa_synced, 0, 1);
static K_SEM_DEFINE(sem_base, 0, 1);
static K_SEM_DEFINE(sem_syncable, 0, 1);
static K_SEM_DEFINE(sem_started, 0, 1);
static K_SEM_DEFINE(sem_lost, 0, 1);
static K_SEM_DEFINE(sem_stopped, 0, 1);

static struct bt_bap_stream stream;
static struct bt_bap_stream *streams[] = {&stream};
static struct bt_bap_broadcast_sink *sink;
static struct bt_le_per_adv_sync *pa_sync;
static bt_addr_le_t found_addr;
static uint8_t found_sid;
static uint16_t found_interval;
static uint32_t found_id;
static volatile bool searching;
static uint32_t bis_bitfield;
static bool big_encrypted;

static const struct bt_audio_codec_cap codec_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ |
		BT_AUDIO_CODEC_CAP_FREQ_32KHZ | BT_AUDIO_CODEC_CAP_FREQ_48KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10, BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 26U, MAX_SDU,
	1U, BT_AUDIO_CONTEXT_TYPE_ANY);

static struct bt_pacs_cap cap = {
	.codec_cap = &codec_cap,
};

static void stream_started_cb(struct bt_bap_stream *s)
{
	const struct bt_audio_codec_cfg *cfg = s->codec_cfg;
	int freq = bt_audio_codec_cfg_get_freq(cfg);
	int dur = bt_audio_codec_cfg_get_frame_dur(cfg);
	int freq_hz = freq > 0 ? bt_audio_codec_cfg_freq_to_freq_hz(freq) : -1;
	int dur_us = dur > 0 ? bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur) : -1;

	if (freq_hz <= 0 || dur_us != FRAME_US) {
		LOG_ERR("Unsupported stream: %d Hz, %d us", freq_hz, dur_us);
		return;
	}

	lc3_dec = lc3_setup_decoder(FRAME_US, freq_hz, PCM_RATE_HZ, &lc3_mem);
	LOG_INF("BIS started: LC3 %d Hz, %d octets", freq_hz,
		bt_audio_codec_cfg_get_octets_per_frame(cfg));
	k_sem_give(&sem_started);
}

static void stream_stopped_cb(struct bt_bap_stream *s, uint8_t reason)
{
	LOG_INF("BIS stopped (0x%02x)", reason);
	lc3_dec = NULL;
	k_sem_give(&sem_lost);
}

static void stream_recv_cb(struct bt_bap_stream *s, const struct bt_iso_recv_info *info,
			   struct net_buf *buf)
{
	struct lc3_frame frame = {
		.valid = (info->flags & BT_ISO_FLAGS_VALID) && buf->len > 0 && buf->len <= MAX_SDU,
	};

	if (frame.valid) {
		frame.len = buf->len;
		memcpy(frame.data, buf->data, buf->len);
	}

	(void)k_msgq_put(&frame_q, &frame, K_NO_WAIT);
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started_cb,
	.stopped = stream_stopped_cb,
	.recv = stream_recv_cb,
};

static void base_recv_cb(struct bt_bap_broadcast_sink *s, const struct bt_bap_base *base,
			 size_t base_size)
{
	uint32_t bits = 0;

	if (bis_bitfield != 0 || bt_bap_base_get_bis_indexes(base, &bits) != 0 || bits == 0) {
		return;
	}

	bis_bitfield = bits & -bits; /* lowest BIS index */
	k_sem_give(&sem_base);
}

static void syncable_cb(struct bt_bap_broadcast_sink *s, const struct bt_iso_biginfo *biginfo)
{
	big_encrypted = biginfo->encryption;
	k_sem_give(&sem_syncable);
}

static void sink_stopped_cb(struct bt_bap_broadcast_sink *s, uint8_t reason)
{
	LOG_INF("Broadcast sink stopped (0x%02x)", reason);
	k_sem_give(&sem_stopped);
	k_sem_give(&sem_lost);
}

static struct bt_bap_broadcast_sink_cb sink_cb = {
	.base_recv = base_recv_cb,
	.syncable = syncable_cb,
	.stopped = sink_stopped_cb,
};

struct scan_ctx {
	uint32_t broadcast_id;
	bool name_match;
	char name[NAME_LEN];
};

static bool contains_nocase(const char *str, const char *sub)
{
	const size_t sub_len = strlen(sub);

	for (const char *p = str; strlen(p) >= sub_len; p++) {
		if (strncasecmp(p, sub, sub_len) == 0) {
			return true;
		}
	}

	return false;
}

static bool ad_parse_cb(struct bt_data *data, void *user_data)
{
	struct scan_ctx *ctx = user_data;

	switch (data->type) {
	case BT_DATA_SVC_DATA16:
		if (data->data_len >= BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE &&
		    sys_get_le16(data->data) == BT_UUID_BROADCAST_AUDIO_VAL) {
			ctx->broadcast_id = sys_get_le24(data->data + BT_UUID_SIZE_16);
		}
		break;
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_BROADCAST_NAME: {
		char name[NAME_LEN] = {0};

		memcpy(name, data->data, MIN(data->data_len, NAME_LEN - 1));
		if (strlen(CONFIG_BROADCAST_TARGET_NAME) == 0 ||
		    contains_nocase(name, CONFIG_BROADCAST_TARGET_NAME)) {
			ctx->name_match = true;
		}
		if (data->type == BT_DATA_BROADCAST_NAME || ctx->name[0] == '\0') {
			memcpy(ctx->name, name, sizeof(name));
		}
		break;
	}
	default:
		break;
	}

	return true;
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	struct scan_ctx ctx = {
		.broadcast_id = BT_BAP_INVALID_BROADCAST_ID,
		.name_match = strlen(CONFIG_BROADCAST_TARGET_NAME) == 0,
	};

	if (!searching || info->interval == 0U) {
		return; /* no periodic advertising, so not a broadcast source */
	}

	bt_data_parse(ad, ad_parse_cb, &ctx);

	if (ctx.broadcast_id == BT_BAP_INVALID_BROADCAST_ID || !ctx.name_match) {
		return;
	}

	searching = false;
	bt_addr_le_copy(&found_addr, info->addr);
	found_sid = info->sid;
	found_interval = info->interval;
	found_id = ctx.broadcast_id;
	LOG_INF("Found \"%s\" broadcast ID 0x%06x", ctx.name, found_id);
	k_sem_give(&sem_found);
}

static struct bt_le_scan_cb scan_cb = {
	.recv = scan_recv_cb,
};

static void pa_synced_cb(struct bt_le_per_adv_sync *sync,
			 struct bt_le_per_adv_sync_synced_info *info)
{
	if (sync == pa_sync) {
		k_sem_give(&sem_pa_synced);
	}
}

static void pa_term_cb(struct bt_le_per_adv_sync *sync,
		       const struct bt_le_per_adv_sync_term_info *info)
{
	if (sync == pa_sync) {
		LOG_INF("PA sync lost (0x%02x)", info->reason);
		pa_sync = NULL;
		k_sem_give(&sem_lost);
	}
}

static struct bt_le_per_adv_sync_cb pa_cb = {
	.synced = pa_synced_cb,
	.term = pa_term_cb,
};

static struct bt_bap_scan_delegator_cb scan_delegator_cb;

struct src_ids {
	uint8_t ids[CONFIG_BT_BAP_SCAN_DELEGATOR_RECV_STATE_COUNT];
	size_t count;
};

static bool src_id_collect(const struct bt_bap_scan_delegator_recv_state *state, void *user_data)
{
	struct src_ids *src = user_data;

	if (src->count < ARRAY_SIZE(src->ids)) {
		src->ids[src->count++] = state->src_id;
	}

	return false;
}

static void session_reset(void)
{
	struct src_ids src = {0};
	int err;

	headset_speaker_set(false);

	if (sink != NULL) {
		(void)bt_bap_broadcast_sink_stop(sink);
		(void)k_sem_take(&sem_stopped, K_SECONDS(2));
		err = bt_bap_broadcast_sink_delete(sink);
		if (err) {
			LOG_WRN("Broadcast sink delete failed (%d)", err);
		}
		sink = NULL;
	}

	/* Deleting the sink leaves its scan delegator receive state behind;
	 * remove it so the next session can add a new one. The state still
	 * reads PA synced, and rem_src then silently keeps it, so mark it
	 * unsynced first.
	 */
	bt_bap_scan_delegator_foreach_state(src_id_collect, &src);
	for (size_t i = 0; i < src.count; i++) {
		uint32_t no_bis[BT_BAP_BASS_MAX_SUBGROUPS] = {0};

		(void)bt_bap_scan_delegator_set_bis_sync_state(src.ids[i], no_bis);
		(void)bt_bap_scan_delegator_set_pa_state(src.ids[i], BT_BAP_PA_STATE_NOT_SYNCED);
		err = bt_bap_scan_delegator_rem_src(src.ids[i]);
		if (err) {
			LOG_WRN("Receive state %u remove failed (%d)", src.ids[i], err);
		}
	}

	if (pa_sync != NULL) {
		(void)bt_le_per_adv_sync_delete(pa_sync);
		pa_sync = NULL;
	}

	lc3_dec = NULL;
	bis_bitfield = 0;
	k_sem_reset(&sem_found);
	k_sem_reset(&sem_pa_synced);
	k_sem_reset(&sem_base);
	k_sem_reset(&sem_syncable);
	k_sem_reset(&sem_started);
	k_sem_reset(&sem_lost);
	k_sem_reset(&sem_stopped);
}

static int session_run(void)
{
	static const uint8_t no_code[BT_ISO_BROADCAST_CODE_SIZE];
	struct bt_le_per_adv_sync_param pa_param = {0};
	int err;

	led_state_set(LED_STATE_SEARCHING);
	LOG_INF("Scanning for %s%s",
		strlen(CONFIG_BROADCAST_TARGET_NAME) ? "broadcast " : "any broadcast",
		CONFIG_BROADCAST_TARGET_NAME);

	searching = true;
	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("scan_start failed (%d)", err);
		return err;
	}

	k_sem_take(&sem_found, K_FOREVER);
	(void)bt_le_scan_stop();

	bt_addr_le_copy(&pa_param.addr, &found_addr);
	pa_param.sid = found_sid;
	pa_param.skip = 0;
	/* 10 PA intervals, in 10 ms units: interval is in 1.25 ms units */
	pa_param.timeout = CLAMP((found_interval * 125U) / 100U, BT_GAP_PER_ADV_MIN_TIMEOUT,
				 BT_GAP_PER_ADV_MAX_TIMEOUT);

	err = bt_le_per_adv_sync_create(&pa_param, &pa_sync);
	if (err) {
		LOG_ERR("PA sync create failed (%d)", err);
		return err;
	}

	if (k_sem_take(&sem_pa_synced, STEP_TIMEOUT)) {
		LOG_WRN("PA sync timeout");
		return -ETIMEDOUT;
	}

	err = bt_bap_broadcast_sink_create(pa_sync, found_id, &sink);
	if (err) {
		LOG_ERR("sink_create failed (%d)", err);
		return err;
	}

	if (k_sem_take(&sem_base, STEP_TIMEOUT) || k_sem_take(&sem_syncable, STEP_TIMEOUT)) {
		LOG_WRN("No BASE/BIGInfo");
		return -ETIMEDOUT;
	}

	if (big_encrypted) {
		LOG_WRN("Broadcast is encrypted, skipping");
		return -EACCES;
	}

	err = bt_bap_broadcast_sink_sync(sink, bis_bitfield, streams, no_code);
	if (err) {
		LOG_ERR("sink_sync failed (%d)", err);
		return err;
	}

	if (k_sem_take(&sem_started, STEP_TIMEOUT)) {
		LOG_WRN("BIG sync timeout");
		return -ETIMEDOUT;
	}

	led_state_set(LED_STATE_STREAMING);
	headset_speaker_set(true);
	k_sem_take(&sem_lost, K_FOREVER);
	LOG_INF("Stream lost");

	return 0;
}

int role_run(void)
{
	const struct bt_pacs_register_param pacs_param = {
		.snk_pac = true,
		.snk_loc = true,
	};
	int err;

	err = audio_out_start();
	if (err) {
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = bt_pacs_register(&pacs_param);
	if (err) {
		return err;
	}

	err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &cap);
	if (err) {
		return err;
	}

	err = bt_bap_scan_delegator_register(&scan_delegator_cb);
	if (err) {
		return err;
	}

	err = bt_bap_broadcast_sink_register_cb(&sink_cb);
	if (err) {
		return err;
	}

	bt_bap_stream_cb_register(&stream, &stream_ops);
	bt_le_per_adv_sync_cb_register(&pa_cb);
	bt_le_scan_cb_register(&scan_cb);

	while (true) {
		err = session_run();
		if (err) {
			LOG_WRN("Session ended (%d), rescanning", err);
		}
		session_reset();
		k_sleep(K_MSEC(500));
	}

	return 0;
}
