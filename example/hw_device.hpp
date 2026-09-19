#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "video_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <openmedia/codec_api.hpp>
#include <optional>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <openmedia/hw_cuda.h>
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#endif
#ifndef __APPLE__
#include <openmedia/hw_vulkan.h>
#endif
#ifdef OPENMEDIA_VAAPI
#include <openmedia/hw_vaapi.h>
#endif

using namespace openmedia;

enum class HwApi { Vulkan, DX11, DX12, Cuda, VAAPI };

// A hardware backend is picked as a whole, never per codec: the player prefers
// every decoder registered under `decoder_prefix` and falls back to software
// for codecs the backend cannot handle.
struct Backend {
  std::string_view name;
  std::string_view decoder_prefix;
  std::string_view description;
  HwApi api;
};

inline constexpr Backend kBackends[] = {
    {"vulkan", "vulkan_", "Vulkan Video", HwApi::Vulkan},
    {"dx11", "dx11_", "Direct3D 11 (DXVA2)", HwApi::DX11},
    {"dx12", "dx12_", "Direct3D 12 Video", HwApi::DX12},
    {"amf", "amf_", "AMD Advanced Media Framework", HwApi::DX11}, // AMF binds to D3D11
    {"nv", "nvdec_", "NVIDIA NVDEC", HwApi::Cuda},
    {"vaapi", "vaapi_", "VA-API", HwApi::VAAPI},
};

// The device hardware decoders decode onto, plus the download of their
// pictures to host memory for this example's SDL renderer.
class HwDevice {
public:
  HwDevice() = default;
  HwDevice(const HwDevice&) = delete;
  auto operator=(const HwDevice&) -> HwDevice& = delete;
  ~HwDevice() { close(); }

  auto open(HwApi api) -> bool {
    close();
    switch (api) {
#ifndef __APPLE__
      case HwApi::Vulkan: return openVulkan();
#endif
#ifdef _WIN32
      case HwApi::DX11: return adopt(HWDeviceType::DX11, HWD3D11Context_create(OMDX11Init {.adapter_index = -1}));
      case HwApi::DX12: return adopt(HWDeviceType::DX12, HWD3D12Context_create(OMDX12Init {.adapter_index = -1}));
      case HwApi::Cuda: return adopt(HWDeviceType::CUDA, HWCudaContext_create(OMCudaInit {}));
#endif
#ifdef OPENMEDIA_VAAPI
      case HwApi::VAAPI: return adopt(HWDeviceType::VAAPI, HWVAAPIContext_create(OMVAAPIInit {}));
#endif
      default: return false;
    }
  }

  void close() {
    if (device_) destroyContext(*device_);
    device_.reset();
#ifndef __APPLE__
    closeVulkan();
#endif
  }

  auto device() const -> const std::optional<HWDevice>& { return device_; }

  // Downloads a hardware picture into `vf` (always semi-planar).
  auto download(HardwarePicture& hw, const Picture& pic, VideoFrame& vf) const -> bool {
    if (!device_ || device_->type != hw.getType()) return false;

    const int bpp = bitDepth(pic.format) > 8 ? 2 : 1;
    const auto [cw, ch] = pic.getPlaneDimensions(1);
    auto& [y, uv, v] = vf.planes;
    y.stride = align(int(pic.width) * bpp);
    uv.stride = align(int(cw) * 2 * bpp);
    y.data.resize(size_t(y.stride) * pic.height);
    uv.data.resize(size_t(uv.stride) * ch);

    switch (hw.getType()) {
#ifndef __APPLE__
      case HWDeviceType::VULKAN:
        HWVulkanContext_copyToHost(context<OMVulkanContext>(),
                                   static_cast<VulkanHardwarePicture&>(hw).picture,
                                   y.data.data(), y.stride, uv.data.data(), uv.stride, pic.width, pic.height);
        return true;
#endif
#ifdef _WIN32
      case HWDeviceType::CUDA:
        HWCudaContext_copyToHost(context<OMCudaContext>(),
                                 static_cast<CudaHardwarePicture&>(hw).getOMPicture(),
                                 y.data.data(), y.stride, uv.data.data(), uv.stride, pic.width, pic.height);
        return true;
#endif
#ifdef OPENMEDIA_VAAPI
      case HWDeviceType::VAAPI:
        return HWVAAPIContext_copyToHost(context<OMVAAPIContext>(),
                                         static_cast<VAAPIHardwarePicture&>(hw).surface(),
                                         y.data.data(), y.stride, uv.data.data(), uv.stride, pic.width, pic.height);
#endif
      default:
        return false;
    }
  }

private:
  static auto align(int bytes) -> int { return (bytes + 31) & ~31; }

  template <typename T>
  auto context() const -> T* { return static_cast<T*>(device_->context); }

  template <typename Ctx>
  auto adopt(HWDeviceType type, Ctx* ctx) -> bool {
    if (!ctx) return false;
    device_ = HWDevice {type, ctx};
    return true;
  }

  static void destroyContext(const HWDevice& dev) {
    switch (dev.type) {
#ifndef __APPLE__
      case HWDeviceType::VULKAN: HWVulkanContext_delete(static_cast<OMVulkanContext*>(dev.context)); break;
#endif
#ifdef _WIN32
      case HWDeviceType::CUDA: HWCudaContext_delete(static_cast<OMCudaContext*>(dev.context)); break;
      case HWDeviceType::DX11: HWD3D11Context_delete(static_cast<OMDX11Context*>(dev.context)); break;
      case HWDeviceType::DX12: HWD3D12Context_delete(static_cast<OMDX12Context*>(dev.context)); break;
#endif
#ifdef OPENMEDIA_VAAPI
      case HWDeviceType::VAAPI: HWVAAPIContext_delete(static_cast<OMVAAPIContext*>(dev.context)); break;
#endif
      default: break;
    }
  }

#ifndef __APPLE__
  static constexpr uint32_t kNoQueue = UINT32_MAX;

  template <typename Fn>
  auto vk(const char* name) const -> Fn {
    return reinterpret_cast<Fn>(vk_proc_(vk_instance_, name));
  }

  auto openVulkan() -> bool {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
      SDL_Log("[Vulkan] Cannot load the Vulkan library: %s", SDL_GetError());
      return false;
    }
    vk_loaded_ = true;
    vk_proc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!vk_proc_ || !createVkInstance()) return false;

    const auto [physical, graphics_family, decode_family] = pickVkDevice();
    if (!physical) {
      SDL_Log("[Vulkan] No device with graphics and H.264 decode queues");
      return false;
    }
    if (!createVkDevice(physical, graphics_family, decode_family)) return false;

    const OMVulkanInit init {
        .proc = vk_proc_,
        .instance = vk_instance_,
        .physical_device = physical,
        .device = vk_device_,
        .queue_family_index = graphics_family,
        .video_decode_queue_family_index = decode_family,
        .video_encode_queue_family_index = kNoQueue,
    };
    return adopt(HWDeviceType::VULKAN, HWVulkanContext_create(init));
  }

  auto createVkInstance() -> bool {
    std::vector<const char*> layers, extensions;
    uint32_t count = 0;
    const auto enumerate_layers = vk<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties");
    enumerate_layers(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    enumerate_layers(&count, available.data());
    if (std::ranges::any_of(available, [](const auto& l) { return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; })) {
      layers.push_back("VK_LAYER_KHRONOS_validation");
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
      SDL_Log("[Vulkan] Validation layer enabled");
    }

    const VkApplicationInfo app {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledLayerCount = uint32_t(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = uint32_t(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (vk<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &vk_instance_) != VK_SUCCESS) {
      SDL_Log("[Vulkan] vkCreateInstance failed");
      return false;
    }

    if (!layers.empty()) {
      const VkDebugUtilsMessengerCreateInfoEXT debug {
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
          .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
          .pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
            SDL_Log("[Vulkan Validation] %s", data->pMessage);
            return VK_FALSE;
          },
      };
      if (auto create = vk<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT"))
        create(vk_instance_, &debug, nullptr, &vk_messenger_);
    }
    return true;
  }

  struct VkPick {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t graphics_family = kNoQueue;
    uint32_t decode_family = kNoQueue;
  };

  auto pickVkDevice() const -> VkPick {
    const auto enumerate = vk<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    const auto properties = vk<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    const auto queue_families = vk<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>("vkGetPhysicalDeviceQueueFamilyProperties2");

    uint32_t count = 0;
    enumerate(vk_instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(vk_instance_, &count, devices.data());

    for (VkPhysicalDevice device : devices) {
      VkPhysicalDeviceProperties props;
      properties(device, &props);
      SDL_Log("[Vulkan] Checking %s", props.deviceName);

      queue_families(device, &count, nullptr);
      std::vector<VkQueueFamilyVideoPropertiesKHR> video(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
      std::vector<VkQueueFamilyProperties2> families(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
      for (uint32_t i = 0; i < count; ++i) families[i].pNext = &video[i];
      queue_families(device, &count, families.data());

      VkPick pick {device};
      for (uint32_t i = 0; i < count; ++i) {
        if (pick.graphics_family == kNoQueue && (families[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT))
          pick.graphics_family = i;
        if (pick.decode_family == kNoQueue && (video[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR))
          pick.decode_family = i;
      }
      if (pick.graphics_family != kNoQueue && pick.decode_family != kNoQueue) return pick;
    }
    return {};
  }

  auto createVkDevice(VkPhysicalDevice physical, uint32_t graphics_family, uint32_t decode_family) -> bool {
    const auto enumerate = vk<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    uint32_t count = 0;
    enumerate(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    enumerate(physical, nullptr, &count, available.data());
    const auto supported = [&](const char* name) {
      return std::ranges::any_of(available, [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
    };

    std::vector<const char*> extensions;
    for (const char* name : {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
                             VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
                             VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
                             VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME}) {
      if (supported(name)) extensions.push_back(name);
    }

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    for (uint32_t family : {graphics_family, decode_family}) {
      if (queues.empty() || queues.back().queueFamilyIndex != family)
        queues.push_back({.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                          .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority});
    }

    VkPhysicalDeviceSynchronization2Features sync2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .synchronization2 = VK_TRUE};
    const VkDeviceCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = supported(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) ? &sync2 : nullptr,
        .queueCreateInfoCount = uint32_t(queues.size()),
        .pQueueCreateInfos = queues.data(),
        .enabledExtensionCount = uint32_t(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (vk<PFN_vkCreateDevice>("vkCreateDevice")(physical, &info, nullptr, &vk_device_) != VK_SUCCESS) {
      SDL_Log("[Vulkan] vkCreateDevice failed");
      return false;
    }
    return true;
  }

  void closeVulkan() {
    if (vk_device_) {
      vk<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(vk_device_);
      vk<PFN_vkDestroyDevice>("vkDestroyDevice")(vk_device_, nullptr);
    }
    if (vk_messenger_)
      vk<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(vk_instance_, vk_messenger_, nullptr);
    if (vk_instance_) vk<PFN_vkDestroyInstance>("vkDestroyInstance")(vk_instance_, nullptr);
    if (vk_loaded_) SDL_Vulkan_UnloadLibrary();
    vk_device_ = VK_NULL_HANDLE;
    vk_messenger_ = VK_NULL_HANDLE;
    vk_instance_ = VK_NULL_HANDLE;
    vk_proc_ = nullptr;
    vk_loaded_ = false;
  }

  bool vk_loaded_ = false;
  PFN_vkGetInstanceProcAddr vk_proc_ = nullptr;
  VkInstance vk_instance_ = VK_NULL_HANDLE;
  VkDevice vk_device_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT vk_messenger_ = VK_NULL_HANDLE;
#endif

  std::optional<HWDevice> device_;
};
