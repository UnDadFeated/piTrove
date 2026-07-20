#include "cache.h"
#include "video_decoder.h"
#include <SDL3/SDL_timer.h>
#include "util.h"
#include "config.h"

extern Logger g_logger;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>


struct VideoLimits {
    int max_width = 1920;
    int max_height = 1080;
    int64_t max_bitrate = 20 * 1000 * 1000;
    int max_duration_seconds = 300;
};

static bool video_within_budget(AVFormatContext* fmt, const VideoLimits& limits) {
    if (!g_cfg.video_decode_budget_enabled) {
        return true; // Budget enforcement disabled: downscale and play any video using HW decoding
    }
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (stream->codecpar->width > limits.max_width ||
                stream->codecpar->height > limits.max_height) {
                return false;
            }
            if (stream->codecpar->bit_rate > limits.max_bitrate) {
                return false;
            }
        }
    }
    if (fmt->duration > 0) {
        int seconds = static_cast<int>(fmt->duration / AV_TIME_BASE);
        if (seconds > limits.max_duration_seconds) {
            return false;
        }
    }
    return true;
}

static AVBufferRef* create_hw_device() {
    AVBufferRef* hw_device_ctx = nullptr;
    if (av_hwdevice_ctx_create(&hw_device_ctx,
                               AV_HWDEVICE_TYPE_DRM,
                               nullptr, nullptr, 0) >= 0) {
        return hw_device_ctx;
    }
    return nullptr;
}

}

#define DEBUG_LOG(fmt, ...) \
    do { g_logger.debug(fmt, ##__VA_ARGS__); } while (0)

VideoDecoder::VideoDecoder() : m_thread(0) {}

void* VideoDecoder::decode_thread_entry(void* arg) {
    VideoDecoder* self = static_cast<VideoDecoder*>(arg);
    try { self->decode_loop(); }
    catch (...) {}
    return nullptr;
}

VideoDecoder::~VideoDecoder() { stop(); }

void VideoDecoder::init_audio() {
    std::lock_guard lk(m_audio_mtx);
    if (m_audio_initialized) return;

    DEBUG_LOG("AUDIO: Initializing SDL audio device");

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 48000;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;

    m_audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (m_audio_device < 0) {
        DEBUG_LOG("AUDIO: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }

    m_audio_stream = SDL_CreateAudioStream(&spec, &spec);
    if (!m_audio_stream) {
        DEBUG_LOG("AUDIO: SDL_CreateAudioStream failed: %s", SDL_GetError());
        SDL_CloseAudioDevice(m_audio_device);
        m_audio_device = -1;
        return;
    }

    SDL_BindAudioStream(m_audio_device, m_audio_stream);
    m_audio_initialized = true;
    DEBUG_LOG("AUDIO: Device opened id=%d", m_audio_device);
}

void VideoDecoder::shutdown_audio() {
    std::lock_guard lk(m_audio_mtx);
    if (!m_audio_initialized) return;

    DEBUG_LOG("AUDIO: Shutting down audio device");
    m_audio_initialized = false;
    if (m_audio_stream) {
        SDL_DestroyAudioStream(m_audio_stream);
        m_audio_stream = nullptr;
    }
    if (m_audio_device >= 0) {
        SDL_CloseAudioDevice(m_audio_device);
        m_audio_device = -1;
    }
}

void VideoDecoder::push_audio_samples(const int16_t* samples, int num_frames) {
    std::lock_guard lk(m_audio_mtx);
    if (!m_audio_initialized || !m_audio_stream) return;

    int bytes = num_frames * 2 * 2;
    SDL_PutAudioStreamData(m_audio_stream, samples, bytes);

    int volume = 0;
    {
        std::lock_guard lk(g_config_mtx);
        volume = g_cfg.video_volume;
    }
    float gain = volume / 100.0f;
    SDL_SetAudioStreamGain(m_audio_stream, gain);
}

bool VideoDecoder::start(const std::string& path, int target_width, int target_height) {
    stop();
    m_path = path;
    m_target_width = target_width;
    m_target_height = target_height;
    m_eof.store(false);
    decode_start_time.store(0.0, std::memory_order_relaxed);
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
    g_logger.info("VIDEO_DEC: stop() called, m_running=%d", m_running.load());
    m_running.store(false);
    m_queue_cv.notify_all();
    if (m_thread != 0) {
        pthread_join(m_thread, nullptr);
        m_thread = 0;
    }
    shutdown_audio();
    std::lock_guard lk(m_queue_mtx);
    while (!m_frame_queue.empty()) {
        m_frame_queue.pop();
    }
    m_eof.store(false);
}

bool VideoDecoder::is_running() const { return m_running.load(); }
bool VideoDecoder::is_eof() const { return m_eof.load(); }

double VideoDecoder::get_frame_duration() const {
    return m_frame_duration;
}

double VideoDecoder::get_video_remaining(double fallback_duration) const {
    if (m_eof.load(std::memory_order_relaxed)) return 0.0;
    double total = m_video_total_duration.load(std::memory_order_relaxed);
    double last_pts = m_last_frame_pts.load(std::memory_order_relaxed);
        if (total <= 0.0) total = fallback_duration;
    double start_t = decode_start_time.load(std::memory_order_relaxed);
    if (total <= 0.0 || start_t <= 0.0) return 0.0;
    double elapsed = (av_gettime_relative() / 1000000.0) - start_t;
    return std::max(0.0, total - elapsed);
}


bool VideoDecoder::get_frame(VideoFrame& out) {
    std::lock_guard lk(m_queue_mtx);
    if (m_frame_queue.empty()) return false;
    out = std::move(m_frame_queue.front());
    m_frame_queue.pop();
    m_queue_cv.notify_one();
    return true;
}
bool VideoDecoder::has_frames() const {
    std::lock_guard lk(m_queue_mtx);
    return !m_frame_queue.empty();
}

void VideoDecoder::decode_loop() {
    static constexpr int MAX_AUDIO_SAMPLES = 8192;
    std::vector<int16_t> audio_resample_buf(MAX_AUDIO_SAMPLES * 2);
    try {
    DEBUG_LOG("VIDEO_DEC: Starting decode thread for %s", m_path.c_str());

    avformat_network_init();
    struct NetworkDeinitGuard {
        ~NetworkDeinitGuard() { avformat_network_deinit(); }
    } network_guard;

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) { m_running.store(false); return; }

    static const int64_t IO_TIMEOUT_US = 30LL * 1000000LL;
    struct IOInterruptData { int64_t start_time; std::atomic<bool>* running; };
    IOInterruptData io_data{av_gettime_relative(), &m_running};

    fmt_ctx->interrupt_callback.callback = [](void* opaque) -> int {
        auto* d = static_cast<IOInterruptData*>(opaque);
        if (!d->running->load()) return 1;
        return (av_gettime_relative() - d->start_time > IO_TIMEOUT_US) ? 1 : 0;
    };
    fmt_ctx->interrupt_callback.opaque = &io_data;

    if (avformat_open_input(&fmt_ctx, m_path.c_str(), nullptr, nullptr) != 0) {
        g_logger.error("VIDEO_DEC: Failed to open %s", m_path.c_str());
        
        m_running.store(false);
        m_eof.store(true);
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        g_logger.error("VIDEO_DEC: Failed to find stream info for %s", m_path.c_str());
    VideoLimits limits;
    {
        std::shared_lock lk(g_config_mtx);
        limits.max_width = g_cfg.video_max_width;
        limits.max_height = g_cfg.video_max_height;
        limits.max_bitrate = g_cfg.video_max_bitrate;
        limits.max_duration_seconds = g_cfg.video_max_duration_seconds;
    }
    if (!video_within_budget(fmt_ctx, limits)) {
        g_logger.warn("VIDEO_DEC: %s exceeds decode budget. Skipping.", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
        avformat_network_deinit();
        return;
    }
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    int video_stream_idx = -1, audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (video_stream_idx == -1 && fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video_stream_idx = (int)i;
        if (audio_stream_idx == -1 && fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audio_stream_idx = (int)i;
        if (video_stream_idx >= 0 && audio_stream_idx >= 0) break;
    }

    if (video_stream_idx == -1) {
        g_logger.error("VIDEO_DEC: No video stream found in %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
        return;
    }

    DEBUG_LOG("VIDEO_DEC: Video stream=%d, Audio stream=%d", video_stream_idx, audio_stream_idx);

    // Extract video frame rate from stream metadata
    AVRational fr = av_guess_frame_rate(fmt_ctx, fmt_ctx->streams[video_stream_idx], nullptr);
    if (fr.num > 0 && fr.den > 0) {
        double fps = av_q2d(fr);
        m_frame_duration = 1.0 / fps;
        DEBUG_LOG("VIDEO_DEC: Detected FPS=%.2f via av_guess_frame_rate, frame_duration=%.3fs", fps, m_frame_duration);
    } else {
        m_frame_duration = 0.033333; // fallback 30fps
        DEBUG_LOG("VIDEO_DEC: Could not detect FPS, using fallback 30fps");
    }
    // Extract video duration with stream fallback
    m_video_total_duration.store(0.0);
    if (fmt_ctx->duration > 0 && (int64_t)fmt_ctx->duration != (int64_t)0x8000000000000000LL) {
        m_video_total_duration.store(fmt_ctx->duration / 1000000.0);
    }
    if (m_video_total_duration.load() <= 0.0 && video_stream_idx >= 0 && fmt_ctx->streams[video_stream_idx]->duration > 0 && fmt_ctx->streams[video_stream_idx]->time_base.den > 0) {
        double dur = fmt_ctx->streams[video_stream_idx]->duration * av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
        m_video_total_duration.store(dur);
    }


    m_last_frame_pts.store(0.0);

    // Video codec — probe hardware acceleration (V4L2 M2M) first, fallback to software
    AVCodecParameters* vp = fmt_ctx->streams[video_stream_idx]->codecpar;
    // DRM hwaccel for V4L2 stateless decode (Pi 5 HEVC, Pi 4/5 H264)
    AVBufferRef* hw_dev = create_hw_device();
    const AVCodec* vc = avcodec_find_decoder(vp->codec_id);
    if (vc && hw_dev) {
        g_logger.info("VIDEO_DEC: Hardware video acceleration enabled (%s via DRM)", vc->name);
    }
    if (!vc) {
    av_buffer_unref(&hw_dev);
        g_logger.error("VIDEO_DEC: Unsupported video codec for %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
        return;
    }
    AVCodecContext* vcc = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(vcc, vp);
    if (hw_dev) vcc->hw_device_ctx = av_buffer_ref(hw_dev);
    // Enable multi-threaded decoding based on codec capabilities
    {
        int threads = std::thread::hardware_concurrency();
        vcc->thread_count = std::min(4, std::max(1, threads - 1));
        if (vc->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
            vcc->thread_type = FF_THREAD_FRAME;
        } else if (vc->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
            vcc->thread_type = FF_THREAD_SLICE;
        }
    }
    bool is_hw = (hw_dev != nullptr);
    if (avcodec_open2(vcc, vc, nullptr) < 0) {
        if (is_hw) {
            g_logger.warn("VIDEO_DEC: Hardware decoder %s failed to configure, falling back to software decoder", vc->name);
            avcodec_free_context(&vcc);
            vc = avcodec_find_decoder(vp->codec_id);
            if (vc) {
                vcc = avcodec_alloc_context3(vc);
                avcodec_parameters_to_context(vcc, vp);
                int threads = std::thread::hardware_concurrency();
                vcc->thread_count = std::min(2, std::max(1, threads - 1));
                vcc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            }
        }
        if (!vc || avcodec_open2(vcc, vc, nullptr) < 0) {
            g_logger.error("VIDEO_DEC: Failed to open video codec for %s", m_path.c_str());
            if (vcc) avcodec_free_context(&vcc);
            avformat_close_input(&fmt_ctx);
            m_running.store(false);
            m_eof.store(true);
            return;
        }
    }

    // Audio codec
    AVCodecContext* acc = nullptr;
    int audio_channels = 2;
    int audio_sample_rate = 48000;
    if (audio_stream_idx >= 0) {
        AVCodecParameters* ap = fmt_ctx->streams[audio_stream_idx]->codecpar;
        const AVCodec* ac = avcodec_find_decoder(ap->codec_id);
        if (ac) {
            acc = avcodec_alloc_context3(ac);
            avcodec_parameters_to_context(acc, ap);
            if (avcodec_open2(acc, ac, nullptr) >= 0) {
                audio_channels = acc->ch_layout.nb_channels;
                audio_sample_rate = acc->sample_rate;
                DEBUG_LOG("AUDIO: Codec=%s sr=%d ch=%d fmt=%d",
                          ac->name, audio_sample_rate, audio_channels, acc->sample_fmt);
            } else {
                DEBUG_LOG("AUDIO: Failed to open audio codec");
                avcodec_free_context(&acc);
                acc = nullptr;
            }
        }
    }

    if (acc) init_audio();

    // Swresample — FFmpeg 6 uses swr_alloc_set_opts2 with robust channel layout fallbacks
    SwrContext* swr = nullptr;
    if (acc && audio_sample_rate > 0) {
        AVChannelLayout src_ch, dst_ch;
        memset(&src_ch, 0, sizeof(src_ch));
        memset(&dst_ch, 0, sizeof(dst_ch));

        int ch_cnt = (acc->ch_layout.nb_channels > 0) ? acc->ch_layout.nb_channels : (audio_channels > 0 ? audio_channels : 2);
        av_channel_layout_default(&src_ch, ch_cnt);
        av_channel_layout_default(&dst_ch, 2);

        int ret = swr_alloc_set_opts2(&swr,
            &dst_ch, AV_SAMPLE_FMT_S16, 48000,
            &src_ch, (AVSampleFormat)acc->sample_fmt, audio_sample_rate,
            0, nullptr);
        if (ret < 0 || !swr) {
            DEBUG_LOG("AUDIO: swr_alloc_set_opts2 failed: %d", ret);
            swr = nullptr;
        } else {
            ret = swr_init(swr);
            if (ret < 0) {
                DEBUG_LOG("AUDIO: swr_init failed: %d", ret);
                swr_free(&swr);
                swr = nullptr;
            }
        }
        av_channel_layout_uninit(&src_ch);
        av_channel_layout_uninit(&dst_ch);
    }
    // Compute scaled target dimensions maintaining video aspect ratio
    int dst_w = vcc->width, dst_h = vcc->height;
    if (m_target_width > 0 && m_target_height > 0) {
        float video_ar = (float)vcc->width / (float)vcc->height;
        float screen_ar = (float)m_target_width / (float)m_target_height;
        if (video_ar >= screen_ar) {
            dst_w = m_target_width;
            dst_h = (int)(m_target_width / video_ar);
        } else {
            dst_h = m_target_height;
            dst_w = (int)(m_target_height * video_ar);
        }
    }
    // Scaler — deferred creation until first frame reveals actual pixel format
    // (V4L2 M2M HW decoders may output DRM_PRIME which needs transfer first)
    SwsContext* sws = nullptr;
    AVPixelFormat actual_pix_fmt = AV_PIX_FMT_NONE;
    g_logger.info("VIDEO_DEC: Decoding %s (%dx%d -> %dx%d)", m_path.c_str(), vcc->width, vcc->height, dst_w, dst_h);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgba = av_frame_alloc();
    int nbytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, dst_w, dst_h, 1);
    uint8_t* buf = (uint8_t*)av_malloc(nbytes);
    av_image_fill_arrays(rgba->data, rgba->linesize, buf, AV_PIX_FMT_RGBA, dst_w, dst_h, 1);

    AVFrame* aframe = nullptr;
    if (acc) aframe = av_frame_alloc();

    bool eof = false;
    int vf_count = 0, af_count = 0, pkt_count = 0;
    while (is_running() && !eof) {
        io_data.start_time = av_gettime_relative();
        int ret = av_read_frame(fmt_ctx, pkt);
        // hot-path debug log omitted
        pkt_count++;
        if (ret < 0) {
            g_logger.info("VIDEO_DEC: av_read_frame error ret=%d (AVERROR_EOF=%d, AVERROR(EAGAIN)=%d)", ret, AVERROR_EOF, AVERROR(EAGAIN));
        }
        if (ret == AVERROR_EOF) {
            eof = true;
            // Flush audio decoder
            if (acc && aframe) {
                ret = avcodec_send_packet(acc, nullptr);
                if (ret >= 0) {
                    while (ret >= 0) {
                        ret = avcodec_receive_frame(acc, aframe);
                        if (ret != 0) break;
                        if (swr && aframe->nb_samples > 0 && aframe->extended_data) {
                            int max_s = av_rescale_rnd(aframe->nb_samples, 48000, audio_sample_rate, AV_ROUND_UP);
                            int os = swr_convert(swr, (uint8_t**)audio_resample_buf.data(), max_s,
                                                 (const uint8_t**)aframe->extended_data, aframe->nb_samples);
                            if (os > 0) { push_audio_samples(audio_resample_buf.data(), os); af_count += os; }
                            
                        }
                    }
                }
            }
            g_logger.info("VIDEO_DEC: Outer loop exited. is_running=%d eof=%d vf_count=%d af_count=%d", is_running(), eof, vf_count, af_count);
    g_logger.info("VIDEO_DEC: After outer loop. is_running=%d eof=%d vf=%d pkt=%d", is_running(), eof, vf_count, af_count);

    // Flush video decoder to drain remaining buffered frames
            if (vcc) {
                int send_ret = avcodec_send_packet(vcc, nullptr);
                g_logger.debug("VIDEO_DEC: flush send ret=%d", send_ret);
                if (send_ret == 0 || send_ret == AVERROR(EAGAIN)) {
                    while (is_running()) {
                        int flush_ret = avcodec_receive_frame(vcc, frame);
                        g_logger.debug("VIDEO_DEC: flush receive ret=%d", flush_ret);
                        if (flush_ret == AVERROR(EAGAIN) || flush_ret == AVERROR_EOF) break;
                        if (flush_ret < 0) {
                            g_logger.warn("VIDEO_DEC: Bad frame during flush ret=%d, skipping", flush_ret);
                            break;
                        }
                        vf_count++;
                        // Transfer HW frames during flush
                        AVFrame* sw_frame2 = frame;
                        AVFrame* tmp_sw2 = nullptr;
                        if (frame->format == AV_PIX_FMT_DRM_PRIME || frame->hw_frames_ctx) {
                            tmp_sw2 = av_frame_alloc();
                            if (av_hwframe_transfer_data(tmp_sw2, frame, 0) >= 0) {
                                sw_frame2 = tmp_sw2;
                            } else {
                                g_logger.warn("VIDEO_DEC: flush hw transfer failed, skipping");
                                av_frame_free(&tmp_sw2);
                                av_frame_unref(frame);
                                continue;
                            }
                        }
                        // Deferred scaler for flush path
                        AVPixelFormat flush_fmt = (AVPixelFormat)sw_frame2->format;
                        if (!sws || flush_fmt != actual_pix_fmt) {
                            if (sws) sws_freeContext(sws);
                            actual_pix_fmt = flush_fmt;
                            sws = sws_getContext(sw_frame2->width, sw_frame2->height,
                                actual_pix_fmt, dst_w, dst_h, AV_PIX_FMT_RGBA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                            if (!sws) {
                                if (tmp_sw2) av_frame_free(&tmp_sw2);
                                break;
                            }
                        }
                        sws_scale(sws, sw_frame2->data, sw_frame2->linesize, 0, sw_frame2->height,
                                  rgba->data, rgba->linesize);
                        if (tmp_sw2) av_frame_free(&tmp_sw2);
                        VideoFrame vf;
                        vf.width = dst_w; vf.height = dst_h;
                        vf.data = new uint8_t[nbytes];
                        memcpy(vf.data, buf, nbytes);
                        int64_t pts_raw1 = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : (frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->pkt_dts);
                        if (pts_raw1 != AV_NOPTS_VALUE) {
                            vf.pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * pts_raw1;
                        }
                        {
                            std::unique_lock<std::mutex> lk(m_queue_mtx);
                            m_queue_cv.wait(lk, [this] {
                                return m_frame_queue.size() < MAX_QUEUED_FRAMES || !m_running.load();
                            });
                            if (!m_running.load()) break;
                            if (decode_start_time.load(std::memory_order_relaxed) == 0.0) {
                                decode_start_time.store(av_gettime_relative() / 1000000.0, std::memory_order_relaxed);
                            }
                            m_frame_queue.push(std::move(vf));
                        }
                        av_frame_unref(frame);
                        if (vf_count % 100 == 0) g_logger.debug("VIDEO_DEC: queue_depth=%zu", m_frame_queue.size());
                    }
                }
            }
            // Set EOF flag after flush completes so guards know decoder truly finished
            if (eof) { m_eof.store(true); g_logger.info("VIDEO_DEC: Natural EOF reached. vf=%d pkt=%d", vf_count, af_count); }
            break;
        }
        if (ret < 0) break;

        if (pkt->stream_index == video_stream_idx) {
            ret = avcodec_send_packet(vcc, pkt);
            // hot-path debug log omitted
            av_packet_unref(pkt);
            if (ret < 0) continue;
            // Drain all frames from decoder after sending packet
            while (true) {
            ret = avcodec_receive_frame(vcc, frame);
            // hot-path debug log omitted
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                g_logger.warn("VIDEO_DEC: Bad frame ret=%d, flushing decoder", ret);
                avcodec_flush_buffers(vcc);
                break;
            }

            // Skip corrupted noise frames flagged by decoder
            if (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                g_logger.warn("VIDEO_DEC: Skipping corrupt frame flags=0x%x error=0x%x", frame->flags, frame->decode_error_flags);
                av_frame_unref(frame);
                continue;
            }

            vf_count++;
            // Transfer HW frames (DRM_PRIME/V4L2) to CPU-accessible format before scaling
            AVFrame* sw_frame = frame;
            AVFrame* tmp_sw = nullptr;
            if (frame->format == AV_PIX_FMT_DRM_PRIME || frame->hw_frames_ctx) {
                tmp_sw = av_frame_alloc();
                if (av_hwframe_transfer_data(tmp_sw, frame, 0) >= 0) {
                    sw_frame = tmp_sw;
                } else {
                    g_logger.warn("VIDEO_DEC: av_hwframe_transfer_data failed, skipping frame");
                    av_frame_free(&tmp_sw);
                    av_frame_unref(frame);
                    continue;
                }
            }
            // Deferred scaler creation on first frame (actual format now known)
            AVPixelFormat cur_fmt = (AVPixelFormat)sw_frame->format;
            if (!sws || cur_fmt != actual_pix_fmt) {
                if (sws) sws_freeContext(sws);
                actual_pix_fmt = cur_fmt;
                int scale_w = sw_frame->width;
                int scale_h = sw_frame->height;
                if (scale_w > 1920 && !is_hw) {
                    float ar = (float)scale_h / (float)scale_w;
                    scale_w = 1920;
                    scale_h = (int)(1920.0f * ar);
                }
                sws = sws_getContext(sw_frame->width, sw_frame->height,
                    actual_pix_fmt, dst_w, dst_h, AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) {
                    g_logger.error("VIDEO_DEC: Failed to create scaler for fmt=%d", (int)actual_pix_fmt);
                    if (tmp_sw) av_frame_free(&tmp_sw);
                    break;
                }
                g_logger.info("VIDEO_DEC: Created scaler for fmt=%d (%dx%d -> %dx%d)",
                    (int)actual_pix_fmt, sw_frame->width, sw_frame->height, dst_w, dst_h);
            }
            sws_scale(sws, sw_frame->data, sw_frame->linesize, 0, sw_frame->height,
                      rgba->data, rgba->linesize);
            if (tmp_sw) av_frame_free(&tmp_sw);

            VideoFrame vf;
            vf.width = dst_w;
            vf.height = dst_h;
            vf.data = new uint8_t[nbytes];
            memcpy(vf.data, buf, nbytes);
            int64_t pts_raw2 = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : (frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->pkt_dts);
            if (pts_raw2 != AV_NOPTS_VALUE) {
                vf.pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * pts_raw2;
            }
            // Track last decoded frame PTS for accurate countdown timer
            if (vf.pts > 0) m_last_frame_pts.store(vf.pts + m_frame_duration, std::memory_order_relaxed);

            {
                std::unique_lock<std::mutex> lk(m_queue_mtx);
                m_queue_cv.wait(lk, [this] {
                    return m_frame_queue.size() < MAX_QUEUED_FRAMES || !m_running.load();
                });
                if (!m_running.load()) break;
                m_frame_queue.push(std::move(vf));
            }
            av_frame_unref(frame);
            } // end while drain
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                g_logger.warn("VIDEO_DEC: Skipping to next packet after bad frame");
                continue;
            }
            }

        else if (pkt->stream_index == audio_stream_idx && acc && aframe) {
            ret = avcodec_send_packet(acc, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;
            while (ret >= 0) {
                ret = avcodec_receive_frame(acc, aframe);
                if (ret != 0) break;
                if (swr && aframe->nb_samples > 0 && aframe->extended_data) {
                    int max_s = std::min((int)av_rescale_rnd(aframe->nb_samples, 48000, audio_sample_rate, AV_ROUND_UP), MAX_AUDIO_SAMPLES);
                    int os = swr_convert(swr, (uint8_t**)audio_resample_buf.data(), max_s,
                                         (const uint8_t**)aframe->extended_data, aframe->nb_samples);
                    if (os > 0) { push_audio_samples(audio_resample_buf.data(), os); af_count += os; }
                    
                }
            }
        } else {
            av_packet_unref(pkt);
        }
    }

    g_logger.info("VIDEO_DEC: Done %s (%d vf, %d af)", m_path.c_str(), vf_count, af_count);
    // Mark decoder as stopped immediately so the render loop stops spinning
    // on "decoder already running" while we clean up FFmpeg resources below
    m_running.store(false);

    try {
    av_frame_free(&rgba);
    av_frame_free(&frame);
    if (aframe) av_frame_free(&aframe);
    av_packet_free(&pkt);
    av_free(buf);
    sws_freeContext(sws);
    swr_free(&swr);
    av_buffer_unref(&hw_dev);
    if (acc) avcodec_free_context(&acc);
    avcodec_free_context(&vcc);
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();
    } catch (...) {}

    } catch (const std::exception& e) {
        g_logger.error("VIDEO_DEC: Exception: %s", e.what());
    } catch (...) {
        g_logger.error("VIDEO_DEC: Unknown exception");
        m_running.store(false);
    }
}
