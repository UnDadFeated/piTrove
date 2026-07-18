#include "video_decoder.h"
#include "util.h"

extern Logger g_logger;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

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
    g_logger.info("VIDEO_DEC: Starting decode thread for %s", m_path.c_str());

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

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = (int)i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        g_logger.error("VIDEO_DEC: No video stream found in %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    AVCodecParameters* codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        g_logger.error("VIDEO_DEC: Unsupported codec for %s", m_path.c_str());
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_params);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        g_logger.error("VIDEO_DEC: Failed to open codec for %s", m_path.c_str());
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    // Setup swscale context
    SwsContext* sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        g_logger.error("VIDEO_DEC: Failed to create scaler for %s", m_path.c_str());
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        m_running.store(false);
        return;
    }

    g_logger.info("VIDEO_DEC: Decoding %s (%dx%d)", m_path.c_str(), codec_ctx->width, codec_ctx->height);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgba_frame = av_frame_alloc();
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, codec_ctx->width, codec_ctx->height, 1);
    uint8_t* rgba_buffer = (uint8_t*)av_malloc(num_bytes);
    av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize, rgba_buffer,
                         AV_PIX_FMT_RGBA, codec_ctx->width, codec_ctx->height, 1);

    bool eof = false;
    while (is_running() && !eof) {
        int ret = av_read_frame(fmt_ctx, packet);
        if (ret == AVERROR_EOF) {
            eof = true;
            break;
        }
        if (ret < 0) {
            g_logger.error("VIDEO_DEC: Read error on %s", m_path.c_str());
            break;
        }

        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            continue;
        }

        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            continue;
        }
        if (ret < 0) {
            g_logger.error("VIDEO_DEC: Decode error");
            break;
        }

        // Convert to RGBA
        sws_scale(sws_ctx, frame->data, frame->linesize, 0, codec_ctx->height,
                  rgba_frame->data, rgba_frame->linesize);

        // Create VideoFrame and push to queue
        VideoFrame vf;
        vf.width = codec_ctx->width;
        vf.height = codec_ctx->height;
        vf.data = new uint8_t[num_bytes];
        memcpy(vf.data, rgba_buffer, num_bytes);

        std::lock_guard lk(m_queue_mtx);
        // Keep only the latest frame to avoid memory buildup during rendering lag
        while (!m_frame_queue.empty()) {
            m_frame_queue.pop();
        }
        m_frame_queue.push(std::move(vf));
    }

    g_logger.info("VIDEO_DEC: Decode finished for %s", m_path.c_str());

    // Cleanup - protect from exceptions
    try {
    av_frame_free(&rgba_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_free(rgba_buffer);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    } catch (...) { /* swallow cleanup errors */ }

    } catch (const std::exception& e) {
        g_logger.error("VIDEO_DEC: Exception in decode_loop: %s", e.what());
    } catch (...) {
        g_logger.error("VIDEO_DEC: Unknown exception in decode_loop");
    }
    m_running.store(false);
}
