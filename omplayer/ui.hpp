#pragma once
#include "media_player.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>

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
  static constexpr float kHitExpand = 14.0f;
  static constexpr float kVolumeStep = 0.05f;

  explicit PlayerUI(MediaPlayer& player)
      : player_(player) {}

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
          updateBarCache();
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
        if (dragging_)
          player_.seek(progressFromX(e.motion.x));
        break;

      default: break;
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // Render – call after SDL_RenderClear, before SDL_RenderPresent.
  // -----------------------------------------------------------------------
  void render(SDL_Renderer* r) {
    int win_w = 800, win_h = 600;
    SDL_GetRenderOutputSize(r, &win_w, &win_h);
    updateBarCache(win_w, win_h);

    drawMedia(r, win_w, win_h);
    drawHUD(r, win_w, win_h);
    if (player_.isActive()) drawProgressBar(r, win_w, win_h);
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

      // Leave room for HUD at top (80 px) and progress bar at bottom (40 px).
      const int avail_h = win_h - 120;
      const float scale = std::min(
          float(win_w) / float(vw),
          float(avail_h) / float(vh));

      const SDL_FRect dst {
          float((win_w - int(vw * scale)) / 2),
          float(80 + (avail_h - int(vh * scale)) / 2),
          vw * scale, vh * scale};
      SDL_RenderTexture(r, tex, nullptr, &dst);

    } else if (player_.hasImage()) {
      SDL_Texture* tex = player_.getImageTexture();
      if (!tex) return;
      auto [iw, ih] = player_.getImageSize();
      if (iw == 0 || ih == 0) return;

      const float scale = std::min(
          float(win_w - 40) / float(iw),
          float(win_h - 100) / float(ih));

      const SDL_FRect dst {
          float((win_w - int(iw * scale)) / 2),
          float((win_h - int(ih * scale) - 40) / 2),
          iw * scale, ih * scale};
      SDL_RenderTexture(r, tex, nullptr, &dst);
    }
  }

  // -----------------------------------------------------------------------
  // HUD text
  // -----------------------------------------------------------------------

  void drawHUD(SDL_Renderer* r, int /*win_w*/, int /*win_h*/) const {
    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);

    // File name or hint
    const char* top_line =
        player_.current_file.empty()
            ? "Drop a video, audio, or image file here"
            : player_.current_file.c_str();
    SDL_RenderDebugText(r, 20, 14, top_line);

    // Volume
    char vol[32];
    std::snprintf(vol, sizeof(vol), "Volume: %d%%",
                  int(player_.getVolume() * 100.f));
    SDL_RenderDebugText(r, 20, 34, vol);

    if (player_.current_file.empty()) return;

    if (player_.hasImage()) {
      auto [iw, ih] = player_.getImageSize();
      char info[64];
      std::snprintf(info, sizeof(info), "Image: %ux%u", iw, ih);
      SDL_RenderDebugText(r, 20, 54, info);

    } else if (player_.isActive()) {
      const char* mode = player_.hasVideo() ? "Video" : "Audio";
      char status[32];
      std::snprintf(status, sizeof(status), "Status: %s", mode);
      SDL_RenderDebugText(r, 20, 54, status);

      SDL_RenderDebugText(r, 20, 74, player_.getProgressString().c_str());
    }
  }

  // -----------------------------------------------------------------------
  // Progress bar
  // -----------------------------------------------------------------------

  void drawProgressBar(SDL_Renderer* r, int win_w, int win_h) const {
    const SDL_FRect& bar = bar_cache_;
    const float progress = player_.getProgress();

    // Subtle glow behind the bar when hovered / dragging
    const bool hovered = dragging_ || isNearBar(mouse_x_, mouse_y_);
    if (hovered) {
      SDL_FRect glow {bar.x - 2, bar.y - 3, bar.w + 4, bar.h + 6};
      SDL_SetRenderDrawColor(r, 255, 255, 255, 30);
      SDL_RenderFillRect(r, &glow);
    }

    // Track border
    SDL_FRect border {bar.x - 1, bar.y - 1, bar.w + 2, bar.h + 2};
    SDL_SetRenderDrawColor(r, 90, 90, 90, 255);
    SDL_RenderRect(r, &border);

    // Track background
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderFillRect(r, &bar);

    // Filled portion
    if (progress > 0.0f) {
      SDL_FRect fill {bar.x, bar.y, bar.w * progress, bar.h};
      SDL_SetRenderDrawColor(r, 230, 230, 230, 255);
      SDL_RenderFillRect(r, &fill);
    }

    // Playhead dot
    if (progress > 0.0f && progress < 1.0f) {
      const float cx = bar.x + bar.w * progress;
      const float cy = bar.y + bar.h * 0.5f;
      const float r2 = hovered ? 6.0f : 4.0f;
      SDL_FRect dot {cx - r2, cy - r2, r2 * 2, r2 * 2};
      SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
      SDL_RenderFillRect(r, &dot);
    }
  }

  // -----------------------------------------------------------------------
  // Bar geometry helpers
  // -----------------------------------------------------------------------

  void updateBarCache() {
    int w = 800, h = 600;
    // Can't call SDL here without a renderer; rely on caller passing dims.
    updateBarCache(w, h);
  }

  void updateBarCache(int win_w, int win_h) {
    bar_cache_ = {
        kBarMarginX,
        float(win_h) - kBarBottom - kBarH,
        float(win_w) - kBarMarginX * 2.f,
        kBarH};
  }

  auto isNearBar(float x, float y) const -> bool {
    if (!player_.isActive()) return false;
    return x >= bar_cache_.x &&
           x <= bar_cache_.x + bar_cache_.w &&
           y >= bar_cache_.y - kHitExpand &&
           y <= bar_cache_.y + bar_cache_.h + kHitExpand;
  }

  auto progressFromX(float x) const -> float {
    return std::clamp(
        (x - bar_cache_.x) / bar_cache_.w, 0.0f, 1.0f);
  }

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------
  MediaPlayer& player_;
  SDL_FRect bar_cache_ {};
  float mouse_x_ = 0;
  float mouse_y_ = 0;
  bool dragging_ = false;
};
