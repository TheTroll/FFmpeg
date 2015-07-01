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

#include <string.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "internal.h"
#include "avcodec.h"
#include "qsv.h"
#include "qsvdec.h"

#include "libavcodec/qsv_internal.h"
#define SYNC_TIME_DEFAULT   5000    // 5s

#include <unistd.h>
#include <fcntl.h>
#include "va/va.h" 
#include "va/va_drm.h"

int ff_qsv_dec_init(AVCodecContext *avctx, QSVDecContext *q)
{
    int ret;
    mfxIMPL impl     = MFX_IMPL_AUTO_ANY;
    mfxVersion ver   = { { QSV_VERSION_MINOR, QSV_VERSION_MAJOR } };
    mfxBitstream *bs = &q->bs;

    if ((ret = ff_qsv_codec_id_to_mfx(avctx->codec_id)) < 0)
        return ret;

    q->param.mfx.CodecId = ret;

    if ((ret = MFXInit(impl, &ver, &q->session)) < 0)
        return ff_qsv_error(ret);

    // open hardware
    {
        const char *cardpath = getenv("MFX_DRM_CARD");
        int card;
        VADisplay va_display;
        int ver_maj = 1;
        int ver_min = 0;

        if (!cardpath)
           cardpath = "/dev/dri/card0";

        av_log(avctx, AV_LOG_INFO, "Opening VA Manually\n"); 

        card = open(cardpath, O_RDWR); // primary card
        if(card < 0){
            av_log(avctx, AV_LOG_ERROR, "open %s error! Use MFX_DRM_CARD to specify the right card\n", cardpath);
            return ff_qsv_error(MFX_ERR_DEVICE_FAILED);
        }
        va_display = vaGetDisplayDRM(card);
        if (!va_display)
        {
             close(card);
             av_log(avctx, AV_LOG_ERROR, "vaGetDisplayDRM error!\n"); 
             return ff_qsv_error(MFX_ERR_DEVICE_FAILED);
        }

        ret=vaInitialize(va_display, &ver_maj, &ver_min);
        if (ret){
            av_log(avctx, AV_LOG_ERROR, "vaInitialize error! ret:%d\n", ret); 
            return ret;
        }
        ret = MFXVideoCORE_SetHandle(q->session, MFX_HANDLE_VA_DISPLAY, (mfxHDL) va_display);
            if (ret < 0){
            av_log(avctx, AV_LOG_ERROR, "MFXVideoCORE_SetHandle error! ret:%d\n", ret); 
            return ret;
        }
    }

    MFXQueryIMPL(q->session, &impl);

    if (impl & MFX_IMPL_SOFTWARE)
        av_log(avctx, AV_LOG_INFO,
               "Using Intel QuickSync decoder software implementation.\n");
    else if (impl & MFX_IMPL_HARDWARE)
        av_log(avctx, AV_LOG_VERBOSE,
               "Using Intel QuickSync decoder hardware accelerated implementation.\n");
    else
        av_log(avctx, AV_LOG_INFO,
               "Unknown Intel QuickSync decoder implementation %d.\n", impl);

    q->param.IOPattern  = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    q->param.AsyncDepth = q->async_depth;

    if ((ret = MFXVideoDECODE_DecodeHeader(q->session, bs, &q->param)) < 0)
        return ff_qsv_error(ret);

    avctx->width                   = q->param.mfx.FrameInfo.CropW;
    avctx->height                  = q->param.mfx.FrameInfo.CropH;
    avctx->coded_width             = q->param.mfx.FrameInfo.Width;
    avctx->coded_height            = q->param.mfx.FrameInfo.Height;
    avctx->time_base.den           = q->param.mfx.FrameInfo.FrameRateExtN;
    avctx->time_base.num           = q->param.mfx.FrameInfo.FrameRateExtD /
                                     avctx->ticks_per_frame;
    avctx->sample_aspect_ratio.num = q->param.mfx.FrameInfo.AspectRatioW;
    avctx->sample_aspect_ratio.den = q->param.mfx.FrameInfo.AspectRatioH;

    if (!q->need_reinit)
        bs->DataLength = bs->DataOffset = 0;

    //bs->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME; // can't decode PAFF

    memset(&q->req, 0, sizeof(q->req));
    ret = MFXVideoDECODE_QueryIOSurf(q->session, &q->param, &q->req);
    if (ret < 0)
        return ff_qsv_error(ret);

    q->last_ret = MFX_ERR_MORE_DATA;

    if ((ret = MFXVideoDECODE_Init(q->session, &q->param)))
        ret = ff_qsv_error(ret);

    return ret;
}

static int bitstream_realloc(mfxBitstream *bs, int size)
{
    uint8_t *tmp;

    if (bs->MaxLength >= size)
        return 0;

    tmp = av_realloc(bs->Data, size);
    if (!tmp) {
        av_freep(&bs->Data);
        return AVERROR(ENOMEM);
    }

    bs->Data      = tmp;
    bs->MaxLength = size;

    return 0;
}

static int bitstream_enqueue(mfxBitstream *bs, uint8_t *data, int size)
{
    int bs_size = bs->DataLength + size;
    int ret;

    if ((ret = bitstream_realloc(bs, bs_size)) < 0)
        return ret;

    if (bs_size > bs->MaxLength - bs->DataOffset) {
        memmove(bs->Data, bs->Data + bs->DataOffset, bs->DataLength);
        bs->DataOffset = 0;
    }

    memcpy(bs->Data + bs->DataOffset + bs->DataLength, data, size);

    bs->DataLength += size;

    return 0;
}

static void free_buffer_pool(QSVDecContext *q)
{
    for (int i = 0; i < q->nb_buf; i++) {
        av_frame_free((AVFrame **)&q->buf[i]->frame);
        av_freep(&q->buf[i]);
    }
    av_freep(&q->buf);
    q->nb_buf = 0;
}

static int set_surface_data(AVCodecContext *avctx, QSVDecContext *q,
                            QSVDecBuffer *buf)
{
    mfxFrameSurface1 *surf = &buf->surface;
    AVFrame *frame         = buf->frame;
    int ret;

    if (!frame) {
        if (!(frame = av_frame_alloc())) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_alloc() failed\n");
            return AVERROR(ENOMEM);
        }
    }

    if (!frame->data[0]) {
        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_get_buffer() failed\n");
            av_frame_free(&frame);
            return ret;
        }
    }

    buf->frame = frame;
    surf->Data.MemId = buf;
    surf->Data.Y     = frame->data[0];
    surf->Data.UV    = frame->data[1];
    surf->Data.Pitch = frame->linesize[0];
    surf->Info       = q->param.mfx.FrameInfo;

    return 0;
}

static int realloc_buffer_pool(QSVDecContext *q, int old_nmemb, int new_nmemb)
{
    QSVDecBuffer **pp = av_realloc_array(q->buf, new_nmemb, sizeof(QSVDecBuffer *));
    if (!pp) {
        av_log(q, AV_LOG_ERROR, "av_realloc_array() failed\n");
        return AVERROR(ENOMEM);
    }

    q->buf = pp;
    q->nb_buf = new_nmemb;

    for (int i = old_nmemb; i < q->nb_buf; i++) {
        if (!(q->buf[i] = av_mallocz(sizeof(QSVDecBuffer)))) {
            q->nb_buf = i;
            av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static void release_buffer(QSVDecBuffer *list)
{
    list->sync = 0;
}

static QSVDecBuffer *get_buffer(AVCodecContext *avctx, QSVDecContext *q)
{
    int i;

    if (!q->buf)
        if (realloc_buffer_pool(q, 0, q->req.NumFrameSuggested))
            return NULL;

    for (i = 0; i < q->nb_buf; i++)
        if (!q->buf[i]->surface.Data.Locked && !q->buf[i]->sync)
            break;

    if (i == q->nb_buf)
        if (realloc_buffer_pool(q, q->nb_buf, q->nb_buf * 2))
            return NULL;

    q->buf[i]->sync_next = NULL;

    if (set_surface_data(avctx, q, q->buf[i]) < 0)
        return NULL;

    return q->buf[i];
}

static void add_sync_list(QSVDecContext *q, QSVDecBuffer *list)
{
    list->sync_next = NULL;

    if (q->pending_sync_end)
        q->pending_sync_end->sync_next = list;
    else
        q->pending_sync = list;

    q->pending_sync_end = list;

    q->nb_sync++;
}

static QSVDecBuffer *remove_sync_list(QSVDecContext *q)
{
    QSVDecBuffer *list = q->pending_sync;

    q->pending_sync = list->sync_next;

    if (!q->pending_sync)
        q->pending_sync_end = NULL;

    q->nb_sync--;

    list->sync_next = NULL;

    return list;
}

static void free_sync_list(QSVDecContext *q)
{
    q->pending_sync     = NULL;
    q->pending_sync_end = NULL;
    q->nb_sync          = 0;
}

static int realloc_ts(QSVDecContext *q, int old_nmemb, int new_nmemb)
{
    QSVDecTimeStamp *tmp = av_realloc_array(q->ts, new_nmemb, sizeof(*q->ts));
    if (!tmp)
        return AVERROR(ENOMEM);

    q->ts = tmp;
    q->nb_ts = new_nmemb;

    for (int i = old_nmemb; i < q->nb_ts; i++)
        q->ts[i].pts = q->ts[i].dts = AV_NOPTS_VALUE;

    return 0;
}

static int get_dts(QSVDecContext *q, int64_t pts, int64_t *dts)
{
    int i;

    if (pts == AV_NOPTS_VALUE) {
        *dts = AV_NOPTS_VALUE;
        return 0;
    }

    for (i = 0; i < q->nb_ts; i++) {
        if (q->ts[i].pts == pts)
            break;
    }
    if (i == q->nb_ts) {
        av_log(q, AV_LOG_ERROR,
               "Requested pts %"PRId64" does not match any dts\n",
               pts);
//        return AVERROR_BUG;
    }
    *dts = q->ts[i].dts;

    q->ts[i].pts = AV_NOPTS_VALUE;

    return 0;
}

static int put_dts(QSVDecContext *q, int64_t pts, int64_t dts)
{
    int ret, i;

    if (!q->ts) {
        // Initialize
        q->put_dts_cnt = 0;
        ret = realloc_ts(q, 0, q->req.NumFrameSuggested);
        if (ret < 0)
            return ret;
    }

    for (i = 0; i < q->nb_ts; i++) {
        if (q->ts[i].pts == AV_NOPTS_VALUE)
            break;
    }

    if (i == q->nb_ts) {
        ret = realloc_ts(q, q->nb_ts, q->nb_ts * 2);
        if (ret < 0)
            return ret;
    }

    q->ts[i].pts = pts;
    q->ts[i].dts = dts;
    q->put_dts_cnt++;

    return 0;
}

int ff_qsv_dec_frame(AVCodecContext *avctx, QSVDecContext *q,
                     AVFrame *frame, int *got_frame,
                     AVPacket *avpkt)
{
    QSVDecBuffer *workbuf;
    mfxFrameSurface1 *outsurf;
    mfxSyncPoint outsync = 0;
    mfxBitstream *inbs   = &q->bs;
    int size             = avpkt->size;
    int busymsec         = 0;
    int paff_2nd_field   = avctx->codec_id == AV_CODEC_ID_H264 &&
                           avpkt->pts == AV_NOPTS_VALUE &&
                           avpkt->dts == AV_NOPTS_VALUE &&
                           avpkt->size;
    int ret;

    *got_frame = 0;

    if (size) {
        ret = ff_packet_list_put(&q->pending_dec,
                                 &q->pending_dec_end, avpkt);
        if (ret < 0)
            return ret;
        if (!paff_2nd_field)
            avctx->has_b_frames++;
    }

    // (2) Flush cached frames before reinit
    if (q->need_reinit)
        inbs = NULL;

    ret = q->last_ret;
    do {
        if (ret == MFX_ERR_MORE_DATA) {
            if (!inbs) {
                break;
            } else if (q->pending_dec) {
                AVPacket pkt = { 0 };

                ff_packet_list_get(&q->pending_dec, &q->pending_dec_end, &pkt);

                ret = put_dts(q, pkt.pts, pkt.dts);
                if (!ret) {
                    q->bs.TimeStamp = pkt.pts;
                    // QSV calculates TimeStamp of output based on the first
                    //  TimeStamp when q->bs.TimeStamp = MFX_TIMESTAMP_UNKNOWN
                    if (q->ts_by_qsv)
                        if (q->put_dts_cnt > 1 || pkt.pts == AV_NOPTS_VALUE)
                            q->bs.TimeStamp = MFX_TIMESTAMP_UNKNOWN;

                    ret = bitstream_enqueue(&q->bs, pkt.data, pkt.size);
                }

                av_packet_unref(&pkt);

                if (ret < 0)
                    return ret;
            } else if (!size) {
                // Flush cached frames when EOF
                inbs = NULL;
            } else {
                break;
            }
        } else if (ret == MFX_WRN_VIDEO_PARAM_CHANGED) {
            // Detected new seaquence header has compatible video parameter
            // Automatically bitstream move forward next time
        } else if (ret == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) {
            // Detected new seaquence header has incompatible video parameter
            av_log(avctx, AV_LOG_INFO,
                   "Detected new video parameters in the bitstream\n");
            if (inbs) {
                // (1) Flush cached frames before reinit
                inbs = NULL;
                q->need_reinit = 1;
            } else {
                return AVERROR_BUG;
            }
        }

        workbuf = get_buffer(avctx, q);
        if (!workbuf)
            break;

        ret = MFXVideoDECODE_DecodeFrameAsync(q->session, inbs,
                                              &workbuf->surface,
                                              &outsurf, &outsync);

        if (ret == MFX_WRN_DEVICE_BUSY || ret == MFX_ERR_DEVICE_FAILED) {
            if (busymsec > q->timeout) {
                av_log(avctx, AV_LOG_WARNING, "Timeout, device is so busy\n");
                return AVERROR(EIO);
            } else {
                av_usleep(1000);
                busymsec++;
            }
        } else {
            busymsec = 0;
        }
    } while (ret == MFX_ERR_MORE_SURFACE ||
             ret == MFX_ERR_MORE_DATA ||
             ret == MFX_ERR_DEVICE_FAILED ||
             ret == MFX_WRN_DEVICE_BUSY ||
             ret == MFX_WRN_VIDEO_PARAM_CHANGED ||
             ret == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);

    q->last_ret = ret;

    if (ret == MFX_ERR_MORE_DATA)
        ret = 0;

    if (outsync && outsurf) {
        QSVDecBuffer *buf = outsurf->Data.MemId;
        buf->sync = outsync;
        add_sync_list(q, buf);
    }

    if (q->pending_sync &&
        (!size || q->need_reinit ||
         (q->nb_sync >= q->req.NumFrameMin && !paff_2nd_field))) {
        int64_t pts, dts;
        mfxFrameSurface1 *surf;
        QSVDecBuffer *outbuf = q->pending_sync;

        ret = MFXVideoCORE_SyncOperation(q->session, outbuf->sync,
                                         SYNC_TIME_DEFAULT);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "MFXVideoCORE_SyncOperation(): %d\n", ret);
            return ff_qsv_error(ret);
        }

        remove_sync_list(q);

        surf = &outbuf->surface;
        pts = surf->Data.TimeStamp;
        if (q->ts_by_qsv) {
            dts = pts;
        } else {
            ret = get_dts(q, pts, &dts);
            if (ret < 0)
                return ret;
        }

        av_frame_move_ref(frame, outbuf->frame);

        frame->pkt_pts = frame->pts = pts;
        frame->pkt_dts = dts;
        frame->repeat_pict =
            surf->Info.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING ? 4 :
            surf->Info.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING ? 2 :
            surf->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED ? 1 : 0;
        frame->top_field_first =
            !!(surf->Info.PicStruct & MFX_PICSTRUCT_FIELD_TFF);
        frame->interlaced_frame =
            !(surf->Info.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);
        frame->sample_aspect_ratio.num = surf->Info.AspectRatioW;
        frame->sample_aspect_ratio.den = surf->Info.AspectRatioH;

        release_buffer(outbuf);

        q->decoded_cnt++;

        *got_frame = 1;

        if (avctx->has_b_frames > 0)
            avctx->has_b_frames--;
    }

    if (ret < 0)
        return ff_qsv_error(ret);

    return size;
}

int ff_qsv_dec_flush(QSVDecContext *q)
{
    int ret = MFXVideoDECODE_Reset(q->session, &q->param);

    q->last_ret      = MFX_ERR_MORE_DATA;
    q->decoded_cnt   = 0;
    q->bs.DataOffset = q->bs.DataLength = 0;

    free_buffer_pool(q);

    free_sync_list(q);

    av_freep(&q->ts);

    ff_packet_list_free(&q->pending_dec, &q->pending_dec_end);

    return ff_qsv_error(ret);
}

int ff_qsv_dec_close(QSVDecContext *q)
{
    int ret = MFXClose(q->session);

    free_buffer_pool(q);

    av_freep(&q->ts);

    ff_packet_list_free(&q->pending_dec, &q->pending_dec_end);

    return ff_qsv_error(ret);
}

int ff_qsv_dec_reinit(AVCodecContext *avctx, QSVDecContext *q)
{
    int ret;

    MFXClose(q->session);

    q->decoded_cnt = 0;

    free_buffer_pool(q);

    free_sync_list(q);

    av_freep(&q->ts);

    ret = ff_qsv_dec_init(avctx, q);

    q->need_reinit = 0;

    return ret;
}
