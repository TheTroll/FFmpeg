/*
 * Intel MediaSDK QSV decoder utility functions
 *
 * copyright (c) 2013 Luca Barbato
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_QSVDEC_H
#define AVCODEC_QSVDEC_H

#include <stdint.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/avutil.h"

#define QSV_VERSION_MAJOR 1
#define QSV_VERSION_MINOR 0


typedef struct QSVDecTimeStamp {
    int64_t pts;
    int64_t dts;
} QSVDecTimeStamp;

typedef struct QSVDecBuffer {
    mfxFrameSurface1 surface;
    mfxSyncPoint sync;
    AVFrame *frame;
    struct QSVDecBuffer *sync_next;
} QSVDecBuffer;

typedef struct QSVDecContext {
    AVClass *class;
    mfxSession session;
    mfxIMPL impl;
    mfxVersion ver;
    mfxVideoParam param;
    mfxFrameAllocRequest req;
    mfxBitstream bs;
    QSVDecTimeStamp *ts;
    int nb_ts;
    int put_dts_cnt;
    int decoded_cnt;
    int ts_by_qsv;
    int last_ret;
    int need_reinit;
    int async_depth;
    int timeout;
    AVPacketList *pending_dec, *pending_dec_end;
    QSVDecBuffer **buf;
    int nb_buf;
    QSVDecBuffer *pending_sync, *pending_sync_end;
    int nb_sync;
} QSVDecContext;

int ff_qsv_dec_init(AVCodecContext *s, QSVDecContext *q);

int ff_qsv_dec_frame(AVCodecContext *s, QSVDecContext *q,
                     AVFrame *frame, int *got_frame,
                     AVPacket *avpkt);

int ff_qsv_dec_flush(QSVDecContext *q);

int ff_qsv_dec_close(QSVDecContext *q);

int ff_qsv_dec_reinit(AVCodecContext *s, QSVDecContext *q);

#endif /* AVCODEC_QSVDEC_H */
