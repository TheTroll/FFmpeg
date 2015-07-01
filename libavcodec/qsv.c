/*
 * Intel MediaSDK QSV encoder/decoder shared code
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <mfx/mfxvideo.h>

#include "libavutil/error.h"

#include "avcodec.h"
#include "qsv_internal.h"

#include <unistd.h>
#include <fcntl.h>
#include "va/va.h"
#include "va/va_drm.h"

int ff_qsv_codec_id_to_mfx(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return MFX_CODEC_AVC;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
        return MFX_CODEC_MPEG2;
    case AV_CODEC_ID_VC1:
        return MFX_CODEC_VC1;
    default:
        break;
    }

    return AVERROR(ENOSYS);
}

int ff_qsv_error(int mfx_err)
{
    switch (mfx_err) {
    case MFX_ERR_NONE:
        return 0;
    case MFX_ERR_MEMORY_ALLOC:
    case MFX_ERR_NOT_ENOUGH_BUFFER:
        return AVERROR(ENOMEM);
    case MFX_ERR_INVALID_HANDLE:
        return AVERROR(EINVAL);
    case MFX_ERR_DEVICE_FAILED:
    case MFX_ERR_DEVICE_LOST:
    case MFX_ERR_LOCK_MEMORY:
        return AVERROR(EIO);
    case MFX_ERR_NULL_PTR:
    case MFX_ERR_UNDEFINED_BEHAVIOR:
    case MFX_ERR_NOT_INITIALIZED:
        return AVERROR_BUG;
    case MFX_ERR_UNSUPPORTED:
    case MFX_ERR_NOT_FOUND:
        return AVERROR(ENOSYS);
    case MFX_ERR_MORE_DATA:
    case MFX_ERR_MORE_SURFACE:
    case MFX_ERR_MORE_BITSTREAM:
        return AVERROR(EAGAIN);
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
    case MFX_ERR_INVALID_VIDEO_PARAM:
        return AVERROR(EINVAL);
    case MFX_ERR_ABORTED:
    case MFX_ERR_UNKNOWN:
    default:
        return AVERROR_UNKNOWN;
    }
}

int ff_qsv_init_internal_session(AVCodecContext *avctx, mfxSession *session)
{
    mfxIMPL impl   = MFX_IMPL_AUTO_ANY;
    mfxVersion ver = { { QSV_VERSION_MINOR, QSV_VERSION_MAJOR } };

    const char *desc;
    int ret;

    ret = MFXInit(impl, &ver, session);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error initializing an internal MFX session\n");
        return ff_qsv_error(ret);
    }

    av_log(avctx, AV_LOG_INFO, "MFXInit returned %d\n", ret);

    // open hardware
    {
        const char *cardpath = getenv("MFX_DRM_CARD");
        int card;
        VADisplay va_display;
        int ver_maj = 1;
        int ver_min = 0;
        int ret;

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
        ret = MFXVideoCORE_SetHandle(*session, MFX_HANDLE_VA_DISPLAY, (mfxHDL) va_display);
            if (ret < 0){
            av_log(avctx, AV_LOG_ERROR, "MFXVideoCORE_SetHandle error! ret:%d\n", ret); 
            return ret;
        }
    }

    MFXQueryIMPL(*session, &impl);

    switch (MFX_IMPL_BASETYPE(impl)) {
    case MFX_IMPL_SOFTWARE:
        desc = "software";
        break;
    case MFX_IMPL_HARDWARE:
    case MFX_IMPL_HARDWARE2:
    case MFX_IMPL_HARDWARE3:
    case MFX_IMPL_HARDWARE4:
        desc = "hardware accelerated";
        break;
    default:
        desc = "unknown";
    }

    av_log(avctx, AV_LOG_INFO,
           "Initialized an internal MFX session using %s implementation\n",
           desc);

    return 0;
}
