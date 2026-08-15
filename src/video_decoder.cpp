#include <thread>
#include "cache.h"
#include "video_decoder.h"
#include <span>
#include <SDL3/SDL_timer.h>
#include "util.h"
#include "config.h"

#include <fstream>
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

// Runtime Pi 4/5 detection
static std::string hwaccel_path = "none";
// Set when the HW M2M pipeline stalls (frame crawl): stay on software
// decode for the rest of the process lifetime — kernel M2M state is
// degraded until the container restarts (see is_pi5() gate history).
static bool hw_disabled_session = false;
[[maybe_unused]] static bool is_pi5() {
    static bool detected = false;
    static bool result = false;
    if (!detected) {
        std::ifstream f("/proc/cpuinfo");
        std::string m((std::istreambuf_iterator<char>(f)), {});
        result = m.find("Cortex-A76") != std::string::npos || m.find("0xd0b") != std::string::npos;
        detected = true;
    }
    return result;
}
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME || *p == AV_PIX_FMT_NV12) {
            return *p;
        }
    }
    return pix_fmts[0];
}

static AVBufferRef* create_hw_device() {
    if (hw_disabled_session) return nullptr; // session-wide SW fallback after crawl
    AVBufferRef* hw_device_ctx = nullptr;
    if (av_hwdevice_ctx_create(&hw_device_ctx,
                               AV_HWDEVICE_TYPE_DRM,
                               nullptr, nullptr, 0) >= 0) {
        hwaccel_path = "drm";
        return hw_device_ctx;
    }
    // V4L2 M2M fallback: only if DRM hwaccel failed (Pi 4)
    if (!hw_device_ctx &&
        (avcodec_find_decoder_by_name("hevc_v4l2m2m") ||
         avcodec_find_decoder_by_name("h264_v4l2m2m"))) {
        hwaccel_path = "v4l2m2m";
    }
    return nullptr;
}

}

#define DEBUG_LOG(fmt, ...) \
    do { g_logger.debug(fmt, ##__VA_ARGS__); } while (0)

VideoDecoder::VideoDecoder() {}


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
        DEBUG_LOG("AUDIO: SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return;
    }

    m_audio_stream = SDL_CreateAudioStream(&spec, &spec);
    if (!m_audio_stream) {
        DEBUG_LOG("AUDIO: SDL_CreateAudioStream failed: {}", SDL_GetError());
        SDL_CloseAudioDevice(m_audio_device);
        m_audio_device = -1;
        return;
    }

    SDL_BindAudioStream(m_audio_device, m_audio_stream);
    m_audio_initialized = true;
    DEBUG_LOG("AUDIO: Device opened id={}", m_audio_device);
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

void VideoDecoder::push_audio_samples(std::span<const int16_t> samples, double pts_s) {
    // A/V sync: keep audio on the shared presentation clock. A chunk whose pts is
    // pts_s seconds belongs at anchor_wall + (pts_s - anchor_pts)*1000 ms; if it would
    // land early, delay the push (capped at 500ms - the decode loop's video queue
    // backpressure bounds the decode lead anyway).
    if (m_av_sync.load(std::memory_order_relaxed) && pts_s > 0.0) {
        note_audio_anchor(pts_s);
        if (m_anchor_set.load(std::memory_order_acquire)) {
            double due_ms = m_anchor_wall_ms.load(std::memory_order_acquire)
                          + (pts_s - m_anchor_pts.load(std::memory_order_acquire)) * 1000.0;
            double delay_ms = due_ms - SDL_GetTicks();
            if (delay_ms > 50.0) {
                if (delay_ms > 500.0) delay_ms = 500.0;
                std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(delay_ms * 1000.0)));
            }
        }
    }
    std::lock_guard lk(m_audio_mtx);
    if (!m_audio_initialized || !m_audio_stream) return;

    int bytes = static_cast<int>(samples.size_bytes());
    SDL_PutAudioStreamData(m_audio_stream, samples.data(), bytes);

    int volume = 0;
    {
        std::lock_guard lk(g_config_mtx);
        volume = g_cfg.video_volume;
    }
    float gain = volume / 100.0f;
    SDL_SetAudioStreamGain(m_audio_stream, gain);
}

void VideoDecoder::set_av_sync(bool on) { m_av_sync.store(on, std::memory_order_relaxed); }

bool VideoDecoder::av_sync_ready() const {
    return m_av_sync.load(std::memory_order_relaxed)
        && m_pts_valid.load(std::memory_order_relaxed)
        && m_anchor_set.load(std::memory_order_acquire);
}

double VideoDecoder::get_anchor_wall_ms() const { return m_anchor_wall_ms.load(std::memory_order_acquire); }

double VideoDecoder::get_anchor_pts() const { return m_anchor_pts.load(std::memory_order_acquire); }

// Anchor the presentation clock to the first pushed event (video frame or audio
// chunk), whichever arrives first. Single writer: the decode thread.
void VideoDecoder::note_frame_anchor(double pts_s, bool has_pts, bool stream_eof) {
    if (!m_av_sync.load(std::memory_order_relaxed)) return;
    if (!has_pts) {
        // Trailing flush frames after demux EOF carry no pts - expected, not a
        // stream problem. Only latch the fallback for genuine mid-stream pts loss.
        if (!stream_eof && m_pts_valid.load(std::memory_order_relaxed)) {
            m_pts_valid.store(false, std::memory_order_relaxed);
            g_logger.warn("AV_SYNC: frame without valid pts, falling back to counter pacing");
        }
        return;
    }
    if (!m_anchor_set.load(std::memory_order_relaxed)) {
        m_anchor_pts.store(pts_s, std::memory_order_relaxed);
        m_anchor_wall_ms.store((double)SDL_GetTicks(), std::memory_order_relaxed);
        m_anchor_set.store(true, std::memory_order_release);
        g_logger.info("[TRACE] AV_SYNC: frame anchor set (pts={:.3f})", pts_s);
    }
    if (!m_v0_set.load(std::memory_order_relaxed)) {
        m_v0_pts.store(pts_s, std::memory_order_relaxed);
        m_v0_set.store(true, std::memory_order_release);
    }
}

void VideoDecoder::note_audio_anchor(double pts_s) {
    if (!m_av_sync.load(std::memory_order_relaxed)) return;
    if (!m_anchor_set.load(std::memory_order_relaxed)) {
        m_anchor_pts.store(pts_s, std::memory_order_relaxed);
        m_anchor_wall_ms.store((double)SDL_GetTicks(), std::memory_order_relaxed);
        m_anchor_set.store(true, std::memory_order_release);
        g_logger.info("[TRACE] AV_SYNC: audio anchor set (pts={:.3f})", pts_s);
    }
}

bool VideoDecoder::start(const std::string& path, int target_width, int target_height) {
    stop();
    m_path = path;
    m_target_width = target_width;
    m_target_height = target_height;
    m_eof.store(false);
    m_start_failed.store(false);
    decode_start_time.store(0.0, std::memory_order_relaxed);
    m_running.store(true);
    m_thread = std::jthread([this]() {
        try { this->decode_loop(); }
        catch (...) {}
    });
    return true;
}

// Note: stop() signals the decode thread but does not join it.
// jthread's move-assignment in start() implicitly joins the old thread.
void VideoDecoder::stop() {
    g_logger.info("VIDEO_DEC: stop() called, m_running={}", m_running.load());
    m_running.store(false);
    m_queue_cv.notify_all();
    shutdown_audio();
    std::lock_guard lk(m_queue_mtx);
    while (!m_frame_queue.empty()) {
        m_frame_queue.pop();
    }
    m_eof.store(false);
}

bool VideoDecoder::is_running() const { return m_running.load(); }
bool VideoDecoder::is_eof() const { return m_eof.load(); }
// Atomic exchange: main loop consumes the flag so a stale failure from a
// previous item never causes the next item to be skipped.
bool VideoDecoder::consume_start_failed() { return m_start_failed.exchange(false); }

double VideoDecoder::get_frame_duration() const {
    return m_frame_duration;
}

double VideoDecoder::get_video_remaining(double fallback_duration) const {
    if (m_eof.load(std::memory_order_relaxed)) return 0.0;
    double total = m_video_total_duration.load(std::memory_order_relaxed);
    if (total <= 0.0) total = fallback_duration;

    if (av_sync_ready() && m_v0_set.load(std::memory_order_acquire)) {
        // Presentation-clock based: current stream time = wall time elapsed since the
        // anchor, re-baselined to the first video frame's pts (v0), which `total` is
        // relative to. Monotonic with real time even if decoding stalls.
        double t_rel = ((double)SDL_GetTicks() - m_anchor_wall_ms.load(std::memory_order_acquire)) / 1000.0
                     + (m_anchor_pts.load(std::memory_order_acquire) - m_v0_pts.load(std::memory_order_acquire));
        return std::max(0.0, total - t_rel);
    }
    double last_pts = m_last_frame_pts.load(std::memory_order_relaxed);
    if (total > 0.0 && last_pts > 0.0) {
        return std::max(0.0, total - last_pts);
    }

    double start_t = decode_start_time.load(std::memory_order_relaxed);
    if (start_t <= 0.0) {
        return total;
    }
    double elapsed = (av_gettime_relative() / 1000000.0) - start_t;
    if (total > 0.0) {
        return std::max(0.0, total - elapsed);
    }
    return 0.0;
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

// TEMP DIAG: push-rate instrumentation (locate 4K60 bottleneck)
static void log_push_rate(const char* site) {
    static uint64_t s_n = 0;
    static uint64_t s_t0 = 0;
    uint64_t now = av_gettime_relative();
    if (s_t0 == 0) s_t0 = now;
    s_n++;
    if (s_n % 200 == 0) {
        double secs = (now - s_t0) / 1e6;
        if (secs > 0.0) g_logger.info("[TRACE] VIDEO_DEC: push_rate site={} n={} last200 rate={:.1f}fps", site, (int)s_n, 200.0 / secs);
        s_t0 = now;
    }
}
void VideoDecoder::decode_loop() {
    static constexpr int MAX_AUDIO_SAMPLES = 8192;
    std::vector<int16_t> audio_resample_buf(MAX_AUDIO_SAMPLES * 2);
    try {
    DEBUG_LOG("VIDEO_DEC: Starting decode thread for {}", m_path);

    avformat_network_init();
    struct NetworkDeinitGuard {
        ~NetworkDeinitGuard() { avformat_network_deinit(); }
    } network_guard;

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) { m_start_failed.store(true); m_running.store(false); return; }
    fmt_ctx->flags |= AVFMT_FLAG_DISCARD_CORRUPT;

    static const int64_t IO_TIMEOUT_US = 30LL * 1000000LL; // 30s timeout for CIFS NAS network share reads
    struct IOInterruptData { int64_t start_time; std::atomic<bool>* running; };
    IOInterruptData io_data{av_gettime_relative(), &m_running};

    fmt_ctx->interrupt_callback.callback = [](void* opaque) -> int {
        auto* d = static_cast<IOInterruptData*>(opaque);
        if (!d->running->load()) return 1;
        return (av_gettime_relative() - d->start_time > IO_TIMEOUT_US) ? 1 : 0;
    };
    fmt_ctx->interrupt_callback.opaque = &io_data;

    if (avformat_open_input(&fmt_ctx, m_path.c_str(), nullptr, nullptr) != 0) {
        g_logger.error("VIDEO_DEC: Failed to open {}", m_path.c_str());
        m_start_failed.store(true);
        m_running.store(false);
        m_eof.store(true);
        return;
    }

    // Small chunk probing for instant video startup over network SMB/CIFS shares (YouTube style)
    g_logger.info("[TRACE] VIDEO_DEC: Probing stream metadata (probesize=500KB, max_analyze=1s)...");
    fmt_ctx->probesize = 500000;              // 500 KB probe chunk instead of default 5 MB
    fmt_ctx->max_analyze_duration = 1000000; // 1s analyze duration max

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        g_logger.error("VIDEO_DEC: Failed to find stream info for {}", m_path.c_str());
        m_start_failed.store(true);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
        return;
    }

    // Video decode budget check (must run AFTER successful stream info extraction)
    VideoLimits limits;
    {
        std::shared_lock lk(g_config_mtx);
        limits.max_width = g_cfg.video_max_width;
        limits.max_height = g_cfg.video_max_height;
        limits.max_bitrate = g_cfg.video_max_bitrate;
        limits.max_duration_seconds = g_cfg.video_max_duration_seconds;
    }
    if (!video_within_budget(fmt_ctx, limits)) {
        g_logger.warn("VIDEO_DEC: {} exceeds decode budget. Skipping.", m_path.c_str());
        m_start_failed.store(true);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
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
        g_logger.error("VIDEO_DEC: No video stream found in {}", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
        return;
    }

    DEBUG_LOG("VIDEO_DEC: Video stream={}, Audio stream={}", video_stream_idx, audio_stream_idx);

    // Extract video framerate from stream metadata (prefer avg_framerate, clamp to sane range)
    AVRational fr = fmt_ctx->streams[video_stream_idx]->avg_frame_rate;
    if (fr.num == 0) fr = fmt_ctx->streams[video_stream_idx]->r_frame_rate;
    if (fr.num == 0) fr = av_guess_frame_rate(fmt_ctx, fmt_ctx->streams[video_stream_idx], nullptr);
    if (fr.num > 0 && fr.den > 0) {
        double framerate = av_q2d(fr);
        if (framerate > 1.0 && framerate <= 144.0) {
            m_frame_duration = 1.0 / framerate;
        } else {
            // Clamp to 30fps if framerate seems wrong
            m_frame_duration = 0.033333;
        }
        DEBUG_LOG("VIDEO_DEC: Detected framerate={:.2f} via stream avg_framerate, frame_duration={:.3f}s", framerate, m_frame_duration);
    } else {
        m_frame_duration = 0.033333; // fallback 30fps
        DEBUG_LOG("VIDEO_DEC: Could not detect framerate, using fallback 30fps");
    }
    // Extract video duration (prioritize video frame count * frame_duration for exact stream sync)
    m_video_total_duration.store(0.0);
    AVStream* st = (video_stream_idx >= 0) ? fmt_ctx->streams[video_stream_idx] : nullptr;
    if (st && st->nb_frames > 0 && m_frame_duration > 0.0) {
        double dur = (double)st->nb_frames * m_frame_duration;
        m_video_total_duration.store(dur);
    }
    if (m_video_total_duration.load() <= 0.0 && st && st->duration > 0 && st->time_base.den > 0) {
        double dur = st->duration * av_q2d(st->time_base);
        m_video_total_duration.store(dur);
    }
    if (m_video_total_duration.load() <= 0.0 && fmt_ctx->duration > 0 && (int64_t)fmt_ctx->duration != (int64_t)0x8000000000000000LL) {
        m_video_total_duration.store(fmt_ctx->duration / 1000000.0);
    }


    m_last_frame_pts.store(0.0);
    m_decoded_frames.store(0, std::memory_order_relaxed);
    m_anchor_set.store(false, std::memory_order_relaxed);
    m_anchor_pts.store(0.0, std::memory_order_relaxed);
    m_anchor_wall_ms.store(0.0, std::memory_order_relaxed);
    m_v0_set.store(false, std::memory_order_relaxed);
    m_v0_pts.store(0.0, std::memory_order_relaxed);
    m_pts_valid.store(true, std::memory_order_relaxed);

    // Video codec — probe hardware acceleration (V4L2 M2M) first, fallback to software
    AVCodecParameters* vp = fmt_ctx->streams[video_stream_idx]->codecpar;
    // DRM hwaccel for V4L2 stateless decode (Pi 4/5 H264, Pi 4 HEVC)
    AVBufferRef* hw_dev = create_hw_device();
    if (!hw_dev && hw_disabled_session) {
        g_logger.warn("VIDEO_DEC: HW accel disabled for this session (crawl recovery); using software decode");
    }
    const AVCodec* vc = avcodec_find_decoder(vp->codec_id);

    // 100% GPU Hardware Acceleration enabled for all video codecs (H.264 & HEVC) on Pi 4 & Pi 5.

    // Pi 4 fallback: use V4L2 M2M codec directly if DRM hwaccel not available
    if (!hw_dev && hwaccel_path == "v4l2m2m" && vp->codec_id == AV_CODEC_ID_HEVC) {
        vc = avcodec_find_decoder_by_name("hevc_v4l2m2m");
    }
    if (!hw_dev && hwaccel_path == "v4l2m2m" && vp->codec_id == AV_CODEC_ID_H264) {
        vc = avcodec_find_decoder_by_name("h264_v4l2m2m");
    }
    if (vc && hw_dev) {
        g_logger.info("VIDEO_DEC: Hardware video acceleration enabled ({} via DRM)", vc->name);
    }
    if (!vc) {
    av_buffer_unref(&hw_dev);
        g_logger.error("VIDEO_DEC: Unsupported video codec for {}", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        m_eof.store(true);
        return;
    }
    AVCodecContext* vcc = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(vcc, vp);
    vcc->err_recognition = AV_EF_IGNORE_ERR;
    vcc->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    if (hw_dev) {
        vcc->hw_device_ctx = av_buffer_ref(hw_dev);
        vcc->get_format = get_hw_format;
        vcc->extra_hw_frames = 16; // Provide 16 hardware surface frames for complex 4K DPB reference structures
    }
    // Enable multi-threaded decoding based on codec capabilities
    {
        int threads = std::thread::hardware_concurrency();
        vcc->thread_count = (threads > 1) ? threads : 4;
        if ((vc->capabilities & AV_CODEC_CAP_FRAME_THREADS) && (vc->capabilities & AV_CODEC_CAP_SLICE_THREADS)) {
            vcc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        } else if (vc->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
            vcc->thread_type = FF_THREAD_FRAME;
        } else if (vc->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
            vcc->thread_type = FF_THREAD_SLICE;
        }
    }
    bool is_hw = (hw_dev != nullptr);
    if (avcodec_open2(vcc, vc, nullptr) < 0) {
        if (is_hw) {
            g_logger.warn("VIDEO_DEC: Hardware decoder {} failed to configure, falling back to software decoder", vc->name);
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
            g_logger.error("VIDEO_DEC: Failed to open video codec for {}", m_path.c_str());
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
                DEBUG_LOG("AUDIO: Codec={} sr={} ch={} fmt={}",
                          ac->name, audio_sample_rate, audio_channels, (int)acc->sample_fmt);
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
            DEBUG_LOG("AUDIO: swr_alloc_set_opts2 failed: {}", ret);
            swr = nullptr;
        } else {
            ret = swr_init(swr);
            [[unlikely]]
            if (ret < 0) {
                DEBUG_LOG("AUDIO: swr_init failed: {}", ret);
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
    // 4K pacing fix: sources above the display target (1080p) are downscaled to
    // the target size as NV12 (bilinear, no color conversion) so the queued frame,
    // texture upload and queue stay small and the display loop can pace 60fps.
    // 1080p and below keep native resolution (renderer GPU-scales to display).
    uint8_t* nv12_scale_buf = nullptr;
    SwsContext* nv12_sws = nullptr;
    int nv12_sws_w = 0, nv12_sws_h = 0;
    if (m_target_width > 0 && m_target_height > 0 && vcc->width > 0 &&
        (vcc->width > dst_w || vcc->height > dst_h)) {
        nv12_scale_buf = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_NV12, dst_w, dst_h, 1));
        g_logger.info("VIDEO_DEC: downscaling {}x{} -> {}x{} NV12 (display pacing)", vcc->width, vcc->height, dst_w, dst_h);
    }
    auto fill_nv12 = [&](AVFrame* sw, VideoFrame& vf) {
        vf.is_nv12 = true;
        int out_w = sw->width, out_h = sw->height;
        const uint8_t* y_src = sw->data[0];
        const uint8_t* uv_src = sw->data[1];
        int y_ls = sw->linesize[0], uv_ls = sw->linesize[1];
        if (nv12_scale_buf && (sw->width > dst_w || sw->height > dst_h)) {
            if (!nv12_sws || nv12_sws_w != sw->width || nv12_sws_h != sw->height) {
                if (nv12_sws) sws_freeContext(nv12_sws);
                nv12_sws = sws_getContext(sw->width, sw->height, AV_PIX_FMT_NV12,
                                          dst_w, dst_h, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                          nullptr, nullptr, nullptr);
                nv12_sws_w = sw->width;
                nv12_sws_h = sw->height;
            }
            uint8_t* ddata[3] = {nullptr, nullptr, nullptr};
            int dlines[4] = {0, 0, 0, 0};
            av_image_fill_arrays(ddata, dlines, nv12_scale_buf, AV_PIX_FMT_NV12, dst_w, dst_h, 1);
            sws_scale(nv12_sws, (const uint8_t* const*)sw->data, sw->linesize, 0, sw->height, ddata, dlines);
            y_src = ddata[0]; uv_src = ddata[1]; y_ls = dlines[0]; uv_ls = dlines[1];
            out_w = dst_w; out_h = dst_h;
        }
        vf.width = out_w;
        vf.height = out_h;
        vf.linesize_y = y_ls;
        vf.linesize_uv = uv_ls;
        int size_y = y_ls * out_h;
        int size_uv = uv_ls * (out_h / 2);
        vf.data = new uint8_t[size_y];
        vf.data_uv = new uint8_t[size_uv];
        memcpy(vf.data, y_src, size_y);
        memcpy(vf.data_uv, uv_src, size_uv);
    };
    // Scaler — deferred creation until first frame reveals actual pixel format
    // (V4L2 M2M HW decoders may output DRM_PRIME which needs transfer first)
    SwsContext* sws = nullptr;
    AVPixelFormat actual_pix_fmt = AV_PIX_FMT_NONE;
    g_logger.info("VIDEO_DEC: Decoding {} ({}x{} -> {}x{})", m_path.c_str(), vcc->width, vcc->height, dst_w, dst_h);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgba = av_frame_alloc();
    int nbytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, dst_w, dst_h, 1);
    uint8_t* buf = (uint8_t*)av_malloc(nbytes);
    av_image_fill_arrays(rgba->data, rgba->linesize, buf, AV_PIX_FMT_RGBA, dst_w, dst_h, 1);

    AVFrame* aframe = nullptr;
    if (acc) aframe = av_frame_alloc();

    decode_start_time.store(av_gettime_relative() / 1000000.0, std::memory_order_relaxed);
    bool eof = false;
    int vf_count = 0, af_count = 0, ret = 0;
    // Stall detection: if no video frame produced within this many ms, abort
    static constexpr long long STALL_TIMEOUT_US = 3000000; // 3-second fast recovery for GPU stalls // 5 seconds in microseconds
    long long last_frame_ms = av_gettime_relative();
    // Crawl window state (decode-thread-local): count frames pushed per 3s window
    long long crawl_start_us = last_frame_ms;
    int crawl_count = 0;
    int crawl_strikes = 0;
    int consecutive_demux_fails = 0;
    while (is_running() && !eof) {
        // Wi-Fi Protection & Smooth Paced I/O: Yield if frame queue has sufficient buffer
        if (m_frame_queue.size() >= 8) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Stall detection: abort if no frame produced for too long
        if (video_stream_idx >= 0 && (av_gettime_relative() - last_frame_ms) > STALL_TIMEOUT_US) {
            g_logger.warn("VIDEO_DEC: Decoder stalled for {}s, aborting decode of {}", (av_gettime_relative() - last_frame_ms) / 1000000, m_path.c_str());
            break;
        }
        // Crawl watchdog: a stalled HW M2M pipeline keeps demuxing (resetting
        // last_frame_ms above) while delivering frames at a crawl, so the
        // hard-stall check never fires. Two consecutive 3s windows with <10
        // pushed frames means the decoder is broken, not just slow: advance
        // the item and stay on software decode for this process.
        long long now_us_crawl = av_gettime_relative();
        if (video_stream_idx >= 0 && now_us_crawl - crawl_start_us >= 3000000LL) {
            if (crawl_count < 10) {
                if (++crawl_strikes >= 2) {
                    g_logger.error("VIDEO_DEC: [E530] Frame crawl detected ({} frames/3s window): HW M2M pipeline stalled, disabling HW accel for this session, advancing item", crawl_count);
                    trigger_error(530);
                    hw_disabled_session = true;
                    m_eof.store(true);
                    eof = true;
                    break;
                }
            } else {
                crawl_strikes = 0;
            }
            crawl_start_us = now_us_crawl;
            crawl_count = 0;
        }
        io_data.start_time = av_gettime_relative();
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret >= 0) {
            last_frame_ms = av_gettime_relative(); // Reset watchdog on active demux progress
        }
        [[unlikely]]
        if (ret < 0) {
            g_logger.info("VIDEO_DEC: av_read_frame error ret={} (AVERROR_EOF={}, AVERROR(EAGAIN)={})", ret, AVERROR_EOF, AVERROR(EAGAIN));
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (++consecutive_demux_fails > 100) {
                    g_logger.warn("VIDEO_DEC: Packet starvation detected (100 consecutive failed reads), triggering EOF recovery for {}", m_path.c_str());
                    eof = true;
                    break;
                }
                g_logger.warn("VIDEO_DEC: [E527] av_read_frame error ret={}, bypassing corrupt packet", ret);
                trigger_error(527);
                av_packet_unref(pkt);
                continue;
            }
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
                            uint8_t* outbuf[1] = { (uint8_t*)audio_resample_buf.data() };
                            int os = swr_convert(swr, outbuf, max_s,
                                                 (const uint8_t**)aframe->extended_data, aframe->nb_samples);
                            int64_t apts = (aframe->best_effort_timestamp != AV_NOPTS_VALUE) ? aframe->best_effort_timestamp : aframe->pts;
                            double pa_s = (apts != AV_NOPTS_VALUE) ? av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base) * apts : -1.0;
                            if (os > 0) { push_audio_samples({audio_resample_buf.data(), (size_t)(os * 2)}, pa_s); af_count += os; }
                            
                        }
                    }
                }
            }
            g_logger.info("VIDEO_DEC: Outer loop exited. is_running={} eof={} vf_count={} af_count={}", is_running(), eof, vf_count, af_count);
    g_logger.info("VIDEO_DEC: After outer loop. is_running={} eof={} vf={} pkt={}", is_running(), eof, vf_count, af_count);

    // Flush video decoder to drain remaining buffered frames
            if (vcc) {
                int send_ret = avcodec_send_packet(vcc, nullptr);
                g_logger.debug("VIDEO_DEC: flush send ret={}", send_ret);
                if (send_ret == 0 || send_ret == AVERROR(EAGAIN)) {
                    while (is_running()) {
                        int flush_ret = avcodec_receive_frame(vcc, frame);
                        g_logger.debug("VIDEO_DEC: flush receive ret={}", flush_ret);
                        if (flush_ret == AVERROR(EAGAIN) || flush_ret == AVERROR_EOF) break;
                        if (flush_ret < 0) {
                            g_logger.warn("VIDEO_DEC: Bad frame during flush ret={}, skipping", flush_ret);
                            break;
                        }
                        vf_count++;
                        last_frame_ms = av_gettime_relative();
                        crawl_count++;
                        m_decoded_frames.fetch_add(1, std::memory_order_relaxed);
                        AVFrame* sw_frame2 = frame;
                        AVFrame* tmp_sw2 = nullptr;
                        int64_t pts_raw1_pre = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : (frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->pkt_dts);
                        if (frame->format == AV_PIX_FMT_DRM_PRIME || frame->hw_frames_ctx) {
                            tmp_sw2 = av_frame_alloc();
                            if (av_hwframe_transfer_data(tmp_sw2, frame, 0) >= 0) {
                                sw_frame2 = tmp_sw2;
                                av_frame_unref(frame);
                            } else {
                                g_logger.warn("VIDEO_DEC: flush hw transfer failed, skipping");
                                av_frame_free(&tmp_sw2);
                                av_frame_unref(frame);
                                continue;
                            }
                        }
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
                        VideoFrame vf;
                        if (flush_fmt == AV_PIX_FMT_NV12 && nv12_scale_buf) {
                            fill_nv12(sw_frame2, vf);
                        } else {
                            sws_scale(sws, sw_frame2->data, sw_frame2->linesize, 0, sw_frame2->height,
                                      rgba->data, rgba->linesize);
                            vf.width = dst_w; vf.height = dst_h;
                            vf.data = new uint8_t[nbytes];
                            memcpy(vf.data, buf, nbytes);
                        }
                        if (tmp_sw2) av_frame_free(&tmp_sw2); // after last use of sw_frame2
                        int64_t pts_raw1 = pts_raw1_pre;
                        if (pts_raw1 != AV_NOPTS_VALUE) {
                            vf.pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * pts_raw1;
                        }
                        note_frame_anchor(vf.pts, pts_raw1 != AV_NOPTS_VALUE, eof);
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
                        g_logger.info("[TRACE] VIDEO_DEC: Pushed frame #{}, queue_size={}", vf_count, m_frame_queue.size());
                        log_push_rate("flush");
                        av_frame_unref(frame);
                        if (vf_count % 100 == 0) g_logger.debug("VIDEO_DEC: queue_depth={}", m_frame_queue.size());
                    }
                }
            }
            // Set EOF flag after flush completes so guards know decoder truly finished
            if (eof) { m_eof.store(true); g_logger.info("VIDEO_DEC: Natural EOF reached. vf={} pkt={}", vf_count, af_count); }
            break;
        }
        [[unlikely]]
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                if (ret == AVERROR_EOF) break;
            }

        if (pkt->stream_index == video_stream_idx) {
            ret = avcodec_send_packet(vcc, pkt);
            if (ret == AVERROR(EAGAIN)) {
                // GPU Decoder queue full: drain decoded frames first to release HW buffers
                while (is_running()) {
                    int r_drain = avcodec_receive_frame(vcc, frame);
                    if (r_drain == AVERROR(EAGAIN) || r_drain == AVERROR_EOF) break;
                    if (r_drain < 0) { av_frame_unref(frame); break; }
                    
                    vf_count++;
                    m_decoded_frames.fetch_add(1, std::memory_order_relaxed);
                    consecutive_demux_fails = 0;
                    
                    AVFrame* sw_frame = frame;
                    AVFrame* tmp_sw = nullptr;
                    int64_t pts_raw_pre = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : (frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->pkt_dts);
                    if (frame->format == AV_PIX_FMT_DRM_PRIME || frame->hw_frames_ctx) {
                        tmp_sw = av_frame_alloc();
                        if (av_hwframe_transfer_data(tmp_sw, frame, 0) >= 0) {
                            sw_frame = tmp_sw;
                            av_frame_unref(frame);
                        } else {
                            av_frame_free(&tmp_sw);
                            av_frame_unref(frame);
                            continue;
                        }
                    }
                    VideoFrame vf;
                    vf.format = sw_frame->format;
                    vf.linesize_y = sw_frame->linesize[0];
                    vf.linesize_uv = sw_frame->linesize[1];
                    if (sw_frame->format == AV_PIX_FMT_NV12) {
                        fill_nv12(sw_frame, vf);
                        if (tmp_sw) av_frame_free(&tmp_sw);
                    } else {
                        if (tmp_sw) av_frame_free(&tmp_sw);
                        av_frame_unref(frame);
                        continue;
                    }
                    int64_t pts_raw2 = pts_raw_pre;
                    if (pts_raw2 != AV_NOPTS_VALUE) vf.pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * pts_raw2;
                    note_frame_anchor(vf.pts, pts_raw2 != AV_NOPTS_VALUE, eof);
                    {
                        std::unique_lock<std::mutex> lk(m_queue_mtx);
                        m_queue_cv.wait(lk, [this] { return m_frame_queue.size() < MAX_QUEUED_FRAMES || !m_running.load(); });
                        if (!m_running.load()) break;
                        m_frame_queue.push(std::move(vf));
                    }
                        log_push_rate("drain");
                    last_frame_ms = av_gettime_relative();
                }
                // Retry sending the packet after GPU buffer space freed up
                ret = avcodec_send_packet(vcc, pkt);
            }
            av_packet_unref(pkt);
            if (ret < 0) continue;

            // Drain remaining frames from decoder
            while (true) {
            ret = avcodec_receive_frame(vcc, frame);
            // hot-path debug log omitted
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            [[unlikely]]
            if (ret < 0) {
                g_logger.warn("VIDEO_DEC: [E527] Bad frame ret={}, skipping frame", ret);
                trigger_error(527);
                av_frame_unref(frame);
                continue;
            }

            // Skip corrupted noise frames flagged by decoder
            if (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                g_logger.warn("VIDEO_DEC: [E527] Skipping corrupt frame flags=0x{} error=0x{}", frame->flags, frame->decode_error_flags);
                trigger_error(527);
                av_frame_unref(frame);
                continue;
            }

            vf_count++;
            m_decoded_frames.fetch_add(1, std::memory_order_relaxed);
            consecutive_demux_fails = 0;
            last_frame_ms = av_gettime_relative(); // Stall detection
            crawl_count++;

            // Capture PTS before any av_frame_unref() to prevent use-after-free on HW path
            int64_t pts_raw = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? frame->best_effort_timestamp
                : (frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->pkt_dts);

            // Transfer HW frames (DRM_PRIME/V4L2) to CPU-accessible format before scaling
            AVFrame* sw_frame = frame;
            AVFrame* tmp_sw = nullptr;
            if (frame->format == AV_PIX_FMT_DRM_PRIME || frame->hw_frames_ctx) {
                tmp_sw = av_frame_alloc();
                if (av_hwframe_transfer_data(tmp_sw, frame, 0) >= 0) {
                    sw_frame = tmp_sw;
                    av_frame_unref(frame); // Release V4L2 output buffer immediately to prevent DPB overflow
                } else {
                    g_logger.warn("VIDEO_DEC: av_hwframe_transfer_data failed, skipping frame");
                    av_frame_free(&tmp_sw);
                    av_frame_unref(frame);
                    continue;
                }
            }
            VideoFrame vf;
            vf.format = sw_frame->format;
            vf.linesize_y = sw_frame->linesize[0];
            vf.linesize_uv = sw_frame->linesize[1];

            if (sw_frame->format == AV_PIX_FMT_NV12) {
                fill_nv12(sw_frame, vf);
                if (tmp_sw) av_frame_free(&tmp_sw);
            } else if (sw_frame->format == AV_PIX_FMT_YUV420P) {
                vf.width = sw_frame->width;
                vf.height = sw_frame->height;
                vf.is_nv12 = true;
                vf.linesize_y = sw_frame->linesize[0];
                vf.linesize_uv = sw_frame->width;
                int size_y = vf.linesize_y * vf.height;
                int size_uv = vf.linesize_uv * (vf.height / 2);
                vf.data = new uint8_t[size_y];
                vf.data_uv = new uint8_t[size_uv];
                memcpy(vf.data, sw_frame->data[0], size_y);

                const uint8_t* u_plane = sw_frame->data[1];
                const uint8_t* v_plane = sw_frame->data[2];
                int uv_w = sw_frame->width / 2;
                int uv_h = sw_frame->height / 2;
                int u_stride = sw_frame->linesize[1];
                int v_stride = sw_frame->linesize[2];
                for (int y = 0; y < uv_h; y++) {
                    const uint8_t* u_row = u_plane + y * u_stride;
                    const uint8_t* v_row = v_plane + y * v_stride;
                    uint8_t* out_row = vf.data_uv + y * vf.linesize_uv;
                    for (int x = 0; x < uv_w; x++) {
                        out_row[x * 2] = u_row[x];
                        out_row[x * 2 + 1] = v_row[x];
                    }
                }
                if (tmp_sw) av_frame_free(&tmp_sw);
            } else {
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
                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws) {
                        g_logger.error("VIDEO_DEC: Failed to create scaler for fmt={}", (int)actual_pix_fmt);
                        if (tmp_sw) av_frame_free(&tmp_sw);
                        break;
                    }
                    g_logger.info("VIDEO_DEC: Created scaler for fmt={} ({}x{} -> {}x{})",
                        (int)actual_pix_fmt, sw_frame->width, sw_frame->height, dst_w, dst_h);
                }
                sws_scale(sws, sw_frame->data, sw_frame->linesize, 0, sw_frame->height,
                          rgba->data, rgba->linesize);
                if (tmp_sw) av_frame_free(&tmp_sw);

                vf.width = dst_w;
                vf.height = dst_h;
                vf.data = new uint8_t[nbytes];
                memcpy(vf.data, buf, nbytes);
            }
            // Use pre-captured PTS (saved before av_frame_unref on HW path)
            if (pts_raw != AV_NOPTS_VALUE) {
                vf.pts = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base) * pts_raw;
            }
            // Track last decoded frame PTS for accurate countdown timer
            if (vf.pts > 0) m_last_frame_pts.store(vf.pts + m_frame_duration, std::memory_order_relaxed);
            note_frame_anchor(vf.pts, pts_raw != AV_NOPTS_VALUE, eof);

            {
                std::unique_lock<std::mutex> lk(m_queue_mtx);
                m_queue_cv.wait(lk, [this] {
                    return m_frame_queue.size() < MAX_QUEUED_FRAMES || !m_running.load();
                });
                if (!m_running.load()) break;
                m_frame_queue.push(std::move(vf));
            }
                crawl_count++;
                log_push_rate("main");
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
            [[unlikely]]
            if (ret < 0) continue;
            while (ret >= 0) {
                ret = avcodec_receive_frame(acc, aframe);
                if (ret != 0) break;
                if (swr && aframe->nb_samples > 0 && aframe->extended_data) {
                    int max_s = std::min((int)av_rescale_rnd(aframe->nb_samples, 48000, audio_sample_rate, AV_ROUND_UP), MAX_AUDIO_SAMPLES);
                    uint8_t* outbuf[1] = { (uint8_t*)audio_resample_buf.data() };
                    int os = swr_convert(swr, outbuf, max_s,
                                         (const uint8_t**)aframe->extended_data, aframe->nb_samples);
                    int64_t apts = (aframe->best_effort_timestamp != AV_NOPTS_VALUE) ? aframe->best_effort_timestamp : aframe->pts;
                    double pa_s = (apts != AV_NOPTS_VALUE) ? av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base) * apts : -1.0;
                    if (os > 0) { push_audio_samples({audio_resample_buf.data(), (size_t)(os * 2)}, pa_s); af_count += os; }
                    
                }
            }
        } else {
            av_packet_unref(pkt);
        }
    }

    g_logger.info("VIDEO_DEC: Done {} ({} vf, {} af)", m_path.c_str(), vf_count, af_count);
    // Mark decoder as stopped immediately so the render loop stops spinning
    // on "decoder already running" while we clean up FFmpeg resources below
    m_running.store(false);
    m_eof.store(true); // Mark EOF so render loop transitions to next item (even on stall)

    try {
    if (nv12_scale_buf) av_free(nv12_scale_buf);
    if (nv12_sws) sws_freeContext(nv12_sws);
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
    } catch (...) {}

    } catch (const std::exception& e) {
    [[unlikely]]
        g_logger.error("VIDEO_DEC: Exception: {}", e.what());
    } catch (...) {
        g_logger.error("VIDEO_DEC: Unknown exception");
        m_running.store(false);
    }
}
