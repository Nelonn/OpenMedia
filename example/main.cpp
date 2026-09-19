#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "media_player.hpp"
#include "ui.hpp"

#include <algorithm>
#include <span>
#include <string_view>

namespace {

auto findBackend(std::string_view name) -> const Backend* {
  const auto it = std::ranges::find(kBackends, name, &Backend::name);
  return it == std::end(kBackends) ? nullptr : it;
}

void logUsage() {
  SDL_Log("Usage: OpenMediaExample [backend] [file]");
  for (const Backend& b : kBackends)
    SDL_Log("  %-7.*s %.*s", int(b.name.size()), b.name.data(), int(b.description.size()), b.description.data());
  SDL_Log("Without a backend the player decodes in software.");
}

// [backend] [file]. A single codec ("vulkan_h264") is rejected: the backend
// decides which codecs it handles.
struct Args {
  const Backend* backend = nullptr;
  const char* file = nullptr;
  bool ok = true;
};

auto parseArgs(std::span<char*> args) -> Args {
  Args parsed;
  if (!args.empty()) {
    const std::string_view first = args.front();
    if ((parsed.backend = findBackend(first))) {
      args = args.subspan(1);
    } else if (std::ranges::any_of(kBackends, [&](const Backend& b) { return first.starts_with(b.decoder_prefix); })) {
      SDL_Log("'%s' names a single codec; pass a backend instead.", args.front());
      logUsage();
      parsed.ok = false;
    }
  }
  if (!args.empty()) parsed.file = args.front();
  return parsed;
}

} // namespace

int main(int argc, char* argv[]) {
  const Args args = parseArgs(std::span(argv + 1, argc - 1));
  if (!args.ok || !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  if (!SDL_CreateWindowAndRenderer("OpenMedia Player", 800, 600,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer)) {
    SDL_Log("%s", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  {
    MediaPlayer player(renderer);
    if (args.backend) {
      const auto desc = args.backend->description;
      if (player.useBackend(*args.backend))
        SDL_Log("[Player] %.*s acceleration enabled", int(desc.size()), desc.data());
      else
        SDL_Log("[Player] %.*s unavailable, decoding in software", int(desc.size()), desc.data());
    }

    PlayerUI ui(player);
    if (args.file && !player.play(args.file))
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenMedia Player", player.lastError().c_str(), window);

    for (bool running = true; running;) {
      for (SDL_Event event; SDL_PollEvent(&event);) running &= ui.handleEvent(event);
      player.tick();
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);
      ui.render(renderer, window);
      SDL_RenderPresent(renderer);
    }
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
