#pragma once

#include "audio_sink.hpp"
#include "av_clock.hpp"
#include "frame_queue.hpp"
#include "video_renderer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <openmedia/audio.hpp>
#include <openmedia/codec_api.hpp>
#include <openmedia/codec_registry.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/format_detector.hpp>
#include <openmedia/format_registry.hpp>
#include <openmedia/io.hpp>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace openmedia;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace detail {

static auto toSdlFormat(OMSampleFormat fmt) noexcept -> SDL_AudioFormat {
    switch (fmt) {
        case OM_SAMPLE_U8:  return SDL_AUDIO_U8;
        case OM_SAMPLE_S16: return SDL_AUDIO_S16;
        case OM_SAMPLE_S32: return SDL_AUDIO_S32;
        case OM_SAMPLE_F32: return SDL_AUDIO_F32;
        default:            return SDL_AUDIO_S16;
    }
}

// Interleave planar PCM planes into a contiguous buffer.
static auto interleave(const AudioSamples& s) -> std::vector<uint8_t> {
    const uint32_t ch        = s.format.channels;
    const uint32_t nb        = s.nb_samples;
    const size_t   bps       = getBytesPerSample(s.format.sample_format);
    const size_t   frame_sz  = bps * ch;
    std::vector<uint8_t> out(static_cast<size_t>(nb) * frame_sz);

    if (s.format.planar) {
        for (uint32_t c = 0; c < ch; ++c) {
            const uint8_t* src = s.planes.getData(c);
            if (!src) continue;
            for (uint32_t i = 0; i < nb; ++i)
                std::memcpy(out.data() + (i * ch + c) * bps, src + i * bps, bps);
        }
    } else {
        const uint8_t* src = s.planes.getData(0);
        if (src) std::memcpy(out.data(), src, out.size());
    }
    return out;
}

// Some codecs deliver sub-32-bit samples stored in 32-bit words with the
// payload in the MSB. Shift it down so SDL sees full-range values.
static auto normaliseBits(std::vector<uint8_t> src,
                                           uint8_t bits) -> std::vector<uint8_t> {
    if (bits == 0 || bits == 8 || bits == 32) return src;
    const int shift = 32 - static_cast<int>(bits);
    const size_t n  = src.size() / 4;
    std::vector<uint8_t> dst(src.size());
    for (size_t i = 0; i < n; ++i) {
        int32_t s = 0;
        std::memcpy(&s, src.data() + i * 4, 4);
        s <<= shift;
        std::memcpy(dst.data() + i * 4, &s, 4);
    }
    return dst;
}

static auto formatTime(int64_t pts, Rational tb) -> std::string {
    if (pts < 0 || tb.den == 0) return "00:00";
    const int total_s = static_cast<int>(
        static_cast<double>(pts) * tb.num / tb.den);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total_s / 60, total_s % 60);
    return buf;
}

static auto buildPixels(const Picture& pic) -> std::vector<uint32_t> {
    std::vector<uint32_t> pixels(pic.width * pic.height);
    for (uint32_t y = 0; y < pic.height; ++y) {
        const uint8_t* src = pic.planes.getData(0) + y * pic.planes.getLinesize(0);
        uint32_t*      dst = pixels.data() + y * pic.width;
        for (uint32_t x = 0; x < pic.width; ++x) {
            const uint8_t r = src[x * 4], g = src[x * 4 + 1],
                          b = src[x * 4 + 2], a = src[x * 4 + 3];
            dst[x] = (uint32_t(a) << 24) | (uint32_t(b) << 16) |
                     (uint32_t(g) <<  8) |  uint32_t(r);
        }
    }
    return pixels;
}

} // namespace detail

// ---------------------------------------------------------------------------
// MediaPlayer
// ---------------------------------------------------------------------------
class MediaPlayer {
public:
    // Public status (read from main thread)
    std::string current_file;

    MediaPlayer() {
        format_detector_.addAllStandard();
        registerBuiltInCodecs(&codec_registry_);
        registerBuiltInFormats(&format_registry_);
    }

    ~MediaPlayer() { stop(); }

    void setRenderer(SDL_Renderer* r) {
        renderer_ = r;
        video_renderer_.setRenderer(r);
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void stop() {
        stopWorker();

        audio_sink_.close();

        video_frame_queue_.flush();
        video_renderer_.reset();

        if (demuxer_) {
            demuxer_->close();
            demuxer_.reset();
        }
        audio_decoder_.reset();
        video_decoder_.reset();

        if (image_texture_) {
            SDL_DestroyTexture(image_texture_);
            image_texture_ = nullptr;
        }

        clock_.reset(0);
        has_image_         = false;
        has_video_         = false;
        has_audio_         = false;
        seek_pending_      = false;
        current_file.clear();
        audio_stream_index_ = -1;
        video_stream_index_ = -1;
        image_stream_index_ = -1;
        total_duration_    = 0;
    }

    auto play(const std::string& path) -> bool {
        stop();
        path_ = path;

        auto input = InputStream::createFileStream(path);
        if (!input || !input->isValid()) {
            SDL_Log("[Player] Cannot open: %s", path.c_str());
            return false;
        }

        uint8_t probe[2048];
        const size_t n   = input->read(probe);
        const DetectedFormat fmt = format_detector_.detect({probe, n});
        if (fmt.isUnknown()) {
            SDL_Log("[Player] Unknown format: %s", path.c_str());
            return false;
        }
        input->seek(0, Whence::BEG);

        if (fmt.isContainer()) {
            if (const auto* desc = format_registry_.getFormat(fmt.container);
                desc && desc->isDemuxing())
                demuxer_ = desc->demuxer_factory();
        }
        if (!demuxer_) {
            SDL_Log("[Player] No demuxer for format %d", int(fmt.container));
            return false;
        }
        if (demuxer_->open(std::move(input)) != OM_SUCCESS) {
            SDL_Log("[Player] Demuxer open failed");
            return false;
        }

        onDemuxerOpen();
        return true;
    }

    // -----------------------------------------------------------------------
    // Controls
    // -----------------------------------------------------------------------

    void setVolume(float v) {
        volume_ = std::clamp(v, 0.0f, 1.5f);
        audio_sink_.setGain(volume_);
    }

    void seek(float progress) {
        if (!demuxer_ || total_duration_ <= 0) return;
        {
            std::lock_guard lock(seek_mutex_);
            pending_seek_progress_ = std::clamp(progress, 0.0f, 1.0f);
            seek_pending_          = true;
            last_seek_time_        = SteadyClock::now();
        }
        seek_cv_.notify_one();
    }

    // -----------------------------------------------------------------------
    // Per-frame render-loop call (main thread).
    // -----------------------------------------------------------------------
    void tickVideo() {
        // Update wall clock when there is no audio driving it.
        if (!audio_sink_.started()) clock_.wallTick();

        video_renderer_.tick(video_frame_queue_, clock_);
    }

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    auto  getVolume()      const -> float { return volume_; }
    auto   hasImage()       const -> bool { return has_image_; }
    auto   hasVideo()       const -> bool { return has_video_; }
    auto   hasAudio()       const -> bool { return has_audio_; }

    auto   isActive()       const -> bool {
        return has_video_ || audio_sink_.started();
    }

    auto getProgress() const -> float {
        if (total_duration_ <= 0) return 0.0f;
        return static_cast<float>(clock_.masterPts()) /
               static_cast<float>(total_duration_);
    }

    auto getProgressString() const -> std::string {
        return detail::formatTime(clock_.masterPts(), clock_.timeBase()) +
               " / " +
               detail::formatTime(total_duration_, clock_.timeBase());
    }

    auto getVideoTexture() -> SDL_Texture*  { return video_renderer_.texture(); }
    auto getVideoSize() const -> std::pair<uint32_t,uint32_t> {
        return {video_renderer_.textureWidth(), video_renderer_.textureHeight()};
    }

    auto getImageTexture() -> SDL_Texture* {
        std::lock_guard lock(image_mutex_);
        return image_texture_;
    }
    auto getImageSize() const -> std::pair<uint32_t,uint32_t> {
        return {image_width_, image_height_};
    }

private:
    // -----------------------------------------------------------------------
    // Setup
    // -----------------------------------------------------------------------

    void onDemuxerOpen() {
        const auto& tracks = demuxer_->tracks();
        for (size_t i = 0; i < tracks.size(); ++i) {
            const auto& t = tracks[i];
            if (t.format.type == OM_MEDIA_AUDIO && audio_stream_index_ < 0)
                audio_stream_index_ = int32_t(i);
            else if (t.format.type == OM_MEDIA_VIDEO && !t.isImage() &&
                     video_stream_index_ < 0)
                video_stream_index_ = int32_t(i);
            else if (t.isImage() && image_stream_index_ < 0)
                image_stream_index_ = int32_t(i);
        }

        // Still image: decode immediately, no worker needed.
        if (image_stream_index_ >= 0 &&
            video_stream_index_ < 0 &&
            audio_stream_index_ < 0) {
            setupImageDecoder(tracks[size_t(image_stream_index_)]);
            return;
        }

        if (video_stream_index_ >= 0)
            setupVideoDecoder(tracks[size_t(video_stream_index_)]);
        if (audio_stream_index_ >= 0)
            setupAudioDecoder(tracks[size_t(audio_stream_index_)]);

        if (video_stream_index_ >= 0 || audio_stream_index_ >= 0) {
            current_file = path_;
            startWorker();
        }
    }

    auto makeDecoder(const Track& track, std::unique_ptr<Decoder>& dec) -> bool {
        dec = codec_registry_.createDecoder(track.format.codec_id);
        if (!dec) {
            SDL_Log("[Player] No decoder for codec %d", int(track.format.codec_id));
            return false;
        }
        DecoderOptions opts;
        opts.format    = track.format;
        opts.time_base = track.time_base;
        opts.extradata = track.extradata;
        if (dec->configure(opts) != OM_SUCCESS) {
            dec.reset();
            SDL_Log("[Player] Decoder configure failed");
            return false;
        }
        return true;
    }

    void setupVideoDecoder(const Track& track) {
        if (!makeDecoder(track, video_decoder_)) return;

        // Use the video track's timebase as the master reference when
        // there is no audio.  When audio is present, setupAudioDecoder
        // will overwrite this.
        clock_.setTimeBase(track.time_base);
        clock_.setMode(AVClock::Mode::WALL);
        clock_.reset(0);

        total_duration_ = track.duration;
        video_time_base_ = track.time_base;
        has_video_ = true;

        SDL_Log("[Player] Video %dx%d codec=%s tb=%d/%d",
                track.format.image.width, track.format.image.height,
                getCodecMeta(track.format.codec_id).name.data(),
                track.time_base.num, track.time_base.den);
    }

    void setupAudioDecoder(const Track& track) {
        if (!makeDecoder(track, audio_decoder_)) return;

        // Audio track drives the master clock – use its timebase.
        clock_.setTimeBase(track.time_base);
        clock_.setMode(AVClock::Mode::AUDIO);
        clock_.reset(0);

        if (video_stream_index_ < 0) {
            total_duration_ = track.duration;
        }
        audio_time_base_ = track.time_base;
        has_audio_ = true;

        SDL_Log("[Player] Audio codec=%s tb=%d/%d",
                getCodecMeta(track.format.codec_id).name.data(),
                track.time_base.num, track.time_base.den);
    }

    void setupImageDecoder(const Track& track) {
        if (!makeDecoder(track, video_decoder_)) return;
        image_width_    = track.format.image.width;
        image_height_   = track.format.image.height;
        total_duration_ = track.duration;
        decodeAndShowImage();
    }

    // -----------------------------------------------------------------------
    // Worker
    // -----------------------------------------------------------------------

    void startWorker() {
        video_frame_queue_.resetFlush();
        stop_requested_ = false;
        worker_         = std::thread([this] { workerLoop(); });
    }

    void stopWorker() {
        {
            std::lock_guard lock(seek_mutex_);
            stop_requested_ = true;
        }
        seek_cv_.notify_all();
        video_frame_queue_.flush();
        if (worker_.joinable()) worker_.join();
    }

    void workerLoop() {
        using namespace std::chrono_literals;

        while (!stop_requested_) {
            // ---- handle pending seek ----
            {
                std::unique_lock lock(seek_mutex_);
                if (seek_pending_ &&
                    (SteadyClock::now() - last_seek_time_) >= kSeekSettle) {
                    const float p   = pending_seek_progress_;
                    seek_pending_   = false;
                    lock.unlock();
                    doSeek(p);
                    continue;
                }
            }

            // ---- back-pressure: sleep if queues are well-fed ----
            const bool video_full = has_video_ &&
                video_frame_queue_.size() >= video_frame_queue_.capacity() * 3 / 4;
            const bool audio_full = has_audio_ && !audio_sink_.needsData();

            if (video_full && audio_full) {
                std::unique_lock lock(seek_mutex_);
                seek_cv_.wait_for(lock, 10ms, [&] {
                    return stop_requested_.load() || seek_pending_;
                });
                continue;
            }

            // ---- decode one packet ----
            auto res = demuxer_->readPacket();
            if (res.isErr()) {
                SDL_Log("[Worker] EOF/error %d", res.unwrapErr());
                // Wait instead of spinning on EOF.
                std::unique_lock lock(seek_mutex_);
                seek_cv_.wait_for(lock, 200ms, [&] {
                    return stop_requested_.load() || seek_pending_;
                });
                continue;
            }
            processPacket(res.unwrap(), /*allow_open_device=*/true);
        }
    }

    // -----------------------------------------------------------------------
    // Seek
    // -----------------------------------------------------------------------

    void doSeek(float progress) {
        audio_sink_.pause();
        audio_sink_.clearBuffer();
        if (audio_decoder_) audio_decoder_->flush();
        if (video_decoder_) video_decoder_->flush();
        video_frame_queue_.flush();
        video_frame_queue_.resetFlush();

        const int32_t ref = (audio_stream_index_ >= 0)
                              ? audio_stream_index_
                              : video_stream_index_;

        const int64_t target_pts = int64_t(
            progress * float(total_duration_));

        // Convert target PTS → nanoseconds for demuxer seek.
        const Rational& tb = (audio_stream_index_ >= 0)
                               ? audio_time_base_
                               : video_time_base_;
        int64_t target_ns = 0;
        if (tb.den != 0)
            target_ns = int64_t(
                double(target_pts) * tb.num / tb.den * 1e9);

        if (demuxer_->seek(target_ns, ref) == OM_SUCCESS) {
            clock_.reset(target_pts);
        }

        // Warm the pipeline with a few packets.
        for (int i = 0; i < 12 && !stop_requested_; ++i) {
            auto res = demuxer_->readPacket();
            if (res.isErr()) break;
            processPacket(res.unwrap(), /*allow_open_device=*/false);
        }
    }

    // -----------------------------------------------------------------------
    // Packet processing
    // -----------------------------------------------------------------------

    void processPacket(const Packet& pkt, bool allow_open_device) {
        if (pkt.stream_index == audio_stream_index_ && audio_decoder_)
            processAudioPacket(pkt, allow_open_device);
        else if (pkt.stream_index == video_stream_index_ && video_decoder_)
            processVideoPacket(pkt);
    }

    void processAudioPacket(const Packet& pkt, bool allow_open_device) {
        auto result = audio_decoder_->decode(pkt);
        if (result.isErr()) return;

        for (auto& frame : result.unwrap()) {
            if (!std::holds_alternative<AudioSamples>(frame.data)) continue;
            const AudioSamples& s = std::get<AudioSamples>(frame.data);
            if (s.nb_samples == 0) continue;

            // Open SDL audio device on first decoded frame.
            if (!audio_sink_.isOpen()) {
                if (!allow_open_device) continue;
                const size_t bps = getBytesPerSample(s.format.sample_format);
                if (!audio_sink_.open(
                        detail::toSdlFormat(s.format.sample_format),
                        int(s.format.channels),
                        int(s.format.sample_rate),
                        bps, &clock_)) {
                    continue;
                }
            }

            // Interleave + normalise bit-width.
            auto pcm = detail::normaliseBits(
                detail::interleave(s), s.format.bits_per_sample);

            // Blocking write with stop-check.
            size_t written = 0;
            while (written < pcm.size() && !stop_requested_) {
                written += audio_sink_.pushPcm(pcm.data() + written,
                                               pcm.size() - written);
                if (written < pcm.size())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            // Prime buffering – unpauses device once threshold is reached.
            audio_sink_.tickBuffering(int64_t(frame.pts));
        }
    }

    void processVideoPacket(const Packet& pkt) {
        auto result = video_decoder_->decode(pkt);
        if (result.isErr()) return;

        for (auto& frame : result.unwrap()) {
            if (!std::holds_alternative<Picture>(frame.data)) continue;
            const Picture& pic = std::get<Picture>(frame.data);
            if (pic.width == 0 || pic.height == 0) continue;

            VideoFrame vf;
            vf.width  = pic.width;
            vf.height = pic.height;
            vf.pts    = int64_t(frame.pts);

            // Pre-compute pts_sec using the video track's own timebase.
            vf.pts_sec = clock_.ptsToSeconds(vf.pts);

            vf.y_stride = pic.planes.getLinesize(0);
            vf.u_stride = pic.planes.getLinesize(1);
            vf.v_stride = pic.planes.getLinesize(2);

            const uint8_t* y = pic.planes.getData(0);
            const uint8_t* u = pic.planes.getData(1);
            const uint8_t* v = pic.planes.getData(2);

            vf.y_plane.assign(y, y + vf.y_stride * pic.height);
            vf.u_plane.assign(u, u + vf.u_stride * (pic.height / 2));
            vf.v_plane.assign(v, v + vf.v_stride * (pic.height / 2));

            // Back-pressure: sleep-retry until there is space or we're stopped.
            // Never block inside the queue itself.
            while (!stop_requested_ && !video_frame_queue_.isFlushing()) {
                if (video_frame_queue_.tryPush(std::move(vf))) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    // -----------------------------------------------------------------------
    // Still image
    // -----------------------------------------------------------------------

    void decodeAndShowImage() {
        if (!video_decoder_ || !renderer_) return;
        auto res = demuxer_->readPacket();
        if (res.isErr()) return;

        auto result = video_decoder_->decode(res.unwrap());
        if (result.isErr() || result.unwrap().empty()) return;

        Frame& f = result.unwrap()[0];
        if (!std::holds_alternative<Picture>(f.data)) return;

        const Picture& pic  = std::get<Picture>(f.data);
        const auto     pix  = detail::buildPixels(pic);

        std::lock_guard lock(image_mutex_);
        if (image_texture_) {
            SDL_DestroyTexture(image_texture_);
            image_texture_ = nullptr;
        }
        image_texture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STATIC,
            int(pic.width), int(pic.height));
        if (!image_texture_) return;

        SDL_UpdateTexture(image_texture_, nullptr, pix.data(),
                          int(pic.width * sizeof(uint32_t)));
        image_width_  = pic.width;
        image_height_ = pic.height;
        has_image_    = true;
        current_file  = path_;
        SDL_Log("[Player] Image %dx%d loaded", image_width_, image_height_);
    }

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    // Infrastructure
    FormatDetector format_detector_;
    CodecRegistry  codec_registry_;
    FormatRegistry format_registry_;

    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<Decoder> audio_decoder_;
    std::unique_ptr<Decoder> video_decoder_;
    std::string              path_;

    // Stream indices
    int32_t audio_stream_index_ = -1;
    int32_t video_stream_index_ = -1;
    int32_t image_stream_index_ = -1;

    // Timebases per track (may differ!)
    Rational audio_time_base_ {1, 44100};
    Rational video_time_base_ {1, 90000};

    // A/V pipeline
    AVClock         clock_;
    AudioSink       audio_sink_;
    FrameQueue      video_frame_queue_ {8};
    VideoRenderer   video_renderer_;
    SDL_Renderer*   renderer_    = nullptr;

    // Audio state
    float volume_    = 1.0f;
    bool  has_audio_ = false;

    // Video state
    bool has_video_ = false;

    // Image state
    SDL_Texture*   image_texture_ = nullptr;
    mutable std::mutex image_mutex_;
    uint32_t       image_width_   = 0;
    uint32_t       image_height_  = 0;
    bool           has_image_     = false;

    // Timeline
    int64_t total_duration_ = 0;

    // Worker
    std::thread             worker_;
    std::atomic<bool>       stop_requested_ {false};
    std::mutex              seek_mutex_;
    std::condition_variable seek_cv_;
    bool                    seek_pending_          = false;
    float                   pending_seek_progress_ = 0.0f;
    TimePoint               last_seek_time_;

    static constexpr auto kSeekSettle = std::chrono::milliseconds(100);
};
