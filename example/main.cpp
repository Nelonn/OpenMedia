#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "media_player.hpp"
#include "ui.hpp"

#include <string>
#include <string_view>

namespace {

// ---------------------------------------------------------------------------
// Backend selection
//
// The optional first argument picks a hardware *backend*, not an individual
// codec.  Passing "vulkan" means "use the Vulkan decoders for whatever this
// clip happens to contain"; the player prefers every decoder registered under
// that backend and falls back to software when the clip's codec is not one of
// them.  Pinning a single codec (vulkan_h264, vulkan_h265, dx12_av1, ...) is
// deliberately not accepted — the backend decides which codec it can handle.
// ---------------------------------------------------------------------------
struct BackendOption {
  const char* name;        // what the user types
  const char* prefix;      // codec-name prefix this backend registers under
  const char* description;
};

constexpr BackendOption kBackends[] = {
    {"vulkan", "vulkan_", "Vulkan Video"},
    {"dx11",   "dx11_",   "Direct3D 11 (DXVA2)"},
    {"dx12",   "dx12_",   "Direct3D 12 Video"},
    {"amf",    "amf_",    "AMD Advanced Media Framework"},
    {"nv",     "nvdec_",  "NVIDIA NVDEC"},
    {"vaapi",  "vaapi_",  "VA-API"},
};

auto findBackend(std::string_view arg) -> const BackendOption* {
  for (const auto& backend : kBackends) {
    if (arg == backend.name) return &backend;
  }
  return nullptr;
}

// Detects "vulkan_h264" and friends so we can tell the user what to pass
// instead of silently treating it as a file name.
auto findBackendByCodecName(std::string_view arg) -> const BackendOption* {
  for (const auto& backend : kBackends) {
    if (arg.starts_with(backend.prefix)) return &backend;
  }
  return nullptr;
}

void logUsage() {
  SDL_Log("Usage: OpenMediaExample [backend] [file]");
  SDL_Log("Backends (optional, first argument):");
  for (const auto& backend : kBackends) {
    SDL_Log("  %-7s %s", backend.name, backend.description);
  }
  SDL_Log("Without a backend the player uses software decoding.");
}

// Creates the hardware device the chosen backend decodes onto.
// AMF binds to a D3D11 device, NVDEC to a CUDA context, and so on.
auto enableBackend(MediaPlayer& player, std::string_view backend) -> bool {
  if (backend == "vulkan") return player.enableVulkan();
  if (backend == "dx11")   return player.enableDX11();
  if (backend == "dx12")   return player.enableDX12();
  if (backend == "amf")    return player.enableDX11();
  if (backend == "nv")     return player.enableCuda();
  if (backend == "vaapi")  return player.enableVAAPI();
  return false;
}

} // namespace

int main(int argc, char* argv[]) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;

  SDL_Window* window = SDL_CreateWindow(
      "OpenMedia Player", 800, 600,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    SDL_Log("[Error] %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_SetRenderVSync(renderer, 1);

  MediaPlayer player;
  player.setRenderer(renderer);

  // ---- argument parsing -------------------------------------------------
  const BackendOption* backend = nullptr;
  int path_arg = 1;

  if (argc > 1) {
    const std::string_view first = argv[1];
    if (const auto* selected = findBackend(first)) {
      backend = selected;
      path_arg = 2;
    } else if (const auto* by_codec = findBackendByCodecName(first)) {
      // The user asked for one specific codec; point them at the backend.
      SDL_Log("[Player] '%s' names a single codec. Pass the backend '%s' instead.",
              argv[1], by_codec->name);
      logUsage();
      SDL_DestroyRenderer(renderer);
      SDL_DestroyWindow(window);
      SDL_Quit();
      return 1;
    }
  }

  const std::string initial_file = (argc > path_arg) ? argv[path_arg] : std::string();

  if (backend) {
    // Prefer this backend's decoders for video; software stays as the fallback
    // when the backend has no decoder for the clip's codec.
    if (enableBackend(player, backend->name)) {
      player.setPreferredDecoderPrefix(backend->prefix);
      SDL_Log("[Player] %s acceleration enabled.", backend->description);
    } else {
      SDL_Log("[Player] %s acceleration NOT available, falling back to software.",
              backend->description);
    }
  } else {
    SDL_Log("[Player] Software decoding selected.");
  }

  PlayerUI ui(player);
  if (!initial_file.empty()) {
    if (!player.play(initial_file)) {
      SDL_ShowSimpleMessageBox(
          SDL_MESSAGEBOX_ERROR,
          "OpenMedia Player",
          player.getLastError().c_str(),
          window);
    }
  }

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (!ui.handleEvent(event))
        running = false;
    }

    player.tickVideo();

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    ui.render(renderer, window);

    SDL_RenderPresent(renderer);
  }

  player.stop();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
