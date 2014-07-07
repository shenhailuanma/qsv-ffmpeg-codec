/*
 * Intel MediaSDK QSV encoder utility functions
 *
 * copyright (c) 2013 Yukinori Yamazoe
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
#include <strings.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include <unistd.h>
#include <fcntl.h>
#include "va/va.h"
#include "va/va_drm.h"

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "internal.h"
#include "qsv.h"
#include "qsvenc.h"

static int realloc_surface_pool(QSVEncContext *q, int old_nmemb, int new_nmemb)
{
    QSVEncSurfaceList **pp = av_realloc_array(q->surf, new_nmemb, sizeof(QSVEncSurfaceList *));
    if (!pp) {
        av_log(q, AV_LOG_ERROR, "av_realloc_array() failed\n");
        return AVERROR(ENOMEM);
    }

    q->surf = pp;
    q->nb_surf = new_nmemb;

    for (int i = old_nmemb; i < q->nb_surf; i++) {
        if (!(q->surf[i] = av_mallocz(sizeof(QSVEncSurfaceList)))) {
            q->nb_surf = i;
            av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static void free_surface_pool(QSVEncContext *q)
{
    for (int i = 0; i < q->nb_surf; i++) {
        if (q->surf[i]->surface.Data.MemId)
            av_frame_free((AVFrame **)(&q->surf[i]->surface.Data.MemId));
        av_freep(&q->surf[i]);
    }
    av_freep(&q->surf);
}

static int realloc_buffer_pool(QSVEncContext *q, int old_nmemb, int new_nmemb)
{
    int size = q->param.mfx.BufferSizeInKB * 1000;
    QSVEncBuffer **pp = av_realloc_array(q->buf, new_nmemb, sizeof(QSVEncBuffer *));
    if (!pp) {
        av_log(q, AV_LOG_ERROR, "av_realloc_array() failed\n");
        return AVERROR(ENOMEM);
    }

    q->buf = pp;
    q->nb_buf = new_nmemb;

    for (int i = old_nmemb; i < q->nb_buf; i++) {
        if (!(q->buf[i] = av_mallocz(sizeof(QSVEncBuffer)))) {
            q->nb_buf = i;
            av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
            return AVERROR(ENOMEM);
        }
        if (!(q->buf[i]->data = av_mallocz(size))) {
            q->nb_buf = i;
            av_freep(&q->buf[i]);
            av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
            return AVERROR(ENOMEM);
        }
        q->buf[i]->bs.Data      = q->buf[i]->data;
        q->buf[i]->bs.MaxLength = size;
    }

    return 0;
}

static void free_buffer_pool(QSVEncContext *q)
{
    for (int i = 0; i < q->nb_buf; i++) {
        av_freep(&q->buf[i]->data);
        av_freep(&q->buf[i]);
    }
    av_freep(&q->buf);
}

static void init_param_default(QSVH264EncContext * qh, QSVEncContext *q )
{
    // init options
    //qh->coder_type    =  FF_CODER_TYPE_VLC;

    // init mfx params
    q->param.mfx.CodecId            = MFX_CODEC_AVC;
    q->param.mfx.CodecProfile       = MFX_PROFILE_AVC_MAIN;
    q->param.mfx.CodecLevel         = 0;
    q->param.mfx.TargetUsage        = 4;
    q->param.mfx.GopPicSize         = 50;
    q->param.mfx.GopRefDist         = 0;
    q->param.mfx.GopOptFlag         = MFX_GOP_CLOSED;
    q->param.mfx.IdrInterval        = 0; // every I-frame is an IDR-frame.
    q->param.mfx.NumSlice           = 0;
    q->param.mfx.NumRefFrame        = 0;
    q->param.mfx.EncodedOrder       = 0;
    q->param.mfx.BufferSizeInKB     = 0;

    q->param.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    q->param.mfx.TargetKbps = 2000;
    q->param.mfx.MaxKbps = 2000;
}

static int profile_string_to_int( const char *str )
{
    if( !strcasecmp( str, "baseline" ) )
        return MFX_PROFILE_AVC_BASELINE;
    if( !strcasecmp( str, "main" ) )
        return MFX_PROFILE_AVC_MAIN;
    if( !strcasecmp( str, "high" ) )
        return MFX_PROFILE_AVC_HIGH;
    //if( !strcasecmp( str, "extended" ) )
    //    return MFX_PROFILE_AVC_EXTENDED;

    return -1;
}

static int preset_string_to_int( const char *str )
{
    if( !strcasecmp( str, "quality" ) )
        return MFX_TARGETUSAGE_BEST_QUALITY;
    if( !strcasecmp( str, "normal" ) )
        return MFX_TARGETUSAGE_BALANCED;
    if( !strcasecmp( str, "speed" ) )
        return MFX_TARGETUSAGE_BEST_SPEED;
    //if( !strcasecmp( str, "extended" ) )
    //    return MFX_PROFILE_AVC_EXTENDED;

    return -1;
}

static int parse_enum( const char *arg, const char * const *names, int *dst )
{
    for( int i = 0; names[i]; i++ )
        if( !strcmp( arg, names[i] ) )
        {
            *dst = i;
            return 0;
        }
    return -1;
}

static int parse_cqm( const char *str, uint8_t *cqm, int length )
{
    int i = 0;
    do {
        int coef;
        if( !sscanf( str, "%d", &coef ) || coef < 1 || coef > 255 )
            return -1;
        cqm[i++] = coef;
    } while( i < length && (str = strchr( str, ',' )) && str++ );
    return (i == length) ? 0 : -1;
}

static int h264_qsv_atobool( const char *str, int *b_error )
{
    if( !strcmp(str, "1") ||
        !strcmp(str, "true") ||
        !strcmp(str, "yes") )
        return 1;
    if( !strcmp(str, "0") ||
        !strcmp(str, "false") ||
        !strcmp(str, "no") )
        return 0;
    *b_error = 1;
    return 0;
}

static int h264_qsv_atoi( const char *str, int *b_error )
{
    char *end;
    int v = strtol( str, &end, 0 );
    if( end == str || *end != '\0' )
        *b_error = 1;
    return v;
}

static double h264_qsv_atof( const char *str, int *b_error )
{
    char *end;
    double v = strtod( str, &end );
    if( end == str || *end != '\0' )
        *b_error = 1;
    return v;
}

#define atobool(str) ( name_was_bool = 1, h264_qsv_atobool( str, &b_error ) )
#define atoi(str) h264_qsv_atoi( str, &b_error )
#define atof(str) h264_qsv_atof( str, &b_error )

static int parse_qsv_params(AVCodecContext *avctx, QSVH264EncContext * qh, const char *name, const char *value)
{
    char *name_buf = NULL;
    int b_error = 0;
    int name_was_bool;
    int value_was_null = !value;
    int i;
    int val;


    if( !name ){
        av_log(avctx, AV_LOG_WARNING,"Error: bad param name.\n");
        return -1;
    }
        
    if( !value )
        value = "true";

    if( value[0] == '=' )
        value++;

    if( strchr( name, '_' ) ) // s/_/-/g
    {
        char *c;
        name_buf = strdup(name);
        while( (c = strchr( name_buf, '_' )) )
            *c = '-';
        name = name_buf;
    }

    if( (!strncmp( name, "no-", 3 ) && (i = 3)) ||
        (!strncmp( name, "no", 2 ) && (i = 2)) )
    {
        name += i;
        value = atobool(value) ? "false" : "true";
    }
    name_was_bool = 0;

#define OPT(STR) else if( !strcmp( name, STR ) )
#define OPT2(STR0, STR1) else if( !strcmp( name, STR0 ) || !strcmp( name, STR1 ) )

    if(0);
    OPT("profile"){
        qh->profile = profile_string_to_int(value);
        if(qh->profile < 0){
            b_error = -1;
        }
    }
    OPT("preset"){
        qh->preset = preset_string_to_int(value);
        if(qh->preset < 0){
            b_error =  -1;
        }
    }
    else
        return -1;


#undef OPT
#undef OPT2
#undef atobool
#undef atoi
#undef atof

    if( name_buf )
        free( name_buf );

    b_error |= value_was_null && !name_was_bool;

    return b_error ? -1 : 0;
}

static void parse_h264_qsv_params(AVCodecContext *avctx, QSVH264EncContext * qh)
{
    if(qh->h264_qsv_params){
        av_log(avctx, AV_LOG_VERBOSE, "h264_qsv_params not NULL, to parse it.\n");

        AVDictionary *dict    = NULL;
        AVDictionaryEntry *en = NULL;

        if (!av_dict_parse_string(&dict, qh->h264_qsv_params, "=", ":", 0)) {
            while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX))) {
                av_log(avctx, AV_LOG_VERBOSE, "h264_qsv_params %s=%s\n", en->key, en->value);
                
                if (parse_qsv_params(avctx, qh, en->key, en->value) < 0)
                    av_log(avctx, AV_LOG_WARNING,"Error parsing option '%s = %s'.\n",en->key, en->value);
            }

            av_dict_free(&dict);
        }
    }else{
        av_log(avctx, AV_LOG_VERBOSE, "h264_qsv_params is NULL, no need to parse it.\n");
    }    
}

static int init_video_param(AVCodecContext *avctx, QSVH264EncContext * qh)
{
    float quant;
    int ret;

    QSVEncContext *q = &qh->qsv;

    // parse h264_qsv_params
    parse_h264_qsv_params(avctx, qh);

    ret = ff_qsv_codec_id_to_mfx(avctx->codec_id);
    if (ret < 0)
        return ret;
    q->param.mfx.CodecId            = ret;

    if(qh->profile > 0)
        q->param.mfx.CodecProfile       = qh->profile;
    if(qh->level > 0)
        q->param.mfx.CodecLevel         = qh->level;
    if(qh->preset > 0)
        q->param.mfx.TargetUsage        = qh->preset;
    if(avctx->gop_size > 0)
        q->param.mfx.GopPicSize         = av_clip(avctx->gop_size, 0, 250);

    q->param.mfx.GopRefDist         = av_clip(avctx->max_b_frames, -1, 16) + 1;

    
    if(avctx->slices > 0)
        q->param.mfx.NumSlice           = avctx->slices;
    if(avctx->refs)
        q->param.mfx.NumRefFrame        = av_clip(avctx->refs, 0, 4);

    if(avctx->bit_rate > 0 && avctx->rc_max_rate == avctx->bit_rate){
        q->param.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    }else if(avctx->bit_rate > 0 && avctx->rc_max_rate >= 0){
        q->param.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
    }else{
        q->param.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
    }

    switch (q->param.mfx.RateControlMethod) {
    case MFX_RATECONTROL_CBR: // API 1.0
        av_log(avctx, AV_LOG_VERBOSE, "RateControlMethod: CBR\n");
        q->param.mfx.TargetKbps = avctx->bit_rate / 1000;
        q->param.mfx.MaxKbps    = avctx->bit_rate / 1000;
        av_log(avctx, AV_LOG_VERBOSE, "TargetKbps: %d\n", q->param.mfx.TargetKbps);
        break;
    case MFX_RATECONTROL_VBR: // API 1.0
        av_log(avctx, AV_LOG_VERBOSE, "RateControlMethod: VBR\n");
        q->param.mfx.TargetKbps = avctx->bit_rate / 1000; // >1072
        q->param.mfx.MaxKbps    = avctx->rc_max_rate / 1000;
        av_log(avctx, AV_LOG_VERBOSE, "TargetKbps: %d\n", q->param.mfx.TargetKbps);
        if (q->param.mfx.MaxKbps)
            av_log(avctx, AV_LOG_VERBOSE, "MaxKbps: %d\n", q->param.mfx.MaxKbps);
        break;
    case MFX_RATECONTROL_CQP: // API 1.1
        av_log(avctx, AV_LOG_VERBOSE, "RateControlMethod: CQP\n");
        if (qh->qpi >= 0) {
            q->param.mfx.QPI = qh->qpi;
        } else {
            quant = avctx->global_quality / FF_QP2LAMBDA;
            if (avctx->i_quant_factor)
                quant *= fabs(avctx->i_quant_factor);
            quant += avctx->i_quant_offset;
            q->param.mfx.QPI = av_clip(quant, 0, 51);
        }

        if (qh->qpp >= 0) {
            q->param.mfx.QPP = qh->qpp;
        } else {
            quant = avctx->global_quality / FF_QP2LAMBDA;
            q->param.mfx.QPP = av_clip(quant, 0, 51);
        }

        if (qh->qpb >= 0) {
            q->param.mfx.QPB = qh->qpb;
        } else {
            quant = avctx->global_quality / FF_QP2LAMBDA;
            if (avctx->b_quant_factor)
                quant *= fabs(avctx->b_quant_factor);
            quant += avctx->b_quant_offset;
            q->param.mfx.QPB = av_clip(quant, 0, 51);
        }

        av_log(avctx, AV_LOG_VERBOSE, "QPI: %d, QPP: %d, QPB: %d\n",
               q->param.mfx.QPI, q->param.mfx.QPP, q->param.mfx.QPB);
        break;
    default:
        av_log(avctx, AV_LOG_ERROR,
               "RateControlMethod: %d is undefined.\n",
               q->param.mfx.RateControlMethod);
        return AVERROR(EINVAL);
    }

    q->param.mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
    // q->param.mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_UNKNOWN;  // use this init will error
    av_log(avctx, AV_LOG_VERBOSE, "field_order:%d \n", avctx->field_order);
    q->param.mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;

    q->param.mfx.FrameInfo.Width         = FFALIGN(avctx->width, 16);
    q->param.mfx.FrameInfo.Height        = q->param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE ? 
                                                  FFALIGN(avctx->height, 16):FFALIGN(avctx->height, 32);
    q->param.mfx.FrameInfo.CropX         = 0;
    q->param.mfx.FrameInfo.CropY         = 0;
    q->param.mfx.FrameInfo.CropW         = avctx->width;
    q->param.mfx.FrameInfo.CropH         = avctx->height;
    q->param.mfx.FrameInfo.FrameRateExtN = avctx->time_base.den;
    q->param.mfx.FrameInfo.FrameRateExtD = avctx->time_base.num;
    q->param.mfx.FrameInfo.AspectRatioW  = avctx->sample_aspect_ratio.num;
    q->param.mfx.FrameInfo.AspectRatioH  = avctx->sample_aspect_ratio.den;

    q->param.mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;

    if ((q->param.mfx.FrameInfo.FrameRateExtN / q->param.mfx.FrameInfo.FrameRateExtD) > 1000) {
        av_log(avctx, AV_LOG_WARNING, "FrameRate: %d/%d (perhaps too high)\n",
               q->param.mfx.FrameInfo.FrameRateExtN,
               q->param.mfx.FrameInfo.FrameRateExtD);
    } else {
        av_log(avctx, AV_LOG_VERBOSE, "FrameRate: %d/%d\n",
               q->param.mfx.FrameInfo.FrameRateExtN,
               q->param.mfx.FrameInfo.FrameRateExtD);
    }

    q->extco.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION;
    q->extco.Header.BufferSz      = sizeof(q->extco);
    q->extco.RateDistortionOpt    = MFX_CODINGOPTION_UNKNOWN;
    q->extco.EndOfSequence        = MFX_CODINGOPTION_UNKNOWN;
    q->extco.CAVLC                = avctx->coder_type == FF_CODER_TYPE_VLC ?
                                    MFX_CODINGOPTION_ON :
                                    MFX_CODINGOPTION_UNKNOWN;
    q->extco.ResetRefList         = MFX_CODINGOPTION_UNKNOWN;
    q->extco.MaxDecFrameBuffering = MFX_CODINGOPTION_UNKNOWN;
    q->extco.AUDelimiter          = MFX_CODINGOPTION_UNKNOWN; // or OFF
    q->extco.EndOfStream          = MFX_CODINGOPTION_UNKNOWN;
    q->extco.PicTimingSEI         = MFX_CODINGOPTION_UNKNOWN; // or OFF
    q->extco.VuiNalHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
    q->extco.FramePicture         = MFX_CODINGOPTION_ON;

    if (q->extco.CAVLC == MFX_CODINGOPTION_ON)
        av_log(avctx, AV_LOG_VERBOSE, "CAVLC: ON\n");

    q->extparam[q->param.NumExtParam] = (mfxExtBuffer *)&q->extco;
    q->param.ExtParam = q->extparam;
    q->param.NumExtParam++;

    return 0;
}

static int get_video_param(AVCodecContext *avctx, QSVEncContext *q)
{
    if (avctx->flags & CODEC_FLAG_GLOBAL_HEADER) {
        int size = sizeof(q->spspps);
        uint8_t *tmp = av_malloc(size);
        if (!tmp) {
            av_log(avctx, AV_LOG_ERROR, "av_malloc() failed\n");
            return AVERROR(ENOMEM);
        }

        q->extcospspps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
        q->extcospspps.Header.BufferSz = sizeof(q->extcospspps);
        q->extcospspps.SPSBuffer       = q->spspps[0];
        q->extcospspps.SPSBufSize      = sizeof(q->spspps[0]);
        q->extcospspps.PPSBuffer       = q->spspps[1];
        q->extcospspps.PPSBufSize      = sizeof(q->spspps[1]);

        q->extparam[q->param.NumExtParam] = (mfxExtBuffer *)&q->extcospspps;
        q->param.ExtParam = q->extparam;
        q->param.NumExtParam++;

        MFXVideoENCODE_GetVideoParam(q->session, &q->param);

        q->param.NumExtParam--;

        memcpy(tmp, q->spspps, size);
        avctx->extradata = tmp;
        avctx->extradata_size = size;
    } else {
        MFXVideoENCODE_GetVideoParam(q->session, &q->param);
    }

    if (avctx->max_b_frames < 0)
        avctx->max_b_frames = q->param.mfx.GopRefDist - 1;

    return 0;
}

int ff_qsv_enc_init(AVCodecContext *avctx, QSVH264EncContext * qh)
{
    if(NULL == avctx || NULL == qh){
        return ff_qsv_error(MFX_ERR_NULL_PTR);
    }
    QSVEncContext *q = &qh->qsv;

    memset(q, 0, sizeof(QSVEncContext));

    int ret;

    q->ver.Major = QSV_VERSION_MAJOR;
    q->ver.Minor = QSV_VERSION_MINOR;

    ret = MFXInit(MFX_IMPL_AUTO_ANY, &q->ver, &q->session);
    if (ret) {
        mfxVersion ver = { { 0, 1 } };
        av_log(avctx, AV_LOG_ERROR, "MFXInit(): %d\n", ret);
        if (!MFXInit(MFX_IMPL_AUTO_ANY, &ver, &q->session)) {
            MFXQueryVersion(q->session, &ver);
            if (ver.Major < QSV_VERSION_MAJOR || ver.Minor < QSV_VERSION_MINOR)
                av_log(avctx, AV_LOG_ERROR,
                       "Detected Intel Media SDK API version %d.%d, "
                       "require version %d.%d or above.\n",
                       ver.Major, ver.Minor,
                       QSV_VERSION_MAJOR, QSV_VERSION_MINOR);
            MFXClose(q->session);
        }
        return ff_qsv_error(ret);
    }

    MFXQueryIMPL(q->session, &q->impl);

    switch (MFX_IMPL_BASETYPE(q->impl)) {
    case MFX_IMPL_SOFTWARE:
        av_log(avctx, AV_LOG_VERBOSE,
               "Using Intel QuickSync encoder software implementation.\n");
        break;
    case MFX_IMPL_HARDWARE:
    case MFX_IMPL_HARDWARE2:
    case MFX_IMPL_HARDWARE3:
    case MFX_IMPL_HARDWARE4:
        av_log(avctx, AV_LOG_VERBOSE,
               "Using Intel QuickSync encoder hardware accelerated implementation.\n");
        break;
    default:
        av_log(avctx, AV_LOG_VERBOSE,
               "Unknown Intel QuickSync encoder implementation %d.\n", q->impl);
    }

    MFXQueryVersion(q->session, &q->ver);
    av_log(avctx, AV_LOG_VERBOSE,
           "Intel Media SDK API version %d.%d\n", q->ver.Major, q->ver.Minor);

    // open hardware
    int card;
    VADisplay va_display;
    card = open("/dev/dri/card0", O_RDWR); // primary card
    if(card < 0){
        av_log(avctx, AV_LOG_ERROR, "open /dev/dri/card0 error!\n"); 
        return ff_qsv_error(MFX_ERR_DEVICE_FAILED);
    }
    va_display = vaGetDisplayDRM(card);
    if (!va_display)
    {
         close(card);
         av_log(avctx, AV_LOG_ERROR, "vaGetDisplayDRM error!\n"); 
         return ff_qsv_error(MFX_ERR_DEVICE_FAILED);
    }

    vaInitialize(va_display, &q->ver.Major, &q->ver.Minor);
    if (ret){
        av_log(avctx, AV_LOG_ERROR, "vaInitialize error! ret:%d\n", ret); 
        return ret;
    }
    ret = MFXVideoCORE_SetHandle(q->session, MFX_HANDLE_VA_DISPLAY, (mfxHDL) va_display);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "MFXVideoCORE_SetHandle error! ret:%d\n", ret); 
        return ret;
    }

    // init_param_default
    init_param_default(qh, q);
    
    q->param.IOPattern  = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    //q->param.AsyncDepth = q->options.async_depth;
    q->param.AsyncDepth = ASYNC_DEPTH_DEFAULT;

    ret = init_video_param(avctx, qh);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "init_video_param error!\n"); 
        return ret;
    }
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: param.IOPattern:%d \n", q->param.IOPattern);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: param.AsyncDepth:%d \n", q->param.AsyncDepth);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: param.mfx.CodecId :%d \n", q->param.mfx.CodecId);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: MFX_CODEC_AVC :%d \n", MFX_CODEC_AVC);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.CodecProfile :%d \n", q->param.mfx.CodecProfile);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.CodecLevel :%d \n", q->param.mfx.CodecLevel);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.TargetUsage :%d \n", q->param.mfx.TargetUsage);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.GopPicSize :%d \n", q->param.mfx.GopPicSize);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.GopRefDist :%d \n", q->param.mfx.GopRefDist);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.GopOptFlag :%d \n", q->param.mfx.GopOptFlag);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.IdrInterval :%d \n", q->param.mfx.IdrInterval);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.NumSlice :%d \n", q->param.mfx.NumSlice);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.NumRefFrame :%d \n", q->param.mfx.NumRefFrame);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.EncodedOrder :%d \n", q->param.mfx.EncodedOrder);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.BufferSizeInKB :%d \n", q->param.mfx.BufferSizeInKB);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.RateControlMethod :%d \n", q->param.mfx.RateControlMethod);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.TargetKbps :%d \n", q->param.mfx.TargetKbps);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.MaxKbps :%d \n", q->param.mfx.MaxKbps);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.FourCC :%d \n", q->param.mfx.FrameInfo.FourCC);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.Width :%d \n", q->param.mfx.FrameInfo.Width);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.Height :%d \n", q->param.mfx.FrameInfo.Height);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.CropX :%d \n", q->param.mfx.FrameInfo.CropX);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.CropY :%d \n", q->param.mfx.FrameInfo.CropY);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.CropW :%d \n", q->param.mfx.FrameInfo.CropW);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.CropH :%d \n", q->param.mfx.FrameInfo.CropH);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.FrameRateExtN :%d \n", q->param.mfx.FrameInfo.FrameRateExtN);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.FrameRateExtD :%d \n", q->param.mfx.FrameInfo.FrameRateExtD);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.AspectRatioW :%d \n", q->param.mfx.FrameInfo.AspectRatioW);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.AspectRatioH :%d \n", q->param.mfx.FrameInfo.AspectRatioH);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.PicStruct :%d \n", q->param.mfx.FrameInfo.PicStruct);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.mfx.FrameInfo.ChromaFormat :%d \n", q->param.mfx.FrameInfo.ChromaFormat);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.NumExtParam:%d \n", q->param.NumExtParam);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: q->param.Protected:%d \n", q->param.Protected);
    av_log(avctx, AV_LOG_VERBOSE, "zx debug: sizeof(FrameInfo):%d \n", sizeof(q->param.mfx.FrameInfo));

    ret = MFXVideoENCODE_QueryIOSurf(q->session, &q->param, &q->req);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "MFXVideoENCODE_QueryIOSurf(): %d\n", ret);
        return ff_qsv_error(ret);
    }

    if (ret = MFXVideoENCODE_Init(q->session, &q->param)) {
        av_log(avctx, AV_LOG_ERROR, "MFXVideoENCODE_Init(): %d\n", ret);
        return ff_qsv_error(ret);
    }

    if (ret = get_video_param(avctx, q))
        return ret;

    if (ret = realloc_surface_pool(q, 0, q->req.NumFrameSuggested))
        return ret;

    if (ret = realloc_buffer_pool(q, 0, q->req.NumFrameSuggested))
        return ret;

    return ret;
}

static QSVEncSurfaceList *get_surface_pool(QSVEncContext *q)
{
    int i;

    for (i = 0; i < q->nb_surf; i++)
        if (!q->surf[i]->surface.Data.Locked && !q->surf[i]->pending)
            break;

    if (i == q->nb_surf)
        if (realloc_surface_pool(q, q->nb_surf, q->nb_surf * 2))
            return NULL;

    if (q->surf[i]->surface.Data.MemId)
        av_frame_free((AVFrame **)(&q->surf[i]->surface.Data.MemId));

    return q->surf[i];
}

static AVFrame *clone_aligned_frame(AVCodecContext *avctx,
                                    const AVFrame *frame)
{
    int width    = frame->linesize[0];
    int height   = FFALIGN(frame->height, 32);
    int size     = width * height;
    AVFrame *ret = NULL;

    // check AVFrame buffer alignment for QSV
    if (!(width % 16) && frame->buf[0] && (frame->buf[0]->size >= size)) {
        if (!(ret = av_frame_clone(frame))) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_clone() failed\n");
            goto fail;
        }
    } else {
        if (!(ret = av_frame_alloc())) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_alloc() failed\n");
            goto fail;
        }
        if (ff_get_buffer(avctx, ret, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_get_buffer() failed\n");
            goto fail;
        }
        if (av_frame_copy_props(ret, frame) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_copy_props() failed\n");
            goto fail;
        }
        av_image_copy(ret->data, ret->linesize, frame->data, frame->linesize,
                      frame->format, frame->width, frame->height);
    }

    return ret;

fail:
    if (ret)
        av_frame_free(&ret);

    return NULL;
}

static void set_surface_param(QSVEncContext *q, mfxFrameSurface1 *surf,
                              AVFrame *frame)
{
    surf->Info = q->param.mfx.FrameInfo;

    surf->Info.PicStruct =
        !frame->interlaced_frame ? MFX_PICSTRUCT_PROGRESSIVE :
        frame->top_field_first   ? MFX_PICSTRUCT_FIELD_TFF :
                                   MFX_PICSTRUCT_FIELD_BFF;
    if (frame->repeat_pict == 1)
        surf->Info.PicStruct |= MFX_PICSTRUCT_FIELD_REPEATED;
    else if (frame->repeat_pict == 2)
        surf->Info.PicStruct |= MFX_PICSTRUCT_FRAME_DOUBLING;
    else if (frame->repeat_pict == 4)
        surf->Info.PicStruct |= MFX_PICSTRUCT_FRAME_TRIPLING;

    surf->Data.MemId     = frame;
    surf->Data.Y         = frame->data[0];
    surf->Data.UV        = frame->data[1];
    surf->Data.Pitch     = frame->linesize[0];
    surf->Data.TimeStamp = frame->pts;
}

static int add_surface_list(AVCodecContext *avctx, QSVEncContext *q,
                            const AVFrame *frame)
{
    QSVEncSurfaceList *list;
    AVFrame *clone;

    if (!(list = get_surface_pool(q)))
        return AVERROR(ENOMEM);

    if (!(clone = clone_aligned_frame(avctx, frame)))
        return AVERROR(ENOMEM);

    set_surface_param(q, &list->surface, clone);

    list->pending = 1;
    list->next = NULL;

    if (q->pending_enc_end)
        q->pending_enc_end->next = list;
    else
        q->pending_enc = list;

    q->pending_enc_end = list;

    return 0;
}

static void remove_surface_list(QSVEncContext *q)
{
    if (q->pending_enc) {
        QSVEncSurfaceList *list = q->pending_enc;
        q->pending_enc = q->pending_enc->next;
        list->pending = 0;
        list->next = NULL;
        if (!q->pending_enc)
            q->pending_enc_end = NULL;
    }
}

static QSVEncBuffer *get_buffer(QSVEncContext *q)
{
    int i;

    for (i = 0; i < q->nb_buf; i++)
        if (!q->buf[i]->sync)
            break;

    if (i == q->nb_buf)
        if (realloc_buffer_pool(q, q->nb_buf, q->nb_buf * 2))
            return NULL;

    q->buf[i]->bs.DataOffset = 0;
    q->buf[i]->bs.DataLength = 0;
    q->buf[i]->next          = NULL;

    return q->buf[i];
}

static void release_buffer(QSVEncBuffer *buf)
{
    buf->sync = 0;
}

static void add_sync_list(QSVEncContext *q, QSVEncBuffer *list)
{
    list->next = NULL;

    if (q->pending_sync_end)
        q->pending_sync_end->next = list;
    else
        q->pending_sync = list;

    q->pending_sync_end = list;

    q->nb_sync++;
}

static void remove_sync_list(QSVEncContext *q)
{
    QSVEncBuffer *list = q->pending_sync;

    q->pending_sync = q->pending_sync->next;

    if (!q->pending_sync)
        q->pending_sync_end = NULL;

    q->nb_sync--;

    list->next = NULL;
}

static void print_interlace_msg(AVCodecContext *avctx, QSVEncContext *q)
{
    if (q->param.mfx.CodecId == MFX_CODEC_AVC) {
        if (q->param.mfx.CodecProfile == MFX_PROFILE_AVC_BASELINE ||
            q->param.mfx.CodecLevel < MFX_LEVEL_AVC_21 ||
            q->param.mfx.CodecLevel > MFX_LEVEL_AVC_41)
            av_log(avctx, AV_LOG_WARNING,
                   "Interlaced coding is supported"
                   " at Main/High Profile Level 2.1-4.1\n");
    }
}

int ff_qsv_enc_frame(AVCodecContext *avctx, QSVEncContext *q,
                     AVPacket *pkt, const AVFrame *frame, int *got_packet)
{
    mfxFrameSurface1 *insurf = NULL;
    QSVEncBuffer *outbuf     = NULL;
    int busymsec             = 0;
    int timeoutmsec         = 5000; // 5s
    int ret                  = MFX_ERR_NONE;

    *got_packet = 0;

    if (frame) {
        if (ret = add_surface_list(avctx, q, frame))
            return ret;

        ret = MFX_ERR_MORE_DATA;
    }
    
    do {
        if (q->pending_enc)
            insurf = &q->pending_enc->surface;
        else if (ret != MFX_ERR_NONE)
            break;

        outbuf = get_buffer(q);
        if (!outbuf)
            return AVERROR(ENOMEM);

        do{
            ret = MFXVideoENCODE_EncodeFrameAsync(q->session, NULL, insurf,
                                              &outbuf->bs, &outbuf->sync);
            if(ret == MFX_WRN_DEVICE_BUSY){

                busymsec += 5;
                //av_log(avctx, AV_LOG_WARNING, "Timeout, device is so busy. cnt:%d\n", busymsec);
                av_usleep(5000);
            }
            if (busymsec > timeoutmsec) {
                av_log(avctx, AV_LOG_WARNING, "Timeout, device is so busy. cnt:%d\n", busymsec);
                break;
            }
        }while(ret == MFX_WRN_DEVICE_BUSY);

        if (ret == MFX_WRN_DEVICE_BUSY) {
            if (busymsec > q->options.timeout) {
                av_log(avctx, AV_LOG_WARNING, "Timeout, device is so busy\n");
                return 0;
            }
            av_usleep(1000);
            busymsec++;
        } else {
            busymsec = 0;
            remove_surface_list(q);
        }
    } while (ret == MFX_ERR_MORE_DATA || ret == MFX_WRN_DEVICE_BUSY);

    if (ret == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM && frame->interlaced_frame)
        print_interlace_msg(avctx, q);

    ret = ret == MFX_ERR_MORE_DATA ? 0 : ff_qsv_error(ret);

    if (outbuf && outbuf->sync)
        add_sync_list(q, outbuf);

    if (q->pending_sync &&
        (q->nb_sync >= q->req.NumFrameMin || !frame)) {
        outbuf = q->pending_sync;

        ret = MFXVideoCORE_SyncOperation(q->session, outbuf->sync,
                                         SYNC_TIME_DEFAULT);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "MFXVideoCORE_SyncOperation(): %d\n", ret);
            return ff_qsv_error(ret);
        }

        remove_sync_list(q);

        if ((ret = ff_alloc_packet(pkt, outbuf->bs.DataLength)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_alloc_packet() failed\n");
            release_buffer(outbuf);
            return ret;
        }

        pkt->pts  = outbuf->bs.TimeStamp;
        pkt->size = outbuf->bs.DataLength;

        if (outbuf->bs.FrameType &
            (MFX_FRAMETYPE_I  | MFX_FRAMETYPE_IDR |
             MFX_FRAMETYPE_xI | MFX_FRAMETYPE_xIDR))
            pkt->flags |= AV_PKT_FLAG_KEY;

        memcpy(pkt->data, outbuf->bs.Data + outbuf->bs.DataOffset,
               outbuf->bs.DataLength);

        release_buffer(outbuf);

        *got_packet = 1;
    }

    return ret;
}

int ff_qsv_enc_close(AVCodecContext *avctx, QSVEncContext *q)
{
    MFXVideoENCODE_Close(q->session);

    MFXClose(q->session);

    free_surface_pool(q);

    free_buffer_pool(q);

    return 0;
}
