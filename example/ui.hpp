#pragma once

#include <SDL3/SDL.h>

#include "media_player.hpp"

#include <algorithm>

// mpv-style overlay: the picture letterboxed to the window, and a seek bar
// that fades out when the mouse goes idle. Holds no media state of its own.
class PlayerUI {
public:
  explicit PlayerUI(MediaPlayer& player) : player_(player) {}

  // Returns false when the application should quit.
  auto handleEvent(const SDL_Event& e) -> bool {
    switch (e.type) {
      case SDL_EVENT_QUIT:
        return false;

      case SDL_EVENT_DROP_FILE:
        if (!player_.play(e.drop.data))
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenMedia Player", player_.lastError().c_str(), nullptr);
        break;

      case SDL_EVENT_MOUSE_WHEEL:
        player_.setVolume(player_.volume() + (e.wheel.y > 0 ? kVolumeStep : -kVolumeStep));
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT && overBar(e.button.x, e.button.y)) {
          dragging_ = true;
          player_.seek(progressAt(e.button.x));
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT && dragging_) {
          dragging_ = false;
          player_.seek(progressAt(e.button.x));
        }
        break;

      case SDL_EVENT_MOUSE_MOTION:
        mouse_ = {e.motion.x, e.motion.y};
        last_motion_ = SDL_GetTicks();
        if (dragging_) player_.seek(progressAt(e.motion.x));
        break;

      default:
        break;
    }
    return true;
  }

  void render(SDL_Renderer* r, SDL_Window* window) {
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(r, &w, &h);
    bar_ = {kBarMarginX, h - kBarBottom - kBarH, w - kBarMarginX * 2, kBarH};

    fitWindowAspect(window);
    drawPicture(r, w, h);
    if (player_.isPlaying()) drawBar(r, w, h);
  }

private:
  static constexpr float kBarH = 8.0f;
  static constexpr float kBarMarginX = 20.0f;
  static constexpr float kBarBottom = 20.0f;
  static constexpr float kPanelH = 60.0f;
  static constexpr float kHitSlop = 14.0f;
  static constexpr float kVolumeStep = 0.05f;
  static constexpr Uint64 kFadeMs = 1000;

  void drawPicture(SDL_Renderer* r, int w, int h) const {
    SDL_Texture* tex = player_.texture();
    const auto [tw, th] = player_.textureSize();
    if (!tex || tw == 0 || th == 0) return;
    const float scale = std::min(float(w) / tw, float(h) / th);
    const SDL_FRect dst {(w - tw * scale) / 2, (h - th * scale) / 2, tw * scale, th * scale};
    SDL_RenderTexture(r, tex, nullptr, &dst);
  }

  void drawBar(SDL_Renderer* r, int w, int h) const {
    const auto alpha = Uint8(255 * barOpacity(h));
    if (alpha == 0) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const SDL_FRect panel {0, h - kPanelH, float(w), kPanelH};
    SDL_SetRenderDrawColor(r, 0, 0, 0, alpha / 2);
    SDL_RenderFillRect(r, &panel);

    SDL_SetRenderDrawColor(r, 80, 80, 80, alpha);
    SDL_RenderFillRect(r, &bar_);

    const float progress = player_.duration() > 0 ? float(player_.position() / player_.duration()) : 0.0f;
    const SDL_FRect filled {bar_.x, bar_.y, bar_.w * progress, bar_.h};
    SDL_SetRenderDrawColor(r, 255, 255, 255, alpha);
    SDL_RenderFillRect(r, &filled);

    if (dragging_ || overBar(mouse_.x, mouse_.y)) {
      constexpr float kKnob = 6.0f;
      const SDL_FRect knob {bar_.x + bar_.w * progress - kKnob, bar_.y + bar_.h / 2 - kKnob, kKnob * 2, kKnob * 2};
      SDL_RenderFillRect(r, &knob);
    }
  }

  // Fully visible while the mouse is over the lower half, else fades after idling.
  auto barOpacity(int h) const -> float {
    if (dragging_ || mouse_.y > h / 2.0f) return 1.0f;
    const Uint64 idle = SDL_GetTicks() - last_motion_;
    return idle >= kFadeMs ? 0.0f : 1.0f - float(idle) / kFadeMs;
  }

  void fitWindowAspect(SDL_Window* window) {
    const auto [tw, th] = player_.textureSize();
    if (!player_.isPlaying() && !tw) return;
    const float aspect = tw && th ? float(tw) / th : 1.0f;
    if (aspect == aspect_) return;
    aspect_ = aspect;
    SDL_SetWindowAspectRatio(window, aspect, aspect);
  }

  auto overBar(float x, float y) const -> bool {
    return player_.isPlaying() && x >= bar_.x && x <= bar_.x + bar_.w &&
           y >= bar_.y - kHitSlop && y <= bar_.y + bar_.h + kHitSlop;
  }

  auto progressAt(float x) const -> float { return std::clamp((x - bar_.x) / bar_.w, 0.0f, 1.0f); }

  MediaPlayer& player_;
  SDL_FRect bar_ {};
  SDL_FPoint mouse_ {};
  Uint64 last_motion_ = SDL_GetTicks();
  bool dragging_ = false;
  float aspect_ = 0.0f;
};
