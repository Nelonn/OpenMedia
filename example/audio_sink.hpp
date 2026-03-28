#pragma once

#include "av_clock.hpp"
#include "ring_buffer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// AudioSink
//
// Owns one SDL audio device + SDL_AudioStream.  Pulls PCM from a RingBuffer
// on the audio callback thread and advances the shared AVClock.
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

    // 2-second ring buffer
    ring_ = std::make_unique<RingBuffer>(
        static_cast<size_t>(sample_rate) * 2 * frame_bytes_);

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

    SDL_SetAudioStreamGetCallback(stream_, audioCallbackS, this);
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
    if (!ring_) return 0;
    return ring_->write(data, len);
  }

  // Poll buffering state; returns true once playback has started.
  // Call this after pushing audio data.
  bool tickBuffering(double current_seconds) {
    if (!open_ || started_ || !ring_) return false;
    const double ratio = ring_->fillRatio();
    if (ratio >= kStartThresh) {
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
    if (ring_) ring_->clear();
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
    return !ring_ || ring_->fillRatio() < kHighThresh;
  }

  double fillRatio() const {
    return ring_ ? ring_->fillRatio() : 0.0;
  }

private:
  static void SDLCALL audioCallbackS(void* userdata, SDL_AudioStream* stream,
                                     int need, int /*total*/) {
    static_cast<AudioSink*>(userdata)->audioCallback(stream, need);
  }

  void audioCallback(SDL_AudioStream* stream, int need) {
    if (!ring_ || !clock_) return;

    const size_t available = ring_->currentSize();
    const size_t to_read = std::min(available, static_cast<size_t>(need));

    if (to_read > 0) {
      tmp_buf_.resize(to_read);
      const size_t n = ring_->read(tmp_buf_.data(), to_read);
      SDL_PutAudioStreamData(stream, tmp_buf_.data(), static_cast<int>(n));

      // Advance the master clock by the number of PCM frames consumed.
      if (sample_rate_ > 0 && frame_bytes_ > 0) {
        const double secs_consumed = static_cast<double>(n) / (sample_rate_ * frame_bytes_);
        clock_->audioAdvance(secs_consumed);
      }
    } else {
      // Underrun – push silence to avoid SDL starvation.
      silence_buf_.assign(static_cast<size_t>(need), 0);
      SDL_PutAudioStreamData(stream, silence_buf_.data(), need);
    }
  }

  AVClock* clock_ = nullptr;
  SDL_AudioDeviceID device_ = 0;
  SDL_AudioStream* stream_ = nullptr;
  std::unique_ptr<RingBuffer> ring_;

  int sample_rate_ = 0;
  int channels_ = 0;
  size_t bps_ = 0;         // bytes per sample
  size_t frame_bytes_ = 0; // bytes per interleaved PCM frame

  float gain_ = 1.0f;
  bool open_ = false;
  bool started_ = false;

  std::vector<uint8_t> tmp_buf_;
  std::vector<uint8_t> silence_buf_;
};
