#include <iostream>

using namespace std;

#define __STDC_CONSTANT_MACROS

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavutil/avstring.h"  
#include "libavutil/time.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <unistd.h> 
#include <iostream>

#define VIDEO_PICTURE_QUEUE_SIZE 3

typedef struct _VideoPicture {
    AVFrame *frame;
    double pts;
    int width;
    int height;
    int format;
    int uploaded;
    int flip_v;
} VideoPicture;

typedef struct _FrameQueue {
    VideoPicture queue[VIDEO_PICTURE_QUEUE_SIZE];
    int rindex; //read index
    int windex; //write index
    int size;
    int max_size;
    int rindex_shown; //nums of pictures having been readed
    SDL_mutex *mutex;
    SDL_cond *cond;
} FrameQueue;

typedef struct _PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct _VideoState {
    int seek_req;
    int seek_flags;
    int seek_pos;
    int seek_rel;

    AVFormatContext *ic;
    int video_stream;
    int audio_stream;

    FrameQueue pictq;

    double audio_clock;
    AVCodecContext *audio_avctx;
    AVStream *audio_st;
    PacketQueue audioq;
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;

    double frame_timer;
    double video_clock;
    AVCodecContext *video_avctx;
    AVStream *video_st;
    PacketQueue videoq;

    SDL_Texture *vid_texture;
    struct SwsContext *img_convert_ctx;

    SDL_Thread *read_tid;
    SDL_Thread *video_tid;

    char *filename;
    int abort_request;
} VideoState;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

static unsigned sws_flags = SWS_BICUBIC;

void frame_queue_destroy(FrameQueue *f);
int packet_queue_init(PacketQueue *q);
void packet_queue_flush(PacketQueue *q);
void packet_queue_start(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
void packet_queue_destroy(PacketQueue *q);
void packet_queue_abort(PacketQueue *q);
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr);
void audio_callback(void *userdata, uint8_t *stream, int len);
void saveFrame(AVFrame *pFrame, int w, int h, int iFrame);

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 1024

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0


static int default_width  = 640;
static int default_height = 480;
static SDL_Window *window;
static SDL_Renderer *renderer;

AVPacket flush_pkt;

void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || (unsigned int)stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        packet_queue_abort(&is->audioq);
        SDL_CloseAudio();
        break;
    case AVMEDIA_TYPE_VIDEO:
        packet_queue_abort(&is->videoq);
        break;
    default:
        break;
    }
}

void do_exit(VideoState *is) {
    if(is) {
        is->abort_request = 1;
        SDL_WaitThread(is->read_tid, NULL);

        /* close each stream */
        if (is->audio_stream >= 0)
            stream_component_close(is, is->audio_stream);
        if (is->video_stream >= 0)
            stream_component_close(is, is->video_stream);

        avformat_close_input(&is->ic);

        packet_queue_destroy(&is->videoq);
        packet_queue_destroy(&is->audioq);

        /* free all pictures */
        frame_queue_destroy(&is->pictq);

        sws_freeContext(is->img_convert_ctx);
        av_free(is->filename);

        if (is->vid_texture)
            SDL_DestroyTexture(is->vid_texture);

        av_free(is);

    }

    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();
    exit(0);
}

int audio_open(void *arg, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) {

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.freq = wanted_sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_MIN_BUFFER_SIZE * 2;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = arg;

    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        cout<<"[audio]could not open audio, error:"<<SDL_GetError()<<endl;
        return -1;
    }

    return  spec.size;
}

double get_audio_clock(VideoState *is) {
    double pts;
    int hw_buf_size, bytes_per_sec;

    pts = is->audio_clock;
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;
    bytes_per_sec = 0;

    if(is->audio_st) {
        bytes_per_sec = 2 * is->audio_avctx->channels * is->audio_avctx->sample_rate;
    }

    if(bytes_per_sec > 0) {
        pts -= (double)hw_buf_size / bytes_per_sec;
    }

    return pts;
}

int video_decode_frame(VideoState *is, AVFrame *frame) {
    int got_picture = 0;
    AVPacket pkt1, *pkt = &pkt1;
    AVCodecContext *avctx = is->video_avctx;

    if(is->abort_request == 1)
        return -1;

    if(packet_queue_get(&is->videoq, pkt, 1) < 0) {
        return -1;
    }

    //flush codec
    if(pkt->data == flush_pkt.data) {
        cout<<"[video] codec flush"<<endl;
        avcodec_flush_buffers(avctx);
        return 0;
    }

    //decode video frame
    if(avcodec_send_packet(avctx, pkt) == AVERROR(EAGAIN) ) {
        cout<<"[video] could not send the packet to decoder! error:"<<AVERROR(EAGAIN)<<endl;
    }

    av_packet_unref(pkt);

    int ret = AVERROR(EAGAIN);
    do {
        ret = avcodec_receive_frame(avctx, frame);
        if(ret >= 0) {
            got_picture = 1;
        }

        if (ret == AVERROR_EOF) {
            cout<<"eof"<<endl;
            avcodec_flush_buffers(avctx);
            break;
        }

        if(ret >= 0)
            break;

    } while(ret != AVERROR(EAGAIN));

    if(got_picture) {
       // cout<<frame->width<<endl;
        frame->pts = frame->best_effort_timestamp;
        if(frame->pts == AV_NOPTS_VALUE) {
            frame->pts = 0;
        }
    }

    return got_picture;
}

int frame_queue_init(FrameQueue *f, int max_size) {
    memset(f, 0, sizeof(FrameQueue));

    if(!(f->mutex = SDL_CreateMutex())) {
        return AVERROR(ENOMEM);
    }

    if(!(f->cond = SDL_CreateCond())) {
        return AVERROR(ENOMEM);
    }

    f->max_size = max_size;
    for(int i = 0; i<max_size; i++) {
        if(!(f->queue[i].frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

void frame_queue_destroy(FrameQueue *f) {
    for(int i = 0; i<f->max_size; i++) {
        av_frame_unref(f->queue[i].frame);
        av_frame_free(&f->queue[i].frame);
    }

    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

void frame_queue_push(FrameQueue *f) {
    f->windex++;
    if(f->windex == f->max_size)
        f->windex = 0;

    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

void frame_queue_next(FrameQueue *f) {
    if(!f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }

    av_frame_unref(f->queue[f->rindex].frame);

    f->rindex++;
    if (f->rindex == f->max_size)
        f->rindex = 0;

    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

VideoPicture *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

VideoPicture *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

VideoPicture *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

VideoPicture *frame_queue_peek_writable(FrameQueue *f) {
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while(f->size >= f->max_size) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    return &f->queue[f->windex];
}

VideoPicture *frame_queue_peek_readable(FrameQueue *f) {
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while(f->size - f->rindex_shown <= 0) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

int queue_picture(VideoState *is, double pts, AVFrame *src_frame) {
    VideoPicture *vp;
    double frame_delay;

    //computer the exact PTS for the picuure
    if(pts != 0) {
        is->video_clock = pts;
    }
    else {
        pts = is->video_clock;
    }
    //update video clock for next frame
    frame_delay = av_q2d(is->video_avctx->time_base);
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;
    //cout<<"[ video_clock]:"<<is->video_clock<<endl;

    if(!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->uploaded = 0;
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;
    vp->pts = pts;

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);

    return 0;
}

int video_thread(void *arg) {
    //cout<<"video thread"<<endl;
    VideoState *is = (VideoState *)arg;
    AVFrame *frame = av_frame_alloc();
    double pts = 0;
    AVRational tb = is->video_st->time_base;
    int ret = 0;

    if(!frame) {
        return AVERROR(ENOMEM);
    }

    for(;;){
        ret = video_decode_frame(is, frame);
        if(ret < 0)
            goto end;
        else if(ret == 0)
            continue;

        pts = frame->pts * av_q2d(tb);

        ret = queue_picture(is, pts, frame);
        av_frame_unref(frame);
        if(ret < 0)
            goto end;
    }

end:
    av_frame_free(&frame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *pFormatCtx = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;

    if(stream_index < 0 || (unsigned int)stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

    avctx = avcodec_alloc_context3(NULL);
    if(!avctx)
        return AVERROR(ENOMEM);

    if(avcodec_parameters_to_context(avctx, pFormatCtx->streams[stream_index]->codecpar) < 0) {
        goto fail;
    }

    codec = avcodec_find_decoder(avctx->codec_id);
    if(!codec) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        cout<<"open2 failed"<<endl;
        goto fail;
    }

    switch(avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        sample_rate = avctx->sample_rate;
        nb_channels = avctx->channels;
        channel_layout = avctx->channel_layout;

        //prepare audio output
        if((ret = audio_open(is, channel_layout, nb_channels, sample_rate)) < 0) {
            goto fail;
        }

        is->audio_stream = stream_index;
        is->audio_st = pFormatCtx->streams[stream_index];
        is->audio_avctx = avctx;

        //start audio packet queue
        packet_queue_start(&is->audioq);

        SDL_PauseAudio(0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = pFormatCtx->streams[stream_index];
        is->video_avctx = avctx;    

        //start video packet queue
        packet_queue_start(&is->videoq);

        is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
        if(!is->video_tid) {
            cout<<"video tid error"<<endl;
            ret = -1;
            goto fail;
        }
        else {
            ret = 0;
        }

        break;
    default:
        break;
    }

    goto out;

fail:
    avcodec_free_context(&avctx);

out:
    return ret;
}

void set_default_window_size(int width, int height) {
    default_width  = width;
    default_height = height;
}

int read_thread(void * arg) {
    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *pkt = &pkt1;
    int video_stream = -1;
    int audio_stream = -1;
    int ret = 0;
    int eof = 0;

    is->abort_request = 0;
    is->audio_stream = -1;
    is->video_stream = -1;

    pFormatCtx = avformat_alloc_context();
    if(!pFormatCtx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    //open file
    if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) < 0) {
        ret = -1;
        goto fail;
    }

    //rettive stream information
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        ret = -1;
        goto fail;
    }

    av_dump_format(pFormatCtx, 0, is->filename, 0);
    is->ic = pFormatCtx;

    //find video/audio stream
    for(unsigned int i = 0; i<pFormatCtx->nb_streams; i++) {
        AVStream *st = pFormatCtx->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        if(type == AVMEDIA_TYPE_VIDEO && video_stream < 0) {
            video_stream = i;
        }
        else if(type == AVMEDIA_TYPE_AUDIO && audio_stream < 0) {
            audio_stream = i;
        }
    }

    if(video_stream >= 0) {
        AVStream *st = pFormatCtx->streams[video_stream];
        AVCodecParameters *codecpar = st->codecpar;
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height);
    }

    //open video stream
    if(video_stream >= 0) {
        stream_component_open(is, video_stream);
    }

    //open audio stream
    if(audio_stream >= 0) {\
        stream_component_open(is, audio_stream);
    }

    if(video_stream < 0 && audio_stream < 0) {
        ret = -1;
        goto fail;
    }

    for(;;) {
        if(is->abort_request)
            break;

        if(is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if(ret <0) {
                cout<<"error while seeking"<<endl;
            }
            else {
                if(is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }

                if(is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
            }

            is->seek_req = 0;
            eof = 0;
        }

        if(eof) {
            //play end
            if(is->audioq.size + is->videoq.size == 0) {
                cout<<"play eof!"<<endl;
                ret = AVERROR_EOF;
                goto fail;
            }

            eof = 0;
            continue;
        }

        if((ret = av_read_frame(pFormatCtx, pkt)) < 0) {
            if(ret == AVERROR_EOF) {
                eof = 1;
            }

            if(pFormatCtx->pb && pFormatCtx->pb->error)
                break;

            SDL_Delay(10);
            continue;
        }
        else {
            eof = 0;
        }

        if(pkt->stream_index == is->audio_stream) {
            //cout<<"put audio packet"<<endl;
            packet_queue_put(&is->audioq, pkt);
        }
        else if(pkt->stream_index == is->video_stream) {
            //cout<<"put video packet"<<endl;
            packet_queue_put(&is->videoq, pkt);
        }
        else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if(pFormatCtx && !is->ic) {
        avformat_close_input(&pFormatCtx);
    }

    if(ret != 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }

    cout<<"playback end!"<<endl;

    return 0;   
}

int video_open(VideoState *is) {
    int w, h;

    w = default_width;
    h = default_height;

    if(!window) {
        window = SDL_CreateWindow("My Video", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  w, h, SDL_WINDOW_OPENGL);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if(window) {
            SDL_RendererInfo info;
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if(!renderer) {
                renderer = SDL_CreateRenderer(window, -1, 0);
            }

            if(renderer) {
                if (!SDL_GetRendererInfo(renderer, &info)) {
                        cout<<"initialized "<<info.name<<"renderer."<<endl;
                }
            }
        }
    }
    else {
        SDL_SetWindowSize(window, w, h);
    }

    if(!window || !renderer) {
        do_exit(is);
    }

    return 0;
}

void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;

    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1) {
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    }

    for (int i = 0; sdl_texture_format_map[i].format != AV_PIX_FMT_NONE; i++) {
         if (sdl_texture_format_map[i].format == format) {
             *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
             break;
         }
    }
}

int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode) {
    Uint32 format;
    int access, w, h;

    if(SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        SDL_DestroyTexture(*texture);

        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height))) {
            return -1;
        }

        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0) {
            return -1;
        }
    }

    return 0;
}

int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;

    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);

    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, 
        frame->width, frame->height, sdl_blendmode) < 0)
        return -1;

    switch(sdl_pix_fmt) {
    case SDL_PIXELFORMAT_UNKNOWN:
        /* This should only happen if we are not using avfilter... */
        *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                frame->width, frame->height, (enum AVPixelFormat)frame->format, frame->width, frame->height,
                AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
        if (*img_convert_ctx != NULL) {
            uint8_t *pixels[4];
            int pitch[4];
            if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                          0, frame->height, pixels, pitch);
                SDL_UnlockTexture(*tex);
            }
        }
        else {
            ret = -1;
        } 
        break;
    case SDL_PIXELFORMAT_IYUV:
        SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], 
                             frame->data[2], frame->linesize[2]);
            
        break;
    default:
        SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
        break;
    }

    return ret;
}

void video_image_display(VideoState *is) {
    VideoPicture *vp;

    vp = frame_queue_peek_last(&is->pictq);
    if(!vp->uploaded) {
        //cout<<"pts:"<<vp->pts<<endl;

        if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx) < 0)
            return;
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }
    
    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, NULL, 0, NULL, vp->flip_v? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
   // SDL_RenderCopy(renderer, is->vid_texture, NULL, NULL);

}

//diaplay the current picture
void video_diaplay(VideoState *is) {
    if(!window) {
        video_open(is);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if(is->video_st)
        video_image_display(is);

    SDL_RenderPresent(renderer);
}

double compute_target_delay(double last_duration, VideoState *is, double pts) {
    double sync_threshold, diff = 0;
    double delay = 0;

    diff = pts - get_audio_clock(is); //diff<0 => video slow, diff>0 => video quick
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, last_duration));
    if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
        if(diff <= -sync_threshold) { //video slow
            delay = FFMAX(0, last_duration + diff);
        }
        else if(diff >= sync_threshold && last_duration > AV_SYNC_FRAMEDUP_THRESHOLD) { //video fast
            delay = last_duration + diff;
        }
        else if(diff >= sync_threshold) { //video fast
            delay = 2 * last_duration;
        }
    }    

   // cout<<"video:delay = "<<delay<<" A-V = "<<-diff<<endl;

    return delay;
}

//display each frame
void video_refresh(void *arg, double *remaining_time) {
    VideoState *is = (VideoState *)arg;

    if(is->video_st) {
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        }
        else {
            double last_duration, delay;
            double time;
            VideoPicture *vp, *lastvp;

            //dequeue the picture
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            last_duration = vp->pts - lastvp->pts;
           if(last_duration <= 0 || last_duration >= 1.0)
                last_duration = lastvp->pts;

            delay = compute_target_delay(last_duration, is, vp->pts);
        
            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }
        
            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            frame_queue_next(&is->pictq);

display:
            if(is->pictq.rindex_shown)
                video_diaplay(is); 
        }    
    }

}

void refresh_loop_wait_event(VideoState* is, SDL_Event *event) {
   double remaining_time = 0.0; 
   SDL_PumpEvents();
    while(!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));

        remaining_time = REFRESH_RATE;

        video_refresh(is, &remaining_time);

        SDL_PumpEvents();    
    }
}

void stream_seek(VideoState* is, int64_t pos, int rel) {
    if(!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;

        is->seek_req = 1;
    }
}

void event_loop(VideoState *is) {
    SDL_Event event;
    double incr, pos;

    for(;;) {
        refresh_loop_wait_event(is, &event);
        switch(event.type) {
        case FF_QUIT_EVENT:
            cout<<"do exit"<<endl;
            do_exit(is);
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
            case SDLK_LEFT:
                cout<<"left"<<endl;
                incr = -2.0;
                goto do_seek;
            case SDLK_RIGHT:
                cout<<"right"<<endl;
                incr = 2.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                pos = get_audio_clock(is);
                pos += incr;
                stream_seek(is, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE));
                break;
            default:
                break;          
            }
            break;        
        default:
            break;
        }
    }
}

int main(int argc, char** argv) {
    VideoState *is = NULL;
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    //using SDL
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        cout<<"could not initialize SDL - "<< SDL_GetError()<<endl;
        return -1;
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    is = (VideoState *)av_mallocz(sizeof(VideoState));
    if(!is)
        return -1;

    is->filename = av_strdup(argv[1]);
    if(!is->filename) {
        av_free(is);
        return -1;
    }

    /* start video display */
    if(frame_queue_init(&is->pictq, VIDEO_PICTURE_QUEUE_SIZE) < 0) {
        goto fail;
    }

    if(packet_queue_init(&is->audioq) < 0 ||
       packet_queue_init(&is->videoq) < 0) {
        goto fail;
    }

    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if(!is->read_tid) {
fail:
        is->abort_request = 1;
        SDL_WaitThread(is->read_tid, NULL);

         //close each stream 
        if (is->audio_stream >= 0)
            stream_component_close(is, is->audio_stream);
        if (is->video_stream >= 0)
            stream_component_close(is, is->video_stream);

        avformat_close_input(&is->ic);

        packet_queue_destroy(&is->videoq);
        packet_queue_destroy(&is->audioq);

        // free all pictures 
        frame_queue_destroy(&is->pictq);

        sws_freeContext(is->img_convert_ctx);
        av_free(is->filename);

        if (is->vid_texture)
            SDL_DestroyTexture(is->vid_texture);

        av_free(is);
        return -1;
    }

    event_loop(is);

    return 0;




/*
    AVFrame *pFrame = NULL;
    //alloc video frame
    pFrame = av_frame_alloc();
    
    AVFrame *pFrameYUV = NULL;
    pFrameYUV = av_frame_alloc();

    uint8_t *buffer = NULL;
    int numBytes = 0;
    numBytes =  av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);

    struct SwsContext *sws_ctx = NULL;
    AVPacket packet;

    //initial SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
                             pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                             NULL, NULL, NULL);

    SDL_Window *window;
    window = SDL_CreateWindow("My Video", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
    pCodecCtx->width, pCodecCtx->height, SDL_WINDOW_OPENGL);
    if(!window) {
        cout<<"SDL:could not create window."<<endl;
        return -1;
    }

    SDL_Renderer *render = SDL_CreateRenderer(window, -1, 0);
    if(!render) {
        cout<<"SDL: could not create renderer."<<endl;
        return -1;
    } 
    SDL_SetRenderDrawColor(render, 0, 0, 0, 255);
    SDL_RenderClear(render);
    SDL_RenderPresent(render);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(render, pCodecCtx->width, pCodecCtx->height);
    
    SDL_Texture *texture;
    texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);


    
    SDL_Event event;
 
    i = 0;

    while(av_read_frame(pFormatCtx, &packet) >= 0) {
        if(packet.stream_index == videoStream) {
            //decode video frame
            if(avcodec_send_packet(pCodecCtx, &packet) ==  AVERROR(EAGAIN)) {
                cout<<"[video] could not send the packet to decoder!"<<endl;
            }

            av_packet_unref(&packet);
            
            int ret = AVERROR(EAGAIN);
            do {
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if(ret >= 0) {
                    //convert the image from its native format to YUVi
                    sws_scale(sws_ctx, (const uint8_t * const *)pFrame->data, pFrame->linesize, 0,
                          pFrame->height, pFrameYUV->data, pFrameYUV->linesize);

                    //if(++i < 5) {
                       // saveFrame(pFrameYUV, pCodecCtx->width, pCodecCtx->height, i);
                    //}
                    SDL_UpdateYUVTexture(texture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0], pFrameYUV->data[1], pFrameYUV->linesize[1], pFrameYUV->data[2], pFrameYUV->linesize[2]);
                    SDL_RenderClear(render);
                    SDL_RenderCopy(render, texture, NULL, NULL);
                    SDL_RenderPresent(render);
                }

                if (ret == AVERROR_EOF) {
                    avcodec_flush_buffers(pCodecCtx);
                    break;
                }
                   
                if(ret >= 0)
                    break;

            } while(ret != AVERROR(EAGAIN)); 

            SDL_Delay(30);
        } 
        else if(packet.stream_index == audioStream) {
            //cout<<"put audio packet"<<endl;
            packet_queue_put(&audioq, &packet);
        }
        else {
            av_packet_unref(&packet);
        }

        //av_packet_unref(&packet);
        SDL_PollEvent(&event);
        switch(event.type) {
           case SDL_QUIT:
               //SDL_Quit();
               cout<<"sdl quit!"<<endl;
               quit = 1;
               goto label;
               break;
           case SDL_KEYDOWN:
               //cout<<"sdl key down!"<<endl;
               break;
           case SDL_MOUSEMOTION:
               //cout<<"sdl mouse motion!"<<endl;
               break;
           default:
               //cout<<"sdl other event!"<<endl;
               break;
        }
    }

label:

    quit = 1;    
    SDL_CloseAudio();
    SDL_Quit();
    av_free(buffer);
    av_free(pFrameYUV);
    av_free(pFrame);

    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtx_aud);    
    avformat_close_input(&pFormatCtx);

    return 0;*/
}

void audio_callback(void *arg, uint8_t *stream, int len){
    VideoState *is = (VideoState *)arg;
    int n, audio_size;
    uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE *3) / 2];
    double pts;

    SDL_memset(stream, 0, len);

    while(len > 0) {
        if(is->audio_buf_index >= is->audio_buf_size) {
            /*we have already set all our data, get more*/
            audio_size = audio_decode_frame(is, audio_buf, sizeof(audio_buf), &pts);
            if(audio_size < 0) {
                /*error output silence*/
                is->audio_buf_size = 1024;
                memset(audio_buf, 0, is->audio_buf_size);
            }
            else {
                is->audio_buf_size = audio_size;
            }

            is->audio_buf_index = 0;
        }

        n = is->audio_buf_size - is->audio_buf_index;
        if(n > len)
            n = len;

        memcpy(stream, (uint8_t *)audio_buf + is->audio_buf_index, n);
        //SDL_MixAudio(stream, audio_buf + audio_buf_index, len, SDL_MIX_MAXVOLUME);
        len -= n;
        stream += n;
        is->audio_buf_index += n;
    }
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr){
    AVPacket pkt1, *pkt = &pkt1;
    AVFrame *pFrame = av_frame_alloc();
    AVCodecContext *avctx = is->audio_avctx;
    int data_size = -1;
    //int size = 0;
    int64_t dec_channel_layout;
    SwrContext *swrCtx = NULL;
    //AVRational tb;
    double pts;

    if(is->abort_request == 1)
        return -1;

    if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
        return -1;
    }

    //flush codec
    if(pkt->data == flush_pkt.data) {
        cout<<"[audio] codec flush"<<endl;
        avcodec_flush_buffers(avctx);
        return 0;
    }

    //if update, update the audio clock with pts
    if(pkt->pts != AV_NOPTS_VALUE) {
        is->audio_clock = av_q2d(is->audio_st->time_base) * pkt->pts;
       // cout<<"[packet audio_clock]:"<<is->audio_clock<<endl;
    }

    //decode audio frame
    if(avcodec_send_packet(avctx, pkt) == AVERROR(EAGAIN) ) {
        cout<<"[audio] could not send the packet to decoder! error:"<<AVERROR(EAGAIN)<<endl;
    }

    av_packet_unref(pkt);

    int ret = AVERROR(EAGAIN);
    do {
        ret = avcodec_receive_frame(avctx, pFrame);
        if(ret >= 0) {
            //get audio channel layout
            if(pFrame->channel_layout && pFrame->channels == av_get_channel_layout_nb_channels(pFrame->channel_layout)) {
                dec_channel_layout = pFrame->channel_layout;
            }
            else {
                dec_channel_layout = av_get_default_channel_layout(pFrame->channels);
            }

            AVSampleFormat dec_format = AV_SAMPLE_FMT_S16;
            swrCtx = swr_alloc_set_opts(NULL, dec_channel_layout, dec_format, pFrame->sample_rate, pFrame->channel_layout, (AVSampleFormat)pFrame->format, pFrame->sample_rate, 0, NULL);
            if(!swrCtx || swr_init(swrCtx) < 0) {
                break;
            }

            int dec_nb_samples = av_rescale_rnd(swr_get_delay(swrCtx, pFrame->sample_rate) + pFrame->nb_samples, pFrame->sample_rate, pFrame->sample_rate, AVRounding(1));
            int nb = swr_convert(swrCtx, &audio_buf, dec_nb_samples, (const uint8_t **)pFrame->data, pFrame->nb_samples);
            data_size = pFrame->channels * nb * av_get_bytes_per_sample(dec_format);
        }

        if (ret == AVERROR_EOF) {
            cout<<"eof"<<endl;
            avcodec_flush_buffers(avctx);
            break;
        }

        if(ret >= 0) {
            /*tb = (AVRational){1, pFrame->sample_rate};
            if(pFrame->pts == AV_NOPTS_VALUE) {
                pts = 0;
            }
            else {
                pFrame->pts = av_rescale_q(pFrame->pts, avctx->time_base, tb);
                pts = pFrame->pts * av_q2d(tb);
            }
            
            //cout<<"audio pts:"<<pts<<endl;
            if(pts != 0)
                is->audio_clock = pts + (double) pFrame->nb_samples / pFrame->sample_rate;
            else
                is->audio_clock = 0;
            //cout<<"audio clock :"<<is->audio_clock<<endl; 
            */
            pts = is->audio_clock;
            *pts_ptr = pts;
            is->audio_clock += (double)data_size / (double)(2 * avctx->channels * avctx->sample_rate);
//            cout<<"[modify audio_clock]:"<<is->audio_clock<<endl;
            break;
        }

    } while(ret != AVERROR(EAGAIN));

    av_frame_free(&pFrame);
    if(swrCtx)
        swr_free(&swrCtx);

    return data_size;    
}

void packet_queue_destroy(PacketQueue *q) {
    //packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

void packet_queue_abort(PacketQueue *q) {
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if(!q->mutex) {
        return AVERROR(ENOMEM);
    }

    q->cond = SDL_CreateCond();
    if(!q->cond) {
        return AVERROR(ENOMEM);
    }

    q->abort_request = 1;

    return 0;
}

void packet_queue_flush(PacketQueue *q) {
    AVPacketList *pkt, *pktl;
   
    SDL_LockMutex(q->mutex);
    for(pkt = q->first_pkt; pkt != NULL; pkt = pktl) {
        pktl = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }

    cout<<endl;
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
 
    SDL_UnlockMutex(q->mutex);
}

void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    SDL_UnlockMutex(q->mutex);
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pktl;
    AVPacket copy = {0};
    
    if(pkt != &flush_pkt && av_packet_ref(&copy, pkt) < 0) {
        return -1;
    }

    SDL_LockMutex(q->mutex);

    if(q->abort_request) {
        return -1;
    }

    pktl = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if(!pktl) {
        return -1;
    }

    pktl->pkt = *pkt;
    pktl->next = NULL;

    if(!q->last_pkt)
        q->first_pkt = pktl;
    else
        q->last_pkt->next = pktl;

    q->last_pkt = pktl;
    q->nb_packets++;
    q->size += pktl->pkt.size;
    
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);

    return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pktl;
    int ret = -1;
    
    SDL_LockMutex(q->mutex);
 
    for(;;) {
        if(q->abort_request) {
            ret = -1;
            break;
        }

        pktl = q->first_pkt;
        if(pktl) {
            q->first_pkt = pktl->next;

            if(!q->first_pkt) {
                q->last_pkt = NULL;
            }

            q->nb_packets--;
            q->size -= pktl->pkt.size;
            *pkt = pktl->pkt;
            av_free(pktl);
            
            ret = 1;
            break;
        }
        else if(!block) {
            ret = 0;
            break;
        }
        else {
            //SDL_CondWaitTimeout(q->cond, q->mutex, 10);
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);

    return ret;
}

void saveFrame(AVFrame *pFrame, int w, int h, int iFrame) {
    FILE *pFile = NULL;
    char szFileName[32];

    //open file
    sprintf(szFileName, "frame%d.ppm", iFrame);
    pFile = fopen(szFileName, "wb");
    if(pFile == NULL)
        return;

    //write header
    fprintf(pFile, "P6\n%d %d\n255\n", w, h);

    //write pixel data
    fwrite(pFrame->data[0], 1, w * h, pFile);
    fwrite(pFrame->data[1], 1, w * h /4, pFile);
    fwrite(pFrame->data[2], 1, w * h /4, pFile);

    fclose(pFile);
}
