#include <iostream>

using namespace std;

#define __STDC_CONSTANT_MACROS

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"  
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <unistd.h> 
#include <iostream>

typedef struct _PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size);
void audio_callback(void *userdata, uint8_t *stream, int len);
void saveFrame(AVFrame *pFrame, int w, int h, int iFrame);

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 1024

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

int quit = 0;
PacketQueue audioq;

int main(int argc, char** argv) {

    av_register_all();

    AVFormatContext *pFormatCtx = NULL;

    //open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) 
        return -1;

    //retrive streram information
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        return -1;
    }    

    av_dump_format(pFormatCtx, 0, argv[1], 0);

    unsigned int i = 0;
    AVCodecContext *pCodecCtx_aud = NULL;
    AVCodecContext *pCodecCtx = NULL;

    //find the first video stream
    int videoStream = -1;
    int audioStream = -1;
    for(i = 0; i<pFormatCtx->nb_streams; i++) {
        AVStream *st = pFormatCtx->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        if(type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        }
        else if(type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }

    if(videoStream == -1)
        return -1;
    
    if(audioStream == -1)
        return -1;
    
    cout<<"video id:"<<videoStream<<endl;
    cout<<"audio id:"<<audioStream<<endl;

    pCodecCtx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);

    pCodecCtx_aud = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(pCodecCtx_aud, pFormatCtx->streams[audioStream]->codecpar);

    AVCodec *pCodec = NULL;
    //find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if(pCodec == NULL) {
        cout<<"[video]Unsupported codec!"<<endl;
        return -1;
    }

    //open video codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        cout<<"[audio]couldn't open codec!"<<endl;
        return -1;
    }


    AVCodec *pCodec_aud = NULL;
    //find the decoder for audio stream
    pCodec_aud = avcodec_find_decoder(pCodecCtx_aud->codec_id);
    if(pCodec_aud == NULL) {
        cout<<"[audio]Unsupport codec!"<<endl;
        return -1;
    }

    //open audio codec
    if(avcodec_open2(pCodecCtx_aud, pCodec_aud, NULL) < 0) {
        cout<<"[audio]couldn't open codec!"<<endl;
    }

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


    //output to screen, using SDL
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        cout<<"could not initialize SDL - "<< SDL_GetError()<<endl;
        return -1;
    }

    /*
    [video] Configure to show, using SDL
    */
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

    /*
    [audio] set audio settings from codec, using SDL
    */
    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.freq = pCodecCtx_aud->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = pCodecCtx_aud->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_MIN_BUFFER_SIZE * 2;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = pCodecCtx_aud;

    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        cout<<"[audio]could not open audio, error:"<<SDL_GetError()<<endl;
        return -1;
    }
    //init audio data queue
    packet_queue_init(&audioq);

    SDL_PauseAudio(0);
    
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
    cout<<"playback end!"<<endl;
    quit = 1;    
    SDL_CloseAudio();
    SDL_Quit();
    av_free(buffer);
    av_free(pFrameYUV);
    av_free(pFrame);

    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtx_aud);    
    avformat_close_input(&pFormatCtx);

    return 0;
}

void audio_callback(void *userdata, uint8_t *stream, int len){
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int n, audio_size;

    uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE *3) / 2];
    unsigned int audio_buf_size = 0;
    unsigned int audio_buf_index = 0;

    SDL_memset(stream, 0, len);

    while(len > 0) {
        if(audio_buf_index >= audio_buf_size) {
            /*we have already set all our data, get more*/
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if(audio_size < 0) {
                /*error output silence*/
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            }
            else {
                audio_buf_size = audio_size;
            }

            audio_buf_index = 0;
        }

        n = audio_buf_size - audio_buf_index;
        if(n > len)
            n = len;

        memcpy(stream, (uint8_t *)audio_buf+audio_buf_index, n);
        //SDL_MixAudio(stream, audio_buf + audio_buf_index, len, SDL_MIX_MAXVOLUME);
        len -= n;
        stream += n;
        audio_buf_index += n;
    }
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size){
    AVPacket pkt;
    AVFrame *pFrame = av_frame_alloc();
    int data_size = -1;
    //int size = 0;
    int64_t dec_channel_layout;
    SwrContext *swrCtx = NULL;

    if(quit == 1)
        return -1;

    if(packet_queue_get(&audioq, &pkt, 1) < 0) {
        return -1;
    }

    //decode audio frame
    if(avcodec_send_packet(aCodecCtx, &pkt) == AVERROR(EAGAIN) ) {
        cout<<"[audio] could not send the packet to decoder! error:"<<AVERROR(EAGAIN)<<endl;
        av_packet_unref(&pkt);
    }

    av_packet_unref(&pkt);

    int ret = AVERROR(EAGAIN);
    do {
        ret = avcodec_receive_frame(aCodecCtx, pFrame);
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
            avcodec_flush_buffers(aCodecCtx);
            break;
        }

        if(ret >= 0)
            break;

    } while(ret != AVERROR(EAGAIN));

    av_frame_free(&pFrame);
    swr_free(&swrCtx);
    return data_size;    
}

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pktl;
   /* AVPacket copy = {0};
    
    if(av_packet_ref(&copy, pkt) < 0) {
        return -1;
    }
   */
    pktl = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if(!pktl)
        return -1;

    pktl->pkt = *pkt;
    pktl->next = NULL;

    SDL_LockMutex(q->mutex);

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
 
    while(1) {
        if(quit == 1) {
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
            SDL_CondWaitTimeout(q->cond, q->mutex, 10);
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
