#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <openmedia/audio.hpp>
#include <openmedia/codec_api.hpp>
#include <openmedia/codec_registry.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/format_detector.hpp>
#include <openmedia/format_registry.hpp>
#include <openmedia/io.hpp>
#include <queue>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace openmedia;
using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

static constexpr auto SEEK_SETTLE = std::chrono::milliseconds(120);
static constexpr size_t VIDEO_QUEUE_MAX = 8;
static constexpr double AV_SYNC_THRESHOLD = 0.05; // seconds

// Audio buffering thresholds
static constexpr double AUDIO_BUFFER_LOW_THRESHOLD = 0.25;   // 25% - start refilling
static constexpr double AUDIO_BUFFER_HIGH_THRESHOLD = 0.75;  // 75% - stop refilling
static constexpr double AUDIO_BUFFER_START_THRESHOLD = 0.30; // 30% - start playback

// Worker loop throttling
static constexpr auto WORKER_SLEEP_DURATION = std::chrono::milliseconds(5);
static constexpr auto WORKER_TIMEOUT = std::chrono::milliseconds(100);

static auto toSdlFormat(OMSampleFormat fmt) -> SDL_AudioFormat {
  switch (fmt) {
    case OM_SAMPLE_U8: return SDL_AUDIO_U8;
    case OM_SAMPLE_S16: return SDL_AUDIO_S16;
    case OM_SAMPLE_S32: return SDL_AUDIO_S32;
    case OM_SAMPLE_F32: return SDL_AUDIO_F32;
    default: return SDL_AUDIO_S16;
  }
}

static auto interleaveAudio(const AudioSamples& samples) -> std::vector<uint8_t> {
  const uint32_t channels = samples.format.channels;
  const uint32_t nb_samples = samples.nb_samples;
  const size_t bps = getBytesPerSample(samples.format.sample_format);
  const size_t frame_size = bps * channels;
  std::vector<uint8_t> out(static_cast<size_t>(nb_samples) * frame_size);
  if (samples.format.planar) {
    for (uint32_t ch = 0; ch < channels; ++ch) {
      const uint8_t* src = samples.planes.getData(ch);
      if (!src) continue;
      for (uint32_t i = 0; i < nb_samples; ++i)
        memcpy(out.data() + (i * channels + ch) * bps, src + i * bps, bps);
    }
  } else {
    const uint8_t* src = samples.planes.getData(0);
    if (src) memcpy(out.data(), src, out.size());
  }
  return out;
}

static auto unpackBits(std::vector<uint8_t> src, uint8_t bits) -> std::vector<uint8_t> {
  if (bits == 0 || bits == 32 || bits == 8) return src;
  if (bits == 24 || bits == 16) {
    const int32_t shift = 32 - bits;
    const size_t sample_count = src.size() / 4;
    std::vector<uint8_t> dst(src.size());
    for (size_t i = 0; i < sample_count; ++i) {
      int32_t s = 0;
      memcpy(&s, src.data() + i * 4, 4);
      s <<= shift;
      memcpy(dst.data() + i * 4, &s, 4);
    }
    return dst;
  }
  return src;
}

static auto formatTime(int64_t pts, Rational tb) -> std::string {
  if (pts < 0 || tb.den == 0) return "00:00";
  const int total_s = static_cast<int>(pts * tb.num / tb.den);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", total_s / 60, total_s % 60);
  return buf;
}

static auto buildPixels(const Picture& pic) -> std::vector<uint32_t> {
  std::vector<uint32_t> pixels(pic.width * pic.height);
  for (uint32_t y = 0; y < pic.height; ++y) {
    const uint8_t* src = pic.planes.getData(0) + y * pic.planes.getLinesize(0);
    uint32_t* dst = pixels.data() + y * pic.width;
    for (uint32_t x = 0; x < pic.width; ++x) {
      const uint8_t r = src[x * 4], g = src[x * 4 + 1],
                    b = src[x * 4 + 2], a = src[x * 4 + 3];
      dst[x] = (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(g) << 8) | r;
    }
  }
  return pixels;
}

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------
class RingBuffer {
public:
  explicit RingBuffer(size_t capacity)
      : buf_(capacity), capacity_(capacity) {}

  auto write(const uint8_t* data, size_t len) -> size_t {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(len, availableWriteUnsafe());
    for (size_t i = 0; i < n; ++i)
      buf_[(write_pos_ + i) % capacity_] = data[i];
    write_pos_ += n;
    return n;
  }

  auto read(uint8_t* dst, size_t len) -> size_t {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(len, currentSizeUnsafe());
    for (size_t i = 0; i < n; ++i)
      dst[i] = buf_[(read_pos_ + i) % capacity_];
    read_pos_ += n;
    return n;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    read_pos_ = write_pos_;
  }

  auto currentSize() const -> size_t {
    std::lock_guard lock(mutex_);
    return currentSizeUnsafe();
  }

  auto capacity() const -> size_t { return capacity_; }

  auto availableWrite() const -> size_t {
    std::lock_guard lock(mutex_);
    return availableWriteUnsafe();
  }

private:
  auto currentSizeUnsafe() const -> size_t { return write_pos_ - read_pos_; }
  auto availableWriteUnsafe() const -> size_t { return capacity_ - currentSizeUnsafe(); }

  std::vector<uint8_t> buf_;
  size_t capacity_;
  mutable std::mutex mutex_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
};

struct VideoFrame {
  std::vector<uint8_t> y_plane;
  std::vector<uint8_t> u_plane;
  std::vector<uint8_t> v_plane;
  int y_stride = 0;
  int u_stride = 0;
  int v_stride = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t pts = 0;
};

class VideoFrameQueue {
public:
  void push(VideoFrame frame) {
    std::unique_lock lock(mutex_);
    cond_not_full_.wait(lock, [&] { return queue_.size() < VIDEO_QUEUE_MAX || flush_; });
    if (flush_) return;
    queue_.push(std::move(frame));
    cond_not_empty_.notify_one();
  }

  auto pop(VideoFrame& out) -> bool {
    std::unique_lock lock(mutex_);
    if (!cond_not_empty_.wait_for(lock, std::chrono::milliseconds(20),
                                  [&] { return !queue_.empty() || flush_; })) {
      return false;
    }
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    cond_not_full_.notify_one();
    return true;
  }

  auto frontPts() -> std::optional<int64_t> {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    return queue_.front().pts;
  }

  void flush() {
    std::lock_guard lock(mutex_);
    flush_ = true;
    while (!queue_.empty()) queue_.pop();
    cond_not_full_.notify_all();
    cond_not_empty_.notify_all();
  }

  void resetFlush() {
    std::lock_guard lock(mutex_);
    flush_ = false;
  }

  auto size() -> size_t {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

private:
  std::queue<VideoFrame> queue_;
  std::mutex mutex_;
  std::condition_variable cond_not_full_;
  std::condition_variable cond_not_empty_;
  bool flush_ = false;
};

class MediaPlayer {
public:
  std::string current_file;

  MediaPlayer() {
    format_detector_.addAllStandard();
    registerBuiltInCodecs(&codec_registry_);
    registerBuiltInFormats(&format_registry_);
  }

  ~MediaPlayer() { stop(); }

  void setRenderer(SDL_Renderer* r) { renderer_ = r; }

  void stop() {
    stopWorker();
    closeAudioDevice();

    video_queue_.flush();

    if (demuxer_) {
      demuxer_->close();
      demuxer_.reset();
    }
    audio_decoder_.reset();
    video_decoder_.reset();
    ring_buffer_.reset();

    if (video_texture_) {
      SDL_DestroyTexture(video_texture_);
      video_texture_ = nullptr;
    }
    if (image_texture_) {
      SDL_DestroyTexture(image_texture_);
      image_texture_ = nullptr;
    }

    audio_ready_ = false;
    audio_started_ = false;
    has_image_ = false;
    has_video_ = false;
    seek_pending_ = false;
    current_file.clear();
    audio_stream_index_ = -1;
    video_stream_index_ = -1;
    image_stream_index_ = -1;
    current_pts_ = 0;
    audio_clock_ = 0;
    playback_start_time_ = Clock::now();
  }

  auto play(const std::string& path) -> bool {
    stop();
    path_ = path;

    auto input = InputStream::createFileStream(path);
    if (!input || !input->isValid()) {
      SDL_Log("Failed to open file: %s", path.c_str());
      return false;
    }

    uint8_t probe[2048];
    const size_t n = input->read(probe);
    const DetectedFormat fmt = format_detector_.detect({probe, n});
    if (fmt.isUnknown()) {
      SDL_Log("Unknown format: %s", path.c_str());
      return false;
    }
    input->seek(0, Whence::BEG);

    if (fmt.isContainer()) {
      if (const auto* desc = format_registry_.getFormat(fmt.container);
          desc && desc->isDemuxing())
        demuxer_ = desc->demuxer_factory();
    }
    if (!demuxer_) {
      SDL_Log("No demuxer for format %d", static_cast<int>(fmt.container));
      return false;
    }
    if (demuxer_->open(std::move(input)) != OM_SUCCESS) {
      SDL_Log("Demuxer open failed");
      return false;
    }

    onDemuxerOpen();
    return true;
  }

  void setVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.5f);
    if (audio_device_) SDL_SetAudioDeviceGain(audio_device_, volume_);
  }

  void seek(float progress) {
    if (!demuxer_ || total_duration_ <= 0) return;
    {
      std::lock_guard lock(seek_mutex_);
      pending_seek_progress_ = std::clamp(progress, 0.0f, 1.0f);
      seek_pending_ = true;
      last_seek_time_ = Clock::now();
    }
    seek_cv_.notify_one();
  }

  auto getVolume() const -> float { return volume_; }
  auto hasImage() const -> bool { return has_image_; }
  auto hasVideo() const -> bool { return has_video_; }
  auto isAudioPlaying() const -> bool { return audio_ready_; }
  auto isVideoPlaying() const -> bool { return has_video_; }

  auto getProgress() const -> float {
    if (total_duration_ <= 0) return 0.0f;
    return static_cast<float>(current_pts_.load()) / static_cast<float>(total_duration_);
  }

  auto getProgressString() const -> std::string {
    return formatTime(current_pts_, time_base_) + " / " +
           formatTime(total_duration_, time_base_);
  }

  auto getImageTexture() -> SDL_Texture* {
    std::lock_guard lock(image_mutex_);
    return image_texture_;
  }

  auto getImageSize() const -> std::pair<uint32_t, uint32_t> {
    return {image_width_, image_height_};
  }

  void tickVideo() {
    if (!has_video_ || !renderer_) return;

    // Use audio clock as master if available
    const int64_t master_pts = current_pts_.load();

    auto front_pts_opt = video_queue_.frontPts();
    if (!front_pts_opt) return;

    const double frame_pts_sec = static_cast<double>(*front_pts_opt) * time_base_.num / time_base_.den;
    const double clock_sec = static_cast<double>(master_pts) * time_base_.num / time_base_.den;
    const double diff = frame_pts_sec - clock_sec;

    // Too far in the future – wait
    if (diff > AV_SYNC_THRESHOLD) return;

    VideoFrame vf;
    if (!video_queue_.pop(vf)) return;

    // Update clock only if audio is not driving
    if (!audio_ready_) current_pts_.store(vf.pts);

    // Upload pixels to texture
    std::lock_guard lock(video_texture_mutex_);
    if (!video_texture_ ||
        video_texture_w_ != vf.width ||
        video_texture_h_ != vf.height) {
      if (video_texture_) SDL_DestroyTexture(video_texture_);
      video_texture_ = SDL_CreateTexture(renderer_,
                                         SDL_PIXELFORMAT_IYUV,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         static_cast<int>(vf.width),
                                         static_cast<int>(vf.height));
      video_texture_w_ = vf.width;
      video_texture_h_ = vf.height;
    }
    if (video_texture_) {
      SDL_UpdateYUVTexture(video_texture_, nullptr,
                           vf.y_plane.data(), vf.y_stride,
                           vf.u_plane.data(), vf.u_stride,
                           vf.v_plane.data(), vf.v_stride);
    }
  }

  auto getVideoTexture() -> SDL_Texture* {
    std::lock_guard lock(video_texture_mutex_);
    return video_texture_;
  }

  auto getVideoSize() const -> std::pair<uint32_t, uint32_t> {
    return {video_texture_w_, video_texture_h_};
  }

private:
  static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream,
                                    int additional_amount, int) {
    auto* self = static_cast<MediaPlayer*>(userdata);
    if (!self->ring_buffer_) return;

    const size_t available = self->ring_buffer_->currentSize();
    const size_t to_read = std::min(available, static_cast<size_t>(additional_amount));

    if (to_read > 0) {
      std::vector<uint8_t> tmp(to_read);
      const size_t actually_read = self->ring_buffer_->read(tmp.data(), to_read);
      SDL_PutAudioStreamData(stream, tmp.data(), static_cast<int>(actually_read));

      // Update audio clock based on bytes consumed
      if (self->audio_sample_rate_ > 0 && self->audio_frame_size_ > 0) {
        const int64_t samples_consumed = actually_read / self->audio_frame_size_;
        const double time_consumed = static_cast<double>(samples_consumed) / self->audio_sample_rate_;
        const int64_t pts_delta = static_cast<int64_t>(time_consumed * self->time_base_.den / self->time_base_.num);

        self->audio_clock_.fetch_add(pts_delta, std::memory_order_relaxed);
        self->current_pts_.store(self->audio_clock_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      }
    } else {
      // Buffer empty – send silence
      std::vector<uint8_t> silence(static_cast<size_t>(additional_amount), 0);
      SDL_PutAudioStreamData(stream, silence.data(), additional_amount);
    }
  }

  void onDemuxerOpen() {
    const auto& tracks = demuxer_->tracks();
    for (size_t i = 0; i < tracks.size(); ++i) {
      const auto& t = tracks[i];
      if (t.format.type == OM_MEDIA_AUDIO && audio_stream_index_ < 0)
        audio_stream_index_ = static_cast<int32_t>(i);
      else if (t.format.type == OM_MEDIA_VIDEO && !t.isImage() && video_stream_index_ < 0)
        video_stream_index_ = static_cast<int32_t>(i);
      else if (t.isImage() && image_stream_index_ < 0)
        image_stream_index_ = static_cast<int32_t>(i);
    }

    if (image_stream_index_ >= 0 && video_stream_index_ < 0 && audio_stream_index_ < 0) {
      setupImageDecoder(tracks[static_cast<size_t>(image_stream_index_)]);
      return;
    }
    if (video_stream_index_ >= 0)
      setupVideoDecoder(tracks[static_cast<size_t>(video_stream_index_)]);
    if (audio_stream_index_ >= 0)
      setupAudioDecoder(tracks[static_cast<size_t>(audio_stream_index_)]);

    if (video_stream_index_ >= 0 || audio_stream_index_ >= 0) {
      current_file = path_;
      startWorker();
    }
  }

  auto makeDecoder(const Track& track, std::unique_ptr<Decoder>& dec) -> bool {
    dec = codec_registry_.createDecoder(track.format.codec_id);
    if (!dec) {
      SDL_Log("No decoder for codec %d", static_cast<int>(track.format.codec_id));
      return false;
    }
    DecoderOptions opts;
    opts.format = track.format;
    opts.extradata = track.extradata;
    if (dec->configure(opts) != OM_SUCCESS) {
      SDL_Log("Decoder configure failed");
      return false;
    }
    return true;
  }

  void setupImageDecoder(const Track& track) {
    if (!makeDecoder(track, audio_decoder_)) return;
    image_width_ = track.format.image.width;
    image_height_ = track.format.image.height;
    total_duration_ = track.duration;
    time_base_ = track.time_base;
    decodeAndShowImage(audio_decoder_);
  }

  void setupAudioDecoder(const Track& track) {
    if (!makeDecoder(track, audio_decoder_)) return;
    if (video_stream_index_ < 0) {
      total_duration_ = track.duration;
      time_base_ = track.time_base;
    }
    current_pts_ = 0;
    audio_clock_ = 0;
    SDL_Log("Audio codec: %s", getCodecMeta(track.format.codec_id).name.data());
  }

  void setupVideoDecoder(const Track& track) {
    if (!makeDecoder(track, video_decoder_)) return;
    total_duration_ = track.duration;
    time_base_ = track.time_base;
    video_width_ = track.format.image.width;
    video_height_ = track.format.image.height;
    has_video_ = true;
    SDL_Log("Video: %dx%d, codec: %s",
            video_width_, video_height_,
            getCodecMeta(track.format.codec_id).name.data());
  }

  auto openAudioDevice(const AudioSamples& samples) -> bool {
    SDL_AudioSpec spec {};
    spec.format = toSdlFormat(samples.format.sample_format);
    spec.channels = static_cast<int>(samples.format.channels);
    spec.freq = static_cast<int>(samples.format.sample_rate);

    const size_t bps = getBytesPerSample(samples.format.sample_format);
    const size_t frame_size = bps * static_cast<size_t>(spec.channels);

    // Store for audio clock calculation
    audio_sample_rate_ = spec.freq;
    audio_frame_size_ = frame_size;

    ring_buffer_ = std::make_unique<RingBuffer>(static_cast<size_t>(spec.freq) * 2 * frame_size);

    audio_device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!audio_device_) {
      SDL_Log("Audio device open failed: %s", SDL_GetError());
      return false;
    }

    audio_stream_ = SDL_CreateAudioStream(&spec, &spec);
    if (!audio_stream_) {
      SDL_Log("Audio stream create failed: %s", SDL_GetError());
      return false;
    }

    SDL_SetAudioStreamGetCallback(audio_stream_, audioCallback, this);
    if (!SDL_BindAudioStream(audio_device_, audio_stream_)) {
      SDL_Log("Audio stream bind failed: %s", SDL_GetError());
      return false;
    }
    SDL_SetAudioDeviceGain(audio_device_, volume_);

    audio_ready_ = true;
    audio_started_ = false;
    SDL_Log("Audio: %d Hz, %d ch (buffering...)", spec.freq, spec.channels);
    return true;
  }

  void closeAudioDevice() {
    if (audio_stream_) {
      SDL_DestroyAudioStream(audio_stream_);
      audio_stream_ = nullptr;
    }
    if (audio_device_) {
      SDL_CloseAudioDevice(audio_device_);
      audio_device_ = 0;
    }
    audio_ready_ = false;
    audio_started_ = false;
  }

  void startWorker() {
    video_queue_.resetFlush();
    stop_requested_ = false;
    worker_ = std::thread([this] { workerLoop(); });
  }

  void stopWorker() {
    {
      std::lock_guard lock(seek_mutex_);
      stop_requested_ = true;
    }
    seek_cv_.notify_one();
    video_queue_.flush();
    if (worker_.joinable()) worker_.join();
  }

  void flushSeek(float progress) {
    if (audio_device_) SDL_PauseAudioDevice(audio_device_);
    if (ring_buffer_) ring_buffer_->clear();
    if (audio_decoder_) audio_decoder_->flush();
    if (video_decoder_) video_decoder_->flush();
    video_queue_.flush();
    video_queue_.resetFlush();
    audio_started_ = false;

    const int32_t ref_stream = (audio_stream_index_ >= 0) ? audio_stream_index_ : video_stream_index_;
    const auto target_pts = static_cast<int64_t>(progress * static_cast<float>(total_duration_));
    int64_t target_ns = 0;
    if (time_base_.den != 0)
      target_ns = static_cast<int64_t>(
          static_cast<double>(target_pts) * time_base_.num / time_base_.den * 1'000'000'000.0);

    if (demuxer_->seek(target_ns, ref_stream) == OM_SUCCESS) {
      current_pts_ = target_pts;
      audio_clock_ = target_pts;
    }

    // Prime with a few packets
    for (int i = 0; i < 10 && !stop_requested_; ++i) {
      auto res = demuxer_->readPacket();
      if (res.isErr()) break;
      processPacket(res.unwrap(), false);
    }
  }

  void workerLoop() {
    while (true) {
      {
        std::unique_lock lock(seek_mutex_);

        // Wait with timeout for better responsiveness
        const auto wait_result = seek_cv_.wait_for(lock, WORKER_TIMEOUT, [&] {
          if (stop_requested_.load()) return true;
          if (seek_pending_ && (Clock::now() - last_seek_time_) >= SEEK_SETTLE) return true;
          if (seek_pending_) return false;

          // Check if we need to decode more
          const bool audio_needs_data = !ring_buffer_ ||
              ring_buffer_->currentSize() < static_cast<size_t>(ring_buffer_->capacity() * AUDIO_BUFFER_HIGH_THRESHOLD);
          const bool video_needs_data = video_queue_.size() < VIDEO_QUEUE_MAX * 3 / 4;

          return audio_needs_data || video_needs_data;
        });

        if (stop_requested_) return;

        if (seek_pending_) {
          const float progress = pending_seek_progress_;
          seek_pending_ = false;
          lock.unlock();
          flushSeek(progress);
          continue;
        }

        // If we hit timeout without being woken, sleep a bit to avoid busy-wait
        if (!wait_result) {
          lock.unlock();
          std::this_thread::sleep_for(WORKER_SLEEP_DURATION);
          continue;
        }
      }

      auto res = demuxer_->readPacket();
      if (res.isErr()) {
        SDL_Log("Demuxer EOF/error: %d", res.unwrapErr());
        break;
      }
      processPacket(res.unwrap(), true);

      // Small sleep to prevent tight loop on Linux
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

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
      const AudioSamples& samples = std::get<AudioSamples>(frame.data);
      if (samples.nb_samples == 0) continue;

      if (!audio_ready_) {
        if (!allow_open_device || !openAudioDevice(samples)) return;
      }

      auto audio_data = unpackBits(interleaveAudio(samples), samples.format.bits_per_sample);

      // Write to ring buffer - may block if full
      size_t written = 0;
      while (written < audio_data.size() && !stop_requested_) {
        const size_t chunk = ring_buffer_->write(audio_data.data() + written,
                                                  audio_data.size() - written);
        written += chunk;
        if (written < audio_data.size()) {
          // Buffer full, wait a bit
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }

      // Start playback after initial buffering
      if (audio_device_ && !audio_started_) {
        const size_t filled = ring_buffer_->currentSize();
        const size_t threshold = static_cast<size_t>(ring_buffer_->capacity() * AUDIO_BUFFER_START_THRESHOLD);
        if (filled >= threshold) {
          playback_start_time_ = Clock::now();
          audio_clock_ = static_cast<int64_t>(frame.pts);
          current_pts_ = audio_clock_.load();
          SDL_ResumeAudioDevice(audio_device_);
          audio_started_ = true;
          SDL_Log("Playback started (buffer: %zu/%zu bytes, PTS: %lld)",
                  filled, ring_buffer_->capacity(), static_cast<long long>(frame.pts));
        }
      }
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
      vf.width = pic.width;
      vf.height = pic.height;
      vf.pts = static_cast<int64_t>(frame.pts);

      const int y_stride = pic.planes.getLinesize(0);
      const int u_stride = pic.planes.getLinesize(1);
      const int v_stride = pic.planes.getLinesize(2);
      const uint8_t* y_src = pic.planes.getData(0);
      const uint8_t* u_src = pic.planes.getData(1);
      const uint8_t* v_src = pic.planes.getData(2);

      vf.y_stride = y_stride;
      vf.u_stride = u_stride;
      vf.v_stride = v_stride;

      vf.y_plane.assign(y_src, y_src + y_stride * pic.height);
      vf.u_plane.assign(u_src, u_src + u_stride * (pic.height / 2));
      vf.v_plane.assign(v_src, v_src + v_stride * (pic.height / 2));

      if (!audio_ready_) current_pts_.store(vf.pts);
      video_queue_.push(std::move(vf));
    }
  }

  void decodeAndShowImage(std::unique_ptr<Decoder>& dec) {
    if (!dec || !renderer_) return;
    auto res = demuxer_->readPacket();
    if (res.isErr()) {
      SDL_Log("Failed to read image packet: %d", res.unwrapErr());
      return;
    }
    auto result = dec->decode(res.unwrap());
    if (result.isErr() || result.unwrap().empty()) {
      SDL_Log("Failed to decode image");
      return;
    }
    Frame& frame = result.unwrap()[0];
    if (!std::holds_alternative<Picture>(frame.data)) {
      SDL_Log("Frame is not a picture");
      return;
    }
    const Picture& pic = std::get<Picture>(frame.data);
    const auto pixels = buildPixels(pic);

    std::lock_guard lock(image_mutex_);
    if (image_texture_) {
      SDL_DestroyTexture(image_texture_);
      image_texture_ = nullptr;
    }
    image_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC,
                                       static_cast<int>(pic.width),
                                       static_cast<int>(pic.height));
    if (!image_texture_) {
      SDL_Log("Texture create failed: %s", SDL_GetError());
      return;
    }
    SDL_UpdateTexture(image_texture_, nullptr, pixels.data(),
                      static_cast<int>(pic.width * sizeof(uint32_t)));
    image_width_ = pic.width;
    image_height_ = pic.height;
    has_image_ = true;
    current_file = path_;
    SDL_Log("Image loaded: %s (%dx%d)", current_file.c_str(), image_width_, image_height_);
  }

  FormatDetector format_detector_;
  CodecRegistry codec_registry_;
  FormatRegistry format_registry_;

  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<Decoder> audio_decoder_;
  std::unique_ptr<Decoder> video_decoder_;
  std::string path_;

  int32_t audio_stream_index_ = -1;
  int32_t video_stream_index_ = -1;
  int32_t image_stream_index_ = -1;

  // Audio
  SDL_AudioDeviceID audio_device_ = 0;
  SDL_AudioStream* audio_stream_ = nullptr;
  std::unique_ptr<RingBuffer> ring_buffer_;
  std::atomic<bool> audio_ready_ {false};
  std::atomic<bool> audio_started_ {false};
  float volume_ = 1.0f;
  int audio_sample_rate_ = 0;
  size_t audio_frame_size_ = 0;

  // Clock / progress
  int64_t total_duration_ = 0;
  std::atomic<int64_t> current_pts_ {0};
  std::atomic<int64_t> audio_clock_ {0};
  Rational time_base_ {1, 1};
  TimePoint playback_start_time_;

  // Video
  VideoFrameQueue video_queue_;
  std::atomic<bool> has_video_ {false};
  SDL_Texture* video_texture_ = nullptr;
  std::mutex video_texture_mutex_;
  uint32_t video_texture_w_ = 0;
  uint32_t video_texture_h_ = 0;
  uint32_t video_width_ = 0;
  uint32_t video_height_ = 0;

  // Still image
  SDL_Texture* image_texture_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::mutex image_mutex_;
  std::atomic<bool> has_image_ {false};
  uint32_t image_width_ = 0;
  uint32_t image_height_ = 0;

  // Worker
  std::thread worker_;
  std::atomic<bool> stop_requested_ {false};
  std::mutex seek_mutex_;
  std::condition_variable seek_cv_;
  bool seek_pending_ = false;
  float pending_seek_progress_ = 0.0f;
  TimePoint last_seek_time_;
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  (void) argc;
  (void) argv;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;

  SDL_Window* window = SDL_CreateWindow("OpenMedia Player", 800, 600, SDL_WINDOW_RESIZABLE);
  if (!window) return 1;
  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) return 1;

  MediaPlayer player;
  player.setRenderer(renderer);

  constexpr float BAR_H = 8.0f;
  constexpr float BAR_MARGIN_X = 20.0f;
  constexpr float BAR_BOTTOM = 20.0f;
  constexpr float HIT_EXPAND = 12.0f;

  const auto getBarRect = [&](int w, int h) -> SDL_FRect {
    return {BAR_MARGIN_X,
            static_cast<float>(h) - BAR_BOTTOM - BAR_H,
            static_cast<float>(w) - BAR_MARGIN_X * 2,
            BAR_H};
  };
  const auto progressFromMouse = [](float mx, const SDL_FRect& bar) -> float {
    return std::clamp((mx - bar.x) / bar.w, 0.0f, 1.0f);
  };

  bool dragging = false;
  bool mouse_near_bar = false;
  bool running = true;

  while (running) {
    int win_w = 800, win_h = 600;
    SDL_GetWindowSize(window, &win_w, &win_h);
    const SDL_FRect bar = getBarRect(win_w, win_h);

    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const bool has_progress = !player.current_file.empty() &&
                              (player.isAudioPlaying() || player.isVideoPlaying());
    mouse_near_bar = has_progress &&
                     mx >= bar.x && mx <= bar.x + bar.w &&
                     my >= bar.y - HIT_EXPAND && my <= bar.y + bar.h + HIT_EXPAND;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_DROP_FILE: player.play(event.drop.data); break;
        case SDL_EVENT_MOUSE_WHEEL: {
          constexpr float STEP = 5.0f / 100.0f;
          player.setVolume(player.getVolume() + (event.wheel.y > 0 ? STEP : -STEP));
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (event.button.button == SDL_BUTTON_LEFT && mouse_near_bar) {
            dragging = true;
            player.seek(progressFromMouse(event.button.x, bar));
          }
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (event.button.button == SDL_BUTTON_LEFT && dragging) {
            dragging = false;
            player.seek(progressFromMouse(event.button.x, bar));
          }
          break;
        case SDL_EVENT_MOUSE_MOTION:
          if (dragging) player.seek(progressFromMouse(event.motion.x, bar));
          break;
        default: break;
      }
    }

    player.tickVideo();

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (player.hasVideo()) {
      if (SDL_Texture* tex = player.getVideoTexture()) {
        auto [vid_w, vid_h] = player.getVideoSize();
        if (vid_w > 0 && vid_h > 0) {
          const float scale = std::min(static_cast<float>(win_w) / vid_w,
                                       static_cast<float>(win_h - 60) / vid_h);
          const SDL_FRect dst {
              static_cast<float>((win_w - static_cast<int>(vid_w * scale)) / 2),
              static_cast<float>((win_h - 60 - static_cast<int>(vid_h * scale)) / 2),
              vid_w * scale, vid_h * scale};
          SDL_RenderTexture(renderer, tex, nullptr, &dst);
        }
      }
    }
    else if (player.hasImage()) {
      if (SDL_Texture* tex = player.getImageTexture()) {
        auto [img_w, img_h] = player.getImageSize();
        if (img_w > 0 && img_h > 0) {
          const float scale = std::min(static_cast<float>(win_w - 40) / img_w,
                                       static_cast<float>(win_h - 100) / img_h);
          const SDL_FRect dst {
              static_cast<float>((win_w - static_cast<int>(img_w * scale)) / 2),
              static_cast<float>((win_h - static_cast<int>(img_h * scale) - 40) / 2),
              img_w * scale, img_h * scale};
          SDL_RenderTexture(renderer, tex, nullptr, &dst);
        }
      }
    }

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20, 20,
                        player.current_file.empty() ? "Drop a video, audio, or image file here"
                                                    : player.current_file.c_str());

    char vol_buf[32];
    snprintf(vol_buf, sizeof(vol_buf), "Volume: %d%%", static_cast<int>(player.getVolume() * 100));
    SDL_RenderDebugText(renderer, 20, 40, vol_buf);

    if (!player.current_file.empty()) {
      if (player.hasImage()) {
        auto [img_w, img_h] = player.getImageSize();
        char info[64];
        snprintf(info, sizeof(info), "Image: %dx%d", img_w, img_h);
        SDL_RenderDebugText(renderer, 20, 60, info);
      } else if (player.hasVideo() || player.isAudioPlaying()) {
        SDL_RenderDebugText(renderer, 20, 60,
                            player.hasVideo() ? "Status: Video" : "Status: Audio");
        SDL_RenderDebugText(renderer, 20, 80, player.getProgressString().c_str());

        const float progress = player.getProgress();
        const SDL_FRect border {bar.x - 2, bar.y - 2, bar.w + 4, bar.h + 4};
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        SDL_RenderRect(renderer, &border);
        SDL_SetRenderDrawColor(renderer, 35, 35, 35, 255);
        SDL_RenderFillRect(renderer, &bar);
        if (progress > 0.0f) {
          const SDL_FRect fill {bar.x, bar.y, bar.w * progress, bar.h};
          SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
          SDL_RenderFillRect(renderer, &fill);
        }
      }
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(8);
  }

  player.stop();
  SDL_Quit();
  return 0;
}
