#pragma once

#include <SDL3/SDL.h>

#include "av_clock.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

// SDL playback fed from a ring buffer, and the source of the master clock:
// every callback tells the clock which timestamp is audible right now.
//
// Playback starts paused and resumes once the buffer is primed, so a slow
// start does not begin with an underrun.
class AudioSink {
public:
  explicit AudioSink(AVClock& clock) : clock_(clock) {}
  ~AudioSink() { close(); }

  // (Re)opens the device whenever the stream format changes.
  auto configure(const SDL_AudioSpec& spec) -> bool {
    if (configured_ && sameSpec(spec, spec_)) return stream_ != nullptr;
    close();
    configured_ = true;
    spec_ = spec;
    frame_bytes_ = SDL_AUDIO_FRAMESIZE(spec);
    ring_.emplace(size_t(spec.freq) * frame_bytes_ * kBufferSeconds);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
    if (!stream_) {
      SDL_Log("[Audio] Cannot open %d Hz x%d: %s", spec.freq, spec.channels, SDL_GetError());
      return false;
    }
    SDL_SetAudioStreamGain(stream_, gain_);
    SDL_Log("[Audio] Opened %d Hz x%d", spec.freq, spec.channels);
    return true;
  }

  void close() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    configured_ = false;
    resetPosition();
  }

  // Queues PCM whose first sample plays at `pts`, blocking while the buffer is full.
  void push(std::span<const uint8_t> pcm, double pts, std::stop_token stop) {
    if (!stream_) return;
    if (!base_pts_) base_pts_ = pts;
    while (!stop.stop_requested()) {
      pcm = pcm.subspan(ring_->write(pcm));
      if (!started_ && ring_->fillRatio() >= kStartFill) start();
      if (pcm.empty()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // End of stream: whatever is buffered plays, even if it never primed.
  void finish() {
    if (stream_ && !started_ && ring_->size() > 0) start();
  }

  // Drops everything buffered (seek). Playback re-primes from the next push.
  void clear() {
    if (!stream_) return;
    SDL_PauseAudioStreamDevice(stream_);
    SDL_ClearAudioStream(stream_);
    ring_->clear();
    resetPosition();
  }

  void pause() {
    if (stream_) SDL_PauseAudioStreamDevice(stream_);
  }

  void setGain(float gain) {
    gain_ = gain;
    if (stream_) SDL_SetAudioStreamGain(stream_, gain_);
  }

  auto started() const -> bool { return started_; }
  auto drained() const -> bool { return !stream_ || ring_->size() == 0; }

private:
  static constexpr size_t kBufferSeconds = 2;
  static constexpr double kStartFill = 0.3;

  static auto sameSpec(const SDL_AudioSpec& a, const SDL_AudioSpec& b) -> bool {
    return a.format == b.format && a.channels == b.channels && a.freq == b.freq;
  }

  static void SDLCALL callback(void* self, SDL_AudioStream* stream, int need, int) {
    static_cast<AudioSink*>(self)->fill(stream, size_t(need));
  }

  // Runs on SDL's audio thread.
  void fill(SDL_AudioStream* stream, size_t need) {
    // An underrun is played as silence; the device still moves on, and so must
    // the clock, or video would wait forever for audio that is waiting on it.
    scratch_.resize(need);
    const size_t got = ring_->read(scratch_);
    std::fill(scratch_.begin() + got, scratch_.end(), uint8_t(SDL_GetSilenceValueForFormat(spec_.format)));
    SDL_PutAudioStreamData(stream, scratch_.data(), int(need));

    // Everything handed to SDL minus what it still holds is what has been played.
    output_frames_ += need / frame_bytes_;
    const uint64_t queued = uint64_t(std::max(0, SDL_GetAudioStreamQueued(stream))) / frame_bytes_;
    const uint64_t played = output_frames_ > queued ? output_frames_ - queued : 0;
    clock_.syncToAudio(*base_pts_ + double(played) / spec_.freq);
  }

  void start() {
    started_ = true;
    SDL_ResumeAudioStreamDevice(stream_);
  }

  void resetPosition() {
    base_pts_.reset();
    output_frames_ = 0;
    started_ = false;
  }

  AVClock& clock_;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioSpec spec_ {};
  bool configured_ = false;
  size_t frame_bytes_ = 0;
  float gain_ = 1.0f;
  std::optional<RingBuffer> ring_;

  // Written only while the device is paused, read by the callback.
  std::optional<double> base_pts_;
  uint64_t output_frames_ = 0;
  std::atomic<bool> started_ = false;

  std::vector<uint8_t> scratch_;
};
