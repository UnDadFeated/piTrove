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
    const AVCodec* vc = nullptr;
    if (vp->codec_id == AV_CODEC_ID_H264) {
        vc = avcodec_find_decoder_by_name("h264_v4l2m2m");
    } else if (vp->codec_id == AV_CODEC_ID_HEVC) {
        vc = avcodec_find_decoder_by_name("hevc_v4l2m2m");
    }
    if (!vc) {
        vc = avcodec_find_decoder(vp->codec_id);
    } else {
        g_logger.info("VIDEO_DEC: Hardware video acceleration enabled (%s)", vc->name);
    }
    if (!vc) {
        g_logger.error("VIDEO_DEC: Unsupported video codec for %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }
    AVCodecContext* vcc = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(vcc, vp);
    // Enable multi-threaded decoding (maxcores-1)
    {
        int threads = std::thread::hardware_concurrency();
        vcc->thread_count = std::max(1, threads - 1);
        vcc->thread_type = FF_THREAD_FRAME;
    }
    bool is_hw = (vc && std::string(vc->name).find("v4l2m2m") != std::string::npos);
    if (avcodec_open2(vcc, vc, nullptr) < 0) {
        if (is_hw) {
            g_logger.warn("VIDEO_DEC: Hardware decoder %s failed to configure, falling back to software decoder", vc->name);
            avcodec_free_context(&vcc);
            vc = avcodec_find_decoder(vp->codec_id);
            if (vc) {
                vcc = avcodec_alloc_context3(vc);
                avcodec_parameters_to_context(vcc, vp);
                int threads = std::thread::hardware_concurrency();
                vcc->thread_count = std::max(1, threads - 1);
                vcc->thread_type = FF_THREAD_FRAME;
            }
        }
        if (!vc || avcodec_open2(vcc, vc, nullptr) < 0) {
            g_logger.error("VIDEO_DEC: Failed to open video codec for %s", m_path.c_str());
            if (vcc) avcodec_free_context(&vcc);
            avformat_close_input(&fmt_ctx);
            m_running.store(false);
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

    // Swresample — FFmpeg 6 uses swr_alloc_set_opts2
    SwrContext* swr = nullptr;
    if (acc) {
        AVChannelLayout src_ch, dst_ch;
        av_channel_layout_copy(&src_ch, &acc->ch_layout);
        av_channel_layout_from_mask(&dst_ch, AV_CH_LAYOUT_STEREO);

        int ret = swr_alloc_set_opts2(&swr,
            &dst_ch, AV_SAMPLE_FMT_S16, 48000,
            &src_ch, (AVSampleFormat)acc->sample_fmt, audio_sample_rate,
            0, nullptr);
        if (ret < 0) {
            DEBUG_LOG("AUDIO: swr_alloc_set_opts2 failed: %d", ret);
        } else {
            ret = swr_init(swr);
            if (ret < 0) {
                DEBUG_LOG("AUDIO: swr_init failed: %d", ret);
                swr_free(&swr);
            }
        }
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
    // Scaler
    SwsContext* sws = sws_getContext(vcc->width, vcc->height, vcc->pix_fmt,
                                     dst_w, dst_h, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        g_logger.error("VIDEO_DEC: Failed to create scaler for %s", m_path.c_str());
        if (acc) avcodec_free_context(&acc);
        avcodec_free_context(&vcc);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }
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
        int ret = av_read_frame(fmt_ctx, pkt);
        g_logger.debug("VIDEO_DEC: av_read_frame ret=%d stream=%d pkt_count=%d", ret, (pkt ? pkt->stream_index : -1), pkt_count);
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
                        if (swr) {
                            int max_s = av_rescale_rnd(aframe->nb_samples, 48000, audio_sample_rate, AV_ROUND_UP);
                            int16_t* ob = new int16_t[max_s * 2];
                            int os = swr_convert(swr, (uint8_t**)&ob, max_s,
                                                 (const uint8_t**)aframe->data, aframe->nb_samples);
                            if (os > 0) { push_audio_samples(ob, os); af_count += os; }
                            delete[] ob;
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
                        sws_scale(sws, frame->data, frame->linesize, 0, vcc->height,
                                  rgba->data, rgba->linesize);
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
            g_logger.debug("VIDEO_DEC: send_packet vcc ret=%d", ret);
            av_packet_unref(pkt);
            if (ret < 0) continue;
            // Drain all frames from decoder after sending packet
            while (true) {
            ret = avcodec_receive_frame(vcc, frame);
            g_logger.debug("VIDEO_DEC: receive_frame ret=%d vf_count=%d", ret, vf_count);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                g_logger.warn("VIDEO_DEC: Bad frame ret=%d, flushing decoder", ret);
                avcodec_flush_buffers(vcc);
                break;
            }

            vf_count++;
            sws_scale(sws, frame->data, frame->linesize, 0, vcc->height,
                      rgba->data, rgba->linesize);

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
                if (swr) {
                    int max_s = av_rescale_rnd(aframe->nb_samples, 48000, audio_sample_rate, AV_ROUND_UP);
                    int16_t* ob = new int16_t[max_s * 2];
                    int os = swr_convert(swr, (uint8_t**)&ob, max_s,
                                         (const uint8_t**)aframe->data, aframe->nb_samples);
                    if (os > 0) { push_audio_samples(ob, os); af_count += os; }
                    delete[] ob;
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
    if (acc) avcodec_free_context(&acc);
    avcodec_free_context(&vcc);
    avformat_close_input(&fmt_ctx);
    } catch (...) {}

    } catch (const std::exception& e) {
        g_logger.error("VIDEO_DEC: Exception: %s", e.what());
    } catch (...) {
        g_logger.error("VIDEO_DEC: Unknown exception");
        m_running.store(false);
    }
}
