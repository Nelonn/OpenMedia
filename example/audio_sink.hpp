#pragma once

#include "av_clock.hpp"
#include "ring_buffer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// AudioSink
//
// Owns one SDL audio device + SDL_AudioStream.  Pulls PCM from a RingBuffer
// on SDL's audio thread and advances the shared AVClock as samples are
// actually consumed by the device.
//
// Buffering model (hysteresis):
//   fill < LOW_THRESH  → worker told to produce more
//   fill > HIGH_THRESH → worker sleeps
//   fill > START_THRESH on first fill → playback unpaused
// ---------------------------------------------------------------------------
class AudioSink {
public:
  // Thresholds as fractions of the ring buffer capacity.
  static constexpr double kLowThresh = 0.20;
  static constexpr double kHighThresh = 0.80;
  static constexpr double kStartThresh = 0.30;

  ~AudioSink() { close(); }

  // -----------------------------------------------------------------------
// Open the device for a given PCM format.
// `clock` must outlive AudioSink.
// -----------------------------------------------------------------------
  bool open(SDL_AudioFormat sdl_fmt, int channels, int sample_rate,
            size_t bytes_per_sample, AVClock* clock) {
    close();

    clock_ = clock;
    sample_rate_ = sample_rate;
    channels_ = channels;
    bps_ = bytes_per_sample;
    frame_bytes_ = bps_ * static_cast<size_t>(channels);
#if defined(__APPLE__)
    ring_ = std::make_unique<RingBuffer>(
        static_cast<size_t>(sample_rate) * 2 * frame_bytes_);
#else
    queue_capacity_bytes_ =
        static_cast<size_t>(sample_rate) * 2 * frame_bytes_;
#endif

    SDL_AudioSpec spec {};
    spec.format = sdl_fmt;
    spec.channels = channels;
    spec.freq = sample_rate;

    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) {
      SDL_Log("[AudioSink] OpenAudioDevice: %s", SDL_GetError());
      return false;
    }

    stream_ = SDL_CreateAudioStream(&spec, &spec);
    if (!stream_) {
      SDL_Log("[AudioSink] CreateAudioStream: %s", SDL_GetError());
      close();
      return false;
    }

#if defined(__APPLE__)
    SDL_SetAudioStreamGetCallback(stream_, audioCallbackS, this);
#endif
    if (!SDL_BindAudioStream(device_, stream_)) {
      SDL_Log("[AudioSink] BindAudioStream: %s", SDL_GetError());
      close();
      return false;
    }

    SDL_SetAudioDeviceGain(device_, gain_);
    // Device starts paused until buffer is primed.
    SDL_PauseAudioDevice(device_);
    open_ = true;
    started_ = false;
    return true;
  }

  void close() {
    if (stream_) {
      SDL_ClearAudioStream(stream_);
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    if (device_) {
      SDL_CloseAudioDevice(device_);
      device_ = 0;
    }
    open_ = started_ = false;
  }

  // Push interleaved PCM bytes into the ring buffer.
  // Returns bytes written (may be partial if full).
  size_t pushPcm(const uint8_t* data, size_t len) {
#if defined(__APPLE__)
    if (!ring_ || !data || len == 0) return 0;
    return ring_->write(data, len);
#else
    if (!stream_ || !data || len == 0) return 0;

    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0) {
      SDL_Log("[AudioSink] GetAudioStreamQueued: %s", SDL_GetError());
      return 0;
    }

    const size_t queued_bytes = static_cast<size_t>(queued);
    const size_t writable = (queued_bytes < queue_capacity_bytes_)
        ? (queue_capacity_bytes_ - queued_bytes)
        : 0;
    const size_t to_write = std::min(len, writable);
    if (to_write == 0) return 0;

    if (!SDL_PutAudioStreamData(stream_, data, static_cast<int>(to_write))) {
      SDL_Log("[AudioSink] PutAudioStreamData: %s", SDL_GetError());
      return 0;
    }

    if (sample_rate_ > 0 && frame_bytes_ > 0) {
      const double secs_queued = static_cast<double>(to_write) /
                                 static_cast<double>(sample_rate_ * frame_bytes_);
      queued_seconds_ += secs_queued;
    }

    return to_write;
#endif
  }

  // Poll buffering state; returns true once playback has started.
  // Call this after pushing audio data.
  bool tickBuffering(double current_seconds) {
#if defined(__APPLE__)
    if (!open_ || started_ || !ring_) return false;
#else
    if (!open_ || started_) return false;
#endif
    const double ratio = fillRatio();
    if (ratio >= kStartThresh) {
#if defined(__APPLE__)
      clock_->setMode(AVClock::Mode::AUDIO);
#endif
      clock_->setAudioSeconds(current_seconds);
      SDL_ResumeAudioDevice(device_);
      started_ = true;
      SDL_Log("[AudioSink] Playback started (fill=%.1f%%)", ratio * 100.0);
    }
    return started_;
  }

  // Pause/resume for seek.
  void pause() {
    if (device_) SDL_PauseAudioDevice(device_);
  }
  void resume() {
    if (device_ && started_) SDL_ResumeAudioDevice(device_);
  }

  void clearBuffer() {
#if defined(__APPLE__)
    if (ring_) ring_->clear();
#else
    queued_seconds_ = 0.0;
#endif
    if (stream_) SDL_ClearAudioStream(stream_);
    started_ = false;
  }

  void setGain(float g) {
    gain_ = std::clamp(g, 0.0f, 1.5f);
    if (device_) SDL_SetAudioDeviceGain(device_, gain_);
  }

  float gain() const { return gain_; }
  bool isOpen() const { return open_; }
  bool started() const { return started_; }

  // True when worker should produce more audio data.
  bool needsData() const {
#if defined(__APPLE__)
    return !ring_ || ring_->fillRatio() < kHighThresh;
#else
    return !stream_ || fillRatio() < kHighThresh;
#endif
  }

  double fillRatio() const {
#if defined(__APPLE__)
    return ring_ ? ring_->fillRatio() : 0.0;
 #else
    if (!stream_ || queue_capacity_bytes_ == 0) return 0.0;
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued <= 0) return 0.0;
    return static_cast<double>(queued) /
           static_cast<double>(queue_capacity_bytes_);
#endif
  }

private:
#if defined(__APPLE__)
  static void SDLCALL audioCallbackS(void* userdata, SDL_AudioStream* stream,
                                     int additional_amount, int /*total_amount*/) {
    static_cast<AudioSink*>(userdata)->audioCallback(stream, additional_amount);
  }

  void audioCallback(SDL_AudioStream* stream, int additional_amount) {
    if (!ring_ || !clock_ || additional_amount <= 0) return;

    const size_t requested = static_cast<size_t>(additional_amount);
    const size_t available = ring_->currentSize();
    const size_t to_read = std::min(available, requested);

    if (to_read > 0) {
      tmp_buf_.resize(to_read);
      const size_t read = ring_->read(tmp_buf_.data(), to_read);
      if (read > 0 &&
          !SDL_PutAudioStreamData(stream, tmp_buf_.data(), static_cast<int>(read))) {
        SDL_Log("[AudioSink] PutAudioStreamData: %s", SDL_GetError());
        return;
      }

      if (sample_rate_ > 0 && frame_bytes_ > 0 && read > 0) {
        const double secs_consumed = static_cast<double>(read) /
                                     static_cast<double>(sample_rate_ * frame_bytes_);
        clock_->audioAdvance(secs_consumed);
      }
    }

    if (to_read < requested) {
      silence_buf_.assign(requested - to_read, 0);
      if (!silence_buf_.empty() &&
          !SDL_PutAudioStreamData(stream, silence_buf_.data(),
                                  static_cast<int>(silence_buf_.size()))) {
        SDL_Log("[AudioSink] PutAudioStreamData(silence): %s", SDL_GetError());
      }
    }
  }
#endif

  AVClock* clock_ = nullptr;
  SDL_AudioDeviceID device_ = 0;
  SDL_AudioStream* stream_ = nullptr;
#if defined(__APPLE__)
  std::unique_ptr<RingBuffer> ring_;
#endif

  int sample_rate_ = 0;
  int channels_ = 0;
  size_t bps_ = 0;         // bytes per sample
  size_t frame_bytes_ = 0; // bytes per interleaved PCM frame
#if !defined(__APPLE__)
  size_t queue_capacity_bytes_ = 0;
#endif

  float gain_ = 1.0f;
  bool open_ = false;
  bool started_ = false;
#if defined(__APPLE__)
  std::vector<uint8_t> tmp_buf_;
  std::vector<uint8_t> silence_buf_;
#else
  double queued_seconds_ = 0.0;
#endif
};
