#include "video_decoder.h"
#include "util.h"

extern Logger g_logger;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

// Debug logging macro — compiles out when logger level > DEBUG
#define DEBUG_LOG(fmt, ...) \
    do { \
        g_logger.debug(fmt, ##__VA_ARGS__); \
    } while (0)

VideoDecoder::VideoDecoder() : m_thread(0) {}

void* VideoDecoder::decode_thread_entry(void* arg) {
    VideoDecoder* self = static_cast<VideoDecoder*>(arg);
    try { self->decode_loop(); }
    catch (...) { /* swallow all */ }
    return nullptr;
}

VideoDecoder::~VideoDecoder() {
    stop();
}

void VideoDecoder::init_audio() {
    std::lock_guard lk(m_audio_mtx);
    if (m_audio_initialized) return;

    DEBUG_LOG("AUDIO: Initializing SDL audio device");

    SDL_AudioSpec desired;
    desired.freq = 48000;
    desired.format = SDL_AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;

    if (!SDL_LoadWAV("audio", &desired, nullptr, nullptr)) {
        // Just set manually — we don't need a WAV
        desired.freq = 48000;
        desired.format = SDL_AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 1024;
    }

    m_output_spec = desired;
    int dev_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired);
    if (dev_id <= 0) {
        DEBUG_LOG("AUDIO: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }

    m_audio_spec = new SDL_AudioDeviceSpec;
    m_audio_spec->spec = desired;

    m_audio_stream = SDL_CreateAudioStream(m_audio_spec->spec.format,
                                           m_audio_spec->spec.channels,
                                           m_audio_spec->spec.freq,
                                           m_audio_spec->spec.format,
                                           m_audio_spec->spec.channels,
                                           m_audio_spec->spec.freq);
    if (!m_audio_stream) {
        DEBUG_LOG("AUDIO: SDL_CreateAudioStream failed: %s", SDL_GetError());
        SDL_CloseAudioDevice(dev_id);
        delete m_audio_spec;
        m_audio_spec = nullptr;
        return;
    }

    SDL_PauseAudioDevice(dev_id, false);
    m_audio_initialized = true;
    DEBUG_LOG("AUDIO: Audio device opened (id=%d, freq=%d, ch=%d)",
              dev_id, m_audio_spec->spec.freq, m_audio_spec->spec.channels);
}

void VideoDecoder::shutdown_audio() {
    std::lock_guard lk(m_audio_mtx);
    if (!m_audio_initialized) return;

    DEBUG_LOG("AUDIO: Shutting down audio device");
    if (m_audio_stream) {
        SDL_DestroyAudioStream(m_audio_stream);
        m_audio_stream = nullptr;
    }
    if (m_audio_spec) {
        SDL_CloseAudioDevice(m_audio_spec->spec.device_id);
        delete m_audio_spec;
        m_audio_spec = nullptr;
    }
    m_audio_initialized = false;
}

void VideoDecoder::push_audio_samples(const int16_t* samples, int num_frames) {
    std::lock_guard lk(m_audio_mtx);
    if (!m_audio_initialized || !m_audio_stream) return;

    int bytes = num_frames * 2 * 2; // frames * channels * 2 bytes per sample
    SDL_PutAudioStreamData(m_audio_stream, samples, bytes);

    // Apply volume from config
    int volume = 0;
    {
        std::lock_guard lk(g_config_mtx);
        volume = g_cfg.video_volume;
    }
    float gain = volume / 100.0f;
    SDL_SetAudioStreamGain(m_audio_stream, gain);

    // Submit to device
    SDL_AudioStreamConvert(m_audio_stream, 0);
    const void* buf;
    int buf_len;
    if (SDL_PeekAudioStreamData(m_audio_stream, &buf, &buf_len) >= 0 && buf_len > 0) {
        SDL_FlushAudioStream(m_audio_stream);
        SDL_PutAudioStreamData(m_audio_stream, nullptr, 0);
    }
}

bool VideoDecoder::start(const std::string& path, int target_width, int target_height) {
    if (is_running()) {
        stop();
    }
    m_path = path;
    m_target_width = target_width;
    m_target_height = target_height;
    m_running.store(true);
    int rc = pthread_create(&m_thread, nullptr, decode_thread_entry, this);
    if (rc != 0) {
        g_logger.error("VIDEO_DEC: pthread_create failed: %d", rc);
        m_running.store(false);
        return false;
    }
    return true;
}

void VideoDecoder::stop() {
    m_running.store(false);
    m_queue_cv.notify_all();
    if (m_thread != 0) {
        pthread_join(m_thread, nullptr);
        m_thread = 0;
    }
    shutdown_audio();
    {
        std::lock_guard lk(m_queue_mtx);
        while (!m_frame_queue.empty()) {
            m_frame_queue.front().~VideoFrame();
            m_frame_queue.pop();
        }
    }
}

bool VideoDecoder::is_running() const {
    return m_running.load();
}

bool VideoDecoder::get_frame(VideoFrame& out) {
    std::lock_guard lk(m_queue_mtx);
    if (m_frame_queue.empty()) {
        return false;
    }
    out = std::move(m_frame_queue.front());
    m_frame_queue.pop();
    return true;
}

void VideoDecoder::decode_loop() {
    try {
    DEBUG_LOG("VIDEO_DEC: Starting decode thread for %s", m_path.c_str());

    avformat_network_init();

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, m_path.c_str(), nullptr, nullptr) != 0) {
        g_logger.error("VIDEO_DEC: Failed to open %s", m_path.c_str());
        m_running.store(false);
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        g_logger.error("VIDEO_DEC: Failed to find stream info for %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    // Find video and audio streams
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (video_stream_idx == -1 &&
            fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = (int)i;
        }
        if (audio_stream_idx == -1 &&
            fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = (int)i;
        }
        if (video_stream_idx >= 0 && audio_stream_idx >= 0) break;
    }

    if (video_stream_idx == -1) {
        g_logger.error("VIDEO_DEC: No video stream found in %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    DEBUG_LOG("VIDEO_DEC: Video stream=%d, Audio stream=%d", video_stream_idx, audio_stream_idx);

    // Video codec setup
    AVCodecParameters* v_codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* v_codec = avcodec_find_decoder(v_codec_params->codec_id);
    if (!v_codec) {
        g_logger.error("VIDEO_DEC: Unsupported video codec for %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    AVCodecContext* v_codec_ctx = avcodec_alloc_context3(v_codec);
    avcodec_parameters_to_context(v_codec_ctx, v_codec_params);
    if (avcodec_open2(v_codec_ctx, v_codec, nullptr) < 0) {
        g_logger.error("VIDEO_DEC: Failed to open video codec for %s", m_path.c_str());
        avcodec_free_context(&v_codec_ctx);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    // Audio codec setup
    AVCodecContext* a_codec_ctx = nullptr;
    if (audio_stream_idx >= 0) {
        AVCodecParameters* a_codec_params = fmt_ctx->streams[audio_stream_idx]->codecpar;
        const AVCodec* a_codec = avcodec_find_decoder(a_codec_params->codec_id);
        if (a_codec) {
            a_codec_ctx = avcodec_alloc_context3(a_codec);
            avcodec_parameters_to_context(a_codec_ctx, a_codec_params);
            if (avcodec_open2(a_codec_ctx, a_codec, nullptr) >= 0) {
                DEBUG_LOG("AUDIO: Opened audio codec: %s (sample_rate=%d, ch=%d, fmt=%d)",
                          a_codec->name,
                          a_codec_ctx->sample_rate,
                          a_codec_ctx->channels,
                          a_codec_ctx->sample_fmt);
            } else {
                DEBUG_LOG("AUDIO: Failed to open audio codec, skipping audio");
                avcodec_free_context(&a_codec_ctx);
                a_codec_ctx = nullptr;
            }
        } else {
            DEBUG_LOG("AUDIO: Audio codec not found, skipping audio");
        }
    }

    // Init SDL audio if we have an audio stream
    if (a_codec_ctx) {
        init_audio();
    }

    // Swresample context
    SwrContext* swr_ctx = nullptr;
    if (a_codec_ctx) {
        swr_ctx = swr_alloc_set_opts(nullptr,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
            a_codec_ctx->channel_layout ? a_codec_ctx->channel_layout : av_get_default_channel_layout(a_codec_ctx->channels),
            (AVSampleFormat)a_codec_ctx->sample_fmt, a_codec_ctx->sample_rate,
            0, nullptr);
        if (swr_ctx && swr_init(swr_ctx) < 0) {
            DEBUG_LOG("AUDIO: Failed to init swresample");
            swr_free(&swr_ctx);
        }
    }

    // Setup swscale context
    SwsContext* sws_ctx = sws_getContext(
        v_codec_ctx->width, v_codec_ctx->height, v_codec_ctx->pix_fmt,
        v_codec_ctx->width, v_codec_ctx->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        g_logger.error("VIDEO_DEC: Failed to create scaler for %s", m_path.c_str());
        if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
        avcodec_free_context(&v_codec_ctx);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    g_logger.info("VIDEO_DEC: Decoding %s (%dx%d)", m_path.c_str(), v_codec_ctx->width, v_codec_ctx->height);
    DEBUG_LOG("VIDEO_DEC: Pixel format=%d, has_audio=%d",
              v_codec_ctx->pix_fmt, a_codec_ctx ? 1 : 0);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgba_frame = av_frame_alloc();
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, v_codec_ctx->width, v_codec_ctx->height, 1);
    uint8_t* rgba_buffer = (uint8_t*)av_malloc(num_bytes);
    av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize, rgba_buffer,
                         AV_PIX_FMT_RGBA, v_codec_ctx->width, v_codec_ctx->height, 1);

    // Audio decode buffers
    AVFrame* a_frame = nullptr;
    if (a_codec_ctx) {
        a_frame = av_frame_alloc();
    }

    // Timestamp tracking for A/V sync
    double video_start_pts = 0.0;
    bool video_start_set = false;
    double audio_start_pts = 0.0;
    bool audio_start_set = false;
    double decode_start_time = av_gettime() / 1000000.0;

    bool eof = false;
    int frame_count = 0;
    int audio_frame_count = 0;
    while (is_running() && !eof) {
        int ret = av_read_frame(fmt_ctx, packet);
        if (ret == AVERROR_EOF) {
            eof = true;
            // Flush audio decoder
            if (a_codec_ctx && a_frame) {
                ret = avcodec_send_packet(a_codec_ctx, nullptr);
                if (ret >= 0) {
                    while (ret >= 0) {
                        ret = avcodec_receive_frame(a_codec_ctx, a_frame);
                        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
                        if (ret < 0) break;

                        // Resample and push
                        if (swr_ctx) {
                            const int max_samples = av_rescale_rnd(
                                a_frame->nb_samples, 48000, a_codec_ctx->sample_rate, AV_ROUND_UP);
                            int16_t* out_buf = new int16_t[max_samples * 2];
                            int out_samples = swr_convert(swr_ctx,
                                (uint8_t**)&out_buf, max_samples,
                                (const uint8_t**)a_frame->data, a_frame->nb_samples);
                            if (out_samples > 0) {
                                push_audio_samples(out_buf, out_samples);
                                audio_frame_count += out_samples;
                            }
                            delete[] out_buf;
                        }
                    }
                }
            }
            break;
        }
        if (ret < 0) {
            g_logger.error("VIDEO_DEC: Read error on %s", m_path.c_str());
            break;
        }

        if (packet->stream_index == video_stream_idx) {
            // Set video start timestamp
            if (!video_start_set) {
                video_start_pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * packet->pts;
                video_start_set = true;
                DEBUG_LOG("VIDEO_DEC: Video start PTS=%.3f", video_start_pts);
            }

            ret = avcodec_send_packet(v_codec_ctx, packet);
            av_packet_unref(packet);
            if (ret < 0) continue;

            ret = avcodec_receive_frame(v_codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) continue;
            if (ret < 0) {
                g_logger.error("VIDEO_DEC: Decode error");
                break;
            }

            frame_count++;
            // Convert to RGBA
            sws_scale(sws_ctx, frame->data, frame->linesize, 0, v_codec_ctx->height,
                      rgba_frame->data, rgba_frame->linesize);

            // Create VideoFrame and push to queue
            VideoFrame vf;
            vf.width = v_codec_ctx->width;
            vf.height = v_codec_ctx->height;
            vf.data = new uint8_t[num_bytes];
            memcpy(vf.data, rgba_buffer, num_bytes);
            vf.pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * frame->pts;

            DEBUG_LOG("VIDEO_DEC: Frame #%d queued, PTS=%.3f", frame_count, vf.pts);

            std::lock_guard lk(m_queue_mtx);
            // Keep only the latest frame
            while (!m_frame_queue.empty()) {
                m_frame_queue.pop();
            }
            m_frame_queue.push(std::move(vf));

        } else if (packet->stream_index == audio_stream_idx && a_codec_ctx && a_frame) {
            // Set audio start timestamp
            if (!audio_start_set) {
                audio_start_pts = av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base) * packet->pts;
                audio_start_set = true;
                DEBUG_LOG("AUDIO: Audio start PTS=%.3f", audio_start_pts);
            }

            ret = avcodec_send_packet(a_codec_ctx, packet);
            av_packet_unref(packet);
            if (ret < 0) continue;

            while (ret >= 0) {
                ret = avcodec_receive_frame(a_codec_ctx, a_frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
                if (ret < 0) break;

                // Resample to S16 48kHz stereo and push
                if (swr_ctx) {
                    const int max_samples = av_rescale_rnd(
                        a_frame->nb_samples, 48000, a_codec_ctx->sample_rate, AV_ROUND_UP);
                    int16_t* out_buf = new int16_t[max_samples * 2];
                    int out_samples = swr_convert(swr_ctx,
                        (uint8_t**)&out_buf, max_samples,
                        (const uint8_t**)a_frame->data, a_frame->nb_samples);
                    if (out_samples > 0) {
                        push_audio_samples(out_buf, out_samples);
                        audio_frame_count += out_samples;
                    }
                    delete[] out_buf;
                }
            }
        } else {
            av_packet_unref(packet);
        }
    }

    double decode_elapsed = av_gettime() / 1000000.0 - decode_start_time;
    g_logger.info("VIDEO_DEC: Decode finished for %s (%.1fs, %d video frames, %d audio samples)",
                  m_path.c_str(), decode_elapsed, frame_count, audio_frame_count);
    DEBUG_LOG("VIDEO_DEC: Video duration=%.3fs, Audio duration=%.3fs",
              video_start_pts > 0 ? (frame_count > 0 ? (frame_count * 0.04) : 0) : 0,
              audio_start_pts > 0 ? (audio_frame_count / 48000.0) : 0);

    // Cleanup
    try {
    av_frame_free(&rgba_frame);
    av_frame_free(&frame);
    if (a_frame) av_frame_free(&a_frame);
    av_packet_free(&packet);
    av_free(rgba_buffer);
    sws_freeContext(sws_ctx);
    swr_free(&swr_ctx);
    if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
    avcodec_free_context(&v_codec_ctx);
    avformat_close_input(&fmt_ctx);
    } catch (...) { /* swallow cleanup errors */ }

    } catch (const std::exception& e) {
        g_logger.error("VIDEO_DEC: Exception in decode_loop: %s", e.what());
    } catch (...) {
        g_logger.error("VIDEO_DEC: Unknown exception in decode_loop");
    }
    m_running.store(false);
}
