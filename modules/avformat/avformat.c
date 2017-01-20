/**
 * @file avformat.c  libavformat video-source
 *
 * Copyright (C) 2010 - 2015 Creytiv.com
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
#include "queue.h"
#include "audio.h"

/**
 * @defgroup avformat avformat
 *
 * Video source using FFmpeg/libav libavformat
 *
 *
 * Example config:
 \verbatim
  video_source            avformat,/tmp/testfile.mp4
 \endverbatim
 */


/* backward compat */
#if LIBAVCODEC_VERSION_MAJOR>52 || LIBAVCODEC_VERSION_INT>=((52<<16)+(64<<8))
#define LIBAVCODEC_HAVE_AVMEDIA_TYPES 1
#endif
#ifndef LIBAVCODEC_HAVE_AVMEDIA_TYPES
#define AVMEDIA_TYPE_VIDEO CODEC_TYPE_VIDEO
#endif


#if LIBAVCODEC_VERSION_INT < ((54<<16)+(25<<8)+0)
#define AVCodecID CodecID
#define AV_CODEC_ID_NONE  CODEC_ID_NONE
#endif


#if LIBAVUTIL_VERSION_MAJOR < 52
#define AV_PIX_FMT_YUV420P PIX_FMT_YUV420P
#endif

struct threadq_st {
    PacketQueue q;
    bool thread_run;
    pthread_t thread;
};

static void threadq_destroy(struct threadq_st* st) {
	packet_queue_abort(&st->q);
	if (st->thread_run) {
		st->thread_run = false;
		pthread_join(st->thread, NULL);
	}
	packet_queue_end(&st->q);
}

static int threadq_init(struct threadq_st* st,  void *(*start_routine) (void *), void* arg) {
	packet_queue_init(&st->q);
	st->thread_run = true;
	int err = pthread_create(&st->thread, NULL, start_routine, arg);
	if (err) {
		st->thread_run = false;
	}
	return err;
}

// video send does not really needed for now (at least on mp4 file on core i7)
// TODO: video out of sync during loop, frameq should be buffered somewhere
#define USE_VIDEO_SEND_THREAD 0

struct vidsrc_st {
	const struct vidsrc *vs;  /* inheritance */
	struct common_st* common;
	AVCodec *codec;
	AVCodecContext *ctx;
	struct vidsz sz;
	vidsrc_frame_h *frameh;
	void *arg;
	int sindex;
	int fps;
    struct vidframe* vid_dest;
	struct threadq_st video_decode;
#if USE_VIDEO_SEND_THREAD
	struct threadq_st video_send;
#endif
};


static struct vidsrc *mod_avf;

static struct ausrc *ausrc;
extern struct common_st *common;
extern AVPacket flush_pkt;

static void destructor(void *arg)
{
	struct vidsrc_st *st = arg;
    
    if(st->common->run) {
        st->common->run = false;
        pthread_join(st->common->thread, NULL);
    }

#if USE_VIDEO_SEND_THREAD
	threadq_destroy(&st->video_send);
#endif

	threadq_destroy(&st->video_decode);

	st->common->vs = NULL;
	st->common = NULL;
	//common = mem_deref(st->common);

	if(st->vid_dest)
		mem_deref(st->vid_dest);

	if (st->ctx && st->ctx->codec)
		avcodec_close(st->ctx);
    
    mem_deref(common);
}

#if USE_VIDEO_SEND_THREAD
static void av_destruct_vidframe_packet(AVPacket *pkt) {
	mem_deref(pkt->data);
    pkt->data = NULL;
}
#endif

static void handle_packet(struct vidsrc_st *st, AVPacket *pkt, bool* skip)
{
	AVFrame *frame = NULL;
	struct vidframe vf;
	struct vidsz sz;
	unsigned i;

	if (st->codec) {
		int got_pict, ret;

#if LIBAVUTIL_VERSION_INT >= ((52<<16)+(20<<8)+100)
		frame = av_frame_alloc();
#else
		frame = avcodec_alloc_frame();
#endif

#if LIBAVCODEC_VERSION_INT >= ((57<<16)+(37<<8)+100)

		ret = avcodec_send_packet(st->ctx, pkt);
		if (ret < 0)
			goto out;

		ret = avcodec_receive_frame(st->ctx, frame);
		if (ret < 0)
			goto out;

		got_pict = true;

#elif LIBAVCODEC_VERSION_INT <= ((52<<16)+(23<<8)+0)
		ret = avcodec_decode_video(st->ctx, frame, &got_pict,
					   pkt->data, pkt->size);
#else
		ret = avcodec_decode_video2(st->ctx, frame,
					    &got_pict, pkt);
#endif
		if (ret < 0 || !got_pict)
			goto out;

		sz.w = st->ctx->width;
		sz.h = st->ctx->height;

		/* check if size changed */
		if (!vidsz_cmp(&sz, &st->sz)) {
			info("avformat: size changed: %d x %d  ---> %d x %d\n",
			     st->sz.w, st->sz.h, sz.w, sz.h);
			st->sz = sz;
		}
	}
	else {
		/* No-codec option is not supported */
		return;
	}

	if(!*skip) {
		vf.size = sz;
		vf.fmt  = VID_FMT_YUV420P;
		for (i=0; i<4; i++) {
			vf.data[i]     = frame->data[i];
			vf.linesize[i] = frame->linesize[i];
		}

		if(st->vid_dest)
			vidconv(st->vid_dest, &vf, NULL);
	}
	struct vidframe* vid_dest = st->vid_dest ? st->vid_dest : &vf;

#if USE_VIDEO_SEND_THREAD

	struct vidframe* frameq;
	if(vidframe_alloc(&frameq, vid_dest->fmt, &vid_dest->size))
		goto out;

	vidframe_copy(frameq, vid_dest);
	AVPacket pktq = *pkt;
	pktq.data = frameq;
	pktq.size = 0;
	pktq.destruct = av_destruct_vidframe_packet;
	packet_queue_put(&st->video_send.q, &pktq);
#else
	if(!st->common->time_start)
		st->common->time_start = tmr_jiffies();

	const bool auloop = false;
	const double frame_dt = av_q2d(st->ctx->time_base);
	const uint64_t packet_time = 1000 * frame->pkt_pts * av_q2d(st->common->ic->streams[pkt->stream_index]->time_base) + (auloop ? 1200 : 0);
	const uint64_t elapsed = (!auloop && st->common->as && st->common->as->audio_time) ? st->common->as->audio_time : tmr_jiffies() - st->common->time_start;

	/*if (!auloop && st->common->as && st->common->as->audio_time) {
		if(abs(st->common->as->audio_time - elapsed) > 500 && aubuf_cur_size(st->common->as->aubuf) > st->common->as->psize) {
			elapsed = st->common->as->audio_time;
			st->common->time_start = tmr_jiffies() - elapsed;
		}
	}*/

	int64_t dt = elapsed - packet_time;
	//debug("video elapsed: %d, packet_time: %d, dt: %d, picnum: %d\n", elapsed, packet_time, dt, frame->coded_picture_number);
	//debug("audio elapsed: %d, audio: %d, video: %d\n", st->common->as ? st->common->as->audio_time : 0, st->common->as ? aubuf_cur_size(st->common->as->aubuf): 0, st->video_decode.q.nb_packets);

	if(abs(dt)<=frame_dt || dt <= 0) {
		dt = -dt;
		const uint32_t sec  = dt/1000;
		const uint32_t nano = (dt - sec * 1000) * 1000000;
		const struct timespec delay = {sec, nano};
		(void)nanosleep(&delay, NULL);

		if(!*skip)
			st->frameh(vid_dest, st->arg);
		*skip = false;
	} else {
		debug("video is late by: %d ms, videoq: %d\n", dt, st->video_decode.q.nb_packets);
		//*skip = true;
	}

#endif

 out:
	if (frame) {
#if LIBAVUTIL_VERSION_INT >= ((52<<16)+(20<<8)+100)
		av_frame_free(&frame);
#else
		av_free(frame);
#endif
	}
}

#if USE_VIDEO_SEND_THREAD
static void *video_send_thread(void *data){
	struct vidsrc_st *st = data;
	AVPacket pkt;
	bool skip = false;

	while (st->video_send.thread_run) {
        if (st->video_send.q.abort_request)
        	break;

        if (packet_queue_get(&st->video_send.q, &pkt, 0) <= 0) {
        	continue;
        }

        if (pkt.data == flush_pkt.data) {
            //avcodec_flush_buffers(st->ctx);
            continue;
        }

		if(!st->common->time_start)
			st->common->time_start = tmr_jiffies();

		if (!skip) {
			st->frameh(pkt.data, st->arg);

			const uint64_t packet_time = 1000 * pkt.pts * av_q2d(st->common->ic->streams[pkt.stream_index]->time_base);
			const uint64_t elapsed = tmr_jiffies() - st->common->time_start;
			const int64_t dt = elapsed - packet_time;
			//debug("video elapsed: %d, packet_time: %d, dt: %d\n", elapsed, packet_time, dt);

			if(dt<=0) {
				const struct timespec delay = {0, -dt*1000000/3};
				(void)nanosleep(&delay, NULL);
			} else {
				debug("video is late by: %d ms, videoq: %d\n", dt, st->video_send.q.nb_packets);
				skip = true;
			}
		} else
			skip = false;

#if LIBAVCODEC_VERSION_INT >= ((57<<16)+(12<<8)+100)
		av_packet_unref(&pkt);
#else
		av_free_packet(&pkt);
#endif
	}

	return NULL;
}
#endif

static void *video_decode_thread(void *data) {
	struct vidsrc_st *st = data;
	AVPacket pkt;
	bool skip = false;

	while (st->video_decode.thread_run) {
#if USE_VIDEO_SEND_THREAD
		while (!st->video_decode.q.abort_request &&
				st->video_send.q.nb_packets > 10) {
			//debug("video_sendq: %d\n", st->video_send.q.nb_packets);
			sys_msleep(10);
		}
#endif

        if (st->video_decode.q.abort_request)
        	break;

        if (packet_queue_get(&st->video_decode.q, &pkt, 1) <= 0)
        	continue;

        while(st->common->as && !st->common->as->audio_time) {
			const struct timespec delay = {0, st->common->as->prm.ptime*1000000/2};
			(void)nanosleep(&delay, NULL);
        }

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(st->ctx);
            continue;
        }

        handle_packet(st, &pkt, &skip);

#if LIBAVCODEC_VERSION_INT >= ((57<<16)+(12<<8)+100)
		av_packet_unref(&pkt);
#else
		av_free_packet(&pkt);
#endif
	}
	return NULL;
}

void *avformat_read_thread(void *data)
{
	struct common_st *st = data;

	while (st->run) {
		AVPacket pkt;
		int ret;
		while ( st->run &&
				((st->as && aubuf_cur_size(st->as->aubuf) > st->as->prm.srate * st->as->prm.ch * 2) || !st->as) &&
				((st->vs && st->vs->video_decode.q.nb_packets > 10) || !st->vs)) {
			//if(st->as)
			//	debug("audioBufSize: %d\n", aubuf_cur_size(st->as->aubuf));
			//if(st->vs)
			//	debug("video_decodeq: %d\n", st->vs->video_decode.q.nb_packets);
			sys_msleep(10);
		}

		av_init_packet(&pkt);

		ret = av_read_frame(st->ic, &pkt);
		//const uint64_t packet_time = 1000 * pkt.pts * av_q2d(st->ic->streams[pkt.stream_index]->time_base);

		if (ret < 0) {
			debug("avformat: rewind stream (ret=%d)\n", ret);
            audio_flush_buffer(st->as);            
			while (st->as && aubuf_cur_size(st->as->aubuf) &&
					st->vs && st->vs->video_decode.q.nb_packets) {
				sys_msleep(100);
			}

			if(st->as) {
				packet_queue_flush(&st->as->audioq);
				packet_queue_put(&st->as->audioq, &flush_pkt);
			}
			if(st->vs) {
				packet_queue_flush(&st->vs->video_decode.q);
				packet_queue_put(&st->vs->video_decode.q, &flush_pkt);
#if USE_VIDEO_SEND_THREAD
				packet_queue_flush(&st->vs->video_send.q);
#endif
			}

			av_seek_frame(st->ic, -1, 0, 0);
			if(st->as){
				st->as->audio_time = 0;
				st->as->first_packet_time = 0;
			}

            //wait for the flush packet to be consumed by video thread
            while(st->vs && st->vs->video_decode.q.nb_packets){
                sys_msleep(5);
            }
			st->time_start = 0;
			continue;
		}

		//debug("packet sindex: %d, elaps:%d\n", pkt.stream_index, elapsed);
		//debug("packet sindex: %d, time: %f\n", pkt.stream_index, 1000 * pkt.pts * av_q2d(st->ic->streams[pkt.stream_index]->time_base));
        //debug("queue size: %d\n\n", st->as->audioq.size);

		if (st->vs && pkt.stream_index == st->vs->sindex) {
            packet_queue_put(&st->vs->video_decode.q, &pkt);
		}

		if (st->as && pkt.stream_index == st->as->sindex) {
			// this audio queue is directly consumed by audio_decode_frame
			// keep it this way until we need to move audio decoder to separate thread
            packet_queue_put(&st->as->audioq, &pkt);
            {
            	AVPacket audio_pkt, pkt_temp;
            	memset(&audio_pkt, 0, sizeof(audio_pkt));
                memset(&pkt_temp, 0, sizeof(pkt_temp));
            	audio_decode_frame(st->as, &audio_pkt, &pkt_temp);
            }
		}
	}

	return NULL;
}


static int alloc(struct vidsrc_st **stp, const struct vidsrc *vs,
		 struct media_ctx **mctx, struct vidsrc_prm *prm,
		 const struct vidsz *size, const char *fmt,
		 const char *dev, vidsrc_frame_h *frameh,
		 vidsrc_error_h *errorh, void *arg)
{
#if LIBAVFORMAT_VERSION_INT < ((52<<16) + (110<<8) + 0)
	AVFormatParameters prms;
#endif
	struct vidsrc_st *st;
	bool found_stream = false;
	uint32_t i;
	int ret, err = 0;

	(void)mctx;
	(void)errorh;

	if (!stp || !size || !frameh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

    if (!common) {
		common = mem_zalloc(sizeof(*common), destructor_common);
        if (!common)
            return ENOMEM;
    } else {
        mem_ref(common);
    }

	st->common = common;
	st->vs     = vs;
	st->sz     = *size;
	st->vid_dest = NULL;
	st->frameh = frameh;
	st->arg    = arg;

	if (prm) {
		st->fps = prm->fps;
	}
	else {
		st->fps = 25;
	}

	/*
	 * avformat_open_input() was added in lavf 53.2.0 according to
	 * ffmpeg/doc/APIchanges
	 */
	if(!st->common->ic) {
#if LIBAVFORMAT_VERSION_INT >= ((52<<16) + (110<<8) + 0)
	(void)fmt;
	ret = avformat_open_input(&st->common->ic, dev, NULL, NULL);
#else

	/* Params */
	memset(&prms, 0, sizeof(prms));

	prms.time_base          = (AVRational){1, st->fps};
	prms.channels           = 1;
	prms.width              = size->w;
	prms.height             = size->h;
	prms.pix_fmt            = AV_PIX_FMT_YUV420P;
	prms.channel            = 0;

	ret = av_open_input_file(&st->common->ic, dev, av_find_input_format(fmt),
				 0, &prms);
#endif

	if (ret < 0) {
		err = ENOENT;
		goto out;
	}

#if LIBAVFORMAT_VERSION_INT >= ((53<<16) + (4<<8) + 0)
	ret = avformat_find_stream_info(st->common->ic, NULL);
#else
	ret = av_find_stream_info(st->common->ic);
#endif

	if (ret < 0) {
		warning("avformat: %s: no stream info\n", dev);
		err = ENOENT;
		goto out;
	}

#if 0
	dump_format(st->common->ic, 0, dev, 0);
#endif
	} // if(!st->common->ic)

	for (i=0; i<st->common->ic->nb_streams; i++) {
		const struct AVStream *strm = st->common->ic->streams[i];
		AVCodecContext *ctx = strm->codec;

		if (ctx->codec_type != AVMEDIA_TYPE_VIDEO)
			continue;

		debug("avformat: stream %u:  %u x %u "
		      "  time_base=%d/%d\n",
		      i, ctx->width, ctx->height,
		      ctx->time_base.num, ctx->time_base.den);

		st->sz.w   = ctx->width;
		st->sz.h   = ctx->height;
		st->ctx    = ctx;
		st->sindex = strm->index;

		if(!vidsz_cmp(&st->sz, size)) {
			ret = vidframe_alloc(&st->vid_dest, VID_FMT_YUV420P, size);
			if (ret) {
				err = ENOMEM;
				goto out;
			}
		}

		if (ctx->codec_id != AV_CODEC_ID_NONE) {

			st->codec = avcodec_find_decoder(ctx->codec_id);
			if (!st->codec) {
				err = ENOENT;
				goto out;
			}

#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
			ret = avcodec_open2(ctx, st->codec, NULL);
#else
			ret = avcodec_open(ctx, st->codec);
#endif
			if (ret < 0) {
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

	err = threadq_init(&st->video_decode, video_decode_thread, st);
	if (err) goto out;

#if USE_VIDEO_SEND_THREAD
	err = threadq_init(&st->video_send, video_send_thread, st);
	if (err) goto out;
#endif

	common->vs = st;
	if(!st->common->thread) {
		st->common->run = true;
		err = pthread_create(&st->common->thread, NULL, avformat_read_thread, common);
		if (err) {
			st->common->run = false;
			goto out;
		}
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}

static int file_change_handler(struct re_printf *pf, void *arg)
{
    const struct cmd_arg *carg = arg;
    (void)re_hprintf(pf, "file_change_handler %s\n", carg->prm);
    struct config* cfg = conf_config();
    strncpy(cfg->video.src_dev, carg->prm, sizeof(cfg->video.src_dev));
    strncpy(cfg->audio.src_dev, carg->prm, sizeof(cfg->audio.src_dev));
    return 0;
}

static const struct cmd cmdv[] = {
    {"avformat-file", 0, CMD_PRM, "AVFormat file input", file_change_handler},
};

static int module_init(void)
{
	common = NULL;
	/* register all codecs, demux and protocols */
	avcodec_register_all();
	avdevice_register_all();

#if LIBAVFORMAT_VERSION_INT >= ((53<<16) + (13<<8) + 0)
	avformat_network_init();
#endif

	av_register_all();
	int err;
	err = vidsrc_register(&mod_avf, "avformat", alloc, NULL);
	if (err)
		goto out;

	err = ausrc_register(&ausrc, "avformat", alloc_audio);
	if (err)
		goto out;
    
    err = cmd_register(baresip_commands(), cmdv, ARRAY_SIZE(cmdv));
    if (err)
        goto out;

out:
	return err;
}


static int module_close(void)
{
	common = mem_deref(common);
	mod_avf = mem_deref(mod_avf);
	ausrc = mem_deref(ausrc);

#if LIBAVFORMAT_VERSION_INT >= ((53<<16) + (13<<8) + 0)
	avformat_network_deinit();
#endif

    cmd_unregister(baresip_commands(), cmdv);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(avformat) = {
	"avformat",
	"avsrc",
	module_init,
	module_close
};
