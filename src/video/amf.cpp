#include <components/Component.h>
#include <components/VideoDecoderUVD.h>
#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>
#include <core/Buffer.h>
#include <core/Context.h>
#include <core/D3D12AMF.h>
#include <core/Factory.h>
#include <core/Surface.h>
#include <core/VulkanAMF.h>
#include <d3d11.h>
#include <d3d12.h>
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#include <vulkan/vulkan.h>
#include <windows.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <format>
#include <memory>
#include <openmedia/video.hpp>
#include <vector>
#include <hw_dx11_priv.hpp>
#include <hw_dx12_priv.hpp>
#include <hw_vulkan_priv.hpp>

namespace openmedia {

static HMODULE G_AMF_MODULE = nullptr;
static amf::AMFFactory* G_AMF_FACTORY = nullptr;

static auto load_amf_runtime() -> bool {
  if (G_AMF_FACTORY) {
    return true;
  }

  G_AMF_MODULE = LoadLibraryW(AMF_DLL_NAME);
  if (!G_AMF_MODULE) {
    return false;
  }

  auto init_fn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(G_AMF_MODULE, AMF_INIT_FUNCTION_NAME));
  if (!init_fn) {
    FreeLibrary(G_AMF_MODULE);
    G_AMF_MODULE = nullptr;
    return false;
  }

  AMF_RESULT res = init_fn(AMF_FULL_VERSION, &G_AMF_FACTORY);
  if (res != AMF_OK) {
    FreeLibrary(G_AMF_MODULE);
    G_AMF_MODULE = nullptr;
    return false;
  }

  return true;
}

static auto amf_to_om_error(AMF_RESULT res) -> OMError {
  if (res == AMF_OK) return OM_SUCCESS;
  //if (res == AMF_EOF) return OM_CODEC_END_OF_STREAM;
  return OM_CODEC_DECODE_FAILED;
}

static auto get_amf_decoder_id(OMCodecId codec_id) -> const wchar_t* {
  switch (codec_id) {
    case OM_CODEC_H264:
      return AMFVideoDecoderUVD_H264_AVC;
    case OM_CODEC_H265:
      return AMFVideoDecoderHW_H265_HEVC;
    case OM_CODEC_VP9:
      return AMFVideoDecoderHW_VP9;
    case OM_CODEC_AV1:
      return AMFVideoDecoderHW_AV1;
    default:
      return nullptr;
  }
}

static auto get_amf_encoder_id(OMCodecId codec_id) -> const wchar_t* {
  switch (codec_id) {
    case OM_CODEC_H264:
      return AMFVideoEncoderVCE_AVC;
    case OM_CODEC_H265:
      return AMFVideoEncoder_HEVC;
    case OM_CODEC_AV1:
      return AMFVideoEncoder_AV1;
    default:
      return nullptr;
  }
}

static auto get_amf_format(OMPixelFormat fmt) -> amf::AMF_SURFACE_FORMAT {
  switch (fmt) {
    case OM_FORMAT_NV12:
      return amf::AMF_SURFACE_NV12;
    case OM_FORMAT_YUV420P:
      return amf::AMF_SURFACE_YUV420P;
    case OM_FORMAT_P010:
      return amf::AMF_SURFACE_P010;
    case OM_FORMAT_P012:
      return amf::AMF_SURFACE_P012;
    case OM_FORMAT_P016:
      return amf::AMF_SURFACE_P016;
    case OM_FORMAT_R8G8B8A8:
      return amf::AMF_SURFACE_RGBA;
    case OM_FORMAT_B8G8R8A8:
      return amf::AMF_SURFACE_BGRA;
    case OM_FORMAT_GRAY8:
      return amf::AMF_SURFACE_GRAY8;
    default:
      return amf::AMF_SURFACE_NV12;
  }
}

static auto get_om_format(amf::AMF_SURFACE_FORMAT fmt) -> OMPixelFormat {
  switch (fmt) {
    case amf::AMF_SURFACE_NV12:
      return OM_FORMAT_NV12;
    case amf::AMF_SURFACE_YUV420P:
      return OM_FORMAT_YUV420P;
    case amf::AMF_SURFACE_P010:
      return OM_FORMAT_P010;
    case amf::AMF_SURFACE_P012:
      return OM_FORMAT_P012;
    case amf::AMF_SURFACE_P016:
      return OM_FORMAT_P016;
    case amf::AMF_SURFACE_RGBA:
      return OM_FORMAT_R8G8B8A8;
    case amf::AMF_SURFACE_BGRA:
      return OM_FORMAT_B8G8R8A8;
    default:
      return OM_FORMAT_NV12;
  }
}

static auto create_surface_from_vulkan(amf::AMFContext1* ctx1, VkImage image, VkFormat format,
                                       int32_t width, int32_t height) -> amf::AMFSurfacePtr {
  if (!ctx1) return nullptr;

  amf::AMFSurfacePtr surface;
  amf::AMFVulkanSurface vk_surf = {};
  vk_surf.cbSizeof = sizeof(amf::AMFVulkanSurface);
  vk_surf.hImage = image;
  vk_surf.iWidth = width;
  vk_surf.iHeight = height;
  vk_surf.eFormat = format;

  AMF_RESULT res = ctx1->CreateSurfaceFromVulkanNative(&vk_surf, &surface, nullptr);
  if (res != AMF_OK) {
    return nullptr;
  }

  return surface;
}

static auto create_surface_from_dx11(amf::AMFContext* ctx, ID3D11Texture2D* texture) -> amf::AMFSurfacePtr {
  amf::AMFSurfacePtr surface;
  AMF_RESULT res = ctx->CreateSurfaceFromDX11Native(texture, &surface, nullptr);
  if (res != AMF_OK) {
    return nullptr;
  }
  return surface;
}

static auto create_surface_from_dx12(amf::AMFContext2* ctx2, ID3D12Resource* resource) -> amf::AMFSurfacePtr {
  if (!ctx2) return nullptr;

  amf::AMFSurfacePtr surface;
  AMF_RESULT res = ctx2->CreateSurfaceFromDX12Native(resource, &surface, nullptr);
  if (res != AMF_OK) {
    return nullptr;
  }
  return surface;
}

static auto get_amf_vulkan_extensions() -> std::vector<const char*> {
  // AMF typically requires these Vulkan extensions:
  // - VK_KHR_external_memory_capabilities
  // - VK_KHR_external_semaphore_capabilities
  // - VK_KHR_get_physical_device_properties2
  return {
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
  };
}

struct AMFContextInitResult {
  AMF_RESULT status = AMF_FAIL;
  HWDeviceType device_type = HWDeviceType::NONE;
  amf::AMFContextPtr context;
  amf::AMFContext1Ptr context1;
  amf::AMFContext2Ptr context2;
};

static auto initAMFContext(const std::optional<HWDevice>& hw_device)
    -> AMFContextInitResult {
  AMFContextInitResult result = {};

  amf::AMFContextPtr ctx;
  AMF_RESULT res = G_AMF_FACTORY->CreateContext(&ctx);
  if (res != AMF_OK || !ctx) {
    log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "Failed to create AMF context");
    result.status = res;
    return result;
  }
  result.context = ctx;

  amf::AMFContext1Ptr ctx1;
  amf::AMFContext2Ptr ctx2;
  ctx->QueryInterface(amf::AMFContext1::IID(), reinterpret_cast<void**>(&ctx1));
  if (ctx1) {
    result.context1 = ctx1;
    ctx1->QueryInterface(amf::AMFContext2::IID(), reinterpret_cast<void**>(&ctx2));
    if (ctx2) {
      result.context2 = ctx2;
    }
  }

  if (hw_device) {
    switch (hw_device->type) {
      case HWDeviceType::DX11: {
        ID3D11Device* d3d11_dev = static_cast<OMDX11Context*>(hw_device->context)->device.Get();
        res = result.context->InitDX11(d3d11_dev);
        if (res == AMF_OK) {
          result.device_type = HWDeviceType::DX11;
          log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "AMF initialized with D3D11 device");
        }
        break;
      }
      case HWDeviceType::DX12: {
        if (!result.context2) {
          log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "AMFContext2 not available for DX12");
          res = AMF_FAIL;
        } else {
          ID3D12Device* d3d12_dev = static_cast<OMDX12Context*>(hw_device->context)->device.Get();
          res = result.context2->InitDX12(d3d12_dev);
        }
        if (res == AMF_OK) {
          result.device_type = HWDeviceType::DX12;
          log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "AMF initialized with D3D12 device");
        }
        break;
      }
      case HWDeviceType::VULKAN: {
        if (!result.context1) {
          log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "AMFContext1 not available for Vulkan");
          res = AMF_FAIL;
        } else {
          VkDevice vk_dev = static_cast<OMVulkanContext*>(hw_device->context)->vk_device;
          res = result.context1->InitVulkan(vk_dev);
        }
        if (res == AMF_OK) {
          result.device_type = HWDeviceType::VULKAN;
          log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "AMF initialized with Vulkan device");
        }
        break;
      }
      default:
        result.device_type = HWDeviceType::NONE;
        break;
    }

    if (res != AMF_OK && hw_device->type != HWDeviceType::NONE) {
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING, "Failed to initialize hardware backend, falling back to host memory");
      result.device_type = HWDeviceType::NONE;
      res = AMF_OK; // Reset to OK since we're falling back gracefully
    }
  } else {
    result.device_type = HWDeviceType::NONE;
  }

  result.status = res;
  return result;
}

struct AMFBufferDeleter {
  void operator()(amf::AMFBuffer* buf) const {
    if (buf) {
      buf->Release();
    }
  }
};

class AMFHardwarePicture : public HardwarePicture {
public:
  std::shared_ptr<amf::AMFSurface> surface;

  AMFHardwarePicture(std::shared_ptr<amf::AMFSurface> surf)
      : HardwarePicture(HWDeviceType::AMF), surface(std::move(surf)) {}

  ~AMFHardwarePicture() override = default;
};

class AMFDecoder final : public Decoder {
  amf::AMFContextPtr amf_context_;   // DX11
  amf::AMFContext1Ptr amf_context1_; // Vulkan
  amf::AMFContext2Ptr amf_context2_; // DX12
  amf::AMFComponentPtr decoder_;
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<uint8_t> extradata_;
  amf::AMF_SURFACE_FORMAT output_format_amf_ = amf::AMF_SURFACE_NV12;
  HWDeviceType device_type_ = HWDeviceType::NONE;

  std::vector<std::pair<int64_t, std::shared_ptr<amf::AMFSurface>>> pending_surfaces_;

public:
  AMFDecoder() {}

  ~AMFDecoder() override {
    flush();
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    codec_id_ = options.format.codec_id;

    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 &&
        codec_id_ != OM_CODEC_VP9 && codec_id_ != OM_CODEC_AV1) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING, "AMF decoder only supports H264, H265, VP9, and AV1");
      return OM_CODEC_NOT_SUPPORTED;
    }

    if (!options.hw_device.has_value()) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    width_ = options.format.video.width;
    height_ = options.format.video.height;

    if (width_ == 0 || height_ == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (!options.extradata.empty()) {
      extradata_.assign(options.extradata.begin(), options.extradata.end());
    }

    if (!load_amf_runtime()) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "Failed to load AMF runtime");
      return OM_CODEC_HWACCEL_FAILED;
    }

    auto init_result = initAMFContext(options.hw_device);
    if (init_result.status != AMF_OK) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    amf_context_ = std::move(init_result.context);
    amf_context1_ = std::move(init_result.context1);
    amf_context2_ = std::move(init_result.context2);
    device_type_ = init_result.device_type;

    const wchar_t* decoder_id = get_amf_decoder_id(codec_id_);
    if (!decoder_id) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    amf::AMFComponentPtr comp;
    AMF_RESULT res = G_AMF_FACTORY->CreateComponent(amf_context_.GetPtr(), decoder_id, &comp);
    if (res != AMF_OK || !comp) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "Failed to create AMF decoder component");
      return OM_CODEC_HWACCEL_FAILED;
    }
    decoder_ = comp;

    // Set extradata if available (for H264/H265 Annex B or AVCC)
    if (!extradata_.empty()) {
      amf::AMFBufferPtr extradata_buf;
      res = amf_context_->AllocBuffer(amf::AMF_MEMORY_HOST, extradata_.size(), &extradata_buf);
      if (res == AMF_OK && extradata_buf) {
        memcpy(extradata_buf->GetNative(), extradata_.data(), extradata_.size());
        decoder_->SetProperty(AMF_VIDEO_DECODER_EXTRADATA, static_cast<amf::AMFInterface*>(extradata_buf));
      }
    }

    decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_LOW_LATENCY));

    decoder_->SetProperty(AMF_TIMESTAMP_MODE, static_cast<amf_int64>(AMF_TS_PRESENTATION));

    decoder_->SetProperty(AMF_VIDEO_DECODER_SURFACE_COPY, false);

    output_format_amf_ = amf::AMF_SURFACE_NV12;
    res = decoder_->Init(output_format_amf_, width_, height_);
    if (res != AMF_OK) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "Failed to initialize AMF decoder");
      return OM_CODEC_HWACCEL_FAILED;
    }

    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = OM_FORMAT_NV12;
    initialized_ = true;

    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (!initialized_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    if (packet.bytes.empty()) {
      // Flush/drain request
      return drainFrames(frames);
    }

    AMF_RESULT res = submitInput(packet);
    if (res != AMF_OK) {
      if (res == AMF_NEED_MORE_INPUT) {
        return Ok(std::move(frames));
      }
      return Err(OM_CODEC_DECODE_FAILED);
    }

    auto result = processOutput(frames);
    if (!result.isOk()) {
      return Err(result.unwrapErr());
    }
    return Ok(std::move(frames));
  }

  void flush() override {
    if (decoder_) {
      decoder_->Flush();
    }
    pending_surfaces_.clear();
  }

private:
  auto submitInput(const Packet& packet) -> AMF_RESULT {
    if (!decoder_) return AMF_FAIL;

    amf::AMFBufferPtr buf;
    AMF_RESULT res = amf_context_->AllocBuffer(amf::AMF_MEMORY_HOST, packet.bytes.size(), &buf);
    if (res != AMF_OK) return res;

    memcpy(buf->GetNative(), packet.bytes.data(), packet.bytes.size());
    buf->SetSize(packet.bytes.size());

    buf->SetProperty(L"PresentationTimeStamp", packet.pts);

    if (packet.is_keyframe) {
      buf->SetProperty(L"IsKeyFrame", true);
    }

    return decoder_->SubmitInput(buf);
  }

  auto processOutput(std::vector<Frame>& frames) -> Result<bool, OMError> {
    if (!decoder_) return Err(OM_COMMON_NOT_INITIALIZED);

    amf::AMFDataPtr data;
    AMF_RESULT res = decoder_->QueryOutput(&data);

    if (res == AMF_EOF) {
      // End of stream
      return Ok(false);
    }

    if (res == AMF_INPUT_FULL) {
      // Need more input
      return Ok(false);
    }

    if (res != AMF_OK || !data) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    amf::AMFSurfacePtr surface;
    res = data->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&surface));
    if (res != AMF_OK || !surface) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    int64_t pts = 0;
    data->GetProperty(L"PresentationTimeStamp", &pts);

    Frame frame = {};
    frame.pts = pts;
    frame.dts = pts;

    Picture* pic = nullptr;
    if (std::holds_alternative<Picture>(frame.data)) {
      pic = &std::get<Picture>(frame.data);
    } else {
      frame.data.emplace<Picture>();
      pic = &std::get<Picture>(frame.data);
    }

    pic->format = get_om_format(output_format_amf_);
    pic->width = width_;
    pic->height = height_;
    pic->planes = {};

    auto& host_pic = pic->buffer.emplace<HostPicture>();

    amf::AMFSurfacePtr host_surface;
    amf::AMFDataPtr data_ptr;
    res = surface->Duplicate(amf::AMF_MEMORY_HOST, &data_ptr);
    if (res == AMF_OK && data_ptr) {
      res = data_ptr->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&host_surface));
    }

    if (res == AMF_OK && host_surface) {
      size_t total_size = 0;
      const size_t plane_count = std::min<size_t>(host_surface->GetPlanesCount(), 4);

      for (size_t i = 0; i < plane_count; i++) {
        amf::AMFPlane* plane = host_surface->GetPlaneAt(i);
        if (!plane) {
          continue;
        }
        const int pitch = plane->GetHPitch();
        const int height = plane->GetHeight();
        if (pitch <= 0 || height <= 0) {
          continue;
        }
        total_size += static_cast<size_t>(pitch) * static_cast<size_t>(height);
      }

      host_pic.buffer = BufferPool::getInstance().get(total_size);
      uint8_t* dst = host_pic.buffer->bytes().data();
      size_t offset = 0;

      for (size_t i = 0; i < plane_count; i++) {
        amf::AMFPlane* plane = host_surface->GetPlaneAt(i);
        if (!plane) {
          continue;
        }
        uint8_t* src = static_cast<uint8_t*>(plane->GetNative());
        int pitch = plane->GetHPitch();
        int height = plane->GetHeight();
        if (!src || pitch <= 0 || height <= 0) {
          continue;
        }

        pic->planes.setData(i, dst + offset, static_cast<uint32_t>(pitch));
        memcpy(dst + offset, src, static_cast<size_t>(pitch) * static_cast<size_t>(height));
        offset += static_cast<size_t>(pitch) * static_cast<size_t>(height);
      }
    }

    frames.push_back(std::move(frame));
    return Ok(true);
  }

  auto drainFrames(std::vector<Frame>& frames) -> Result<std::vector<Frame>, OMError> {
    if (!decoder_) return Ok(std::move(frames));

    decoder_->Drain();

    while (true) {
      auto result = processOutput(frames);
      if (!result.isOk()) {
        return Err(result.unwrapErr());
      }
      if (!result.unwrap()) {
        break;
      }
    }

    return Ok(std::move(frames));
  }
};

class AMFEncoder final : public Encoder {
  amf::AMFContextPtr amf_context_;
  amf::AMFContext1Ptr amf_context1_; // For Vulkan
  amf::AMFContext2Ptr amf_context2_; // For DX12
  amf::AMFComponentPtr encoder_;
  bool initialized_ = false;
  VideoFormat input_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t bitrate_ = 0;
  uint32_t max_bitrate_ = 0;
  uint32_t qp_ = 0;
  Rational framerate_ = {};
  amf::AMF_SURFACE_FORMAT input_format_amf_ = amf::AMF_SURFACE_NV12;
  std::vector<uint8_t> extradata_;
  HWDeviceType device_type_ = HWDeviceType::NONE;

public:
  AMFEncoder() {}

  ~AMFEncoder() override {
    //flush();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    codec_id_ = options.format.codec_id;

    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 && codec_id_ != OM_CODEC_AV1) {
      log(OM_CATEGORY_ENCODER, OM_LEVEL_WARNING, "AMF encoder only supports H264, H265, and AV1");
      return OM_CODEC_NOT_SUPPORTED;
    }

    width_ = options.format.video.width;
    height_ = options.format.video.height;

    const auto& rc = options.rate_control;
    if (auto* cqp = std::get_if<CqpParams>(&rc.params)) {
      qp_ = cqp->qp_i;
    } else if (auto* cbr = std::get_if<CbrParams>(&rc.params)) {
      bitrate_ = cbr->bitrate.target_bitrate;
    } else if (auto* vbr = std::get_if<VbrParams>(&rc.params)) {
      bitrate_ = vbr->bitrate.target_bitrate;
    }

    framerate_ = options.format.video.framerate;

    if (width_ == 0 || height_ == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (!load_amf_runtime()) {
      log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "Failed to load AMF runtime");
      return OM_CODEC_HWACCEL_FAILED;
    }

    auto init_result = initAMFContext(options.hw_device);
    if (init_result.status != AMF_OK) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    amf_context_ = std::move(init_result.context);
    amf_context1_ = std::move(init_result.context1);
    amf_context2_ = std::move(init_result.context2);
    device_type_ = init_result.device_type;

    const wchar_t* encoder_id = get_amf_encoder_id(codec_id_);
    if (!encoder_id) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    AMF_RESULT res = G_AMF_FACTORY->CreateComponent(amf_context_.GetPtr(), encoder_id, &encoder_);
    if (res != AMF_OK || !encoder_) {
      log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "Failed to create AMF encoder component");
      return OM_CODEC_HWACCEL_FAILED;
    }

    res = configureEncoder();
    if (res != AMF_OK) {
      log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "Failed to configure AMF encoder");
      return OM_CODEC_HWACCEL_FAILED;
    }

    input_format_amf_ = amf::AMF_SURFACE_NV12;
    res = encoder_->Init(input_format_amf_, width_, height_);
    if (res != AMF_OK) {
      log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "Failed to initialize AMF encoder");
      return OM_CODEC_HWACCEL_FAILED;
    }

    // Get extradata (SPS/PPS for H264/H265, Sequence Header for AV1)
    retrieveExtradata();

    input_format_.width = width_;
    input_format_.height = height_;
    input_format_.format = OM_FORMAT_NV12;
    initialized_ = true;

    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    if (!initialized_) return {};

    EncodingInfo info = {};
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    std::vector<Packet> packets;

    if (!initialized_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    if (std::holds_alternative<Picture>(frame.data)) {
      const auto& pic = std::get<Picture>(frame.data);
      if (std::holds_alternative<HostPicture>(pic.buffer)) {
        AMF_RESULT res = submitFrame(frame);
        if (res != AMF_OK) {
          return Err(OM_CODEC_ENCODE_FAILED);
        }
      }
    }

    return processOutput(packets);
  }

  //void flush() override {
  //  if (encoder_) {
  //    encoder_->Flush();
  //  }
  //}

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    return OM_SUCCESS;
  }

private:
  auto configureEncoder() -> AMF_RESULT {
    if (!encoder_) return AMF_FAIL;

    encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFSize(width_, height_));
    encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_TRANSCODING));
    encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFRate {static_cast<amf_uint32>(framerate_.num), static_cast<amf_uint32>(framerate_.den)});

    switch (device_type_) {
      case HWDeviceType::DX11:
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MEMORY_TYPE, amf_int64(amf::AMF_MEMORY_DX11));
        break;
      case HWDeviceType::DX12:
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MEMORY_TYPE, static_cast<amf_int64>(amf::AMF_MEMORY_DX12));
        break;
      case HWDeviceType::VULKAN:
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MEMORY_TYPE, static_cast<amf_int64>(amf::AMF_MEMORY_VULKAN));
        break;
      default:
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MEMORY_TYPE, static_cast<amf_int64>(amf::AMF_MEMORY_UNKNOWN)); // Auto
        break;
    }

    if (bitrate_ > 0) {
      encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(bitrate_));
      if (max_bitrate_ > bitrate_) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, amf_int64(max_bitrate_));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                              static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
      } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                              static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
      }
    } else if (qp_ > 0) {
      encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                            static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP));
      encoder_->SetProperty(AMF_VIDEO_ENCODER_QP_I, static_cast<amf_int64>(qp_));
      encoder_->SetProperty(AMF_VIDEO_ENCODER_QP_P, static_cast<amf_int64>(qp_));
    } else {
      // Default to VBR
      encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(5000000)); // 5 Mbps
      encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                            static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
    }

    encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                          static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED));

    encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0)); // No B-frames for low latency
    encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(60));   // IDR every 60 frames

    return AMF_OK;
  }

  auto retrieveExtradata() -> AMF_RESULT {
    if (!encoder_) return AMF_FAIL;

    amf::AMFInterfacePtr extradata;
    AMF_RESULT res = encoder_->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &extradata);
    if (res == AMF_OK && extradata) {
      amf::AMFBufferPtr buf;
      res = extradata->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buf));
      if (res == AMF_OK && buf) {
        const uint8_t* data = static_cast<const uint8_t*>(buf->GetNative());
        size_t size = buf->GetSize();
        extradata_.assign(data, data + size);
      }
    }
    return res;
  }

  auto submitFrame(const Frame& frame) -> AMF_RESULT {
    if (!encoder_) return AMF_FAIL;

    const auto& pic = std::get<Picture>(frame.data);
    const auto& host_pic = std::get<HostPicture>(pic.buffer);
    if (!host_pic.buffer) return AMF_FAIL;

    amf::AMFSurfacePtr surface;
    AMF_RESULT res = amf_context_->AllocSurface(amf::AMF_MEMORY_HOST, input_format_amf_,
                                                width_, height_, &surface);
    if (res != AMF_OK || !surface) return res;

    const uint8_t* src = host_pic.buffer->bytes().data();
    size_t offset = 0;

    for (amf_int32 i = 0; i < surface->GetPlanesCount(); i++) {
      amf::AMFPlane* plane = surface->GetPlaneAt(i);
      uint8_t* dst = static_cast<uint8_t*>(plane->GetNative());
      amf_int32 pitch = plane->GetHPitch();
      amf_int32 height = plane->GetHeight();
      size_t plane_size = static_cast<size_t>(pitch) * height;

      memcpy(dst, src + offset, plane_size);
      offset += plane_size;
    }

    surface->SetProperty(AMF_VIDEO_ENCODER_PRESENTATION_TIME_STAMP, static_cast<amf_int64>(frame.pts));

    if (pic.is_keyframe) {
      surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                           static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
    }

    return encoder_->SubmitInput(surface);
  }

  auto processOutput(std::vector<Packet>& packets) -> Result<std::vector<Packet>, OMError> {
    if (!encoder_) return Err(OM_COMMON_NOT_INITIALIZED);

    while (true) {
      amf::AMFDataPtr data;
      AMF_RESULT res = encoder_->QueryOutput(&data);

      if (res == AMF_EOF) {
        return Ok(std::move(packets));
      }

      if (res == AMF_INPUT_FULL || res == AMF_NEED_MORE_INPUT) {
        return Ok(std::move(packets));
      }

      if (res != AMF_OK || !data) {
        break;
      }

      amf::AMFBufferPtr buf;
      res = data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buf));
      if (res != AMF_OK || !buf) {
        continue;
      }

      Packet packet = {};
      const uint8_t* data_ptr = static_cast<const uint8_t*>(buf->GetNative());
      size_t data_size = buf->GetSize();
      packet.allocate(data_size);
      std::memcpy(packet.bytes.data(), data_ptr, data_size);

      int64_t pts = 0;
      data->GetProperty(AMF_VIDEO_ENCODER_PRESENTATION_TIME_STAMP, &pts);
      packet.pts = pts;
      packet.dts = pts;

      amf_int64 frame_type = 0;
      data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &frame_type);
      packet.is_keyframe = (frame_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ||
                            frame_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I);

      packets.push_back(std::move(packet));
    }

    return Ok(std::move(packets));
  }
};

const CodecDescriptor CODEC_AMF_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_h264",
    .long_name = "AMD AMF H.264 Codec",
    .vendor = "AMD",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
    .encoder_factory = [] { return std::make_unique<AMFEncoder>(); },
};

const CodecDescriptor CODEC_AMF_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_h265",
    .long_name = "AMD AMF H.265/HEVC Codec",
    .vendor = "AMD",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H265_MAIN, OM_PROFILE_H265_MAIN_10},
    },
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
    .encoder_factory = [] { return std::make_unique<AMFEncoder>(); },
};

const CodecDescriptor CODEC_AMF_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_av1",
    .long_name = "AMD AMF AV1 Codec",
    .vendor = "AMD",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_AV1_MAIN, OM_PROFILE_AV1_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
    .encoder_factory = [] { return std::make_unique<AMFEncoder>(); },
};

const CodecDescriptor CODEC_AMF_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_vp9",
    .long_name = "AMD AMF VP9 Decoder",
    .vendor = "AMD",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_VP9_0, OM_PROFILE_VP9_2},
    },
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
};

} // namespace openmedia
