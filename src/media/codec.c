/* codec.c — one interface over the vendored decoders.
 * H.264, VP9, AAC and Opus go to libavcodec; AV1 goes to dav1d. Audio comes out resampled
 * to what the HD Audio driver plays: 48 kHz, 16-bit, stereo (libswresample).
 * Every call is synchronous and single-threaded: Reel is one process on one CPU. */
#include "media.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "dav1d/dav1d.h"

struct mdec {
    int kind, codec;
    /* libavcodec */
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frm;
    SwrContext *swr;
    int swr_rate, swr_fmt;
    AVChannelLayout swr_layout;
    int16_t *pend;                 /* resampled audio not handed out yet */
    int pend_n, pend_cap, pend_at;
    int64_t pend_pts;
    /* dav1d */
    Dav1dContext *d1;
    Dav1dPicture d1pic;
    int d1have;
    Dav1dData d1data;              /* data dav1d has not taken yet */
};

static void set_err(char *err, int cap, const char *m){ if(err && cap) snprintf(err, (size_t)cap, "%s", m); }

mdec_t *mdec_open(const mtrack_t *t, char *err, int errcap){
    mdec_t *d = calloc(1, sizeof *d);
    if(!d){ set_err(err, errcap, "out of memory"); return NULL; }
    d->kind = t->kind; d->codec = t->codec;
    if(t->codec == MC_AV1){
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = 1;
        s.max_frame_delay = 1;
        s.logger.callback = NULL;
        if(dav1d_open(&d->d1, &s) < 0){ free(d); set_err(err, errcap, "dav1d would not start"); return NULL; }
        return d;
    }
    enum AVCodecID id = t->codec == MC_H264 ? AV_CODEC_ID_H264 : t->codec == MC_VP9 ? AV_CODEC_ID_VP9
                      : t->codec == MC_AAC ? AV_CODEC_ID_AAC : t->codec == MC_OPUS ? AV_CODEC_ID_OPUS : AV_CODEC_ID_NONE;
    const AVCodec *c = avcodec_find_decoder(id);
    if(!c){ free(d); set_err(err, errcap, "no decoder for this codec"); return NULL; }
    d->ctx = avcodec_alloc_context3(c);
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    if(!d->ctx || !d->pkt || !d->frm){ mdec_close(d); set_err(err, errcap, "out of memory"); return NULL; }
    d->ctx->thread_count = 1;
    d->ctx->pkt_timebase = (AVRational){ 1, 1000000 };
    if(t->extralen > 0){
        d->ctx->extradata = av_mallocz((size_t)t->extralen + AV_INPUT_BUFFER_PADDING_SIZE);
        if(!d->ctx->extradata){ mdec_close(d); set_err(err, errcap, "out of memory"); return NULL; }
        memcpy(d->ctx->extradata, t->extra, (size_t)t->extralen);
        d->ctx->extradata_size = t->extralen;
    }
    if(t->kind == MK_AUDIO){
        d->ctx->sample_rate = t->rate;
        av_channel_layout_default(&d->ctx->ch_layout, t->channels > 0 ? t->channels : 2);
    } else {
        d->ctx->width = t->width; d->ctx->height = t->height;
    }
    int r = avcodec_open2(d->ctx, c, NULL);
    if(r < 0){
        char m[96];
        snprintf(m, sizeof m, "the %s decoder would not open (%d)", media_codec_name(t->codec), r);
        mdec_close(d); set_err(err, errcap, m); return NULL;
    }
    return d;
}

void mdec_close(mdec_t *d){
    if(!d) return;
    if(d->d1){
        if(d->d1have) dav1d_picture_unref(&d->d1pic);
        dav1d_data_unref(&d->d1data);
        dav1d_close(&d->d1);
    }
    if(d->ctx) avcodec_free_context(&d->ctx);
    if(d->pkt) av_packet_free(&d->pkt);
    if(d->frm) av_frame_free(&d->frm);
    if(d->swr) swr_free(&d->swr);
    av_channel_layout_uninit(&d->swr_layout);
    free(d->pend);
    free(d);
}

void mdec_flush(mdec_t *d){
    if(d->d1){
        if(d->d1have){ dav1d_picture_unref(&d->d1pic); d->d1have = 0; }
        dav1d_data_unref(&d->d1data);
        dav1d_flush(d->d1);
        return;
    }
    avcodec_flush_buffers(d->ctx);
    if(d->swr){ swr_free(&d->swr); }
    d->pend_n = d->pend_at = 0;
}

static void free_sample_copy(const uint8_t *buf, void *cookie){ (void)cookie; free((void *)buf); }

int mdec_send(mdec_t *d, const msample_t *s){
    if(d->d1){
        if(d->d1data.sz) return 1;             /* still holding the last one */
        uint8_t *copy = malloc(s->len ? s->len : 1);
        if(!copy) return -1;
        memcpy(copy, s->data, s->len);
        if(dav1d_data_wrap(&d->d1data, copy, s->len, free_sample_copy, NULL) < 0){ free(copy); return -1; }
        d->d1data.m.timestamp = s->pts;
        int r = dav1d_send_data(d->d1, &d->d1data);
        if(r < 0 && r != DAV1D_ERR(EAGAIN)){ dav1d_data_unref(&d->d1data); return -1; }
        return 0;                              /* EAGAIN: kept in d1data, sent again later */
    }
    /* the packet points at the coded frame; libavcodec copies what it keeps */
    av_packet_unref(d->pkt);
    if(av_new_packet(d->pkt, (int)s->len) < 0) return -1;
    memcpy(d->pkt->data, s->data, s->len);
    d->pkt->pts = s->pts;
    d->pkt->dts = s->dts;
    d->pkt->duration = s->dur;
    if(s->key) d->pkt->flags |= AV_PKT_FLAG_KEY;
    int r = avcodec_send_packet(d->ctx, d->pkt);
    if(r == AVERROR(EAGAIN)) return 1;
    if(r == AVERROR_INVALIDDATA) return 0;     /* a damaged frame: skip it, keep going */
    return r < 0 ? -1 : 0;
}

void mdec_drain(mdec_t *d){
    if(d->d1) return;                          /* dav1d hands everything out without it */
    avcodec_send_packet(d->ctx, NULL);
}

int mdec_picture(mdec_t *d, mpic_t *pic){
    if(d->d1){
        if(d->d1have){ dav1d_picture_unref(&d->d1pic); d->d1have = 0; }
        if(d->d1data.sz){                      /* push what dav1d could not take before */
            int r = dav1d_send_data(d->d1, &d->d1data);
            if(r < 0 && r != DAV1D_ERR(EAGAIN)){ dav1d_data_unref(&d->d1data); return -1; }
        }
        int r = dav1d_get_picture(d->d1, &d->d1pic);
        if(r == DAV1D_ERR(EAGAIN)) return 0;
        if(r < 0) return -1;
        d->d1have = 1;
        Dav1dPicture *p = &d->d1pic;
        if(p->p.bpc != 8 || p->p.layout == DAV1D_PIXEL_LAYOUT_I400) return -1;
        pic->w = p->p.w; pic->h = p->p.h;
        pic->y = p->data[0]; pic->u = p->data[1]; pic->v = p->data[2];
        pic->ys = (int)p->stride[0]; pic->uvs = (int)p->stride[1];
        pic->cw = p->p.layout == DAV1D_PIXEL_LAYOUT_I444 ? p->p.w : (p->p.w + 1) / 2;
        pic->ch = p->p.layout == DAV1D_PIXEL_LAYOUT_I420 ? (p->p.h + 1) / 2 : p->p.h;
        pic->pts = p->m.timestamp;
        pic->bt709 = p->seq_hdr->mtrx == DAV1D_MC_BT709 || (p->seq_hdr->mtrx == DAV1D_MC_UNKNOWN && p->p.h >= 720);
        pic->full_range = p->seq_hdr->color_range;
        return 1;
    }
    int r = avcodec_receive_frame(d->ctx, d->frm);
    if(r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
    if(r < 0) return -1;
    AVFrame *f = d->frm;
    int cw, ch;
    switch(f->format){
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: cw = (f->width + 1) / 2; ch = (f->height + 1) / 2; break;
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: cw = (f->width + 1) / 2; ch = f->height; break;
    case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: cw = f->width; ch = f->height; break;
    default: return -1;                        /* 10-bit and the like: not offered by the reader */
    }
    pic->w = f->width; pic->h = f->height;
    pic->y = f->data[0]; pic->u = f->data[1]; pic->v = f->data[2];
    pic->ys = f->linesize[0]; pic->uvs = f->linesize[1];
    pic->cw = cw; pic->ch = ch;
    pic->pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    pic->bt709 = f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720);
    pic->full_range = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    return 1;
}

/* resamples the frame in d->frm into d->pend */
static int resample(mdec_t *d){
    AVFrame *f = d->frm;
    if(!d->swr || d->swr_rate != f->sample_rate || d->swr_fmt != f->format
       || av_channel_layout_compare(&d->swr_layout, &f->ch_layout)){
        if(d->swr) swr_free(&d->swr);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if(swr_alloc_set_opts2(&d->swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
                               &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(d->swr) < 0) return -1;
        d->swr_rate = f->sample_rate; d->swr_fmt = f->format;
        av_channel_layout_uninit(&d->swr_layout);
        av_channel_layout_copy(&d->swr_layout, &f->ch_layout);
    }
    int out_max = swr_get_out_samples(d->swr, f->nb_samples);
    if(out_max > d->pend_cap){
        int16_t *np = realloc(d->pend, (size_t)out_max * 4);
        if(!np) return -1;
        d->pend = np; d->pend_cap = out_max;
    }
    uint8_t *outp[1] = { (uint8_t *)d->pend };
    int n = swr_convert(d->swr, outp, out_max, (const uint8_t **)f->extended_data, f->nb_samples);
    if(n < 0) return -1;
    d->pend_n = n; d->pend_at = 0;
    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    /* what the resampler still holds delays the output a little */
    d->pend_pts = pts - swr_get_delay(d->swr, 1000000);
    return 0;
}

int mdec_audio(mdec_t *d, int16_t *out, int max, int64_t *pts){
    if(d->d1) return -1;
    while(d->pend_at >= d->pend_n){
        int r = avcodec_receive_frame(d->ctx, d->frm);
        if(r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
        if(r < 0) return -1;
        if(resample(d) != 0) return -1;
    }
    int n = d->pend_n - d->pend_at;
    if(n > max) n = max;
    memcpy(out, d->pend + d->pend_at * 2, (size_t)n * 4);
    *pts = d->pend_pts + (int64_t)d->pend_at * 1000000 / 48000;
    d->pend_at += n;
    return n;
}

/* when playback is behind: skip frames nothing else refers to (H.264 B-frames, VP9 frames
 * that are not kept), which saves their decoding and their conversion */
void mdec_skip_nonref(mdec_t *d, int on){
    if(d->ctx) d->ctx->skip_frame = on ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}
