#include "media_player.hpp"
#include "ui.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <string>
#include <string_view>

namespace {

auto isVideoDecoderArg(std::string_view arg) -> bool {
  return arg == "vulkan_h264" || arg == "dx11_h264" || arg == "dx12_h264";
}

auto enableRequestedBackend(MediaPlayer& player, std::string_view decoder) -> bool {
  if (decoder.starts_with("dx11_")) return player.enableDX11();
  if (decoder.starts_with("dx12_")) return player.enableDX12();
  if (decoder.starts_with("vulkan_")) return player.enableVulkan();
  return player.enableVulkan();
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
  //SDL_Renderer* renderer = SDL_CreateRenderer(window, "direct3d11");
  //SDL_Renderer* renderer = SDL_CreateRenderer(window, "direct3d12");
  //SDL_Renderer* renderer = SDL_CreateRenderer(window, "vulkan");
  if (!renderer) {
    SDL_Log("[Error] %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_SetRenderVSync(renderer, 1);

  MediaPlayer player;
  player.setRenderer(renderer);

  std::string requested_decoder;
  std::string initial_file;
  int path_arg = 1;
  if (argc > 1 && isVideoDecoderArg(argv[1])) {
    requested_decoder = argv[1];
    player.setRequestedVideoDecoder(requested_decoder);
    path_arg = 2;
  }
  if (argc > path_arg) {
    initial_file = argv[path_arg];
  }

  if (enableRequestedBackend(player, requested_decoder)) {
    if (!requested_decoder.empty())
      SDL_Log("[Player] %s acceleration enabled.", requested_decoder.c_str());
    else
      SDL_Log("[Player] Vulkan Video acceleration enabled.");
  } else {
    if (!requested_decoder.empty())
      SDL_Log("[Player] %s acceleration NOT available.", requested_decoder.c_str());
    else
      SDL_Log("[Player] Vulkan Video acceleration NOT available.");
  }

  PlayerUI ui(player);
  if (!initial_file.empty()) {
    player.play(initial_file);
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
