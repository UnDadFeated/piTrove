#include "video_decoder.h"
#include "util.h"
#include "config.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
using namespace std::chrono_literals;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static bool until(auto predicate) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return predicate();
}
static std::atomic<int> input_mode{0}; // 0 real, 1 pause until cancellation, 2 immediate failure
static std::atomic<bool> entered{false}, interrupted{false}, release_input{false};
extern "C" int __real_avformat_open_input(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**);
extern "C" int __wrap_avformat_open_input(AVFormatContext** context, const char* path, const AVInputFormat* format, AVDictionary** options) {
    int mode = input_mode.load();
    if (!mode) return __real_avformat_open_input(context, path, format, options);
    if (mode == 1) {
        entered = true;
        interrupted = until([&] { return (*context)->interrupt_callback.callback((*context)->interrupt_callback.opaque) != 0; });
        until([] { return release_input.load(); });
    }
    avformat_free_context(*context); *context = nullptr;
    return AVERROR_EXIT;
}
static std::atomic<int> codec_fail_after{-1};
extern "C" AVCodecContext* __real_avcodec_alloc_context3(const AVCodec*);
extern "C" AVCodecContext* __wrap_avcodec_alloc_context3(const AVCodec* codec) {
    int left = codec_fail_after.load();
    if (left >= 0 && codec_fail_after.fetch_sub(1) == 0) return nullptr;
    return __real_avcodec_alloc_context3(codec);
}
enum class AudioFault { none, open, create, bind, pause };
static AudioFault audio_fault = AudioFault::none;
static int opens = 0, closes = 0, streams = 0, destroys = 0, resumes = 0, invalid_closes = 0;
extern "C" SDL_AudioDeviceID __real_SDL_OpenAudioDevice(SDL_AudioDeviceID, const SDL_AudioSpec*);
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(SDL_AudioDeviceID id, const SDL_AudioSpec* spec) {
    if (audio_fault == AudioFault::open) return 0;
    auto device = __real_SDL_OpenAudioDevice(id, spec); if (device) ++opens; return device;
}
extern "C" void __real_SDL_CloseAudioDevice(SDL_AudioDeviceID);
extern "C" void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID id) {
    if (!id) { ++invalid_closes; return; }
    ++closes; __real_SDL_CloseAudioDevice(id);
}
extern "C" SDL_AudioStream* __real_SDL_CreateAudioStream(const SDL_AudioSpec*, const SDL_AudioSpec*);
extern "C" SDL_AudioStream* __wrap_SDL_CreateAudioStream(const SDL_AudioSpec* source, const SDL_AudioSpec* dest) {
    if (audio_fault == AudioFault::create) return nullptr;
    auto stream = __real_SDL_CreateAudioStream(source, dest); if (stream) ++streams; return stream;
}
extern "C" void __real_SDL_DestroyAudioStream(SDL_AudioStream*);
extern "C" void __wrap_SDL_DestroyAudioStream(SDL_AudioStream* stream) { ++destroys; __real_SDL_DestroyAudioStream(stream); }
extern "C" bool __real_SDL_BindAudioStream(SDL_AudioDeviceID, SDL_AudioStream*);
extern "C" bool __wrap_SDL_BindAudioStream(SDL_AudioDeviceID device, SDL_AudioStream* stream) {
    return audio_fault != AudioFault::bind && __real_SDL_BindAudioStream(device, stream);
}
extern "C" bool __real_SDL_PauseAudioStreamDevice(SDL_AudioStream*);
extern "C" bool __wrap_SDL_PauseAudioStreamDevice(SDL_AudioStream* stream) {
    return audio_fault != AudioFault::pause && __real_SDL_PauseAudioStreamDevice(stream);
}
extern "C" bool __real_SDL_ResumeAudioStreamDevice(SDL_AudioStream*);
extern "C" bool __wrap_SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) { ++resumes; return __real_SDL_ResumeAudioStreamDevice(stream); }

// Real tiny RGB + PCM media, generated with the linked muxer; no external network/files.
static void make_media(const std::string& path, bool video = true, int frames = 3) {
    AVFormatContext* format = nullptr;
    check(avformat_alloc_output_context2(&format, nullptr, "nut", path.c_str()) >= 0, "fixture muxer allocation failed");
    AVStream* visual = video ? avformat_new_stream(format, nullptr) : nullptr;
    if (visual) {
        visual->time_base = {1, 30}; visual->avg_frame_rate = {30, 1};
        auto* p = visual->codecpar; p->codec_type = AVMEDIA_TYPE_VIDEO; p->codec_id = AV_CODEC_ID_RAWVIDEO;
        p->width = 64; p->height = 48; p->format = AV_PIX_FMT_RGB24;
    }
    AVStream* audio = avformat_new_stream(format, nullptr);
    audio->time_base = {1, 48000};
    auto* p = audio->codecpar; p->codec_type = AVMEDIA_TYPE_AUDIO; p->codec_id = AV_CODEC_ID_PCM_S16LE;
    p->sample_rate = 48000; p->format = AV_SAMPLE_FMT_S16; p->bits_per_coded_sample = 16;
    av_channel_layout_default(&p->ch_layout, 2);
    check(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(format, nullptr) >= 0, "fixture header failed");
    auto packet = av_packet_alloc();
    for (int i = 0; i < frames; ++i) {
        if (visual) {
            check(av_new_packet(packet, 64 * 48 * 3) == 0, "fixture packet failed");
            std::memset(packet->data, 90, packet->size); packet->stream_index = visual->index;
            packet->pts = packet->dts = av_rescale_q(i, {1, 30}, visual->time_base);
            packet->duration = av_rescale_q(1, {1, 30}, visual->time_base); packet->flags = AV_PKT_FLAG_KEY;
            check(av_interleaved_write_frame(format, packet) >= 0, "fixture video write failed");
        }
        check(av_new_packet(packet, 1600 * 4) == 0, "fixture packet failed");
        std::memset(packet->data, 0, packet->size); packet->stream_index = audio->index;
        packet->pts = packet->dts = i * 1600; packet->duration = 1600; packet->flags = AV_PKT_FLAG_KEY;
        check(av_interleaved_write_frame(format, packet) >= 0, "fixture audio write failed");
    }
    check(av_write_trailer(format) >= 0, "fixture trailer failed");
    av_packet_free(&packet); avio_closep(&format->pb); avformat_free_context(format);
}
static void test_restart() {
    VideoDecoder decoder;
    input_mode = 1; entered = false; interrupted = false; release_input = false;
    check(decoder.start("old-input", 64, 48) && until([] { return entered.load(); }), "old worker did not enter");
    std::atomic<bool> restarted{false};
    std::jthread restart([&] { decoder.start("new-input", 64, 48); restarted = true; });
    bool stopped = until([] { return interrupted.load(); });
    bool ordered = stopped && !restarted && decoder.get_current_path() == "old-input";
    input_mode = 2; release_input = true; restart.join();
    decoder.stop(); input_mode = 0;
    check(ordered, "start reset state before the old worker joined");
}
static void test_audio(const std::string& path) {
    for (auto fault : {AudioFault::none, AudioFault::open, AudioFault::create, AudioFault::bind, AudioFault::pause}) {
        audio_fault = fault; opens = closes = streams = destroys = resumes = invalid_closes = 0;
        VideoDecoder decoder; decoder.set_av_sync(false);
        check(decoder.start(path, 64, 48) && until([&] { return decoder.is_eof(); }), "audio fixture decode did not finish");
        check(!decoder.has_start_failed() && decoder.has_frames(), "valid fixture rejected");
        decoder.note_presentation_start(0.0);
        decoder.stop();
        check(resumes == (fault == AudioFault::none ? 1 : 0), "legacy playback did not resume valid audio exclusively");
        check(opens == closes && streams == destroys && !invalid_closes, "audio failure leaked or closed an invalid resource");
        check(!decoder.has_frames() && !decoder.is_running(), "stop left queued frames or worker");
    }
    audio_fault = AudioFault::none;
}
int main() {
    char pattern[] = "/tmp/pitrove-playback-XXXXXX"; char* directory = mkdtemp(pattern);
    try {
        check(directory, "temporary directory failed");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"); check(SDL_Init(SDL_INIT_AUDIO), "dummy audio init failed");
        g_logger.level = LogLevel::ERROR;
        { std::unique_lock lock(g_config_mtx); g_cfg.video_decode_budget_enabled = false; }
        std::string path = std::string(directory) + "/sample.nut"; make_media(path);
        test_restart(); test_audio(path);
        const std::string long_path = std::string(directory) + "/queued.nut";
        make_media(long_path, true, 720);
        {
            VideoDecoder decoder; decoder.start(long_path, 64, 48);
            check(until([&] { return decoder.frame_queue_size() >= decoder.get_max_queued_frames(); }), "fixture did not fill the frame queue");
            decoder.stop();
            check(!decoder.is_running() && !decoder.has_frames(), "full queue did not stop and drain");
        }
        for (int fail_after : {0, 1}) {
            codec_fail_after = fail_after;
            VideoDecoder decoder; decoder.start(path, 64, 48);
            check(until([&] { return decoder.is_eof(); }) && decoder.has_start_failed() && !decoder.is_running(), "allocation failure left decoder running");
            decoder.stop(); codec_fail_after = -1;
        }
        path = std::string(directory) + "/audio-only.nut"; make_media(path, false);
        { VideoDecoder decoder; decoder.start(path, 64, 48); check(until([&] { return decoder.is_eof(); }) && decoder.has_start_failed(), "no-video input did not latch failure"); }
        SDL_Quit(); std::filesystem::remove_all(directory);
        std::cout << "Decoder restart, audio failures, legacy audio and allocation regressions passed\n";
    } catch (const std::exception& error) {
        release_input = true;
        std::cerr << error.what() << '\n'; SDL_Quit(); if (directory) std::filesystem::remove_all(directory); return 1;
    }
}
