//
// Created by frank on 2018/2/1.
//
#include <jni.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/avfiltergraph.h"
#include "libavutil/opt.h"
#include "ffmpeg_jni_define.h"

#ifdef __cplusplus
}
#endif

#define TAG "AudioPlayer"
#define VOLUME_VAL 0.50
#define SLEEP_TIME 1000 * 16
#define MAX_AUDIO_FRAME_SIZE 48000 * 4

int init_volume_filter(AVFilterGraph **graph, AVFilterContext **src, AVFilterContext **sink,
        uint64_t channel_layout, AVSampleFormat inputFormat, int sample_rate) {
    AVFilterGraph   *filter_graph;
    AVFilterContext *buffer_ctx;
    const AVFilter  *buffer;
    AVFilterContext *volume_ctx;
    const AVFilter  *volume;
    AVFilterContext *buffersink_ctx;
    const AVFilter  *buffersink;
    AVDictionary *options_dict = NULL;
    uint8_t ch_layout[64];
    int ret;

    /* Create a new filter graph, which will contain all the filters. */
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        LOGE(TAG, "Unable to create filter graph...");
        return AVERROR(ENOMEM);
    }
    /* Create the abuffer filter: feed data into the graph. */
    buffer = avfilter_get_by_name("abuffer");
    if (!buffer) {
        LOGE(TAG, "Could not find the buffer filter...");
        return AVERROR_FILTER_NOT_FOUND;
    }
    buffer_ctx = avfilter_graph_alloc_filter(filter_graph, buffer, "src");
    if (!buffer_ctx) {
        LOGE(TAG, "Could not allocate the buffer instance...");
        return AVERROR(ENOMEM);
    }
    /* Set the filter options through the AVOptions API. */
    av_get_channel_layout_string(reinterpret_cast<char *>(ch_layout), sizeof(ch_layout), 0, channel_layout);
    av_opt_set    (buffer_ctx, "channel_layout", reinterpret_cast<char *>(ch_layout), AV_OPT_SEARCH_CHILDREN);
    av_opt_set    (buffer_ctx, "sample_fmt",     av_get_sample_fmt_name(inputFormat), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q  (buffer_ctx, "time_base",      (AVRational){ 1, sample_rate },      AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(buffer_ctx, "sample_rate",    sample_rate,                         AV_OPT_SEARCH_CHILDREN);
    ret = avfilter_init_str(buffer_ctx, NULL);
    if (ret < 0) {
        LOGE(TAG, "Could not initialize the buffer filter:%d", ret);
        return ret;
    }

    /* Create volume filter. */
    volume = avfilter_get_by_name("volume");
    if (!volume) {
        LOGE(TAG, "Could not find the volume filter...");
        return AVERROR_FILTER_NOT_FOUND;
    }
    volume_ctx = avfilter_graph_alloc_filter(filter_graph, volume, "volume");
    if (!volume_ctx) {
        LOGE(TAG, "Could not allocate the volume instance...");
        return AVERROR(ENOMEM);
    }
    /* Passing the options is as key/value pairs in a dictionary. */
    av_dict_set(&options_dict, "volume", AV_STRINGIFY(VOLUME_VAL), 0);
    ret = avfilter_init_dict(volume_ctx, &options_dict);
    av_dict_free(&options_dict);
    if (ret < 0) {
        LOGE(TAG, "Could not initialize the volume filter:%d", ret);
        return ret;
    }

    /* Create the abuffersink filter: get the filtered data from graph. */
    buffersink = avfilter_get_by_name("abuffersink");
    if (!buffersink) {
        LOGE(TAG, "Could not find the abuffersink filter...");
        return AVERROR_FILTER_NOT_FOUND;
    }
    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "sink");
    if (!buffersink_ctx) {
        LOGE(TAG, "Could not allocate the abuffersink instance...");
        return AVERROR(ENOMEM);
    }
    ret = avfilter_init_str(buffersink_ctx, NULL);
    if (ret < 0) {
        LOGE(TAG, "Could not initialize the abuffersink instance:%d", ret);
        return ret;
    }
    /* Connect the filters */
    ret = avfilter_link(buffer_ctx, 0, volume_ctx, 0);
    if (ret >= 0)
        ret = avfilter_link(volume_ctx, 0, buffersink_ctx, 0);
    if (ret < 0) {
        LOGE(TAG, "Error connecting filters:%d", ret);
        return ret;
    }
    /* Configure the graph. */
    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        LOGE(TAG, "Error configuring the filter graph:%d", ret);
        return ret;
    }
    *graph = filter_graph;
    *src   = buffer_ctx;
    *sink  = buffersink_ctx;
    return 0;
}

AUDIO_PLAYER_FUNC(void, play, jstring input_jstr) {
    int got_frame = 0, ret = 0;
    AVFilterGraph *audioFilterGraph;
    AVFilterContext *audioSrcContext;
    AVFilterContext *audioSinkContext;

    const char *input_cstr = env->GetStringUTFChars(input_jstr, NULL);
    LOGI(TAG, "input url=%s", input_cstr);
    av_register_all();
    AVFormatContext *pFormatCtx = avformat_alloc_context();
    if (avformat_open_input(&pFormatCtx, input_cstr, NULL, NULL) != 0) {
        LOGE(TAG, "Couldn't open the audio file!");
        return;
    }
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        LOGE(TAG, "Couldn't find stream info!");
        return;
    }
    int i = 0, audio_stream_idx = -1;
    for (; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = i;
            break;
        }
    }

    AVCodecContext *codecCtx = pFormatCtx->streams[audio_stream_idx]->codec;
    AVCodec *codec = avcodec_find_decoder(codecCtx->codec_id);
    if (codec == NULL) {
        LOGE(TAG, "Couldn't find audio decoder!");
        return;
    }
    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        LOGE(TAG, "Couldn't open audio decoder");
        return;
    }
    AVPacket *packet = (AVPacket *) av_malloc(sizeof(AVPacket));
    AVFrame *frame = av_frame_alloc();
    SwrContext *swrCtx = swr_alloc();

    enum AVSampleFormat in_sample_fmt = codecCtx->sample_fmt;
    enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int in_sample_rate = codecCtx->sample_rate;
    int out_sample_rate = in_sample_rate;
    uint64_t in_ch_layout = codecCtx->channel_layout;
    uint64_t out_ch_layout = AV_CH_LAYOUT_STEREO;

    swr_alloc_set_opts(swrCtx,
                       out_ch_layout, out_sample_fmt, out_sample_rate,
                       in_ch_layout, in_sample_fmt, in_sample_rate,
                       0, NULL);
    swr_init(swrCtx);
    int out_channel_nb = av_get_channel_layout_nb_channels(out_ch_layout);

    jclass player_class = env->GetObjectClass(thiz);
    if (!player_class) {
        LOGE(TAG, "player_class not found...");
    }
    //get AudioTrack by reflection
    jmethodID audio_track_method = env->GetMethodID(player_class, "createAudioTrack",
                                                       "(II)Landroid/media/AudioTrack;");
    if (!audio_track_method) {
        LOGE(TAG, "audio_track_method not found...");
    }
    jobject audio_track = env->CallObjectMethod(thiz, audio_track_method, out_sample_rate,
                                                   out_channel_nb);
    //call play method
    jclass audio_track_class = env->GetObjectClass(audio_track);
    jmethodID audio_track_play_mid = env->GetMethodID(audio_track_class, "play", "()V");
    env->CallVoidMethod(audio_track, audio_track_play_mid);
    //get write method
    jmethodID audio_track_write_mid = env->GetMethodID(audio_track_class, "write",
                                                          "([BII)I");
    uint8_t *out_buffer = (uint8_t *) av_malloc(MAX_AUDIO_FRAME_SIZE);

    /* Set up the filter graph. */
    AVFrame *filter_frame = av_frame_alloc();
    ret = init_volume_filter(&audioFilterGraph, &audioSrcContext, &audioSinkContext,
            in_ch_layout, in_sample_fmt, in_sample_rate);
    if (ret < 0) {
        LOGE(TAG, "Unable to init filter graph:%d", stderr);
        goto end;
    }

    //read audio frame
    while (av_read_frame(pFormatCtx, packet) >= 0) {
        if (packet->stream_index == audio_stream_idx) {
            ret = avcodec_decode_audio4(codecCtx, frame, &got_frame, packet);
            if (ret < 0) {
                break;
            }
            if (got_frame > 0) {
                ret = av_buffersrc_add_frame(audioSrcContext, frame);
                if (ret < 0) {
                    LOGE(TAG, "Error add the frame to the filter graph:%d", stderr);
                }
                /* Get all the filtered output that is available. */
                ret = av_buffersink_get_frame(audioSinkContext, filter_frame);
                if (ret == AVERROR(EAGAIN)) {
                    LOGE(TAG, "get filter frame again...");
                    continue;
                }
                if (ret < 0) {
                    LOGE(TAG, "Error get the frame from the filter graph:%d", stderr);
                    goto end;
                }

                //convert audio format
                swr_convert(swrCtx, &out_buffer, MAX_AUDIO_FRAME_SIZE,
                            (const uint8_t **) /*frame*/filter_frame->data, /*frame*/filter_frame->nb_samples);
                int out_buffer_size = av_samples_get_buffer_size(NULL, out_channel_nb,
                        /*frame*/filter_frame->nb_samples, out_sample_fmt, 1);

                jbyteArray audio_sample_array = env->NewByteArray(out_buffer_size);
                jbyte *sample_byte_array = env->GetByteArrayElements(audio_sample_array, NULL);
                memcpy(sample_byte_array, out_buffer, (size_t) out_buffer_size);
                env->ReleaseByteArrayElements(audio_sample_array, sample_byte_array, 0);
                //call write method to play
                env->CallIntMethod(audio_track, audio_track_write_mid,
                                      audio_sample_array, 0, out_buffer_size);
                env->DeleteLocalRef(audio_sample_array);
                av_frame_unref(filter_frame);
                usleep(SLEEP_TIME);
            }
        }
        av_packet_unref(packet);
    }
end:
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&filter_frame);
    av_free(out_buffer);
    swr_free(&swrCtx);
    avfilter_graph_free(&audioFilterGraph);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&pFormatCtx);
    env->ReleaseStringUTFChars(input_jstr, input_cstr);

}