/*
 * spdif_media.h -- real compressed-audio test material for the S/PDIF tests.
 *
 * Everything is produced by the real FFmpeg encoders (ac3, eac3, dca, truehd) from a synthetic sine:
 * no byte of "compressed audio" is hand-written. Test-side only; no production code is involved.
 */
#ifndef SPDIF_MEDIA_H
#define SPDIF_MEDIA_H

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	unsigned char *data;		/* all packets back to back */
	int size;
	int *pkt_size;			/* size of each packet */
	int npkts;
	int frame_size;			/* samples per packet reported by the encoder */
} CodedStream;

static void coded_free(CodedStream *s)
{
	free(s->data);
	free(s->pkt_size);
	memset(s, 0, sizeof *s);
}

static void coded_add(CodedStream *s, const AVPacket *p)
{
	s->data = realloc(s->data, s->size + p->size);
	memcpy(s->data + s->size, p->data, p->size);
	s->size += p->size;
	s->pkt_size = realloc(s->pkt_size, (s->npkts + 1) * sizeof(int));
	s->pkt_size[s->npkts++] = p->size;
}

/* Encode `nframes` encoder frames of a 440 Hz stereo sine at `rate` with the named FFmpeg encoder. */
static int coded_encode(CodedStream *out, const char *encoder, int rate, int bit_rate, int nframes)
{
	memset(out, 0, sizeof *out);
	const AVCodec *c = avcodec_find_encoder_by_name(encoder);
	if (!c)
		return -1;
	AVCodecContext *ctx = avcodec_alloc_context3(c);
	ctx->sample_rate = rate;
	ctx->bit_rate = bit_rate;
	av_channel_layout_default(&ctx->ch_layout, 2);
	{
		const enum AVSampleFormat *fmts = NULL;
		int nfmts = 0;
		if (avcodec_get_supported_config(ctx, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&fmts, &nfmts) < 0 || !fmts)
			return -4;
		ctx->sample_fmt = fmts[0];
	}
	ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;	/* dca and truehd encoders are experimental */
	ctx->time_base = (AVRational){ 1, rate };
	if (avcodec_open2(ctx, c, NULL) < 0) {
		avcodec_free_context(&ctx);
		return -2;
	}
	out->frame_size = ctx->frame_size;
	int fs = ctx->frame_size > 0 ? ctx->frame_size : 1024;
	AVFrame *f = av_frame_alloc();
	f->nb_samples = fs;
	f->format = ctx->sample_fmt;
	f->sample_rate = rate;
	av_channel_layout_copy(&f->ch_layout, &ctx->ch_layout);
	av_frame_get_buffer(f, 0);
	AVPacket *p = av_packet_alloc();
	long t = 0;
	for (int n = 0; n <= nframes; n++) {
		if (n < nframes) {
			av_frame_make_writable(f);
			for (int i = 0; i < fs; i++, t++) {
				double v = 0.4 * sin(2 * M_PI * 440.0 * t / rate);
				for (int ch = 0; ch < 2; ch++) {
					switch (ctx->sample_fmt) {
					case AV_SAMPLE_FMT_FLTP: ((float *)f->data[ch])[i] = (float)v; break;
					case AV_SAMPLE_FMT_FLT:  ((float *)f->data[0])[i * 2 + ch] = (float)v; break;
					case AV_SAMPLE_FMT_S16:  ((short *)f->data[0])[i * 2 + ch] = (short)(v * 32767); break;
					case AV_SAMPLE_FMT_S32:  ((int *)f->data[0])[i * 2 + ch] = (int)(v * 2147483647.0); break;
					case AV_SAMPLE_FMT_S16P: ((short *)f->data[ch])[i] = (short)(v * 32767); break;
					case AV_SAMPLE_FMT_S32P: ((int *)f->data[ch])[i] = (int)(v * 2147483647.0); break;
					default: break;
					}
				}
			}
			f->pts = t - fs;
			avcodec_send_frame(ctx, f);
		} else {
			avcodec_send_frame(ctx, NULL);
		}
		while (avcodec_receive_packet(ctx, p) == 0) {
			coded_add(out, p);
			av_packet_unref(p);
		}
	}
	av_packet_free(&p);
	av_frame_free(&f);
	avcodec_free_context(&ctx);
	return out->npkts > 0 ? 0 : -3;
}

#endif
