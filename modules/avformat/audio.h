/**
 * @file audio.h  libavformat video-source
 *
 * Copyright (C) 2016 Jefry Tedjokusumo
 */

struct common_st {
	pthread_t thread;
	bool run;
	AVFormatContext *ic;
	struct vidsrc_st* vs;
	struct ausrc_st* as;
	uint64_t time_start;
};

struct ausrc_st {
	const struct ausrc *as;  /* pointer to base-class (inheritance) */
	struct common_st* common;
	struct aubuf *aubuf;
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	void *arg;
	struct ausrc_prm prm;
	size_t psize;               /**< Packet size in bytes    */
	size_t sampc;
	AVCodec *codec;
	AVCodecContext *ctx;
	int sindex;
    PacketQueue audioq;
	pthread_t audio_thread;
	bool audio_thread_run;
    struct AVAudioResampleContext *avr;
    uint64_t audio_time;
    uint64_t first_packet_time;
};

int alloc_audio(struct ausrc_st **stp, const struct ausrc *as,
			 struct media_ctx **mctx,
			 struct ausrc_prm *prm, const char *dev,
			 ausrc_read_h *rh, ausrc_error_h *errh, void *arg);

void destructor_common(void *arg);

int audio_decode_frame(struct ausrc_st *st, AVPacket* pkt, AVPacket* pkt_temp);

void *avformat_read_thread(void *data);

