/**
 * @file audio.c  libavformat video-source
 *
 * Copyright (C) 2016 Jefry Tedjokusumo
 */
#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#define FF_API_OLD_METADATA 0
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include "libavresample/avresample.h"
#include "queue.h"
#include "audio.h"

struct common_st *common;
extern AVPacket flush_pkt;

void destructor_common(void *arg) {
	struct common_st *st = arg;
	if (st->run) {
		st->run = false;
		pthread_join(st->thread, NULL);
	}

	if (st->ic) {
#if LIBAVFORMAT_VERSION_INT >= ((53<<16) + (21<<8) + 0)
		avformat_close_input(&st->ic);
#else
		av_close_input_file(st->ic);
#endif
	}

}

static void destructor_audio(void *arg) {
	struct ausrc_st *st = arg;

	packet_queue_abort(&st->audioq);
	if (st->audio_thread_run) {
		st->audio_thread_run = false;
		pthread_join(st->audio_thread, NULL);
	}

	st->common->as = NULL;
	st->common = NULL;
	//common = mem_deref(st->common);

	if(st->avr) {
		avresample_close(st->avr);
        avresample_free(&st->avr);
	}

	if (st->ctx && st->ctx->codec)
		avcodec_close(st->ctx);

	mem_deref(st->aubuf);
}

static int play_packet(struct ausrc_st *st)
{
	int16_t buf[st->sampc];

	/* timed read from audio-buffer */
	if (aubuf_get_samp(st->aubuf, st->prm.ptime, buf, st->sampc)) {
		return -1;
	}

	/* call read handler */
	if (st->rh)
		st->rh(buf, st->sampc, st->arg);

	return 0;
}

int audio_decode_frame(struct ausrc_st *st, AVPacket* pkt, AVPacket* pkt_temp) {
	AVFrame *frame = NULL;
	int got_frame, ret;
    int new_packet = 0;
    int flush_complete = 0;
    int data_size = 0;

    for (;;) {
		while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
			if (!frame)
				frame = avcodec_alloc_frame();
			else
				avcodec_get_frame_defaults(frame);

			if (flush_complete)
				break;
			new_packet = 0;

			ret = avcodec_decode_audio4(st->ctx, frame,
							&got_frame, pkt_temp);
			if (ret < 0)
				goto out;

			pkt_temp->data += ret;
			pkt_temp->size -= ret;

			if (!got_frame) {
				/* stop sending empty packets if the decoder is finished */
				if (!pkt_temp->data && (st->codec->capabilities & CODEC_CAP_DELAY))
					flush_complete = 1;
				continue;
			}

			if (st->avr) {
				int out_linesize;
                const int osize = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
				data_size = av_samples_get_buffer_size(&out_linesize, st->prm.ch, frame->nb_samples, AV_SAMPLE_FMT_S16, 0);
				uint8_t* tmp_dest = av_malloc(data_size);
                int out_samples = avresample_convert(st->avr,
                                                 &tmp_dest,
                                                 out_linesize, frame->nb_samples,
                                                 frame->data,
                                                 frame->linesize[0],
                                                 frame->nb_samples);
                if (out_samples < 0) {
                    break;
                }
                data_size = out_samples * osize * st->prm.ch;
				aubuf_write(st->aubuf, tmp_dest, data_size);
				av_free(tmp_dest);

				if(!st->first_packet_time) { // audio timer has priority
					st->first_packet_time = 1000 * frame->pkt_pts * av_q2d(st->common->ic->streams[pkt->stream_index]->time_base);
				}

			} else {
				data_size = frame->linesize[0];
				ret = aubuf_write(st->aubuf, frame->data[0], frame->linesize[0]);
			}

			if (ret < 0)
				debug("aubuf_write err: %d\n", ret);
			goto out;
		}

        if (pkt->data)
        	av_free_packet(pkt);

		memset(pkt_temp, 0, sizeof(*pkt_temp));

        if (st->audioq.abort_request) {
            return -1;
        }

        /* read next packet */
        if ((new_packet = packet_queue_get(&st->audioq, pkt, 1)) < 0)
            return -1;

        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(st->ctx);
            flush_complete = 0;
        }

        *pkt_temp = *pkt;
    }
 out:
	if (frame) {
#if LIBAVUTIL_VERSION_INT >= ((52<<16)+(20<<8)+100)
		av_frame_free(&frame);
#else
		av_free(frame);
#endif
	}
	return data_size;
}

static void *audio_thread(void *data) {
	struct ausrc_st *st = data;
    uint64_t prev_time = 0;

	while (st->audio_thread_run) {
		if (aubuf_cur_size(st->aubuf) > st->psize) {
			const struct timespec delay = {0, st->prm.ptime*1000000/3};
			const uint64_t now = tmr_jiffies();

			const int64_t dl = now - prev_time;
			prev_time = now;
			if ( dl > st->prm.ptime)
				debug("time_elapsed: %d > packet_time: %d\n", dl, st->prm.ptime);

			if(!play_packet(st)) {
				if(!st->audio_time) {
					st->audio_time = st->first_packet_time;
					st->common->time_start = now - st->audio_time;
				}
				st->audio_time += st->prm.ptime;
			}

			(void)nanosleep(&delay, NULL);
		} /*else {
			debug("audio buffer is empty\n");
		}*/
	}
	return NULL;
}

int alloc_audio(struct ausrc_st **stp, const struct ausrc *as,
			 struct media_ctx **mctx,
			 struct ausrc_prm *prm, const char *dev,
			 ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	bool found_stream = false;
	uint32_t i;
	int ret, err = 0;
    AVDictionary *opts = NULL;

	if (!stp || !as || !prm || !rh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), destructor_audio);
	if (!st)
		return ENOMEM;

	if (!common)
		common = mem_zalloc(sizeof(*common), destructor_common);
	if (!common)
		return ENOMEM;

	st->common = common;
	st->as   = as;
	st->rh   = rh;
	st->errh = errh;
	st->arg  = arg;

	st->prm = *prm;
	st->sampc = st->prm.srate * st->prm.ch * st->prm.ptime / 1000;
	st->psize = 2 * st->sampc; // 2 byte per sample (16 bit audio)

	err = aubuf_alloc(&st->aubuf, st->psize, 0);
	if(err)
		goto out;

	if(!st->common->ic) {
		ret = avformat_open_input(&st->common->ic, dev, NULL, NULL);
		if (ret < 0) {
			err = ENOENT;
			goto out;
		}

		ret = avformat_find_stream_info(st->common->ic, NULL);

		if (ret < 0) {
			warning("avformat: %s: no stream info\n", dev);
			err = ENOENT;
			goto out;
		}
#if 0
		dump_format(st->common->ic, 0, dev, 0);
#endif
	}

	for (i=0; i<st->common->ic->nb_streams; i++) {
		const struct AVStream *strm = st->common->ic->streams[i];
		AVCodecContext *ctx = strm->codec;

		if (ctx->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;

		debug("avformat: stream %u:  %u x %u "
		      "  time_base=%d/%d\n",
		      i, ctx->width, ctx->height,
		      ctx->time_base.num, ctx->time_base.den);

		st->ctx    = ctx;
		st->sindex = strm->index;

		if (ctx->codec_id != AV_CODEC_ID_NONE) {

			st->codec = avcodec_find_decoder(ctx->codec_id);
			if (!st->codec) {
				err = ENOENT;
				goto out;
			}

			if (!av_dict_get(opts, "threads", NULL, 0))
		        av_dict_set(&opts, "threads", "auto", 0);

		    ret = avcodec_open2(ctx, st->codec, &opts);
			if (ret < 0) {
				err = ENOENT;
				goto out;
			}

			if(!ctx->channel_layout)
				ctx->channel_layout = av_get_default_channel_layout(ctx->channels);

			st->avr = avresample_alloc_context();
			if(!st->avr) {
				err = ENOMEM;
				goto out;
			}

			av_opt_set_int(st->avr, "in_channel_layout",  ctx->channel_layout, 0);
		    av_opt_set_int(st->avr, "in_sample_fmt",      ctx->sample_fmt,     0);
		    av_opt_set_int(st->avr, "in_sample_rate",     ctx->sample_rate,    0);

		    av_opt_set_int(st->avr, "out_channel_layout", av_get_default_channel_layout(prm->ch), 0);
		    av_opt_set_int(st->avr, "out_sample_fmt",     AV_SAMPLE_FMT_S16, 0);
		    av_opt_set_int(st->avr, "out_sample_rate",    prm->srate,        0);

		    if ((ret = avresample_open(st->avr)) < 0) {
				err = ENOENT;
				goto out;
		    }
		}

		found_stream = true;
		break;
	}

	if (!found_stream) {
		err = ENOENT;
		goto out;
	}

    packet_queue_init(&st->audioq);

	common->as = st;
	if(!st->common->thread) {
		st->common->run = true;
		err = pthread_create(&st->common->thread, NULL, avformat_read_thread, common);
		if (err) {
			st->common->run = false;
			goto out;
		}
	}

	st->audio_thread_run = true;
	st->audio_time = 0;
	st->first_packet_time = 0;
	err = pthread_create(&st->audio_thread, NULL, audio_thread, st);
	if (err) {
		st->audio_thread_run = false;
		goto out;
	}

out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;

}
