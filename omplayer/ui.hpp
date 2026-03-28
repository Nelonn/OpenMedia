#pragma once
#include "media_player.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

auto formatTime(double seconds) -> std::string {
    if (seconds < 0) return "00:00";
    const int total_s = static_cast<int>(seconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total_s / 60, total_s % 60);
    return buf;
}

}

// ---------------------------------------------------------------------------
// PlayerUI
//
// Owns all on-screen drawing logic.  main.cpp calls:
//   ui.handleEvent(event)   – input routing
//   ui.render(renderer)     – draw everything
//
// Deliberately keeps no media state; it only calls into MediaPlayer.
// ---------------------------------------------------------------------------
class PlayerUI {
public:
  // Geometry constants
  static constexpr float kBarH = 8.0f;
  static constexpr float kBarMarginX = 20.0f;
  static constexpr float kBarBottom = 20.0f;
  static constexpr float kBarHeight = 60.0f;  // mpv-style bottom bar height
  static constexpr float kHitExpand = 14.0f;
  static constexpr float kVolumeStep = 0.05f;
  static constexpr Uint32 kFadeDelayMs = 1000;  // 1 second idle timeout
  static constexpr float kTimeWidth = 55.0f;  // Space for time text on each side

  explicit PlayerUI(MediaPlayer& player)
      : player_(player) {
    last_mouse_time_ = SDL_GetTicks();
  }

  // -----------------------------------------------------------------------
  // Input
  // Returns false when the application should quit.
  // -----------------------------------------------------------------------
  auto handleEvent(const SDL_Event& e) -> bool {
    switch (e.type) {
      case SDL_EVENT_QUIT:
        return false;

      case SDL_EVENT_DROP_FILE:
        player_.play(e.drop.data);
        break;

      case SDL_EVENT_MOUSE_WHEEL: {
        const float delta = (e.wheel.y > 0 ? kVolumeStep : -kVolumeStep);
        player_.setVolume(player_.getVolume() + delta);
        break;
      }

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
          if (isNearBar(e.button.x, e.button.y)) {
            dragging_ = true;
            player_.seek(progressFromX(e.button.x));
          }
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT && dragging_) {
          dragging_ = false;
          player_.seek(progressFromX(e.button.x));
        }
        break;

      case SDL_EVENT_MOUSE_MOTION:
        mouse_x_ = e.motion.x;
        mouse_y_ = e.motion.y;
        last_mouse_time_ = SDL_GetTicks();
        mouse_in_window_ = true;
        if (dragging_)
          player_.seek(progressFromX(e.motion.x));
        break;

      case SDL_EVENT_WINDOW_MOUSE_ENTER:
        mouse_in_window_ = true;
        last_mouse_time_ = SDL_GetTicks();
        break;

      case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        mouse_in_window_ = false;
        break;

      default: break;
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // Render – call after SDL_RenderClear, before SDL_RenderPresent.
  // -----------------------------------------------------------------------
  void render(SDL_Renderer* r, SDL_Window* window) {
    int win_w = 800, win_h = 600;
    SDL_GetRenderOutputSize(r, &win_w, &win_h);
    updateBarCache(win_w, win_h);
    updateFadeState();
    updateWindowAspectRatio(window, win_w, win_h);

    drawMedia(r, win_w, win_h);
    if (player_.isActive()) {
      drawBottomBar(r, win_w, win_h);
    }
  }

private:
  // -----------------------------------------------------------------------
  // Media drawing
  // -----------------------------------------------------------------------

  void drawMedia(SDL_Renderer* r, int win_w, int win_h) const {
    if (player_.hasVideo()) {
      SDL_Texture* tex = player_.getVideoTexture();
      if (!tex) return;
      auto [vw, vh] = player_.getVideoSize();
      if (vw == 0 || vh == 0) return;

      // Fill entire window, bar will overlay
      const float scale = std::min(
          float(win_w) / float(vw),
          float(win_h) / float(vh));

      const SDL_FRect dst {
          float((win_w - int(vw * scale)) / 2),
          float((win_h - int(vh * scale)) / 2),
          vw * scale, vh * scale};
      SDL_RenderTexture(r, tex, nullptr, &dst);

    } else if (player_.hasImage()) {
      SDL_Texture* tex = player_.getImageTexture();
      if (!tex) return;
      auto [iw, ih] = player_.getImageSize();
      if (iw == 0 || ih == 0) return;

      // Fill entire window, bar will overlay
      const float scale = std::min(
          float(win_w) / float(iw),
          float(win_h) / float(ih));

      const SDL_FRect dst {
          float((win_w - int(iw * scale)) / 2),
          float((win_h - int(ih * scale)) / 2),
          iw * scale, ih * scale};
      SDL_RenderTexture(r, tex, nullptr, &dst);
    }
  }

  // -----------------------------------------------------------------------
  // Bottom bar (mpv-style)
  // -----------------------------------------------------------------------

  void drawBottomBar(SDL_Renderer* r, int win_w, int win_h) const {
    // Calculate alpha based on fade state
    Uint8 alpha = shouldShowBar() ? 255 : static_cast<Uint8>(255 * fade_alpha_);
    if (alpha == 0) return;

    // Enable alpha blending for transparent background
    SDL_BlendMode prev_blend;
    SDL_GetRenderDrawBlendMode(r, &prev_blend);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    // Draw background rectangle with fixed 50% transparency
    const SDL_FRect bg_rect {
        0.0f,
        float(win_h) - kBarHeight,
        float(win_w),
        kBarHeight};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 128);  // 50% transparent black
    SDL_RenderFillRect(r, &bg_rect);

    // Draw progress bar and time info with fade alpha
    drawProgressBar(r, win_w, win_h, alpha);
    drawTimeInfo(r, win_w, win_h, alpha);

    // Restore previous blend mode
    SDL_SetRenderDrawBlendMode(r, prev_blend);
  }

  void drawProgressBar(SDL_Renderer* r, int win_w, int /*win_h*/, Uint8 alpha) const {
    // Progress bar with space for time text on both sides
    const float bar_x = bar_cache_.x + kTimeWidth;
    const float bar_w = bar_cache_.w - kTimeWidth * 2.0f;
    
    const float progress = player_.getProgress();

    // Track background
    SDL_FRect track {bar_x, bar_cache_.y, bar_w, bar_cache_.h};
    SDL_SetRenderDrawColor(r, 80, 80, 80, alpha);
    SDL_RenderFillRect(r, &track);

    // Filled portion
    if (progress > 0.0f) {
      SDL_FRect fill {bar_x, bar_cache_.y, bar_w * progress, bar_cache_.h};
      SDL_SetRenderDrawColor(r, 255, 255, 255, alpha);
      SDL_RenderFillRect(r, &fill);
    }

    // Playhead dot when hovered or dragging
    const bool hovered = dragging_ || isNearBar(mouse_x_, mouse_y_);
    if (hovered && progress > 0.0f && progress < 1.0f) {
      const float cx = bar_x + bar_w * progress;
      const float cy = bar_cache_.y + bar_cache_.h * 0.5f;
      const float r2 = 6.0f;
      SDL_FRect dot {cx - r2, cy - r2, r2 * 2, r2 * 2};
      SDL_SetRenderDrawColor(r, 255, 255, 255, alpha);
      SDL_RenderFillRect(r, &dot);
    }
  }

  void drawTimeInfo(SDL_Renderer* r, int win_w, int /*win_h*/, Uint8 alpha) const {
    SDL_SetRenderDrawColor(r, 255, 255, 255, alpha);

    // Position text on the same line as the progress bar
    const float text_y = bar_cache_.y + 1.0f;

    // Current time on the LEFT
    std::string current_time = formatTime(player_.getClockSeconds());
    SDL_RenderDebugText(r, bar_cache_.x, text_y, current_time.c_str());

    // Total time on the RIGHT
    std::string total_time = formatTime(player_.getDurationSeconds());
    SDL_RenderDebugText(r, bar_cache_.x + bar_cache_.w - kTimeWidth, text_y, total_time.c_str());
  }

  // -----------------------------------------------------------------------
  // Fade logic
  // -----------------------------------------------------------------------

  void updateFadeState() {
    Uint32 now = SDL_GetTicks();

    // If mouse is outside window, start fading immediately
    if (!mouse_in_window_) {
      Uint32 elapsed = now - last_mouse_time_;
      if (elapsed >= kFadeDelayMs) {
        fade_alpha_ = 0.0f;
      } else {
        fade_alpha_ = 1.0f - (static_cast<float>(elapsed) / static_cast<float>(kFadeDelayMs));
      }
      return;
    }

    // Mouse is inside window - fade based on idle time
    Uint32 elapsed = now - last_mouse_time_;
    if (elapsed >= kFadeDelayMs) {
      fade_alpha_ = 0.0f;
    } else {
      fade_alpha_ = 1.0f - (static_cast<float>(elapsed) / static_cast<float>(kFadeDelayMs));
    }
  }

  auto shouldShowBar() const -> bool {
    if (!player_.isActive()) return false;
    // Show bar if mouse is in bottom half
    return mouse_in_bottom_half_;
  }

  auto isNearBar(float x, float y) const -> bool {
    if (!player_.isActive()) return false;
    // Account for time text space on both sides
    const float bar_x = bar_cache_.x + kTimeWidth;
    const float bar_w = bar_cache_.w - kTimeWidth * 2.0f;
    return x >= bar_x &&
           x <= bar_x + bar_w &&
           y >= bar_cache_.y - kHitExpand &&
           y <= bar_cache_.y + bar_cache_.h + kHitExpand;
  }

  auto progressFromX(float x) const -> float {
    const float bar_x = bar_cache_.x + kTimeWidth;
    const float bar_w = bar_cache_.w - kTimeWidth * 2.0f;
    return std::clamp((x - bar_x) / bar_w, 0.0f, 1.0f);
  }

  // -----------------------------------------------------------------------
  // Bar geometry helpers
  // -----------------------------------------------------------------------

  void updateBarCache(int win_w, int win_h) {
    bar_cache_ = {
        kBarMarginX,
        float(win_h) - kBarBottom - kBarH,
        float(win_w) - kBarMarginX * 2.f,
        kBarH};

    // Check if mouse is in bottom half
    mouse_in_bottom_half_ = (mouse_y_ > win_h / 2.0f);
  }

  void updateWindowAspectRatio(SDL_Window* window, int /*win_w*/, int /*win_h*/) const {
    if (!player_.isActive()) return;

    float aspect = 1.0f;  // Default 1:1 for audio-only

    if (player_.hasVideo()) {
      auto [vw, vh] = player_.getVideoSize();
      if (vw > 0 && vh > 0) {
        aspect = static_cast<float>(vw) / static_cast<float>(vh);
      }
    } else if (player_.hasImage()) {
      auto [iw, ih] = player_.getImageSize();
      if (iw > 0 && ih > 0) {
        aspect = static_cast<float>(iw) / static_cast<float>(ih);
      }
    }

    // Only update if aspect ratio changed
    if (aspect != last_aspect_) {
      SDL_SetWindowAspectRatio(window, aspect, aspect);
      last_aspect_ = aspect;
    }
  }

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------
  MediaPlayer& player_;
  SDL_FRect bar_cache_ {};
  float mouse_x_ = 0;
  float mouse_y_ = 0;
  bool dragging_ = false;
  Uint32 last_mouse_time_ = 0;
  float fade_alpha_ = 1.0f;
  bool mouse_in_bottom_half_ = false;
  bool mouse_in_window_ = true;
  mutable float last_aspect_ = -1.0f;
};
