#pragma once

#include <SDL3/SDL.h>

#include "audio_sink.hpp"
#include "av_clock.hpp"
#include "blocking_queue.hpp"
#include "diagnostics.hpp"
#include "hw_device.hpp"
#include "video_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <openmedia/audio.hpp>
#include <openmedia/codec_api.hpp>
#include <openmedia/codec_registry.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/format_detector.hpp>
#include <openmedia/format_registry.hpp>
#include <openmedia/io.hpp>
#include <openmedia/video.hpp>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace openmedia;

// Deep enough for a whole interleaving chunk of one stream: the demuxer blocks
// on whichever queue is full, and if audio sat behind that chunk the master
// clock would stop and nothing would ever drain. The byte budget bounds memory.
using PacketQueue = BlockingQueue<Packet>;
inline auto makePacketQueue() -> PacketQueue {
  return PacketQueue(1024, 32u << 20, [](const Packet& p) { return p.bytes.size(); });
}

namespace media {

inline auto seconds(int64_t pts, Rational tb) -> double {
  return tb.den ? double(pts) * tb.num / tb.den : 0.0;
}

inline auto audioSpec(const AudioFormat& f) -> SDL_AudioSpec {
  const auto format = [&] {
    switch (f.sample_format) {
      case OM_SAMPLE_U8: return SDL_AUDIO_U8;
      case OM_SAMPLE_S32: return SDL_AUDIO_S32;
      case OM_SAMPLE_F32: return SDL_AUDIO_F32;
      default: return SDL_AUDIO_S16;
    }
  }();
  return {format, int(f.channels), int(f.sample_rate)};
}

// Interleaved PCM; 32-bit samples narrower than 32 bits are scaled up to full range.
inline auto interleavedPcm(const AudioSamples& s) -> std::vector<uint8_t> {
  const size_t channels = s.format.channels;
  const size_t bps = getBytesPerSample(s.format.sample_format);
  std::vector<uint8_t> out(size_t(s.nb_samples) * channels * bps);

  if (!s.format.planar) {
    if (const uint8_t* src = s.planes.getData(0)) std::memcpy(out.data(), src, out.size());
  } else {
    for (size_t c = 0; c < channels; ++c) {
      const uint8_t* src = s.planes.getData(uint32_t(c));
      for (size_t i = 0; src && i < s.nb_samples; ++i)
        std::memcpy(out.data() + (i * channels + c) * bps, src + i * bps, bps);
    }
  }

  const int bits = s.format.bits_per_sample;
  if (bps == 4 && s.format.sample_format == OM_SAMPLE_S32 && bits > 0 && bits < 32) {
    for (size_t i = 0; i < out.size(); i += 4) {
      int32_t v;
      std::memcpy(&v, out.data() + i, 4);
      v = int32_t(uint32_t(v) << (32 - bits));
      std::memcpy(out.data() + i, &v, 4);
    }
  }
  return out;
}

// Copies a software picture's planes.
inline auto hostFrame(const Picture& pic) -> VideoFrame {
  VideoFrame vf;
  for (uint32_t i = 0; i < vf.planes.size(); ++i) {
    const uint8_t* data = pic.planes.getData(i);
    if (!data) continue;
    auto& plane = vf.planes[i];
    plane.stride = int(pic.planes.getLinesize(i));
    plane.data.assign(data, data + size_t(plane.stride) * pic.getPlaneDimensions(i).second);
  }
  return vf;
}

// Feeds `packets` through `decoder` and hands each decoded frame to `on_frame`
// until the stream ends (returns true) or the pipeline stops (returns false).
template <typename OnFrame>
auto decodeStream(Decoder& decoder, PacketQueue& packets, std::stop_token stop,
                  const char* what, OnFrame&& on_frame) -> bool {
  // false: the consumer is gone, stop decoding.
  const auto drain = [&] {
    while (!stop.stop_requested()) {
      auto result = decoder.receiveFrame();
      if (result.isErr()) return true;
      auto received = std::move(result).unwrap();
      if (received.status != ReceivedFrame::Status::Frame) return true;
      if (!on_frame(received.frame)) return false;
    }
    return false;
  };

  OMError last_error = OM_SUCCESS;
  while (auto packet = packets.pop(stop)) {
    // An empty packet marks the end of the stream: flush the decoder's tail.
    const bool eos = packet->bytes.empty();
    const auto send = [&] { return eos ? decoder.sendEndOfStream() : decoder.sendPacket(*packet); };

    OMError err = send();
    if (err == OM_CODEC_NEED_MORE_DATA) {
      if (!drain()) return false;
      err = send();
    }
    if (err != OM_SUCCESS && err != last_error)
      SDL_Log("[Player] %s decode failed: %s (%d)", what, diag::describe(err), int(err));
    last_error = err;

    if (!drain()) return false;
    if (eos) return true;
  }
  return false;
}

} // namespace media

class MediaPlayer {
public:
  explicit MediaPlayer(SDL_Renderer* renderer) : renderer_(renderer) {
    detector_.addAllStandard();
    registerBuiltInCodecs(&codecs_);
    registerBuiltInFormats(&formats_);
    openmedia::setLogger(std::make_unique<diag::SdlLogger>());
  }

  ~MediaPlayer() { stop(); }

  // Prefers the backend's video decoders; software stays the fallback.
  auto useBackend(const Backend& backend) -> bool {
    if (!hw_.open(backend.api)) return false;
    decoder_prefix_ = backend.decoder_prefix;
    return true;
  }

  auto play(const std::string& path) -> bool {
    stop();
    error_.clear();
    if (!openDemuxer(path)) return false;

    const auto& tracks = demuxer_->tracks();
    diag::reportTracks(tracks);
    const auto find = [&](auto pred) -> int {
      const auto it = std::ranges::find_if(tracks, pred);
      return it == tracks.end() ? -1 : int(it - tracks.begin());
    };
    const int video = find([](const Track& t) { return t.format.type == OM_MEDIA_VIDEO && !t.isImage(); });
    const int audio = find([](const Track& t) { return t.format.type == OM_MEDIA_AUDIO; });
    const int image = find([](const Track& t) { return t.isImage(); });

    if (video < 0 && audio < 0) {
      if (image >= 0 && showImage(tracks[image])) return true;
      return fail("Nothing playable in " + path);
    }

    video_ = openStream(tracks, video);
    audio_ = openStream(tracks, audio);
    if (!video_ && !audio_) return fail("No working decoder for " + path);

    if (video_) {
      diag::reportDolbyVision(tracks[video]);
      colors_.report("container", tracks[video].format.video);
    }
    clock_.reset(0.0);
    start();
    return true;
  }

  void stop() {
    halt();
    audio_sink_.close();
    video_renderer_.reset();
    if (demuxer_) demuxer_->close();
    demuxer_.reset();
    audio_ = {};
    video_ = {};
    clock_.reset(0.0);
    duration_ = 0.0;
    finished_ = false;
    pending_seek_.reset();
  }

  // Scrubbing calls this continuously; the seek runs once the target settles.
  void seek(float progress) {
    if (!demuxer_ || duration_ <= 0.0) return;
    pending_seek_ = {std::clamp(progress, 0.0f, 1.0f) * duration_, SteadyClock::now()};
  }

  void setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.5f);
    audio_sink_.setGain(volume_);
  }

  // Once per rendered frame, on the main thread.
  void tick() {
    if (pending_seek_ && SteadyClock::now() - pending_seek_->requested > kSeekSettle)
      seekTo(std::exchange(pending_seek_, std::nullopt)->target);
    video_renderer_.present(frames_, clock_);
    checkFinished();
  }

  auto volume() const -> float { return volume_; }
  auto isPlaying() const -> bool { return audio_ || video_; }
  auto lastError() const -> const std::string& { return error_; }
  auto texture() const -> SDL_Texture* { return video_renderer_.texture(); }
  auto textureSize() const -> std::pair<uint32_t, uint32_t> { return video_renderer_.size(); }
  auto position() const -> double { return std::clamp(clock_.now(), 0.0, duration_); }
  auto duration() const -> double { return duration_; }

private:
  using SteadyClock = std::chrono::steady_clock;
  static constexpr auto kSeekSettle = std::chrono::milliseconds(100);

  struct Stream {
    int index = -1;
    Rational time_base {1, 1};
    std::unique_ptr<Decoder> decoder;
    explicit operator bool() const { return decoder != nullptr; }
  };

  struct PendingSeek {
    double target;
    SteadyClock::time_point requested;
  };

  auto fail(std::string message) -> bool {
    error_ = std::move(message);
    SDL_Log("[Player] %s", error_.c_str());
    return false;
  }

  // ---- setup ----------------------------------------------------------------

  auto openDemuxer(const std::string& path) -> bool {
    auto input = InputStream::createFileStream(path);
    if (!input || !input->isValid()) return fail("Cannot open " + path);

    uint8_t probe[2048];
    const DetectedFormat detected = detector_.detect({probe, input->read(probe)});
    input->seek(0, Whence::BEG);

    const auto* desc = detected.isContainer() ? formats_.getFormat(detected.container) : nullptr;
    if (!desc || !desc->isDemuxing()) return fail("Unsupported format: " + path);

    demuxer_ = desc->demuxer_factory();
    if (const OMError err = demuxer_->open(std::move(input)); err != OM_SUCCESS) {
      demuxer_.reset();
      return fail(std::string("Cannot demux: ") + diag::describe(err));
    }
    return true;
  }

  auto openStream(const std::vector<Track>& tracks, int index) -> Stream {
    if (index < 0) return {};
    const Track& track = tracks[index];
    auto decoder = makeDecoder(track);
    if (!decoder) return {};
    duration_ = std::max(duration_, media::seconds(int64_t(track.duration), track.time_base));
    return {index, track.time_base, std::move(decoder)};
  }

  // Decoders of the preferred backend first, else software before hardware
  // (a hardware decoder may open a device of its own when handed none).
  auto candidates(const Track& track) const -> std::vector<const CodecDescriptor*> {
    std::vector<const CodecDescriptor*> list;
    std::ranges::copy_if(codecs_.getCodecsByCodecId(track.format.codec_id), std::back_inserter(list),
                         [](const CodecDescriptor* d) { return d->isDecoding(); });
    if (track.format.type == OM_MEDIA_VIDEO) {
      std::ranges::stable_partition(list, [&](const CodecDescriptor* d) {
        return decoder_prefix_.empty() ? (d->flags & HARDWARE) == 0 : isPreferred(*d);
      });
    }
    return list;
  }

  auto isPreferred(const CodecDescriptor& d) const -> bool {
    return !decoder_prefix_.empty() && d.name.starts_with(decoder_prefix_);
  }

  auto makeDecoder(const Track& track) -> std::unique_ptr<Decoder> {
    DecoderOptions opts;
    opts.format = track.format;
    opts.time_base = track.time_base;
    opts.extradata = track.extradata;

    for (const CodecDescriptor* desc : candidates(track)) {
      // Only the preferred backend's decoders get the device; the software
      // fallback must stay genuinely software.
      const bool hardware = track.format.type == OM_MEDIA_VIDEO && isPreferred(*desc);
      opts.hw_device = hardware ? hw_.device() : std::nullopt;

      auto decoder = desc->decoder_factory();
      if (!decoder) continue;
      if (const OMError err = decoder->configure(opts); err != OM_SUCCESS) {
        SDL_Log("[Player] %.*s: configure failed: %s (%d)", int(desc->name.size()), desc->name.data(),
                diag::describe(err), int(err));
        continue;
      }
      SDL_Log("[Player] %s track %d: %.*s", track.format.type == OM_MEDIA_VIDEO ? "Video" : "Audio",
              int(track.format.codec_id), int(desc->name.size()), desc->name.data());
      return decoder;
    }
    SDL_Log("[Player] No decoder for codec %d", int(track.format.codec_id));
    return nullptr;
  }

  auto showImage(const Track& track) -> bool {
    auto decoder = makeDecoder(track);
    if (!decoder) return false;
    auto packet = demuxer_->readPacket();
    if (packet.isErr() || decoder->sendPacket(packet.unwrap()) != OM_SUCCESS) return false;
    auto result = decoder->receiveFrame();
    if (result.isErr()) return false;
    auto received = std::move(result).unwrap();
    if (received.status != ReceivedFrame::Status::Frame) return false;
    auto frame = toVideoFrame(received.frame, track.time_base);
    if (!frame) return false;
    video_renderer_.show(*frame);
    return true;
  }

  // ---- pipeline ---------------------------------------------------------------
  //
  //   demux ──► audio packets ──► audio decode ──► AudioSink ──► AVClock (master)
  //         └─► video packets ──► video decode ──► frames ──► VideoRenderer (main)

  void start() {
    finished_ = false;
    streams_running_ = int(bool(audio_)) + int(bool(video_));
    workers_.emplace_back([this](std::stop_token stop) { demuxLoop(stop); });
    if (audio_) workers_.emplace_back([this](std::stop_token stop) { audioLoop(stop); });
    if (video_) workers_.emplace_back([this](std::stop_token stop) { videoLoop(stop); });
  }

  // Stops and joins every worker, then drops whatever they left queued.
  void halt() {
    for (auto& worker : workers_) worker.request_stop();
    workers_.clear();
    audio_packets_.clear();
    video_packets_.clear();
    frames_.clear();
  }

  void demuxLoop(std::stop_token stop) {
    while (!stop.stop_requested()) {
      auto result = demuxer_->readPacket();
      if (result.isErr()) {
        const OMError err = std::move(result).unwrapErr();
        if (err != OM_FORMAT_END_OF_FILE && err != OM_IO_END_OF_STREAM)
          SDL_Log("[Demux] Read failed: %s (%d)", diag::describe(err), int(err));
        if (audio_) audio_packets_.push(Packet {}, stop);
        if (video_) video_packets_.push(Packet {}, stop);
        return;
      }

      Packet packet = result.unwrap();
      if (packet.stream_index == audio_.index)
        audio_packets_.push(std::move(packet), stop);
      else if (packet.stream_index == video_.index)
        video_packets_.push(std::move(packet), stop);
    }
  }

  void audioLoop(std::stop_token stop) {
    bool logged = false;
    const bool ended = media::decodeStream(*audio_.decoder, audio_packets_, stop, "Audio", [&](const Frame& frame) {
      const auto* samples = std::get_if<AudioSamples>(&frame.data);
      if (samples && samples->nb_samples > 0 && !std::exchange(logged, true))
        SDL_Log("[Audio] First frame: %u Hz, %u ch, %u samples, pts %.3fs", samples->format.sample_rate,
                samples->format.channels, samples->nb_samples, media::seconds(frame.pts, audio_.time_base));
      if (samples && samples->nb_samples > 0 && audio_sink_.configure(media::audioSpec(samples->format)))
        audio_sink_.push(media::interleavedPcm(*samples), media::seconds(frame.pts, audio_.time_base), stop);
      return true;
    });
    if (ended) {
      audio_sink_.finish();
      --streams_running_;
    }
  }

  void videoLoop(std::stop_token stop) {
    const bool ended = media::decodeStream(*video_.decoder, video_packets_, stop, "Video", [&](const Frame& frame) {
      auto vf = toVideoFrame(frame, video_.time_base);
      return !vf || frames_.push(std::move(*vf), stop);
    });
    if (ended) --streams_running_;
  }

  auto toVideoFrame(const Frame& frame, Rational time_base) -> std::optional<VideoFrame> {
    const auto* pic = std::get_if<Picture>(&frame.data);
    if (!pic || pic->width == 0 || pic->height == 0) return std::nullopt;
    colors_.report("decoded", *pic);
    frame_order_.report(frame.pts);
    VideoFrame vf;
    if (const auto* hw = std::get_if<std::shared_ptr<HardwarePicture>>(&pic->buffer)) {
      // This example renders through SDL, so hardware pictures come back to host memory.
      if (!*hw || !hw_.download(**hw, *pic, vf)) return std::nullopt;
    } else {
      vf = media::hostFrame(*pic);
    }
    vf.width = pic->width;
    vf.height = pic->height;
    vf.format = pic->format;
    vf.color_space = pic->color_space;
    vf.color_range = pic->color_range;
    vf.pts = media::seconds(int64_t(frame.pts), time_base);
    return vf;
  }

  void seekTo(double target) {
    halt();
    audio_sink_.clear();
    for (Stream* stream : {&audio_, &video_})
      if (*stream) stream->decoder->flush();

    frame_order_.reset();
    const auto started = SteadyClock::now();
    const OMError err = demuxer_->seek(-1, int64_t(target * 1e6));
    const double elapsed_ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - started).count();
    if (err != OM_SUCCESS)
      SDL_Log("[Player] Seek to %.2fs failed: %s", target, diag::describe(err));
    else
      SDL_Log("[Player] Seek to %.2fs took %.1f ms", target, elapsed_ms);
    clock_.reset(target);
    start();
  }

  void checkFinished() {
    if (finished_ || !isPlaying() || streams_running_ > 0) return;
    if (!frames_.empty() || !audio_sink_.drained()) return;
    finished_ = true;
    audio_sink_.pause();
    clock_.pause();
    SDL_Log("[Player] Finished");
  }

  SDL_Renderer* renderer_;
  FormatDetector detector_;
  CodecRegistry codecs_;
  FormatRegistry formats_;
  HwDevice hw_;
  std::string decoder_prefix_;
  diag::ColorReporter colors_;
  diag::FrameOrderReporter frame_order_;

  std::unique_ptr<Demuxer> demuxer_;
  Stream audio_;
  Stream video_;
  double duration_ = 0.0;

  AVClock clock_;
  AudioSink audio_sink_ {clock_};
  VideoRenderer video_renderer_ {renderer_};

  PacketQueue audio_packets_ = makePacketQueue();
  PacketQueue video_packets_ = makePacketQueue();
  FrameQueue frames_ {8};
  std::vector<std::jthread> workers_;
  std::atomic<int> streams_running_ = 0;

  float volume_ = 1.0f;
  bool finished_ = false;
  std::optional<PendingSeek> pending_seek_;
  std::string error_;
};
