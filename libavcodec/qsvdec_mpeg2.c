/*
 * Intel MediaSDK QSV based H.264 decoder
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


#include <stdint.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"
#include "qsv.h"
#include "qsvdec.h"

#include "qsv_internal.h"
#define TIMEOUT_DEFAULT     5000    // 5s
typedef struct QSVDecMPEG2Context {
    AVClass *class;
    QSVDecContext qsv;
    uint8_t *extradata;
    int extradata_size;
    int initialized;
} QSVDecMPEG2Context;

static const uint8_t fake_idr[] = { 0x00, 0x00, 0x01, 0x65 };

static int qsv_dec_init_internal(AVCodecContext *avctx, AVPacket *avpkt)
{
    QSVDecMPEG2Context *q = avctx->priv_data;
    mfxBitstream *bs     = &q->qsv.bs;
    uint8_t *tmp         = NULL;
    uint8_t *header      = avctx->extradata;
    int header_size      = avctx->extradata_size;
    int ret              = 0;

    if (avpkt) {
        header      = avpkt->data;
        header_size = avpkt->size;
    } else if (avctx->extradata_size > 0 && avctx->extradata[0] == 1) {

        tmp = av_malloc(avctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
        if (!tmp)
        {
            ret = AVERROR(ENOMEM);
            av_log(avctx, AV_LOG_INFO, "av_malloc failed\n");
            goto fail;
        }
        q->extradata      = tmp;
        q->extradata_size = avctx->extradata_size;
        memcpy(q->extradata, avctx->extradata, avctx->extradata_size);

        header      = q->extradata;
        header_size = q->extradata_size;
    }

    //FIXME feed it a fake IDR directly
    tmp = av_malloc(header_size + sizeof(fake_idr));
    if (!tmp)
    {
        ret = AVERROR(ENOMEM);
        av_log(avctx, AV_LOG_INFO, "av_malloc failed\n");
        goto fail;
    }
    bs->Data       = tmp;
    bs->DataLength = header_size + sizeof(fake_idr);
    bs->MaxLength  = bs->DataLength;
    memcpy(bs->Data, header, header_size);
    memcpy(bs->Data + header_size, fake_idr, sizeof(fake_idr));

    ret = ff_qsv_dec_init(avctx, &q->qsv);
    if (ret)
        goto fail;

    q->initialized = 1;
    av_log(avctx, AV_LOG_INFO, "QSV Decoder initialized\n");

    return ret;

fail:
    av_freep(&bs->Data);
    av_freep(&q->extradata);

    return ret;
}

static av_cold int qsv_dec_init(AVCodecContext *avctx)
{
    avctx->pix_fmt      = AV_PIX_FMT_NV12;
    avctx->has_b_frames = 0;

    if (!avctx->extradata_size)
        return 0; // Call qsv_dec_init_internal() in qsv_dec_frame()

    return qsv_dec_init_internal(avctx, NULL);
}

static int qsv_dec_frame(AVCodecContext *avctx, void *data,
                         int *got_frame, AVPacket *avpkt)
{
    QSVDecMPEG2Context *q = avctx->priv_data;
    AVFrame *frame       = data;
    int ret;

    if (!q->initialized) {
        ret = qsv_dec_init_internal(avctx, avpkt);
        if (ret)
            return ret;
    }

    // Reinit so finished flushing old video parameter cached frames
    if (q->qsv.need_reinit && q->qsv.last_ret == MFX_ERR_MORE_DATA &&
        !q->qsv.nb_sync) {
        ret = ff_qsv_dec_reinit(avctx, &q->qsv);
        if (ret < 0)
            return ret;
    }

    ret = ff_qsv_dec_frame(avctx, &q->qsv, frame, got_frame, avpkt);

    return ret;
}

static int qsv_dec_close(AVCodecContext *avctx)
{
    QSVDecMPEG2Context *q = avctx->priv_data;
    int ret              = ff_qsv_dec_close(&q->qsv);

    av_freep(&q->qsv.bs.Data);
    av_freep(&q->extradata);

    return ret;
}

static void qsv_dec_flush(AVCodecContext *avctx)
{
    QSVDecMPEG2Context *q = avctx->priv_data;

    ff_qsv_dec_flush(&q->qsv);
}

#define OFFSET(x) offsetof(QSVDecMPEG2Context, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Number which limits internal frame buffering", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },
    { "timeout", "Maximum timeout in milliseconds when the device has been busy", OFFSET(qsv.timeout), AV_OPT_TYPE_INT, { .i64 = TIMEOUT_DEFAULT }, 0, INT_MAX, VD },
    { NULL },
};

AVHWAccel ff_mpeg2_qsv_hwaccel = {
    .name           = "mpeg2_qsv",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MPEG2VIDEO,
    .pix_fmt        = AV_PIX_FMT_QSV,
};

static const AVClass class = {
    .class_name = "mpeg2_qsv",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_mpeg2_qsv_decoder = {
    .name           = "mpeg2_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG 2 Video (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVDecMPEG2Context),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MPEG2VIDEO,
    .init           = qsv_dec_init,
    .decode         = qsv_dec_frame,
    .flush          = qsv_dec_flush,
    .close          = qsv_dec_close,
    .capabilities   = CODEC_CAP_DELAY | /*CODEC_CAP_PKT_TS | */CODEC_CAP_DR1,
    .priv_class     = &class,
};
