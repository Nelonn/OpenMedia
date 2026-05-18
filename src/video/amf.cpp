#include <components/ColorSpace.h>
#include <components/Component.h>
#include <components/VideoDecoderUVD.h>
#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>
#include <core/Buffer.h>
#include <core/Context.h>
#include <core/Factory.h>
#include <core/Surface.h>
#include <core/Trace.h>
#include <d3d11.h>
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#include <openmedia/hw_vulkan.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <format>
#include <memory>
#include <openmedia/video.hpp>
#include <string>
#include <thread>
#include <vector>

#include "dx_h264.hpp"

#include <core/D3D12AMF.h>
#include <core/VulkanAMF.h>

namespace openmedia {

using Microsoft::WRL::ComPtr;

static HMODULE G_AMF_MODULE = nullptr;
static amf::AMFFactory* G_AMF_FACTORY = nullptr;

static std::string wstring_to_utf8(const wchar_t* wstr) {
  if (!wstr) return "";
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "";
  std::string str(size, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size, nullptr, nullptr);
  while (!str.empty() && str.back() == '\0') str.pop_back();
  return str;
}

class OMTraceWriter : public amf::AMFTraceWriter {
public:
  void AMF_CDECL_CALL Write(const wchar_t* scope, const wchar_t* message) override {
    //auto msg = wstring_to_utf8(message);
    //log(OM_CATEGORY_HARDWARE, OM_LEVEL_VERBOSE, "[AMF] {}: {}", wstring_to_utf8(scope), msg.substr(0, msg.size() - 2));
  }
  void AMF_CDECL_CALL Flush() override {}
};

static OMTraceWriter G_AMF_TRACE_WRITER;

static auto load_amf_runtime() -> bool {
  if (G_AMF_FACTORY) return true;

  G_AMF_MODULE = LoadLibraryW(AMF_DLL_NAME);
  if (!G_AMF_MODULE) return false;

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

  amf::AMFTrace* trace = nullptr;
  if (G_AMF_FACTORY->GetTrace(&trace) == AMF_OK && trace) {
    trace->RegisterWriter(L"OpenMediaTrace", &G_AMF_TRACE_WRITER, true);
    trace->SetWriterLevel(L"OpenMediaTrace", AMF_TRACE_INFO);
  }

  return true;
}

static auto get_amf_decoder_id(OMCodecId codec_id) -> const wchar_t* {
  switch (codec_id) {
    case OM_CODEC_H264: return AMFVideoDecoderUVD_H264_AVC;
    case OM_CODEC_H265: return AMFVideoDecoderHW_H265_HEVC;
    case OM_CODEC_VP9: return AMFVideoDecoderHW_VP9;
    case OM_CODEC_AV1: return AMFVideoDecoderHW_AV1;
    default: return nullptr;
  }
}

static auto get_amf_encoder_id(OMCodecId codec_id) -> const wchar_t* {
  switch (codec_id) {
    case OM_CODEC_H264: return AMFVideoEncoderVCE_AVC;
    case OM_CODEC_H265: return AMFVideoEncoder_HEVC;
    case OM_CODEC_AV1: return AMFVideoEncoder_AV1;
    default: return nullptr;
  }
}

static auto get_amf_format(OMPixelFormat fmt) -> amf::AMF_SURFACE_FORMAT {
  switch (fmt) {
    case OM_FORMAT_NV12: return amf::AMF_SURFACE_NV12;
    case OM_FORMAT_YUV420P: return amf::AMF_SURFACE_YUV420P;
    case OM_FORMAT_P010: return amf::AMF_SURFACE_P010;
    case OM_FORMAT_R8G8B8A8: return amf::AMF_SURFACE_RGBA;
    case OM_FORMAT_B8G8R8A8: return amf::AMF_SURFACE_BGRA;
    case OM_FORMAT_GRAY8: return amf::AMF_SURFACE_GRAY8;
    default: return amf::AMF_SURFACE_NV12;
  }
}

static auto get_om_format(amf::AMF_SURFACE_FORMAT fmt) -> OMPixelFormat {
  switch (fmt) {
    case amf::AMF_SURFACE_NV12: return OM_FORMAT_NV12;
    case amf::AMF_SURFACE_YUV420P: return OM_FORMAT_YUV420P;
    case amf::AMF_SURFACE_P010: return OM_FORMAT_P010;
    case amf::AMF_SURFACE_RGBA: return OM_FORMAT_R8G8B8A8;
    case amf::AMF_SURFACE_BGRA: return OM_FORMAT_B8G8R8A8;
    default: return OM_FORMAT_NV12;
  }
}

static auto map_primaries_to_amf(OMColorPrimaries p) -> AMF_COLOR_PRIMARIES_ENUM {
  switch (p) {
    case OM_PRIMARIES_BT709: return AMF_COLOR_PRIMARIES_BT709;
    case OM_PRIMARIES_BT2020: return AMF_COLOR_PRIMARIES_BT2020;
    case OM_PRIMARIES_BT601: return AMF_COLOR_PRIMARIES_SMPTE170M;
    default: return AMF_COLOR_PRIMARIES_UNDEFINED;
  }
}

static auto map_transfer_to_amf(OMTransferCharacteristic t) -> AMF_COLOR_TRANSFER_CHARACTERISTIC_ENUM {
  switch (t) {
    case OM_TRANSFER_BT709: return AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709;
    case OM_TRANSFER_PQ: return AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084;
    case OM_TRANSFER_HLG: return AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
    case OM_TRANSFER_GAMMA22: return AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22;
    default: return AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED;
  }
}

static auto map_matrix_to_amf(OMColorSpace c) -> AMF_COLOR_MATRIX_COEFF_ENUM {
  switch (c) {
    case OM_COLOR_SPACE_BT709: return AMF_COLOR_MATRIX_COEFF_BT_709;
    case OM_COLOR_SPACE_BT2020: return AMF_COLOR_MATRIX_COEFF_BT_2020_NCL;
    case OM_COLOR_SPACE_BT601: return AMF_COLOR_MATRIX_COEFF_BT_601;
    default: return AMF_COLOR_MATRIX_COEFF_UNSPECIFIED;
  }
}

static auto map_primaries_from_amf(amf_int64 p) -> OMColorPrimaries {
  switch (p) {
    case AMF_COLOR_PRIMARIES_BT709: return OM_PRIMARIES_BT709;
    case AMF_COLOR_PRIMARIES_BT2020: return OM_PRIMARIES_BT2020;
    case AMF_COLOR_PRIMARIES_SMPTE170M: return OM_PRIMARIES_BT601;
    default: return OM_PRIMARIES_UNKNOWN;
  }
}

static auto map_transfer_from_amf(amf_int64 t) -> OMTransferCharacteristic {
  switch (t) {
    case AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709: return OM_TRANSFER_BT709;
    case AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084: return OM_TRANSFER_PQ;
    case AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67: return OM_TRANSFER_HLG;
    case AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22: return OM_TRANSFER_GAMMA22;
    default: return OM_TRANSFER_UNKNOWN;
  }
}

static auto map_matrix_from_amf(amf_int64 m) -> OMColorSpace {
  switch (m) {
    case AMF_COLOR_MATRIX_COEFF_BT_709: return OM_COLOR_SPACE_BT709;
    case AMF_COLOR_MATRIX_COEFF_BT_2020_NCL: return OM_COLOR_SPACE_BT2020;
    case AMF_COLOR_MATRIX_COEFF_BT_601: return OM_COLOR_SPACE_BT601;
    default: return OM_COLOR_SPACE_UNKNOWN;
  }
}

struct AMFContextInitResult {
  AMF_RESULT status = AMF_FAIL;
  HWDeviceType device_type = HWDeviceType::NONE;
  amf::AMFContextPtr context;
  amf::AMFContext1Ptr context1;
  amf::AMFContext2Ptr context2;
  OMDX11Context* owned_dx11_context = nullptr;
};

static auto initAMFContext(const std::optional<HWDevice>& hw_device) -> AMFContextInitResult {
  AMFContextInitResult result = {};
  amf::AMFContextPtr ctx;
  AMF_RESULT res = G_AMF_FACTORY->CreateContext(&ctx);
  if (res != AMF_OK || !ctx) return result;
  result.context = ctx;

  amf::AMFContext1Ptr ctx1;
  amf::AMFContext2Ptr ctx2;
  ctx->QueryInterface(amf::AMFContext1::IID(), reinterpret_cast<void**>(&ctx1));
  if (ctx1) {
    result.context1 = ctx1;
    ctx1->QueryInterface(amf::AMFContext2::IID(), reinterpret_cast<void**>(&ctx2));
    if (ctx2) result.context2 = ctx2;
  }

  auto initDefaultDX11 = [&]() -> AMF_RESULT {
    OMDX11Init init = {};
    init.adapter_index = -1;
    OMDX11Context* dx11_ctx = HWD3D11Context_create(init);
    if (!dx11_ctx) return AMF_DIRECTX_FAILED;
    ID3D11Device* device = HWD3D11Context_getDevice(dx11_ctx);
    if (!device) {
      HWD3D11Context_delete(dx11_ctx);
      return AMF_DIRECTX_FAILED;
    }
    AMF_RESULT init_res = result.context->InitDX11(device);
    if (init_res != AMF_OK) {
      HWD3D11Context_delete(dx11_ctx);
      return init_res;
    }
    result.owned_dx11_context = dx11_ctx;
    result.device_type = HWDeviceType::DX11;
    return AMF_OK;
  };

  if (hw_device) {
    switch (hw_device->type) {
      case HWDeviceType::DX11: {
        ID3D11Device* d3d11_dev = HWD3D11Context_getDevice(static_cast<OMDX11Context*>(hw_device->context));
        res = result.context->InitDX11(d3d11_dev);
        if (res == AMF_OK) result.device_type = HWDeviceType::DX11;
        break;
      }
      case HWDeviceType::DX12: {
        if (result.context2) {
          ID3D12CommandQueue* queue = HWD3D12Context_getCommandQueue(static_cast<OMDX12Context*>(hw_device->context));
          res = result.context2->InitDX12(queue);
          if (res == AMF_OK) result.device_type = HWDeviceType::DX12;
        } else
          res = AMF_NOT_SUPPORTED;
        break;
      }
      case HWDeviceType::VULKAN: {
        if (result.context1) {
          auto* vk_ctx = static_cast<OMVulkanContext*>(hw_device->context);
          amf::AMFVulkanDevice amf_vk_dev = {};
          amf_vk_dev.cbSizeof = sizeof(amf_vk_dev);
          amf_vk_dev.hInstance = HWVulkanContext_getInstance(vk_ctx);
          amf_vk_dev.hPhysicalDevice = HWVulkanContext_getPhysicalDevice(vk_ctx);
          amf_vk_dev.hDevice = HWVulkanContext_getDevice(vk_ctx);
          res = result.context1->InitVulkan(&amf_vk_dev);
          if (res == AMF_OK) result.device_type = HWDeviceType::VULKAN;
        } else
          res = AMF_NOT_SUPPORTED;
        break;
      }
      case HWDeviceType::NONE:
        res = AMF_OK;
        result.device_type = HWDeviceType::NONE;
        break;
      default: res = initDefaultDX11(); break;
    }
    if (res != AMF_OK && hw_device->type != HWDeviceType::NONE) res = initDefaultDX11();
  } else
    res = initDefaultDX11();

  result.status = res;
  return result;
}

class AMFHardwarePicture : public HardwarePicture {
public:
  amf::AMFSurfacePtr surface;
  AMFHardwarePicture(amf::AMFSurfacePtr surf)
      : HardwarePicture(HWDeviceType::AMF), surface(surf) {}
  ~AMFHardwarePicture() override = default;
};

class AMFDecoder final : public Decoder {
  amf::AMFContextPtr amf_context_;
  amf::AMFComponentPtr decoder_;
  OMDX11Context* owned_dx11_context_ = nullptr;
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0, height_ = 0;
  std::vector<uint8_t> extradata_;
  dx_h264::State h264_;

public:
  ~AMFDecoder() override { close(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    close();
    codec_id_ = options.format.codec_id;
    h264_ = {};
    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 && codec_id_ != OM_CODEC_VP9 && codec_id_ != OM_CODEC_AV1) return OM_CODEC_NOT_SUPPORTED;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;
    if (!options.extradata.empty()) {
      extradata_.assign(options.extradata.begin(), options.extradata.end());
      if (codec_id_ == OM_CODEC_H264) {
        h264_.parseExtradata(options.extradata);
      }
    }

    if (!load_amf_runtime()) return OM_CODEC_HWACCEL_FAILED;
    auto init_result = initAMFContext(options.hw_device);
    if (init_result.status != AMF_OK) return OM_CODEC_HWACCEL_FAILED;
    amf_context_ = init_result.context;
    owned_dx11_context_ = init_result.owned_dx11_context;

    const wchar_t* decoder_id = get_amf_decoder_id(codec_id_);
    if (!decoder_id) return OM_CODEC_NOT_SUPPORTED;
    AMF_RESULT res = G_AMF_FACTORY->CreateComponent(amf_context_.GetPtr(), decoder_id, &decoder_);
    if (res != AMF_OK || !decoder_) return OM_CODEC_HWACCEL_FAILED;

    if (!extradata_.empty()) {
      amf::AMFBufferPtr extradata_buf;
      if (amf_context_->AllocBuffer(amf::AMF_MEMORY_HOST, extradata_.size(), &extradata_buf) == AMF_OK) {
        memcpy(extradata_buf->GetNative(), extradata_.data(), extradata_.size());
        decoder_->SetProperty(AMF_VIDEO_DECODER_EXTRADATA, static_cast<amf::AMFInterface*>(extradata_buf));
      }
    }

    decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_LOW_LATENCY));
    decoder_->SetProperty(AMF_VIDEO_DECODER_LOW_LATENCY, true);
    decoder_->SetProperty(AMF_TIMESTAMP_MODE, static_cast<amf_int64>(AMF_TS_PRESENTATION));
    decoder_->SetProperty(AMF_VIDEO_DECODER_SURFACE_COPY, true);
    decoder_->SetProperty(AMF_VIDEO_DECODER_SURFACE_CPU, true);

    res = decoder_->Init(amf::AMF_SURFACE_NV12, width_, height_);
    if (res != AMF_OK) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "AMF: decoder->Init failed with error {}", (int) res);
      return OM_CODEC_HWACCEL_FAILED;
    }

    output_format_ = {OM_FORMAT_NV12, width_, height_};
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
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (packet.bytes.empty()) return drainFrames(frames);

    AMF_RESULT res = submitInput(packet);
    if (res == AMF_INPUT_FULL) {
      auto out = processOutput(frames);
      if (out.isErr()) return out;
      frames = std::move(out).unwrap();
      res = submitInput(packet);
    }
    if (res != AMF_OK && res != AMF_NEED_MORE_INPUT) return Err(OM_CODEC_DECODE_FAILED);
    return processOutput(frames);
  }

  void flush() override {
    if (decoder_) decoder_->Flush();
  }

private:
  void close() {
    if (decoder_) {
      decoder_->Terminate();
      decoder_ = nullptr;
    }
    if (amf_context_) amf_context_->Terminate();
    amf_context_ = nullptr;
    if (owned_dx11_context_) {
      HWD3D11Context_delete(owned_dx11_context_);
      owned_dx11_context_ = nullptr;
    }
    initialized_ = false;
  }

  auto submitInput(const Packet& packet) -> AMF_RESULT {
    amf::AMFBufferPtr buf;
    std::span<const uint8_t> bytes = packet.bytes;
    AMF_RESULT res = amf_context_->AllocBuffer(amf::AMF_MEMORY_HOST, bytes.size(), &buf);
    if (res != AMF_OK) return res;
    memcpy(buf->GetNative(), bytes.data(), bytes.size());
    buf->SetSize(bytes.size());
    buf->SetPts(packet.pts);
    if (packet.is_keyframe) buf->SetProperty(L"IsKeyFrame", true);
    return decoder_->SubmitInput(buf);
  }

  auto processOutput(std::vector<Frame>& frames) -> Result<std::vector<Frame>, OMError> {
    int empty_queries = 0;
    while (empty_queries < 5) {
      amf::AMFDataPtr data;
      AMF_RESULT res = decoder_->QueryOutput(&data);
      if (res == AMF_EOF || res == AMF_REPEAT || res == AMF_NEED_MORE_INPUT || res == AMF_INPUT_FULL) {
        if (res == AMF_REPEAT) {
          empty_queries++;
          continue;
        }
        return Ok(std::move(frames));
      }
      if (res != AMF_OK) {
        log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "AMF: QueryOutput failed with error {}", (int) res);
        return Err(OM_CODEC_DECODE_FAILED);
      }
      if (!data) {
        empty_queries++;
        continue;
      }

      amf::AMFSurfacePtr surface;
      data->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&surface));
      if (!surface) continue;

      amf::AMFSurfacePtr host_surface;
      if (surface->GetMemoryType() == amf::AMF_MEMORY_HOST) {
        host_surface = surface;
      } else {
        res = surface->Convert(amf::AMF_MEMORY_HOST);
        if (res == AMF_OK) host_surface = surface;
      }
      if (!host_surface) {
        log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "AMF: Failed to convert surface to host memory");
        return Err(OM_CODEC_DECODE_FAILED);
      }

      Frame frame = {};
      frame.pts = data->GetPts();
      frame.dts = frame.pts;
      frame.data.emplace<Picture>(get_om_format(host_surface->GetFormat()), width_, height_);
      Picture& pic = std::get<Picture>(frame.data);

      amf_int64 primaries = AMF_COLOR_PRIMARIES_UNDEFINED, transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED, matrix = AMF_COLOR_MATRIX_COEFF_UNSPECIFIED;
      surface->GetProperty(AMF_VIDEO_COLOR_PRIMARIES, &primaries);
      surface->GetProperty(AMF_VIDEO_COLOR_TRANSFER_CHARACTERISTIC, &transfer);
      surface->GetProperty(L"ColorMatrix", &matrix);
      pic.color_primaries = map_primaries_from_amf(primaries);
      pic.transfer_char = map_transfer_from_amf(transfer);
      pic.color_space = map_matrix_from_amf(matrix);

      amf::AMFInterfacePtr hdr_intf;
      if (surface->GetProperty(AMF_VIDEO_COLOR_HDR_METADATA, &hdr_intf) == AMF_OK && hdr_intf) {
        amf::AMFBufferPtr hdr_buf;
        if (hdr_intf->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&hdr_buf)) == AMF_OK && hdr_buf) {
          const AMFHDRMetadata* hdr = static_cast<const AMFHDRMetadata*>(hdr_buf->GetNative());
          if (hdr) {
            pic.mastering_display.has_value = true;
            memcpy(pic.mastering_display.display_primaries, hdr->redPrimary, 12);
            pic.mastering_display.white_point[0] = hdr->whitePoint[0];
            pic.mastering_display.white_point[1] = hdr->whitePoint[1];
            pic.mastering_display.max_display_mastering_luminance = hdr->maxMasteringLuminance;
            pic.mastering_display.min_display_mastering_luminance = hdr->minMasteringLuminance;
            pic.content_light_level.has_value = true;
            pic.content_light_level.max_content_light_level = hdr->maxContentLightLevel;
            pic.content_light_level.max_pic_average_light_level = hdr->maxFrameAverageLightLevel;
          }
        }
      }

      for (amf_size i = 0; i < static_cast<amf_size>(std::min<amf_size>(host_surface->GetPlanesCount(), pic.planes.getPlaneCount())); ++i) {
        amf::AMFPlane* plane = host_surface->GetPlaneAt(i);
        const uint8_t* src = static_cast<const uint8_t*>(plane->GetNative());
        uint8_t* dst = pic.planes.getData(i);
        const uint32_t dst_stride = pic.planes.getLinesize(i);
        const size_t row_bytes = std::min<size_t>(static_cast<size_t>(plane->GetWidth()) * plane->GetPixelSizeInBytes(), dst_stride);
        const size_t rows = std::min<size_t>(static_cast<size_t>(plane->GetHeight()), pic.getPlaneDimensions(static_cast<uint32_t>(i)).second);
        const int src_pitch = plane->GetHPitch();
        for (size_t row = 0; row < rows; ++row) std::memcpy(dst + row * dst_stride, src + row * src_pitch, row_bytes);
      }
      frames.push_back(std::move(frame));
      empty_queries = 0;
    }
    return Ok(std::move(frames));
  }

  auto drainFrames(std::vector<Frame>& frames) -> Result<std::vector<Frame>, OMError> {
    if (decoder_) decoder_->Drain();
    while (true) {
      const size_t before = frames.size();
      auto result = processOutput(frames);
      if (!result.isOk()) return result;
      frames = std::move(result).unwrap();
      if (frames.size() == before) break;
    }
    return Ok(std::move(frames));
  }
};

class AMFEncoder final : public Encoder {
  amf::AMFContextPtr amf_context_;
  amf::AMFComponentPtr encoder_;
  OMDX11Context* owned_dx11_context_ = nullptr;
  bool initialized_ = false;
  VideoFormat input_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0, height_ = 0, bitrate_ = 0, qp_ = 0;
  Rational framerate_ = {};
  HWDeviceType device_type_ = HWDeviceType::NONE;

public:
  ~AMFEncoder() override { close(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    close();
    codec_id_ = options.format.codec_id;
    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 && codec_id_ != OM_CODEC_AV1) return OM_CODEC_NOT_SUPPORTED;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    framerate_ = options.format.video.framerate;
    const auto& rc = options.rate_control;
    if (auto* cqp = std::get_if<CqpParams>(&rc.params))
      qp_ = cqp->qp_i;
    else if (auto* cbr = std::get_if<CbrParams>(&rc.params))
      bitrate_ = cbr->bitrate.target_bitrate;
    else if (auto* vbr = std::get_if<VbrParams>(&rc.params))
      bitrate_ = vbr->bitrate.target_bitrate;

    if (!load_amf_runtime()) return OM_CODEC_HWACCEL_FAILED;
    auto init_result = initAMFContext(options.hw_device);
    if (init_result.status != AMF_OK) return OM_CODEC_HWACCEL_FAILED;
    amf_context_ = init_result.context;
    owned_dx11_context_ = init_result.owned_dx11_context;
    device_type_ = init_result.device_type;

    const wchar_t* encoder_id = get_amf_encoder_id(codec_id_);
    if (!encoder_id) return OM_CODEC_NOT_SUPPORTED;
    AMF_RESULT res = G_AMF_FACTORY->CreateComponent(amf_context_.GetPtr(), encoder_id, &encoder_);
    if (res != AMF_OK || !encoder_) return OM_CODEC_HWACCEL_FAILED;

    encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFSize(width_, height_));
    encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_TRANSCODING));
    encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFRate {static_cast<amf_uint32>(framerate_.num), static_cast<amf_uint32>(framerate_.den)});
    encoder_->SetProperty(AMF_VIDEO_COLOR_PRIMARIES, static_cast<amf_int64>(map_primaries_to_amf(options.format.video.color_primaries)));
    encoder_->SetProperty(AMF_VIDEO_COLOR_TRANSFER_CHARACTERISTIC, static_cast<amf_int64>(map_transfer_to_amf(options.format.video.transfer_char)));
    encoder_->SetProperty(L"ColorMatrix", static_cast<amf_int64>(map_matrix_to_amf(options.format.video.color_space)));
    if (options.video_format.format == OM_FORMAT_P010) {
      if (codec_id_ == OM_CODEC_H265)
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, static_cast<amf_int64>(AMF_COLOR_BIT_DEPTH_10));
      else if (codec_id_ == OM_CODEC_AV1)
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, static_cast<amf_int64>(AMF_COLOR_BIT_DEPTH_10));
    }
    if (bitrate_ > 0) {
      encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(bitrate_));
      encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
    } else if (qp_ > 0) {
      encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP));
      encoder_->SetProperty(AMF_VIDEO_ENCODER_QP_I, static_cast<amf_int64>(qp_));
      encoder_->SetProperty(AMF_VIDEO_ENCODER_QP_P, static_cast<amf_int64>(qp_));
    }

    res = encoder_->Init(get_amf_format(options.video_format.format), width_, height_);
    if (res != AMF_OK) return OM_CODEC_HWACCEL_FAILED;

    input_format_ = options.video_format;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    if (!initialized_) return {};
    EncodingInfo info = {};
    amf::AMFInterfacePtr extradata;
    if (encoder_->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &extradata) == AMF_OK && extradata) {
      amf::AMFBufferPtr buf;
      extradata->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buf));
      if (buf) info.extradata.assign(static_cast<const uint8_t*>(buf->GetNative()), static_cast<const uint8_t*>(buf->GetNative()) + buf->GetSize());
    }
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    std::vector<Packet> packets;
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    AMF_RESULT res = submitFrame(frame);
    if (res == AMF_INPUT_FULL) {
      auto out = processOutput(packets);
      if (out.isErr()) return out;
      packets = std::move(out).unwrap();
      res = submitFrame(frame);
    }
    if (res != AMF_OK) return Err(OM_CODEC_ENCODE_FAILED);
    return processOutput(packets);
  }

  auto updateBitrate(const RateControlParams&) -> OMError override { return OM_SUCCESS; }

private:
  void close() {
    if (encoder_) {
      encoder_->Terminate();
      encoder_ = nullptr;
    }
    if (amf_context_) amf_context_->Terminate();
    amf_context_ = nullptr;
    if (owned_dx11_context_) {
      HWD3D11Context_delete(owned_dx11_context_);
      owned_dx11_context_ = nullptr;
    }
    initialized_ = false;
  }

  auto submitFrame(const Frame& frame) -> AMF_RESULT {
    const auto& pic = std::get<Picture>(frame.data);
    amf::AMFSurfacePtr surface;
    AMF_RESULT res = amf_context_->AllocSurface(amf::AMF_MEMORY_HOST, get_amf_format(pic.format), width_, height_, &surface);
    if (res != AMF_OK) return res;
    for (amf_int32 i = 0; i < static_cast<amf_int32>(surface->GetPlanesCount()); i++) {
      amf::AMFPlane* plane = surface->GetPlaneAt(i);
      uint8_t* dst = static_cast<uint8_t*>(plane->GetNative());
      const uint8_t* src = pic.planes.getData(i);
      const uint32_t src_stride = pic.planes.getLinesize(i);
      const size_t row_bytes = std::min<size_t>(static_cast<size_t>(plane->GetWidth()) * plane->GetPixelSizeInBytes(), src_stride);
      for (size_t row = 0; row < static_cast<size_t>(plane->GetHeight()); ++row) std::memcpy(dst + row * plane->GetHPitch(), src + row * src_stride, row_bytes);
    }
    surface->SetPts(static_cast<amf_pts>(frame.pts));
    if (pic.mastering_display.has_value) {
      amf::AMFBufferPtr hdr_buf;
      if (amf_context_->AllocBuffer(amf::AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &hdr_buf) == AMF_OK && hdr_buf) {
        AMFHDRMetadata* hdr = static_cast<AMFHDRMetadata*>(hdr_buf->GetNative());
        memcpy(hdr->redPrimary, pic.mastering_display.display_primaries, 12);
        hdr->whitePoint[0] = pic.mastering_display.white_point[0];
        hdr->whitePoint[1] = pic.mastering_display.white_point[1];
        hdr->maxMasteringLuminance = pic.mastering_display.max_display_mastering_luminance;
        hdr->minMasteringLuminance = pic.mastering_display.min_display_mastering_luminance;
        hdr->maxContentLightLevel = pic.content_light_level.max_content_light_level;
        hdr->maxFrameAverageLightLevel = pic.content_light_level.max_pic_average_light_level;
        surface->SetProperty(AMF_VIDEO_COLOR_HDR_METADATA, static_cast<amf::AMFInterface*>(hdr_buf));
      }
    }
    return encoder_->SubmitInput(surface);
  }

  auto processOutput(std::vector<Packet>& packets) -> Result<std::vector<Packet>, OMError> {
    while (true) {
      amf::AMFDataPtr data;
      AMF_RESULT res = encoder_->QueryOutput(&data);
      if (res == AMF_EOF || res == AMF_INPUT_FULL || res == AMF_NEED_MORE_INPUT || res == AMF_REPEAT) return Ok(std::move(packets));
      if (res != AMF_OK || !data) break;
      amf::AMFBufferPtr buf;
      data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buf));
      if (!buf) continue;
      Packet packet = {};
      packet.allocate(buf->GetSize());
      std::memcpy(packet.bytes.data(), buf->GetNative(), buf->GetSize());
      packet.pts = data->GetPts();
      packet.dts = packet.pts;
      packets.push_back(std::move(packet));
    }
    return Ok(std::move(packets));
  }
};

const CodecDescriptor CODEC_AMF_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_h264",
    .long_name = "AMD AMF H.264/AVC",
    .vendor = "AMD",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
    .encoder_factory = [] { return std::make_unique<AMFEncoder>(); },
};
const CodecDescriptor CODEC_AMF_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_h265",
    .long_name = "AMD AMF H.265/HEVC",
    .vendor = "AMD",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
    .encoder_factory = [] { return std::make_unique<AMFEncoder>(); },
};
const CodecDescriptor CODEC_AMF_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "amf_av1",
    .long_name = "AMD AMF AV1",
    .vendor = "AMD",
    .flags = HARDWARE,
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
    .decoder_factory = [] { return std::make_unique<AMFDecoder>(); },
};

} // namespace openmedia
