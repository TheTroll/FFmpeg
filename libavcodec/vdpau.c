/*
 * Video Decode and Presentation API for UNIX (VDPAU) is used for
 * HW decode acceleration for MPEG-1/2, MPEG-4 ASP, H.264 and VC-1.
 *
 * Copyright (c) 2008 NVIDIA
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

#include <limits.h>
#include "libavutil/avassert.h"
#include "avcodec.h"
#include "internal.h"
#include "h264.h"
#include "vc1.h"

#undef NDEBUG
#include <assert.h>

#include "vdpau.h"
#include "vdpau_internal.h"

/**
 * @addtogroup VDPAU_Decoding
 *
 * @{
 */

static int vdpau_error(VdpStatus status)
{
    switch (status) {
    case VDP_STATUS_OK:
        return 0;
    case VDP_STATUS_NO_IMPLEMENTATION:
        return AVERROR(ENOSYS);
    case VDP_STATUS_DISPLAY_PREEMPTED:
        return AVERROR(EIO);
    case VDP_STATUS_INVALID_HANDLE:
        return AVERROR(EBADF);
    case VDP_STATUS_INVALID_POINTER:
        return AVERROR(EFAULT);
    case VDP_STATUS_RESOURCES:
        return AVERROR(ENOBUFS);
    case VDP_STATUS_HANDLE_DEVICE_MISMATCH:
        return AVERROR(EXDEV);
    case VDP_STATUS_ERROR:
        return AVERROR(EIO);
    default:
        return AVERROR(EINVAL);
    }
}

int ff_vdpau_common_init(AVCodecContext *avctx, VdpDecoderProfile profile,
                         int level)
{
    VDPAUHWContext *hwctx = avctx->hwaccel_context;
    VDPAUContext *vdctx = avctx->internal->hwaccel_priv_data;
    VdpDecoderCreate *create;
    void *func;
    VdpStatus status;
    /* See vdpau/vdpau.h for alignment constraints. */
    uint32_t width  = (avctx->coded_width + 1) & ~1;
    uint32_t height = (avctx->coded_height + 3) & ~3;

    if (hwctx->context.decoder != VDP_INVALID_HANDLE) {
        vdctx->decoder = hwctx->context.decoder;
        vdctx->render  = hwctx->context.render;
        vdctx->device  = VDP_INVALID_HANDLE;
        return 0; /* Decoder created by user */
    }

    vdctx->device           = hwctx->device;
    vdctx->get_proc_address = hwctx->get_proc_address;

    status = vdctx->get_proc_address(vdctx->device, VDP_FUNC_ID_DECODER_CREATE,
                                     &func);
    if (status != VDP_STATUS_OK)
        return vdpau_error(status);
    else
        create = func;

    status = vdctx->get_proc_address(vdctx->device, VDP_FUNC_ID_DECODER_RENDER,
                                     &func);
    if (status != VDP_STATUS_OK)
        return vdpau_error(status);
    else
        vdctx->render = func;

    status = create(vdctx->device, profile, width, height, avctx->refs,
                    &vdctx->decoder);
    return vdpau_error(status);
}

int ff_vdpau_common_uninit(AVCodecContext *avctx)
{
    VDPAUContext *vdctx = avctx->internal->hwaccel_priv_data;
    VdpDecoderDestroy *destroy;
    void *func;
    VdpStatus status;

    if (vdctx->device == VDP_INVALID_HANDLE)
        return 0; /* Decoder created and destroyed by user */

    status = vdctx->get_proc_address(vdctx->device,
                                     VDP_FUNC_ID_DECODER_DESTROY, &func);
    if (status != VDP_STATUS_OK)
        return vdpau_error(status);
    else
        destroy = func;

    status = destroy(vdctx->decoder);
    return vdpau_error(status);
}

int ff_vdpau_common_start_frame(struct vdpau_picture_context *pic_ctx,
                                av_unused const uint8_t *buffer,
                                av_unused uint32_t size)
{
    pic_ctx->bitstream_buffers_allocated = 0;
    pic_ctx->bitstream_buffers_used      = 0;
    pic_ctx->bitstream_buffers           = NULL;
    return 0;
}

int ff_vdpau_common_end_frame(AVCodecContext *avctx, AVFrame *frame,
                              struct vdpau_picture_context *pic_ctx)
{
    VDPAUContext *vdctx = avctx->internal->hwaccel_priv_data;
    VdpVideoSurface surf = ff_vdpau_get_surface_id(frame);
    VdpStatus status;

    status = vdctx->render(vdctx->decoder, surf, (void *)&pic_ctx->info,
                           pic_ctx->bitstream_buffers_used,
                           pic_ctx->bitstream_buffers);

    av_freep(&pic_ctx->bitstream_buffers);
    return vdpau_error(status);
}

#if CONFIG_H263_VDPAU_HWACCEL  || CONFIG_MPEG1_VDPAU_HWACCEL || \
    CONFIG_MPEG2_VDPAU_HWACCEL || CONFIG_MPEG4_VDPAU_HWACCEL || \
    CONFIG_VC1_VDPAU_HWACCEL   || CONFIG_WMV3_VDPAU_HWACCEL
int ff_vdpau_mpeg_end_frame(AVCodecContext *avctx)
{
    MpegEncContext *s = avctx->priv_data;
    Picture *pic = s->current_picture_ptr;
    struct vdpau_picture_context *pic_ctx = pic->hwaccel_picture_private;
    int val;

    val = ff_vdpau_common_end_frame(avctx, pic->f, pic_ctx);
    if (val < 0)
        return val;

    ff_mpeg_draw_horiz_band(s, 0, s->avctx->height);
    return 0;
}
#endif

int ff_vdpau_add_buffer(struct vdpau_picture_context *pic_ctx,
                        const uint8_t *buf, uint32_t size)
{
    VdpBitstreamBuffer *buffers = pic_ctx->bitstream_buffers;

    buffers = av_fast_realloc(buffers, &pic_ctx->bitstream_buffers_allocated,
                              (pic_ctx->bitstream_buffers_used + 1) * sizeof(*buffers));
    if (!buffers)
        return AVERROR(ENOMEM);

    pic_ctx->bitstream_buffers = buffers;
    buffers += pic_ctx->bitstream_buffers_used++;

    buffers->struct_version  = VDP_BITSTREAM_BUFFER_VERSION;
    buffers->bitstream       = buf;
    buffers->bitstream_bytes = size;
    return 0;
}

int av_vdpau_get_profile(AVCodecContext *avctx, VdpDecoderProfile *profile)
{
#define PROFILE(prof)       \
do {                        \
    *profile = prof;        \
    return 0;               \
} while (0)

    switch (avctx->codec_id) {
    case AV_CODEC_ID_MPEG1VIDEO:               PROFILE(VDP_DECODER_PROFILE_MPEG1);
    case AV_CODEC_ID_MPEG2VIDEO:
        switch (avctx->profile) {
        case FF_PROFILE_MPEG2_MAIN:            PROFILE(VDP_DECODER_PROFILE_MPEG2_MAIN);
        case FF_PROFILE_MPEG2_SIMPLE:          PROFILE(VDP_DECODER_PROFILE_MPEG2_SIMPLE);
        default:                               return AVERROR(EINVAL);
        }
    case AV_CODEC_ID_H263:                     PROFILE(VDP_DECODER_PROFILE_MPEG4_PART2_ASP);
    case AV_CODEC_ID_MPEG4:
        switch (avctx->profile) {
        case FF_PROFILE_MPEG4_SIMPLE:          PROFILE(VDP_DECODER_PROFILE_MPEG4_PART2_SP);
        case FF_PROFILE_MPEG4_ADVANCED_SIMPLE: PROFILE(VDP_DECODER_PROFILE_MPEG4_PART2_ASP);
        default:                               return AVERROR(EINVAL);
        }
    case AV_CODEC_ID_H264:
        switch (avctx->profile & ~FF_PROFILE_H264_INTRA) {
        case FF_PROFILE_H264_CONSTRAINED_BASELINE:
        case FF_PROFILE_H264_BASELINE:         PROFILE(VDP_DECODER_PROFILE_H264_BASELINE);
        case FF_PROFILE_H264_MAIN:             PROFILE(VDP_DECODER_PROFILE_H264_MAIN);
        case FF_PROFILE_H264_HIGH:             PROFILE(VDP_DECODER_PROFILE_H264_HIGH);
        default:                               return AVERROR(EINVAL);
        }
    case AV_CODEC_ID_WMV3:
    case AV_CODEC_ID_VC1:
        switch (avctx->profile) {
        case FF_PROFILE_VC1_SIMPLE:            PROFILE(VDP_DECODER_PROFILE_VC1_SIMPLE);
        case FF_PROFILE_VC1_MAIN:              PROFILE(VDP_DECODER_PROFILE_VC1_MAIN);
        case FF_PROFILE_VC1_ADVANCED:          PROFILE(VDP_DECODER_PROFILE_VC1_ADVANCED);
        default:                               return AVERROR(EINVAL);
        }
    }
    return AVERROR(EINVAL);
}

AVVDPAUContext *av_vdpau_alloc_context(void)
{
    return av_mallocz(sizeof(AVVDPAUContext));
}

/* @}*/
