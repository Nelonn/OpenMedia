#pragma once

#include "audio_sink.hpp"
#include "av_clock.hpp"
#include "frame_queue.hpp"
#include "video_renderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <openmedia/audio.hpp>
#include <openmedia/codec_api.hpp>
#include <openmedia/codec_registry.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/format_detector.hpp>
#include <openmedia/format_registry.hpp>
#ifdef _WIN32
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#include <openmedia/hw_cuda.h>
#endif
#ifndef __APPLE__
#include <openmedia/hw_vulkan.h>
#endif
#include <openmedia/io.hpp>
#include <openmedia/video.hpp>
#include <queue>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace openmedia;

// ---------------------------------------------------------------------------
// PacketQueue — ffplay-style bounded, CV-driven packet queue.
//
// The demux thread pushes; per-stream decoder threads pop.
// abort() unblocks all waiters immediately (used on seek / stop).
// ---------------------------------------------------------------------------
class PacketQueue {
public:
    explicit PacketQueue(size_t capacity = 64) : capacity_(capacity) {}

    // Block until space is available or abort() is called.
    auto blockingPush(Packet pkt) -> bool {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_cv_.wait(lock, [&] {
            return aborted_ || queue_.size() < capacity_;
        });
        if (aborted_) return false;
        queue_.push(std::move(pkt));
        not_empty_cv_.notify_one();
        return true;
    }

    // Block until a packet is available or abort() is called.
    auto blockingPop() -> std::optional<Packet> {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_cv_.wait(lock, [&] {
            return aborted_ || !queue_.empty();
        });
        if (aborted_ && queue_.empty()) return std::nullopt;
        Packet pkt = std::move(queue_.front());
        queue_.pop();
        not_full_cv_.notify_one();
        return pkt;
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        while (!queue_.empty()) queue_.pop();
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = false;
        while (!queue_.empty()) queue_.pop();
    }

    auto size() const -> size_t {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    auto isAborted() const -> bool {
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_;
    }

private:
    std::queue<Packet>      queue_;
    mutable std::mutex      mutex_;
    std::condition_variable not_full_cv_;
    std::condition_variable not_empty_cv_;
    size_t                  capacity_;
    bool                    aborted_ = false;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace detail {

static auto getBitsPerComponent(OMPixelFormat fmt) noexcept -> uint8_t {
    switch (fmt) {
        case OM_FORMAT_YUV420P10:
        case OM_FORMAT_YUV422P10:
        case OM_FORMAT_YUV444P10:
            return 10;
        case OM_FORMAT_YUV420P12:
        case OM_FORMAT_YUV422P12:
        case OM_FORMAT_YUV444P12:
            return 12;
        case OM_FORMAT_YUV420P16:
        case OM_FORMAT_YUV422P16:
        case OM_FORMAT_YUV444P16:
        case OM_FORMAT_GRAY16:
        case OM_FORMAT_P016:
        case OM_FORMAT_RGBA64:
            return 16;
        case OM_FORMAT_P010:
            return 10;
        // All other formats are 8-bit
        default:
            return 8;
    }
}

static auto toSdlFormat(OMSampleFormat fmt) noexcept -> SDL_AudioFormat {
    switch (fmt) {
        case OM_SAMPLE_U8:  return SDL_AUDIO_U8;
        case OM_SAMPLE_S16: return SDL_AUDIO_S16;
        case OM_SAMPLE_S32: return SDL_AUDIO_S32;
        case OM_SAMPLE_F32: return SDL_AUDIO_F32;
        default:            return SDL_AUDIO_S16;
    }
}

static auto interleave(const AudioSamples& s) -> std::vector<uint8_t> {
    const uint32_t ch       = s.format.channels;
    const uint32_t nb       = s.nb_samples;
    const size_t   bps      = getBytesPerSample(s.format.sample_format);
    const size_t   frame_sz = bps * ch;
    std::vector<uint8_t> out(static_cast<size_t>(nb) * frame_sz);

    if (s.format.planar) {
        for (uint32_t c = 0; c < ch; ++c) {
            const uint8_t* src = s.planes.getData(c);
            if (!src) continue;
            for (uint32_t i = 0; i < nb; ++i)
                std::memcpy(out.data() + (i * ch + c) * bps, src + i * bps, bps);
        }
    } else {
        const uint8_t* src = s.planes.getData(0);
        if (src) std::memcpy(out.data(), src, out.size());
    }
    return out;
}

static auto normaliseBits(std::vector<uint8_t> src,
                          uint8_t bits) -> std::vector<uint8_t> {
    if (bits == 0 || bits == 8 || bits == 32) return src;
    const int    shift = 32 - static_cast<int>(bits);
    const size_t n     = src.size() / 4;
    std::vector<uint8_t> dst(src.size());
    for (size_t i = 0; i < n; ++i) {
        int32_t s = 0;
        std::memcpy(&s, src.data() + i * 4, 4);
        s <<= shift;
        std::memcpy(dst.data() + i * 4, &s, 4);
    }
    return dst;
}

static auto formatTime(double seconds) -> std::string {
    if (seconds < 0) return "00:00";
    const int total_s = static_cast<int>(seconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total_s / 60, total_s % 60);
    return buf;
}

static auto buildPixels(const Picture& pic) -> std::vector<uint32_t> {
    std::vector<uint32_t> pixels(pic.width * pic.height);
    for (uint32_t y = 0; y < pic.height; ++y) {
        const uint8_t* src = pic.planes.getData(0) + y * pic.planes.getLinesize(0);
        uint32_t*      dst = pixels.data() + y * pic.width;
        for (uint32_t x = 0; x < pic.width; ++x) {
            const uint8_t r = src[x * 4], g = src[x * 4 + 1],
                          b = src[x * 4 + 2], a = src[x * 4 + 3];
            dst[x] = (uint32_t(a) << 24) | (uint32_t(b) << 16) |
                     (uint32_t(g) <<  8) |  uint32_t(r);
        }
    }
    return pixels;
}

} // namespace detail

// ---------------------------------------------------------------------------
// MediaPlayer
// ---------------------------------------------------------------------------
class MediaPlayer {
public:
    std::string current_file;

    MediaPlayer() {
        format_detector_.addAllStandard();
        registerBuiltInCodecs(&codec_registry_);
        registerBuiltInFormats(&format_registry_);
    }

    ~MediaPlayer() {
        stop();
        releaseHardwareDevice();
    }

    void setRenderer(SDL_Renderer* r) {
        renderer_ = r;
        video_renderer_.setRenderer(r);
    }

    void setRequestedVideoDecoder(std::string name) {
        requested_video_decoder_ = std::move(name);
    }

#ifndef __APPLE__
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData) {
        SDL_Log("[Vulkan Validation] %s", pCallbackData->pMessage);
        return VK_FALSE;
    }
#endif

    auto enableVulkan() -> bool {
#ifndef __APPLE__
        releaseHardwareDevice();
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            SDL_Log("[Vulkan] SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
            return false;
        }
        vulkan_library_loaded_ = true;

        auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!vkGetInstanceProcAddr) {
            SDL_Log("[Vulkan] SDL_Vulkan_GetVkGetInstanceProcAddr returned nullptr");
            return false;
        }
        vulkan_get_instance_proc_addr_ = vkGetInstanceProcAddr;

        auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
        if (!vkCreateInstance) {
            SDL_Log("[Vulkan] Failed to load vkCreateInstance via vkGetInstanceProcAddr");
            return false;
        }

        // Basic Vulkan initialization
        VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.apiVersion = VK_API_VERSION_1_3;

        std::vector<const char*> layers;
        std::vector<const char*> instance_extensions;

        // Attempt to enable validation layer
        uint32_t layer_count = 0;
        auto vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));
        if (vkEnumerateInstanceLayerProperties) {
            vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            std::vector<VkLayerProperties> available_layers(layer_count);
            vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
            for (const auto& layer : available_layers) {
                if (std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation") {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    SDL_Log("[Vulkan] Enabling validation layer");
                    instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    break;
                }
            }
        }

        VkInstanceCreateInfo inst_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        inst_info.pApplicationInfo = &app_info;
        inst_info.enabledLayerCount = (uint32_t)layers.size();
        inst_info.ppEnabledLayerNames = layers.data();
        inst_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
        inst_info.ppEnabledExtensionNames = instance_extensions.data();
        
        VkInstance instance;
        if (vkCreateInstance(&inst_info, nullptr, &instance) != VK_SUCCESS) {
            SDL_Log("[Vulkan] vkCreateInstance failed");
            return false;
        }
        vulkan_instance_ = instance;

        // Setup Debug Messenger if layer enabled
        if (!layers.empty()) {
            auto vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (vkCreateDebugUtilsMessengerEXT) {
                VkDebugUtilsMessengerCreateInfoEXT debug_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                debug_info.pfnUserCallback = debugCallback;
                
                vkCreateDebugUtilsMessengerEXT(instance, &debug_info, nullptr, &vulkan_debug_messenger_);
            }
        }
        
        // Load instance functions
        auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
        auto vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(vkGetInstanceProcAddr(instance, "vkCreateDevice"));
        auto vkGetPhysicalDeviceQueueFamilyProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
        auto vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));

        if (!vkEnumeratePhysicalDevices || !vkCreateDevice || !vkGetPhysicalDeviceQueueFamilyProperties2 || !vkGetPhysicalDeviceProperties) {
            SDL_Log("[Vulkan] Failed to load instance functions via vkGetInstanceProcAddr");
            return false;
        }
        
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        if (devices.empty()) {
            SDL_Log("[Vulkan] No physical devices found");
            return false;
        }

        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        uint32_t graphics_queue_family = 0xFFFFFFFF;
        uint32_t decode_queue_family = 0xFFFFFFFF;
        
        for (uint32_t d_idx = 0; d_idx < devices.size(); ++d_idx) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[d_idx], &props);
            SDL_Log("[Vulkan] Checking device %u: %s", d_idx, props.deviceName);

            uint32_t qf_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties2(devices[d_idx], &qf_count, nullptr);
            std::vector<VkQueueFamilyProperties2> qf_props(qf_count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
            std::vector<VkQueueFamilyVideoPropertiesKHR> video_props(qf_count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
            for(uint32_t i=0; i<qf_count; ++i) qf_props[i].pNext = &video_props[i];
            vkGetPhysicalDeviceQueueFamilyProperties2(devices[d_idx], &qf_count, qf_props.data());

            for (uint32_t i = 0; i < qf_count; i++) {
                if ((qf_props[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics_queue_family == 0xFFFFFFFF) {
                    graphics_queue_family = i;
                }
                if ((video_props[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) && decode_queue_family == 0xFFFFFFFF) {
                    decode_queue_family = i;
                }
            }
            if (graphics_queue_family != 0xFFFFFFFF && decode_queue_family != 0xFFFFFFFF) {
                physical_device = devices[d_idx];
                break;
            }
            graphics_queue_family = 0xFFFFFFFF;
            decode_queue_family = 0xFFFFFFFF;
        }

        if (physical_device == VK_NULL_HANDLE) {
            SDL_Log("[Vulkan] No device with Graphics and H264 Decode support found");
            return false;
        }
        
        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        
        VkDeviceQueueCreateInfo graphics_q_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        graphics_q_info.queueFamilyIndex = graphics_queue_family;
        graphics_q_info.queueCount = 1;
        graphics_q_info.pQueuePriorities = &priority;
        queue_infos.push_back(graphics_q_info);

        if (decode_queue_family != graphics_queue_family) {
            VkDeviceQueueCreateInfo decode_q_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            decode_q_info.queueFamilyIndex = decode_queue_family;
            decode_q_info.queueCount = 1;
            decode_q_info.pQueuePriorities = &priority;
            queue_infos.push_back(decode_q_info);
        }
        
        auto vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(vkGetInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
        if (!vkEnumerateDeviceExtensionProperties) {
            SDL_Log("[Vulkan] Failed to load vkEnumerateDeviceExtensionProperties");
            return false;
        }

        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available_extensions(ext_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, available_extensions.data());

        auto hasExtension = [&](const char* name) {
            return std::any_of(available_extensions.begin(), available_extensions.end(), [&](const auto& e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };

        std::vector<const char*> extensions;
        if (hasExtension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME)) extensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        if (hasExtension(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME)) extensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        if (hasExtension(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME)) extensions.push_back(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
        if (hasExtension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME)) extensions.push_back(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
        if (hasExtension(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME)) extensions.push_back(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
        if (hasExtension(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)) extensions.push_back(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
        if (hasExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        
        VkPhysicalDeviceSynchronization2Features sync2_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        sync2_features.synchronization2 = VK_TRUE;

        VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = (std::find_if(extensions.begin(), extensions.end(), [](const char* s) { return std::strcmp(s, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0; }) != extensions.end()) ? &sync2_features : nullptr;
        device_info.queueCreateInfoCount = (uint32_t)queue_infos.size();
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = (uint32_t)extensions.size();
        device_info.ppEnabledExtensionNames = extensions.data();
        
        VkDevice device;
        if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
            SDL_Log("[Vulkan] vkCreateDevice failed. Extensions might be unsupported?");
            return false;
        }
        vulkan_device_ = device;
        
        OMVulkanInit om_init = {};
        om_init.instance = instance;
        om_init.physical_device = physical_device;
        om_init.device = device;
        om_init.queue_family_index = graphics_queue_family;
        om_init.video_decode_queue_family_index = decode_queue_family;
        om_init.video_encode_queue_family_index = 0xFFFFFFFF; // Not used for now
        om_init.proc = vkGetInstanceProcAddr;
        
        OMVulkanContext* ctx = HWVulkanContext_create(om_init);
        if (!ctx) {
            SDL_Log("[Vulkan] HWVulkanContext_create failed");
            return false;
        }
        
        hw_device_ = HWDevice{HWDeviceType::VULKAN, ctx};
        return true;
#endif
    }

    auto enableCuda() -> bool {
#ifdef _WIN32
        releaseHardwareDevice();
        OMCudaInit init = {};
        init.device_index = 0;
        init.cu_context = nullptr;
        auto* ctx = HWCudaContext_create(init);
        if (!ctx) {
            SDL_Log("[CUDA] HWCudaContext_create failed");
            return false;
        }
        hw_device_ = HWDevice{HWDeviceType::CUDA, ctx};
        SDL_Log("[CUDA] Video acceleration enabled.");
        return true;
#else
        return false;
#endif
    }

    auto enableDX11() -> bool {
#ifdef _WIN32
        releaseHardwareDevice();
        OMDX11Init init = {};
        init.adapter_index = -1;
        auto* ctx = HWD3D11Context_create(init);
        if (!ctx) {
            SDL_Log("[DX11] HWD3D11Context_create failed");
            return false;
        }
        hw_device_ = HWDevice{HWDeviceType::DX11, ctx};
        SDL_Log("[DX11] Video acceleration enabled.");
        return true;
#endif
    }

    auto enableDX12() -> bool {
#ifdef _WIN32
        releaseHardwareDevice();
        OMDX12Init init = {};
        init.adapter_index = -1;
        auto* ctx = HWD3D12Context_create(init);
        if (!ctx) {
            SDL_Log("[DX12] HWD3D12Context_create failed");
            return false;
        }
        hw_device_ = HWDevice{HWDeviceType::DX12, ctx};
        SDL_Log("[DX12] Video acceleration enabled.");
        return true;
#endif
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void stop() {
        // 1. Signal all threads and queues before joining.
        stop_requested_ = true;
        audio_packet_queue_.abort();
        video_packet_queue_.abort();
        video_frame_queue_.abort();

        // 2. Join all worker threads.
        if (demux_thread_.joinable())        demux_thread_.join();
        if (audio_decoder_thread_.joinable()) audio_decoder_thread_.join();
        if (video_decoder_thread_.joinable()) video_decoder_thread_.join();

        // 3. Tear down A/V resources.
        audio_sink_.close();
        video_renderer_.reset();

        if (demuxer_) {
            demuxer_->close();
            demuxer_.reset();
        }
        audio_decoder_.reset();
        video_decoder_.reset();

        if (image_texture_) {
            SDL_DestroyTexture(image_texture_);
            image_texture_ = nullptr;
        }

        clock_.reset(0);
        has_image_          = false;
        has_video_          = false;
        has_audio_          = false;
        current_file.clear();
        audio_stream_index_ = -1;
        video_stream_index_ = -1;
        image_stream_index_ = -1;
        total_duration_secs_ = 0;
        stop_requested_     = false;
    }

    auto play(const std::string& path) -> bool {
        stop();
        path_ = path;

        auto input = InputStream::createFileStream(path);
        if (!input || !input->isValid()) {
            SDL_Log("[Player] Cannot open: %s", path.c_str());
            return false;
        }

        uint8_t probe[2048];
        const size_t n  = input->read(probe);
        const DetectedFormat fmt = format_detector_.detect({probe, n});
        if (fmt.isUnknown()) {
            SDL_Log("[Player] Unknown format: %s", path.c_str());
            return false;
        }
        input->seek(0, Whence::BEG);

        if (fmt.isContainer()) {
            if (const auto* desc = format_registry_.getFormat(fmt.container);
                desc && desc->isDemuxing())
                demuxer_ = desc->demuxer_factory();
        }
        if (!demuxer_) {
            SDL_Log("[Player] No demuxer for format %d", int(fmt.container));
            return false;
        }
        if (demuxer_->open(std::move(input)) != OM_SUCCESS) {
            SDL_Log("[Player] Demuxer open failed");
            return false;
        }

        onDemuxerOpen();
        return true;
    }

    // -----------------------------------------------------------------------
    // Controls
    // -----------------------------------------------------------------------

    void setVolume(float v) {
        volume_ = std::clamp(v, 0.0f, 1.5f);
        audio_sink_.setGain(volume_);
    }

    void seek(float progress) {
        if (!demuxer_ || total_duration_secs_ <= 0) return;
        {
            std::lock_guard<std::mutex> lock(seek_mutex_);
            pending_seek_progress_ = std::clamp(progress, 0.0f, 1.0f);
            seek_pending_          = true;
            last_seek_time_        = SteadyClock::now();
        }
        seek_cv_.notify_one();
    }

    // -----------------------------------------------------------------------
    // Per-frame render call (main thread)
    // -----------------------------------------------------------------------

    void tickVideo() {
        if (!audio_sink_.started()) clock_.wallTick();
        video_renderer_.tick(video_frame_queue_, clock_);
    }

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    auto getVolume()      const -> float { return volume_; }
    auto hasImage()       const -> bool  { return has_image_; }
    auto hasVideo()       const -> bool  { return has_video_; }
    auto hasAudio()       const -> bool  { return has_audio_; }

    auto isActive() const -> bool {
        return has_video_ || audio_sink_.started();
    }

    auto getProgress() const -> float {
        if (total_duration_secs_ <= 0) return 0.0f;
        return static_cast<float>(clock_.masterSeconds()) /
               static_cast<float>(total_duration_secs_);
    }

    auto getProgressString() const -> std::string {
        return detail::formatTime(clock_.masterSeconds()) +
               " / " +
               detail::formatTime(total_duration_secs_);
    }

    auto getVideoTexture() -> SDL_Texture* { return video_renderer_.texture(); }
    auto getVideoSize() const -> std::pair<uint32_t, uint32_t> {
        return {video_renderer_.textureWidth(), video_renderer_.textureHeight()};
    }

    auto getImageTexture() -> SDL_Texture* {
        std::lock_guard<std::mutex> lock(image_mutex_);
        return image_texture_;
    }
    auto getImageSize() const -> std::pair<uint32_t, uint32_t> {
        return {image_width_, image_height_};
    }

private:
    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    // Infrastructure
    FormatDetector format_detector_;
    CodecRegistry  codec_registry_;
    FormatRegistry format_registry_;
    std::optional<HWDevice> hw_device_;
    std::string requested_video_decoder_;
#ifndef __APPLE__
    bool vulkan_library_loaded_ = false;
    PFN_vkGetInstanceProcAddr vulkan_get_instance_proc_addr_ = nullptr;
    VkInstance vulkan_instance_ = VK_NULL_HANDLE;
    VkDevice vulkan_device_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT vulkan_debug_messenger_ = VK_NULL_HANDLE;
#endif

    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<Decoder> audio_decoder_;
    std::unique_ptr<Decoder> video_decoder_;
    std::string              path_;

    // Stream indices
    int32_t audio_stream_index_ = -1;
    int32_t video_stream_index_ = -1;
    int32_t image_stream_index_ = -1;

    // Timebases
    Rational audio_time_base_ {1, 44100};
    Rational video_time_base_ {1, 90000};

    // A/V pipeline
    AVClock       clock_;
    AudioSink     audio_sink_;
    VideoRenderer video_renderer_;
    SDL_Renderer* renderer_ = nullptr;

    // Packet queues (demux thread → decoder threads)
    static constexpr size_t kPacketQueueCapacity = 64;
    PacketQueue audio_packet_queue_ {kPacketQueueCapacity};
    PacketQueue video_packet_queue_ {kPacketQueueCapacity};

    // Frame queue (video decoder thread → render thread)
    FrameQueue video_frame_queue_ {8};

    // Audio state
    float volume_    = 1.0f;
    bool  has_audio_ = false;

    // Video state
    bool has_video_ = false;

    // Image state
    SDL_Texture*       image_texture_ = nullptr;
    mutable std::mutex image_mutex_;
    uint32_t           image_width_   = 0;
    uint32_t           image_height_  = 0;
    bool               has_image_     = false;

    // Timeline
    double total_duration_secs_ = 0;

    // Threads
    std::thread       demux_thread_;
    std::thread       audio_decoder_thread_;
    std::thread       video_decoder_thread_;
    std::atomic<bool> stop_requested_ {false};

    // Seek coordination (used only by demux thread and seek() caller)
    std::mutex              seek_mutex_;
    std::condition_variable seek_cv_;
    bool                    seek_pending_          = false;
    float                   pending_seek_progress_ = 0.0f;
    TimePoint               last_seek_time_;

    static constexpr auto kSeekSettle = std::chrono::milliseconds(100);

    static auto decoderMatchesHardware(const CodecDescriptor& descriptor,
                                       HWDeviceType type) -> bool {
        const std::string_view name = descriptor.name;
        switch (type) {
            case HWDeviceType::VULKAN:
                    return name.starts_with("vulkan_");
            case HWDeviceType::CUDA:
                    return name.starts_with("nvdec_");
            case HWDeviceType::DX11:
            case HWDeviceType::DX12:   return name.starts_with("dx12_");
            default:                   return false;
        }
    }

    void releaseHardwareDevice() {
        if (hw_device_) {
            switch (hw_device_->type) {
#ifndef __APPLE__
                case HWDeviceType::VULKAN:
                    HWVulkanContext_delete(static_cast<OMVulkanContext*>(hw_device_->context));
                    break;
#endif
#ifdef _WIN32
                case HWDeviceType::CUDA:
                    HWCudaContext_delete(static_cast<OMCudaContext*>(hw_device_->context));
                    break;
                case HWDeviceType::DX11:
                    HWD3D11Context_delete(static_cast<OMDX11Context*>(hw_device_->context));
                    break;
                case HWDeviceType::DX12:
                    HWD3D12Context_delete(static_cast<OMDX12Context*>(hw_device_->context));
                    break;
#endif
                default:
                    break;
            }
            hw_device_.reset();
        }

#ifndef __APPLE__
        if (vulkan_device_ != VK_NULL_HANDLE && vulkan_get_instance_proc_addr_) {
            auto vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
                vulkan_get_instance_proc_addr_(vulkan_instance_, "vkDeviceWaitIdle"));
            auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
                vulkan_get_instance_proc_addr_(vulkan_instance_, "vkDestroyDevice"));
            if (vkDeviceWaitIdle) vkDeviceWaitIdle(vulkan_device_);
            if (vkDestroyDevice) vkDestroyDevice(vulkan_device_, nullptr);
            vulkan_device_ = VK_NULL_HANDLE;
        }

        if (vulkan_debug_messenger_ != VK_NULL_HANDLE && vulkan_get_instance_proc_addr_) {
            auto vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vulkan_get_instance_proc_addr_(vulkan_instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (vkDestroyDebugUtilsMessengerEXT)
                vkDestroyDebugUtilsMessengerEXT(vulkan_instance_, vulkan_debug_messenger_, nullptr);
            vulkan_debug_messenger_ = VK_NULL_HANDLE;
        }

        if (vulkan_instance_ != VK_NULL_HANDLE && vulkan_get_instance_proc_addr_) {
            auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
                vulkan_get_instance_proc_addr_(vulkan_instance_, "vkDestroyInstance"));
            if (vkDestroyInstance) vkDestroyInstance(vulkan_instance_, nullptr);
            vulkan_instance_ = VK_NULL_HANDLE;
        }

        vulkan_get_instance_proc_addr_ = nullptr;
        if (vulkan_library_loaded_) {
            SDL_Vulkan_UnloadLibrary();
            vulkan_library_loaded_ = false;
        }
#endif
    }

    // -----------------------------------------------------------------------
    // Setup
    // -----------------------------------------------------------------------

    void onDemuxerOpen() {
        const auto& tracks = demuxer_->tracks();
        for (size_t i = 0; i < tracks.size(); ++i) {
            const auto& t = tracks[i];
            if (t.format.type == OM_MEDIA_AUDIO && audio_stream_index_ < 0)
                audio_stream_index_ = int32_t(i);
            else if (t.format.type == OM_MEDIA_VIDEO && !t.isImage() &&
                     video_stream_index_ < 0)
                video_stream_index_ = int32_t(i);
            else if (t.isImage() && image_stream_index_ < 0)
                image_stream_index_ = int32_t(i);
        }

        // Still image path — decode immediately, no threads needed.
        if (image_stream_index_ >= 0 &&
            video_stream_index_ < 0 &&
            audio_stream_index_ < 0) {
            setupImageDecoder(tracks[size_t(image_stream_index_)]);
            return;
        }

        if (video_stream_index_ >= 0)
            setupVideoDecoder(tracks[size_t(video_stream_index_)]);
        if (audio_stream_index_ >= 0)
            setupAudioDecoder(tracks[size_t(audio_stream_index_)]);

        if (video_stream_index_ >= 0 || audio_stream_index_ >= 0) {
            current_file = path_;
            startThreads();
        }
    }

    auto makeDecoder(const Track& track, std::unique_ptr<Decoder>& dec) -> const CodecDescriptor* {
        auto descriptors = codec_registry_.getCodecsByCodecId(track.format.codec_id);
        if (descriptors.empty()) {
            SDL_Log("[Player] No decoders for codec %d", int(track.format.codec_id));
            return nullptr;
        }

        DecoderOptions opts;
        opts.format    = track.format;
        opts.time_base = track.time_base;
        opts.extradata = track.extradata;
        if (track.format.type == OM_MEDIA_VIDEO && hw_device_) {
            opts.hw_device = *hw_device_;
        }

        for (const auto* descriptor : descriptors) {
            if (!descriptor->isDecoding()) continue;
            if (track.format.type == OM_MEDIA_VIDEO) {
                if (!requested_video_decoder_.empty() && descriptor->name != requested_video_decoder_) {
                    continue;
                }
                if (requested_video_decoder_.empty() && hw_device_ && !decoderMatchesHardware(*descriptor, hw_device_->type)) {
                    continue;
                }
            }
            
            dec = descriptor->decoder_factory();
            if (!dec) continue;

            const OMError err = dec->configure(opts);
            if (err == OM_SUCCESS) {
                return descriptor;
            }

            if (track.format.type == OM_MEDIA_VIDEO) {
                SDL_Log("[Player] Decoder %s configure failed err=%d codec_id=%d tb=%d/%d %ux%u extradata=%zu profile=%u level=%d",
                        descriptor->name.data(),
                        int(err),
                        int(track.format.codec_id),
                        track.time_base.num, track.time_base.den,
                        track.format.video.width, track.format.video.height,
                        track.extradata.size(),
                        unsigned(track.format.profile),
                        track.format.level);
            } else if (track.format.type == OM_MEDIA_AUDIO) {
                SDL_Log("[Player] Decoder %s configure failed err=%d codec_id=%d tb=%d/%d rate=%u ch=%u depth=%u extradata=%zu profile=%u level=%d",
                        descriptor->name.data(),
                        int(err),
                        int(track.format.codec_id),
                        track.time_base.num, track.time_base.den,
                        track.format.audio.sample_rate,
                        track.format.audio.channels,
                        track.format.audio.bit_depth,
                        track.extradata.size(),
                        unsigned(track.format.profile),
                        track.format.level);
            }
            dec.reset();
        }

        if (track.format.type == OM_MEDIA_VIDEO && !requested_video_decoder_.empty()) {
            SDL_Log("[Player] Requested decoder %s for codec %d failed or was not found",
                    requested_video_decoder_.c_str(), int(track.format.codec_id));
        } else {
            SDL_Log("[Player] All decoders for codec %d failed", int(track.format.codec_id));
        }
        return nullptr;
    }

    void setupVideoDecoder(const Track& track) {
        const auto* desc = makeDecoder(track, video_decoder_);
        if (!desc) return;
        clock_.setMode(AVClock::Mode::WALL);
        clock_.reset(0.0);
        video_time_base_ = track.time_base;
        total_duration_secs_ = static_cast<double>(track.duration) *
                                track.time_base.num / track.time_base.den;
        has_video_       = true;
        SDL_Log("[Player] Video %dx%d codec=%s tb=%d/%d",
                track.format.video.width, track.format.video.height,
                desc->name.data(),
                track.time_base.num, track.time_base.den);
    }

    void setupAudioDecoder(const Track& track) {
        const auto* desc = makeDecoder(track, audio_decoder_);
        if (!desc) return;
        clock_.setMode(AVClock::Mode::AUDIO);
        clock_.reset(0.0);
        audio_time_base_ = track.time_base;
        if (video_stream_index_ < 0) {
            total_duration_secs_ = static_cast<double>(track.duration) *
                                   track.time_base.num / track.time_base.den;
        }
        has_audio_       = true;
        SDL_Log("[Player] Audio codec=%s tb=%d/%d",
                desc->name.data(),
                track.time_base.num, track.time_base.den);
    }

    void setupImageDecoder(const Track& track) {
        if (!makeDecoder(track, video_decoder_)) return;
        image_width_    = track.format.image.width;
        image_height_   = track.format.image.height;
        total_duration_secs_ = static_cast<double>(track.duration) *
                               track.time_base.num / track.time_base.den;
        decodeAndShowImage();
    }
    // -----------------------------------------------------------------------
    // Thread management
    // -----------------------------------------------------------------------

    void startThreads() {
        audio_packet_queue_.reset();
        video_packet_queue_.reset();
        video_frame_queue_.reset();
        stop_requested_ = false;

        // Demux thread feeds the per-stream packet queues.
        demux_thread_ = std::thread([this] { demuxLoop(); });

        // Per-stream decoder threads drain their packet queue → decoded output.
        if (has_audio_)
            audio_decoder_thread_ = std::thread([this] { audioDecodeLoop(); });
        if (has_video_)
            video_decoder_thread_ = std::thread([this] { videoDecodeLoop(); });
    }

    void stopThreads() {
        stop_requested_ = true;
        seek_cv_.notify_all();          // wake demux from seek-settle wait
        audio_packet_queue_.abort();
        video_packet_queue_.abort();
        video_frame_queue_.abort();

        if (demux_thread_.joinable())        demux_thread_.join();
        if (audio_decoder_thread_.joinable()) audio_decoder_thread_.join();
        if (video_decoder_thread_.joinable()) video_decoder_thread_.join();
    }

    // -----------------------------------------------------------------------
    // Demux thread — mirrors ffplay's read_thread()
    //
    // Responsibilities:
    //   • Route demuxed packets to the correct per-stream PacketQueue.
    //   • Handle seek requests: flush queues, seek demuxer, resume.
    //   • Apply back-pressure: sleep when both packet queues are full.
    // -----------------------------------------------------------------------
    void demuxLoop() {
        using namespace std::chrono_literals;

        while (!stop_requested_) {

            // ---- seek handling (ffplay: check seek_req flag) ----
            {
                std::unique_lock<std::mutex> lock(seek_mutex_);
                if (seek_pending_ &&
                    (SteadyClock::now() - last_seek_time_) >= kSeekSettle) {
                    const float p = pending_seek_progress_;
                    seek_pending_ = false;
                    lock.unlock();
                    doSeek(p);
                    continue;
                }
            }

            // ---- back-pressure (ffplay: infinite_buffer check) ----
            // Both packet queues are well-fed — yield without spinning.
            const bool audio_ok = !has_audio_ ||
                audio_packet_queue_.size() < kPacketQueueCapacity * 3 / 4;
            const bool video_ok = !has_video_ ||
                video_packet_queue_.size() < kPacketQueueCapacity * 3 / 4;

            if (!audio_ok && !video_ok) {
                std::unique_lock<std::mutex> lock(seek_mutex_);
                seek_cv_.wait_for(lock, 10ms, [&] {
                    return stop_requested_.load() || seek_pending_;
                });
                continue;
            }

            // ---- read one packet ----
            auto res = demuxer_->readPacket();
            if (res.isErr()) {
                // EOF — wait; a seek may restart things.
                std::unique_lock<std::mutex> lock(seek_mutex_);
                seek_cv_.wait_for(lock, 200ms, [&] {
                    return stop_requested_.load() || seek_pending_;
                });
                continue;
            }

            Packet pkt = res.unwrap();
            if (pkt.stream_index == audio_stream_index_)
                audio_packet_queue_.blockingPush(std::move(pkt));
            else if (pkt.stream_index == video_stream_index_)
                video_packet_queue_.blockingPush(std::move(pkt));
            // Packets for other streams are discarded.
        }
    }

    // -----------------------------------------------------------------------
    // Audio decoder thread — mirrors ffplay's audio_thread()
    // -----------------------------------------------------------------------
    void audioDecodeLoop() {
        while (!stop_requested_) {
            auto maybe_pkt = audio_packet_queue_.blockingPop();
            if (!maybe_pkt) break; // aborted

            auto result = audio_decoder_->decode(*maybe_pkt);
            if (result.isErr()) continue;

            for (auto& frame : result.unwrap()) {
                if (!std::holds_alternative<AudioSamples>(frame.data)) continue;
                const AudioSamples& s = std::get<AudioSamples>(frame.data);
                if (s.nb_samples == 0) continue;

                if (!audio_sink_.isOpen()) {
                    const size_t bps = getBytesPerSample(s.format.sample_format);
                    if (!audio_sink_.open(
                            detail::toSdlFormat(s.format.sample_format),
                            int(s.format.channels),
                            int(s.format.sample_rate),
                            bps, &clock_))
                        continue;
                }

                auto pcm = detail::normaliseBits(
                    detail::interleave(s), s.format.bits_per_sample);

                // Push PCM to the sink.  The sink's own ringbuffer provides
                // back-pressure; check stop_requested_ between partial writes.
                size_t written = 0;
                while (written < pcm.size() && !stop_requested_) {
                    written += audio_sink_.pushPcm(
                        pcm.data() + written, pcm.size() - written);
                    if (written < pcm.size())
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }

                const double pts_sec = static_cast<double>(frame.pts) * 
                                       audio_time_base_.num / audio_time_base_.den;
                audio_sink_.tickBuffering(pts_sec);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Video decoder thread — mirrors ffplay's video_thread()
    //
    // The original freeze came from this thread doing a sleep-spin inside
    // processVideoPacket while holding no lock, but the render thread was
    // unable to make progress draining the queue (e.g. renderer not ticking
    // fast enough, or a seek draining the queue while the push was mid-retry).
    //
    // Here we use blockingPush() instead: the thread sleeps on a CV inside
    // the queue and is woken the instant a slot opens OR abort() is called.
    // There is no spin, no 2 ms sleep, and the thread exits cleanly on stop.
    // -----------------------------------------------------------------------
    void videoDecodeLoop() {
        while (!stop_requested_) {
            auto maybe_pkt = video_packet_queue_.blockingPop();
            if (!maybe_pkt) break; // aborted

            auto result = video_decoder_->decode(*maybe_pkt);
            if (result.isErr()) continue;

            for (auto& frame : result.unwrap()) {
                if (!std::holds_alternative<Picture>(frame.data)) continue;
                const Picture& pic = std::get<Picture>(frame.data);
                if (pic.width == 0 || pic.height == 0) continue;

                VideoFrame vf;
                vf.width   = pic.width;
                vf.height  = pic.height;
                vf.pixel_format = static_cast<uint32_t>(pic.format);
                vf.pts     = int64_t(frame.pts);
                vf.pts_sec = static_cast<double>(vf.pts) *
                             video_time_base_.num / video_time_base_.den;
                vf.bits_per_component = detail::getBitsPerComponent(pic.format);

                if (std::holds_alternative<std::shared_ptr<HardwarePicture>>(pic.buffer)) {
                  const auto& hw = std::get<std::shared_ptr<HardwarePicture>>(pic.buffer);
                  if (hw && hw->getType() == HWDeviceType::VULKAN) {
#ifndef __APPLE__
                    const auto& vhw = static_cast<const VulkanHardwarePicture&>(*hw);

                    // For this example's software renderer, we MUST resolve/download to host memory.
                    // In a real player, we'd keep it on GPU and use a Vulkan renderer.
                    vf.y_stride = (pic.width + 15) & ~15;
                    vf.u_stride = (pic.width + 15) & ~15; // Interleaved UV pitch for NV12.
                    vf.v_stride = 0;

                    vf.y_plane.resize(vf.y_stride * pic.height);
                    vf.u_plane.resize(vf.u_stride * ((pic.height + 1) / 2));

                    if (!hw_device_ || hw_device_->type != HWDeviceType::VULKAN)
                        continue;

                    HWVulkanContext_copyToHost(static_cast<OMVulkanContext*>(hw_device_->context),
                                               vhw.picture,
                                               vf.y_plane.data(), vf.y_stride,
                                               vf.u_plane.data(), vf.u_stride,
                                               pic.width, pic.height);
#endif
                  } else if (hw && hw->getType() == HWDeviceType::CUDA) {
#ifdef _WIN32
                    auto c_pic = std::static_pointer_cast<CudaHardwarePicture>(hw);
                    vf.y_stride = (pic.width + 15) & ~15;
                    vf.u_stride = (pic.width + 15) & ~15;
                    vf.v_stride = 0;
                    vf.y_plane.resize(vf.y_stride * pic.height);
                    vf.u_plane.resize(vf.u_stride * ((pic.height + 1) / 2));

                    HWCudaContext_copyToHost(static_cast<OMCudaContext*>(hw_device_->context),
                                             c_pic->getOMPicture(),
                                             vf.y_plane.data(), vf.y_stride,
                                             vf.u_plane.data(), vf.u_stride,
                                             pic.width, pic.height);
#endif
                  }
                } else {
                  vf.y_stride = pic.planes.getLinesize(0);
                  vf.u_stride = pic.planes.getLinesize(1);
                  vf.v_stride = pic.planes.getLinesize(2);

                  const uint8_t* y = pic.planes.getData(0);
                  const uint8_t* u = pic.planes.getData(1);
                  const uint8_t* v = pic.planes.getData(2);

                  const auto y_dims = pic.getPlaneDimensions(0);
                  const auto u_dims = pic.getPlaneDimensions(1);
                  const auto v_dims = pic.getPlaneDimensions(2);

                  vf.y_plane.assign(y, y + vf.y_stride * y_dims.second);
                  vf.u_plane.assign(u, u + vf.u_stride * u_dims.second);
                  vf.v_plane.assign(v, v + vf.v_stride * v_dims.second);
                }

                // blockingPush sleeps on a CV until space is available or
                // abort() is called — no spin, no arbitrary sleep.
                if (!video_frame_queue_.blockingPush(std::move(vf))) break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Seek — called from demux thread only (no cross-thread decoder access)
    // -----------------------------------------------------------------------
    void doSeek(float progress) {
        // 1. Abort decoder threads so they drain immediately.
        audio_packet_queue_.abort();
        video_packet_queue_.abort();
        video_frame_queue_.abort();

        // 2. Flush codec internal state.
        audio_sink_.pause();
        audio_sink_.clearBuffer();
        if (audio_decoder_) audio_decoder_->flush();
        if (video_decoder_) video_decoder_->flush();

        // 3. Reset queues for reuse.
        audio_packet_queue_.reset();
        video_packet_queue_.reset();
        video_frame_queue_.reset();

        // 4. Seek the demuxer.
        const double target_secs = static_cast<double>(progress) * total_duration_secs_;
        
        const int64_t target_us = static_cast<int64_t>(target_secs * 1e9) / 1000;

        if (demuxer_->seek(-1, target_us) == OM_SUCCESS)
            clock_.reset(target_secs);

        if (audio_decoder_thread_.joinable()) {
          audio_decoder_thread_.join();
        }
        if (video_decoder_thread_.joinable()) {
          video_decoder_thread_.join();
        }

        // 5. Re-launch decoder threads (they had exited after abort).
        if (has_audio_)
            audio_decoder_thread_ = std::thread([this] { audioDecodeLoop(); });
        if (has_video_)
            video_decoder_thread_ = std::thread([this] { videoDecodeLoop(); });

        // 6. Prime the pipeline by pushing a few packets before returning.
        for (int i = 0; i < 12 && !stop_requested_; ++i) {
            auto res = demuxer_->readPacket();
            if (res.isErr()) break;
            Packet pkt = res.unwrap();
            if (pkt.stream_index == audio_stream_index_)
                audio_packet_queue_.blockingPush(std::move(pkt));
            else if (pkt.stream_index == video_stream_index_)
                video_packet_queue_.blockingPush(std::move(pkt));
        }
    }

    // -----------------------------------------------------------------------
    // Still image
    // -----------------------------------------------------------------------

    void decodeAndShowImage() {
        if (!video_decoder_ || !renderer_) return;
        auto res = demuxer_->readPacket();
        if (res.isErr()) return;

        auto result = video_decoder_->decode(res.unwrap());
        if (result.isErr() || result.unwrap().empty()) return;

        Frame& f = result.unwrap()[0];
        if (!std::holds_alternative<Picture>(f.data)) return;

        const Picture& pic = std::get<Picture>(f.data);
        const auto     pix = detail::buildPixels(pic);

        std::lock_guard<std::mutex> lock(image_mutex_);
        if (image_texture_) {
            SDL_DestroyTexture(image_texture_);
            image_texture_ = nullptr;
        }
        image_texture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STATIC,
            int(pic.width), int(pic.height));
        if (!image_texture_) return;

        SDL_UpdateTexture(image_texture_, nullptr, pix.data(),
                          int(pic.width * sizeof(uint32_t)));
        image_width_  = pic.width;
        image_height_ = pic.height;
        has_image_    = true;
        current_file  = path_;
        SDL_Log("[Player] Image %dx%d loaded", image_width_, image_height_);
    }
};
