#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#include <openmedia/hw_vulkan.h>
#include <openmedia/log.hpp>
#include <openmedia/video.hpp>

#include <components/ColorSpace.h>
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

#include <algorithm>
#include <chrono>
#include <codecs.hpp>
#include <cstring>
#include <optional>
#include <thread>
#include <memory>
#include <util/color_codes.hpp>
#include <vector>
#include <video/decode_report.hpp>
#include <video/hdr_sei.hpp>

#include "dx_h264.hpp"
#include "dx_h265.hpp"

namespace openmedia {
namespace {

// The runtime ships with the display driver and stays loaded for the process:
// the factory owns every context and component made through it, so there is no
// later point at which unloading it would be safe.
auto amfFactory() -> amf::AMFFactory* {
  struct Runtime {
    amf::AMFFactory* factory = nullptr;

    Runtime() {
      HMODULE module = LoadLibraryW(AMF_DLL_NAME);
      if (!module) return;

      auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(module, AMF_INIT_FUNCTION_NAME));
      if (!init || init(AMF_FULL_VERSION, &factory) != AMF_OK) {
        factory = nullptr;
        FreeLibrary(module);
        return;
      }

      amf_uint64 version = 0;
      if (auto query = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(module, AMF_QUERY_VERSION_FUNCTION_NAME)))
        query(&version);
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "amf: runtime {}.{}.{}.{}, built against {}.{}.{}.{}",
          AMF_GET_MAJOR_VERSION(version), AMF_GET_MINOR_VERSION(version),
          AMF_GET_SUBMINOR_VERSION(version), AMF_GET_BUILD_VERSION(version),
          AMF_VERSION_MAJOR, AMF_VERSION_MINOR, AMF_VERSION_RELEASE, AMF_VERSION_BUILD_NUM);
    }
  };

  static const Runtime runtime;
  return runtime.factory;
}

auto decoderId(OMCodecId codec_id) -> const wchar_t* {
  switch (codec_id) {
    case OM_CODEC_H264: return AMFVideoDecoderUVD_H264_AVC;
    case OM_CODEC_H265: return AMFVideoDecoderHW_H265_HEVC;
    case OM_CODEC_VP9: return AMFVideoDecoderHW_VP9;
    case OM_CODEC_AV1: return AMFVideoDecoderHW_AV1;
    default: return nullptr;
  }
}

auto surfaceFormat(OMPixelFormat format) -> amf::AMF_SURFACE_FORMAT {
  switch (format) {
    case OM_FORMAT_YUV420P: return amf::AMF_SURFACE_YUV420P;
    case OM_FORMAT_P010:
    case OM_FORMAT_P012:
    case OM_FORMAT_P016:
    case OM_FORMAT_YUV420P10:
    case OM_FORMAT_YUV420P12:
    case OM_FORMAT_YUV420P16: return amf::AMF_SURFACE_P010;
    case OM_FORMAT_R8G8B8A8: return amf::AMF_SURFACE_RGBA;
    case OM_FORMAT_B8G8R8A8: return amf::AMF_SURFACE_BGRA;
    case OM_FORMAT_GRAY8: return amf::AMF_SURFACE_GRAY8;
    default: return amf::AMF_SURFACE_NV12;
  }
}

auto pixelFormat(amf::AMF_SURFACE_FORMAT format) -> OMPixelFormat {
  switch (format) {
    case amf::AMF_SURFACE_YUV420P: return OM_FORMAT_YUV420P;
    case amf::AMF_SURFACE_P010: return OM_FORMAT_P010;
    case amf::AMF_SURFACE_RGBA: return OM_FORMAT_R8G8B8A8;
    case amf::AMF_SURFACE_BGRA: return OM_FORMAT_B8G8R8A8;
    case amf::AMF_SURFACE_GRAY8: return OM_FORMAT_GRAY8;
    default: return OM_FORMAT_NV12;
  }
}

auto formatName(OMPixelFormat format) -> const char* {
  switch (format) {
    case OM_FORMAT_NV12: return "NV12";
    case OM_FORMAT_P010: return "P010";
    case OM_FORMAT_YUV420P: return "YUV420P";
    case OM_FORMAT_R8G8B8A8: return "RGBA";
    case OM_FORMAT_B8G8R8A8: return "BGRA";
    case OM_FORMAT_GRAY8: return "GRAY8";
    default: return "?";
  }
}

void copyRows(uint8_t* dst, size_t dst_stride, const uint8_t* src, size_t src_stride, size_t row_bytes, size_t rows) {
  for (size_t row = 0; row < rows; ++row) std::memcpy(dst + row * dst_stride, src + row * src_stride, row_bytes);
}

// AMF states its colour enums as the ISO/IEC 23001-8 code points the bitstream
// itself carries, so they go through the same tables as every demuxer and parser
// rather than through a mapping of their own.
void readColorDescription(amf::AMFSurface& surface, Picture& picture) {
  amf_int64 value = 0;
  if (surface.GetProperty(AMF_VIDEO_COLOR_PRIMARIES, &value) == AMF_OK) {
    if (const auto primaries = color_codes::primariesFromCode(static_cast<uint32_t>(value));
        primaries != OM_PRIMARIES_UNKNOWN)
      picture.color_primaries = primaries;
  }
  if (surface.GetProperty(AMF_VIDEO_COLOR_TRANSFER_CHARACTERISTIC, &value) == AMF_OK) {
    if (const auto transfer = color_codes::transferFromCode(static_cast<uint32_t>(value));
        transfer != OM_TRANSFER_UNKNOWN)
      picture.transfer_char = transfer;
  }
  if (surface.GetProperty(AMF_VIDEO_COLOR_RANGE, &value) == AMF_OK && value != AMF_COLOR_RANGE_UNDEFINED)
    picture.color_range = value == AMF_COLOR_RANGE_FULL ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;

  // There is no matrix coefficient property on an AMF surface, so the matrix
  // stays whatever the container stated.
  amf::AMFInterfacePtr hdr_interface;
  if (surface.GetProperty(AMF_VIDEO_COLOR_HDR_METADATA, &hdr_interface) != AMF_OK || !hdr_interface) return;
  amf::AMFBufferPtr hdr_buffer;
  if (hdr_interface->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&hdr_buffer)) != AMF_OK ||
      !hdr_buffer || hdr_buffer->GetSize() < sizeof(AMFHDRMetadata))
    return;
  const auto* hdr = static_cast<const AMFHDRMetadata*>(hdr_buffer->GetNative());
  if (!hdr) return;

  // AMFHDRMetadata normalises the primaries to 50000 and the luminances to
  // 10000, which is what OMMasteringDisplayMetadata carries as well.
  auto& mastering = picture.mastering_display;
  mastering.display_primaries[0][0] = hdr->redPrimary[0];
  mastering.display_primaries[0][1] = hdr->redPrimary[1];
  mastering.display_primaries[1][0] = hdr->greenPrimary[0];
  mastering.display_primaries[1][1] = hdr->greenPrimary[1];
  mastering.display_primaries[2][0] = hdr->bluePrimary[0];
  mastering.display_primaries[2][1] = hdr->bluePrimary[1];
  mastering.white_point[0] = hdr->whitePoint[0];
  mastering.white_point[1] = hdr->whitePoint[1];
  mastering.max_display_mastering_luminance = hdr->maxMasteringLuminance;
  mastering.min_display_mastering_luminance = hdr->minMasteringLuminance;
  mastering.has_value = true;

  picture.content_light_level.max_content_light_level = hdr->maxContentLightLevel;
  picture.content_light_level.max_pic_average_light_level = hdr->maxFrameAverageLightLevel;
  picture.content_light_level.has_value = true;
}

auto hdrMetadata(const OMMasteringDisplayMetadata& mastering, const OMContentLightLevel& light) -> AMFHDRMetadata {
  AMFHDRMetadata hdr = {};
  hdr.redPrimary[0] = mastering.display_primaries[0][0];
  hdr.redPrimary[1] = mastering.display_primaries[0][1];
  hdr.greenPrimary[0] = mastering.display_primaries[1][0];
  hdr.greenPrimary[1] = mastering.display_primaries[1][1];
  hdr.bluePrimary[0] = mastering.display_primaries[2][0];
  hdr.bluePrimary[1] = mastering.display_primaries[2][1];
  hdr.whitePoint[0] = mastering.white_point[0];
  hdr.whitePoint[1] = mastering.white_point[1];
  hdr.maxMasteringLuminance = mastering.max_display_mastering_luminance;
  hdr.minMasteringLuminance = mastering.min_display_mastering_luminance;
  hdr.maxContentLightLevel = light.max_content_light_level;
  hdr.maxFrameAverageLightLevel = light.max_pic_average_light_level;
  return hdr;
}

auto deviceName(HWDeviceType type) -> const char* {
  switch (type) {
    case HWDeviceType::DX11: return "D3D11";
    case HWDeviceType::DX12: return "D3D12";
    case HWDeviceType::VULKAN: return "Vulkan";
    default: return "unsupported";
  }
}

auto bindDevice(amf::AMFContext* context, const HWDevice& device) -> AMF_RESULT {
  switch (device.type) {
    case HWDeviceType::DX11: {
      auto* dx11 = static_cast<OMDX11Context*>(device.context);
      ID3D11Device* d3d11_device = dx11 ? HWD3D11Context_getDevice(dx11) : nullptr;
      return d3d11_device ? context->InitDX11(d3d11_device) : AMF_INVALID_ARG;
    }
    case HWDeviceType::DX12: {
      amf::AMFContext2Ptr context2;
      if (context->QueryInterface(amf::AMFContext2::IID(), reinterpret_cast<void**>(&context2)) != AMF_OK || !context2)
        return AMF_NOT_SUPPORTED;
      auto* dx12 = static_cast<OMDX12Context*>(device.context);
      ID3D12CommandQueue* queue = dx12 ? HWD3D12Context_getCommandQueue(dx12) : nullptr;
      return queue ? context2->InitDX12(queue) : AMF_INVALID_ARG;
    }
    case HWDeviceType::VULKAN: {
      amf::AMFContext1Ptr context1;
      if (context->QueryInterface(amf::AMFContext1::IID(), reinterpret_cast<void**>(&context1)) != AMF_OK || !context1)
        return AMF_NOT_SUPPORTED;
      auto* vulkan = static_cast<OMVulkanContext*>(device.context);
      if (!vulkan) return AMF_INVALID_ARG;
      amf::AMFVulkanDevice vulkan_device = {};
      vulkan_device.cbSizeof = sizeof(vulkan_device);
      vulkan_device.hInstance = HWVulkanContext_getInstance(vulkan);
      vulkan_device.hPhysicalDevice = HWVulkanContext_getPhysicalDevice(vulkan);
      vulkan_device.hDevice = HWVulkanContext_getDevice(vulkan);
      return context1->InitVulkan(&vulkan_device);
    }
    default: return AMF_NOT_SUPPORTED;
  }
}

struct BoundContext {
  amf::AMFContextPtr context;
  const char* device = "none";

  explicit operator bool() const { return context != nullptr; }
};

// A context binds to one device and cannot be re-bound, so a device AMF will not
// take is answered with a fresh context on a device of its own. Letting AMF make
// that device rather than making one here is what keeps it on the AMD GPU of a
// machine that has more than one adapter.
auto createContext(const std::optional<HWDevice>& hw_device) -> BoundContext {
  amf::AMFFactory* factory = amfFactory();
  if (!factory) return {};

  if (hw_device && hw_device->type != HWDeviceType::NONE) {
    amf::AMFContextPtr context;
    if (factory->CreateContext(&context) == AMF_OK && context) {
      const AMF_RESULT res = bindDevice(context, *hw_device);
      if (res == AMF_OK) return {context, deviceName(hw_device->type)};
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING, "amf: cannot share the application's {} device ({}), using its own",
          deviceName(hw_device->type), (int) res);
      context->Terminate();
    }
  }

  amf::AMFContextPtr context;
  if (factory->CreateContext(&context) != AMF_OK || !context) return {};
  if (const AMF_RESULT res = context->InitDX11(nullptr); res != AMF_OK) {
    log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "amf: InitDX11 failed ({})", (int) res);
    context->Terminate();
    return {};
  }
  return {context, "its own D3D11"};
}

} // namespace

class AMFDecoder final : public Decoder, private DecodeReport {
  static constexpr auto kDrainTimeout = std::chrono::milliseconds(500);
  static constexpr auto kSubmitTimeout = std::chrono::milliseconds(500);

  amf::AMFContextPtr context_;
  amf::AMFComponentPtr decoder_;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;

  // AMF answers nothing when asked what colour it decoded, so for H.26x the
  // description is taken from the bitstream instead. The parser is only here
  // until a sequence parameter set turns up, since neither is small.
  std::unique_ptr<dx_h264::State> h264_parser_;
  std::unique_ptr<video_parser::H265AccessUnitParser> h265_parser_;
  uint8_t bit_depth_ = 0;

public:
  AMFDecoder() : DecodeReport("amf") {}
  ~AMFDecoder() override { close(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    close();

    const wchar_t* component_id = decoderId(options.format.codec_id);
    if (!component_id) return OM_CODEC_NOT_SUPPORTED;
    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;
    output_format_ = {};

    amf::AMFFactory* factory = amfFactory();
    if (!factory) return OM_CODEC_HWACCEL_FAILED;
    const BoundContext bound = createContext(options.hw_device);
    if (!bound) return OM_CODEC_HWACCEL_FAILED;
    context_ = bound.context;

    if (factory->CreateComponent(context_, component_id, &decoder_) != AMF_OK || !decoder_)
      return OM_CODEC_HWACCEL_FAILED;

    if (!options.extradata.empty()) setExtradata(options.extradata);

    // Pictures are read back to host memory, so the decoder is asked for copies
    // rather than for the surfaces its DPB still references.
    decoder_->SetProperty(AMF_VIDEO_DECODER_SURFACE_COPY, true);
    decoder_->SetProperty(AMF_VIDEO_DECODER_SURFACE_CPU, true);
    // Timestamps are passed through, so what comes back out is the packet's own
    // pts in the stream's time base and not AMF's 100ns units.
    decoder_->SetProperty(AMF_TIMESTAMP_MODE, static_cast<amf_int64>(AMF_TS_PRESENTATION));
    // Low latency mode keeps no DPB at all, which hands a stream with B-frames
    // back in decode order.
    decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_REGULAR));

    if (codec_id_ == OM_CODEC_H264) h264_parser_ = std::make_unique<dx_h264::State>();
    if (codec_id_ == OM_CODEC_H265) h265_parser_ = std::make_unique<video_parser::H265AccessUnitParser>();
    output_format_.color_space = options.format.video.color_space;
    output_format_.transfer_char = options.format.video.transfer_char;
    output_format_.color_primaries = options.format.video.color_primaries;
    output_format_.color_range = options.format.video.color_range;
    output_format_.mastering_display = options.format.video.mastering_display;
    output_format_.content_light_level = options.format.video.content_light_level;
    readBitstreamColor(options.extradata);

    // Asking for NV12 where the stream is 10-bit makes AMF hand back an 8-bit
    // picture, so the depth the bitstream states decides what to ask for.
    const amf::AMF_SURFACE_FORMAT requested =
        bit_depth_ > 8 ? amf::AMF_SURFACE_P010 : surfaceFormat(options.format.video.format);
    if (const AMF_RESULT res = decoder_->Init(requested, static_cast<amf_int32>(width_), static_cast<amf_int32>(height_));
        res != AMF_OK) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "amf: decoder init failed ({})", (int) res);
      return OM_CODEC_HWACCEL_FAILED;
    }

    amf_int64 negotiated = requested;
    decoder_->GetProperty(AMF_VIDEO_DECODER_OUTPUT_FORMAT, &negotiated);

    output_format_.format = pixelFormat(static_cast<amf::AMF_SURFACE_FORMAT>(negotiated));
    output_format_.width = width_;
    output_format_.height = height_;

    initialized_ = true;
    log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, "amf: decoding {}x{} into {} on {} device",
        width_, height_, formatName(output_format_.format), bound.device);
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
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);

    std::vector<Frame> frames;
    if (packet.bytes.empty()) {
      if (const OMError err = drain(frames); err != OM_SUCCESS) return Err(err);
      return Ok(std::move(frames));
    }

    // HDR10 static metadata is only in the bitstream, and the hardware keeps
    // nothing of what it read, so it is taken out on the way past.
    readBitstreamColor(packet.bytes);
    if (codec_id_ == OM_CODEC_H264 || codec_id_ == OM_CODEC_H265) {
      hdr_sei::parseAnnexB(packet.bytes, codec_id_ == OM_CODEC_H265, output_format_.mastering_display,
                           output_format_.content_light_level);
    }

    if (const OMError err = submit(packet, frames); err != OM_SUCCESS) return Err(err);
    if (auto state = collectOutput(frames); state.isErr()) return Err(std::move(state).unwrapErr());
    return Ok(std::move(frames));
  }

  void flush() override {
    resetReceiveState();
    if (decoder_) decoder_->Flush();
  }

private:
  enum class DecoderState {
    NeedsInput,
    EndOfStream,
  };

  void close() {
    if (decoder_) {
      decoder_->Terminate();
      decoder_ = nullptr;
    }
    if (context_) {
      context_->Terminate();
      context_ = nullptr;
    }
    h264_parser_.reset();
    h265_parser_.reset();
    bit_depth_ = 0;
    initialized_ = false;
  }

  // Parameter sets reach a decoder either in the extradata or in the stream, so
  // both are offered here until one of them yields the colour description.
  void readBitstreamColor(std::span<const uint8_t> annexb) {
    if (annexb.empty()) return;
    if (h264_parser_) {
      h264_parser_->parseExtradata(annexb);
      bit_depth_ = std::max(bit_depth_, dx_h264::lumaBitDepth(*h264_parser_));
      if (dx_h264::applyColorDescription(*h264_parser_, output_format_)) h264_parser_.reset();
    } else if (h265_parser_) {
      h265_parser_->parseExtradata(annexb);
      bit_depth_ = std::max(bit_depth_, dx_h265::lumaBitDepth(*h265_parser_));
      if (dx_h265::applyColorDescription(*h265_parser_, output_format_)) h265_parser_.reset();
    }
  }

  void setExtradata(std::span<const uint8_t> extradata) {
    amf::AMFBufferPtr buffer;
    if (context_->AllocBuffer(amf::AMF_MEMORY_HOST, extradata.size(), &buffer) != AMF_OK || !buffer) return;
    std::memcpy(buffer->GetNative(), extradata.data(), extradata.size());
    decoder_->SetProperty(AMF_VIDEO_DECODER_EXTRADATA, static_cast<amf::AMFInterface*>(buffer));
  }

  // The decoder refuses input while its queue is full or all of its surfaces are
  // out, and the only way on from either is to take the pictures it has ready and
  // offer the same packet again.
  auto submit(const Packet& packet, std::vector<Frame>& frames) -> OMError {
    amf::AMFBufferPtr buffer;
    if (context_->AllocBuffer(amf::AMF_MEMORY_HOST, packet.bytes.size(), &buffer) != AMF_OK || !buffer)
      return OM_COMMON_OUT_OF_MEMORY;
    std::memcpy(buffer->GetNative(), packet.bytes.data(), packet.bytes.size());
    buffer->SetPts(packet.pts);
    if (packet.duration > 0) buffer->SetDuration(packet.duration);

    const auto deadline = std::chrono::steady_clock::now() + kSubmitTimeout;
    while (true) {
      const AMF_RESULT res = decoder_->SubmitInput(buffer);
      if (res == AMF_OK || res == AMF_NEED_MORE_INPUT) return OM_SUCCESS;
      if (res != AMF_INPUT_FULL && res != AMF_DECODER_NO_FREE_SURFACES)
        return rejectFrame(OM_CODEC_DECODE_FAILED, "SubmitInput failed ({})", (int) res);

      auto state = collectOutput(frames);
      if (state.isErr()) return std::move(state).unwrapErr();
      if (std::chrono::steady_clock::now() >= deadline)
        return rejectFrame(OM_CODEC_DECODE_FAILED, "decoder stopped accepting input");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Takes every picture the decoder has ready at this moment.
  auto collectOutput(std::vector<Frame>& frames) -> Result<DecoderState, OMError> {
    while (true) {
      amf::AMFDataPtr data;
      const AMF_RESULT res = decoder_->QueryOutput(&data);
      if (res == AMF_EOF) return Ok(DecoderState::EndOfStream);
      if (res == AMF_REPEAT || res == AMF_NEED_MORE_INPUT || !data) return Ok(DecoderState::NeedsInput);
      if (res != AMF_OK) return Err(rejectFrame(OM_CODEC_DECODE_FAILED, "QueryOutput failed ({})", (int) res));

      amf::AMFSurfacePtr surface;
      if (data->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&surface)) != AMF_OK || !surface)
        continue;
      if (auto frame = toFrame(*surface)) frames.push_back(std::move(*frame));
    }
  }

  auto drain(std::vector<Frame>& frames) -> OMError {
    if (!decoder_) return OM_SUCCESS;
    decoder_->Drain();

    const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
    while (true) {
      auto state = collectOutput(frames);
      if (state.isErr()) return std::move(state).unwrapErr();
      if (state.unwrap() == DecoderState::EndOfStream) return OM_SUCCESS;
      if (std::chrono::steady_clock::now() >= deadline) {
        log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING, "amf: decoder did not finish draining within {} ms",
            static_cast<int>(kDrainTimeout.count()));
        return OM_SUCCESS;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  auto toFrame(amf::AMFSurface& surface) -> std::optional<Frame> {
    if (surface.GetMemoryType() != amf::AMF_MEMORY_HOST && surface.Convert(amf::AMF_MEMORY_HOST) != AMF_OK) {
      rejectFrame(OM_CODEC_DECODE_FAILED, "cannot read a decoded surface back to host memory");
      return std::nullopt;
    }

    Frame frame = {};
    frame.pts = surface.GetPts();
    frame.dts = frame.pts;

    auto& picture = frame.data.emplace<Picture>(pixelFormat(surface.GetFormat()), width_, height_);
    picture.color_space = output_format_.color_space;
    picture.transfer_char = output_format_.transfer_char;
    picture.color_primaries = output_format_.color_primaries;
    picture.color_range = output_format_.color_range;
    picture.mastering_display = output_format_.mastering_display;
    picture.content_light_level = output_format_.content_light_level;
    readColorDescription(surface, picture);

    const auto planes = std::min<uint32_t>(static_cast<uint32_t>(surface.GetPlanesCount()),
                                           picture.planes.getPlaneCount());
    for (uint32_t i = 0; i < planes; ++i) {
      amf::AMFPlane* plane = surface.GetPlaneAt(static_cast<amf_size>(i));
      if (!plane) break;
      const uint32_t dst_stride = picture.planes.getLinesize(i);
      const auto [plane_width, plane_height] = picture.getPlaneDimensions(i);
      copyRows(picture.planes.getData(i), dst_stride,
               static_cast<const uint8_t*>(plane->GetNative()), static_cast<size_t>(plane->GetHPitch()),
               std::min<size_t>(static_cast<size_t>(plane->GetWidth()) * plane->GetPixelSizeInBytes(), dst_stride),
               std::min<size_t>(static_cast<size_t>(plane->GetHeight()), plane_height));
    }
    return frame;
  }
};

namespace {

// Every encoder property is named after its codec -- AVC's "FrameSize" is
// "HevcFrameSize" and "Av1FrameSize" elsewhere -- and a component ignores a name
// it does not know without complaining, so a mix-up shows up not as an error but
// as an encode that quietly keeps the driver's defaults.
struct EncoderIds {
  const wchar_t* component;
  const wchar_t* frame_size;
  const wchar_t* frame_rate;
  const wchar_t* usage;
  amf_int64 usage_transcoding;
  const wchar_t* rate_control;
  const wchar_t* target_bitrate;
  const wchar_t* peak_bitrate;
  const wchar_t* qp_intra;
  const wchar_t* qp_inter;
  // AV1 states its quantizer as a 1-255 q-index where AVC and HEVC use a 0-51 QP.
  int32_t max_qp;
  const wchar_t* extradata;
  const wchar_t* color_bit_depth;
  const wchar_t* input_primaries;
  const wchar_t* input_transfer;
  const wchar_t* input_matrix;
  const wchar_t* input_hdr;
  const wchar_t* output_primaries;
  const wchar_t* output_transfer;
  const wchar_t* output_matrix;
};

constexpr EncoderIds kEncoderAVC = {
    AMFVideoEncoderVCE_AVC,
    AMF_VIDEO_ENCODER_FRAMESIZE,
    AMF_VIDEO_ENCODER_FRAMERATE,
    AMF_VIDEO_ENCODER_USAGE,
    AMF_VIDEO_ENCODER_USAGE_TRANSCODING,
    AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
    AMF_VIDEO_ENCODER_TARGET_BITRATE,
    AMF_VIDEO_ENCODER_PEAK_BITRATE,
    AMF_VIDEO_ENCODER_QP_I,
    AMF_VIDEO_ENCODER_QP_P,
    51,
    AMF_VIDEO_ENCODER_EXTRADATA,
    AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH,
    AMF_VIDEO_ENCODER_INPUT_COLOR_PRIMARIES,
    AMF_VIDEO_ENCODER_INPUT_TRANSFER_CHARACTERISTIC,
    AMF_VIDEO_ENCODER_INPUT_MATRIX_COEFF,
    AMF_VIDEO_ENCODER_INPUT_HDR_METADATA,
    AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
    AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
    AMF_VIDEO_ENCODER_OUTPUT_MATRIX_COEFF,
};

constexpr EncoderIds kEncoderHEVC = {
    AMFVideoEncoder_HEVC,
    AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
    AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
    AMF_VIDEO_ENCODER_HEVC_USAGE,
    AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING,
    AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
    AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
    AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE,
    AMF_VIDEO_ENCODER_HEVC_QP_I,
    AMF_VIDEO_ENCODER_HEVC_QP_P,
    51,
    AMF_VIDEO_ENCODER_HEVC_EXTRADATA,
    AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH,
    AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PRIMARIES,
    AMF_VIDEO_ENCODER_HEVC_INPUT_TRANSFER_CHARACTERISTIC,
    AMF_VIDEO_ENCODER_HEVC_INPUT_MATRIX_COEFF,
    AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA,
    AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES,
    AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
    AMF_VIDEO_ENCODER_HEVC_OUTPUT_MATRIX_COEFF,
};

constexpr EncoderIds kEncoderAV1 = {
    AMFVideoEncoder_AV1,
    AMF_VIDEO_ENCODER_AV1_FRAMESIZE,
    AMF_VIDEO_ENCODER_AV1_FRAMERATE,
    AMF_VIDEO_ENCODER_AV1_USAGE,
    AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING,
    AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
    AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE,
    AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE,
    AMF_VIDEO_ENCODER_AV1_Q_INDEX_INTRA,
    AMF_VIDEO_ENCODER_AV1_Q_INDEX_INTER,
    255,
    AMF_VIDEO_ENCODER_AV1_EXTRA_DATA,
    AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH,
    AMF_VIDEO_ENCODER_AV1_INPUT_COLOR_PRIMARIES,
    AMF_VIDEO_ENCODER_AV1_INPUT_TRANSFER_CHARACTERISTIC,
    AMF_VIDEO_ENCODER_AV1_INPUT_MATRIX_COEFF,
    AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA,
    AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES,
    AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC,
    AMF_VIDEO_ENCODER_AV1_OUTPUT_MATRIX_COEFF,
};

auto encoderIds(OMCodecId codec_id) -> const EncoderIds* {
  switch (codec_id) {
    case OM_CODEC_H264: return &kEncoderAVC;
    case OM_CODEC_H265: return &kEncoderHEVC;
    case OM_CODEC_AV1: return &kEncoderAV1;
    default: return nullptr;
  }
}

// The rate control enumerations are per codec as well, and their values differ:
// CBR is 1 for AVC but 3 for HEVC and AV1.
auto rateControlMethod(OMCodecId codec_id, RateControlMode mode) -> amf_int64 {
  const bool avc = codec_id == OM_CODEC_H264;
  switch (mode) {
    case RateControlMode::CQP:
    case RateControlMode::CRF:
    case RateControlMode::ICQ: return 0; // constant QP
    case RateControlMode::CBR:
    case RateControlMode::ABR: return avc ? 1 : 3;
    case RateControlMode::VBR_LAT: return avc ? 3 : 1; // latency constrained VBR
    case RateControlMode::QVBR: return 4;
    case RateControlMode::HQVBR: return 5;
    case RateControlMode::HQCBR: return 6;
    default: return 2; // peak constrained VBR
  }
}

struct RateControl {
  amf_int64 method = 0;
  std::optional<int64_t> target_bitrate;
  std::optional<int64_t> peak_bitrate;
  std::optional<int32_t> qp_intra;
  std::optional<int32_t> qp_inter;
};

auto rateControl(const RateControlParams& params, OMCodecId codec_id) -> RateControl {
  RateControl rc = {};
  rc.method = rateControlMethod(codec_id, params.getMode());

  const auto take = [&rc](const BitrateParams& bitrate) {
    rc.target_bitrate = bitrate.target_bitrate;
    rc.peak_bitrate = bitrate.max_bitrate;
  };

  const auto& variant = params.params;
  if (const auto* cqp = std::get_if<CqpParams>(&variant)) {
    rc.qp_intra = cqp->qp_i;
    rc.qp_inter = cqp->qp_p;
  } else if (const auto* crf = std::get_if<CrfParams>(&variant)) {
    rc.qp_intra = static_cast<int32_t>(crf->quality);
    rc.qp_inter = static_cast<int32_t>(crf->quality);
  } else if (const auto* cbr = std::get_if<CbrParams>(&variant)) {
    take(cbr->bitrate);
  } else if (const auto* vbr = std::get_if<VbrParams>(&variant)) {
    take(vbr->bitrate);
  } else if (const auto* abr = std::get_if<AbrParams>(&variant)) {
    rc.target_bitrate = abr->target_bitrate;
  } else if (const auto* hqcbr = std::get_if<HqcbrParams>(&variant)) {
    take(hqcbr->bitrate);
  } else if (const auto* hqvbr = std::get_if<HqvbrParams>(&variant)) {
    take(hqvbr->bitrate);
  } else if (const auto* qvbr = std::get_if<QvbrParams>(&variant)) {
    take(qvbr->bitrate);
  } else if (const auto* latency = std::get_if<VbrLatParams>(&variant)) {
    take(latency->bitrate);
  }
  return rc;
}

} // namespace

class AMFEncoder final : public Encoder {
  amf::AMFContextPtr context_;
  amf::AMFComponentPtr encoder_;
  const EncoderIds* ids_ = nullptr;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  VideoFormat input_format_ = {};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;

public:
  ~AMFEncoder() override { close(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    close();

    ids_ = encoderIds(options.format.codec_id);
    if (!ids_) return OM_CODEC_NOT_SUPPORTED;
    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;

    amf::AMFFactory* factory = amfFactory();
    if (!factory) return OM_CODEC_HWACCEL_FAILED;
    const BoundContext bound = createContext(options.hw_device);
    if (!bound) return OM_CODEC_HWACCEL_FAILED;
    context_ = bound.context;

    if (factory->CreateComponent(context_, ids_->component, &encoder_) != AMF_OK || !encoder_)
      return OM_CODEC_HWACCEL_FAILED;

    const Rational framerate = options.format.video.framerate;
    encoder_->SetProperty(ids_->usage, ids_->usage_transcoding);
    encoder_->SetProperty(ids_->frame_size, AMFConstructSize(static_cast<amf_int32>(width_), static_cast<amf_int32>(height_)));
    encoder_->SetProperty(ids_->frame_rate, AMFConstructRate(static_cast<amf_uint32>(framerate.num),
                                                             static_cast<amf_uint32>(framerate.den)));
    applyColor(options.video_format);
    applyRateControl(rateControl(options.rate_control, codec_id_));

    if (const AMF_RESULT res = encoder_->Init(surfaceFormat(options.video_format.format),
                                              static_cast<amf_int32>(width_), static_cast<amf_int32>(height_));
        res != AMF_OK) {
      log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "amf: encoder init failed ({})", (int) res);
      return OM_CODEC_HWACCEL_FAILED;
    }

    input_format_ = options.video_format;
    initialized_ = true;
    log(OM_CATEGORY_ENCODER, OM_LEVEL_INFO, "amf: encoding {}x{} {} on {} device",
        width_, height_, formatName(input_format_.format), bound.device);
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    if (!initialized_) return info;

    amf::AMFInterfacePtr extradata;
    if (encoder_->GetProperty(ids_->extradata, &extradata) == AMF_OK && extradata) {
      amf::AMFBufferPtr buffer;
      extradata->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer));
      if (buffer) {
        const auto* bytes = static_cast<const uint8_t*>(buffer->GetNative());
        info.extradata.assign(bytes, bytes + buffer->GetSize());
      }
    }
    info.mastering_display = input_format_.mastering_display;
    info.content_light_level = input_format_.content_light_level;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    const auto* picture = std::get_if<Picture>(&frame.data);
    if (!picture) return Err(OM_COMMON_INVALID_ARGUMENT);

    amf::AMFSurfacePtr surface;
    if (const OMError err = makeSurface(*picture, frame.pts, surface); err != OM_SUCCESS) return Err(err);

    // As on the decoder, a full input queue is cleared by taking the packets the
    // encoder already has and offering the same surface again.
    std::vector<Packet> packets;
    while (true) {
      const AMF_RESULT res = encoder_->SubmitInput(surface);
      if (res == AMF_OK || res == AMF_NEED_MORE_INPUT) break;
      if (res != AMF_INPUT_FULL) return Err(OM_CODEC_ENCODE_FAILED);
      if (const OMError err = collectOutput(packets); err != OM_SUCCESS) return Err(err);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (const OMError err = collectOutput(packets); err != OM_SUCCESS) return Err(err);
    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& params) -> OMError override {
    if (!initialized_) return OM_COMMON_NOT_INITIALIZED;
    const RateControl rc = rateControl(params, codec_id_);
    if (!rc.target_bitrate && !rc.qp_intra) return OM_CODEC_NOT_SUPPORTED;
    applyRateControl(rc);
    return OM_SUCCESS;
  }

private:
  void close() {
    if (encoder_) {
      encoder_->Terminate();
      encoder_ = nullptr;
    }
    if (context_) {
      context_->Terminate();
      context_ = nullptr;
    }
    initialized_ = false;
  }

  void applyRateControl(const RateControl& rc) {
    encoder_->SetProperty(ids_->rate_control, rc.method);
    if (rc.target_bitrate) encoder_->SetProperty(ids_->target_bitrate, static_cast<amf_int64>(*rc.target_bitrate));
    if (rc.peak_bitrate) encoder_->SetProperty(ids_->peak_bitrate, static_cast<amf_int64>(*rc.peak_bitrate));

    const auto quantizer = [this](int32_t qp) {
      return static_cast<amf_int64>(std::clamp(qp * ids_->max_qp / 51, 1, ids_->max_qp));
    };
    if (rc.qp_intra) encoder_->SetProperty(ids_->qp_intra, quantizer(*rc.qp_intra));
    if (rc.qp_inter) encoder_->SetProperty(ids_->qp_inter, quantizer(*rc.qp_inter));
  }

  void applyColor(const VideoFormat& format) {
    const auto primaries = static_cast<amf_int64>(color_codes::codeFromPrimaries(format.color_primaries));
    const auto transfer = static_cast<amf_int64>(color_codes::codeFromTransfer(format.transfer_char));
    const auto matrix = static_cast<amf_int64>(color_codes::matrixFromColorSpace(format.color_space));
    for (const wchar_t* id : {ids_->input_primaries, ids_->output_primaries}) encoder_->SetProperty(id, primaries);
    for (const wchar_t* id : {ids_->input_transfer, ids_->output_transfer}) encoder_->SetProperty(id, transfer);
    for (const wchar_t* id : {ids_->input_matrix, ids_->output_matrix}) encoder_->SetProperty(id, matrix);

    if (getBytesPerPixel(format.format, 0) > 1)
      encoder_->SetProperty(ids_->color_bit_depth, static_cast<amf_int64>(AMF_COLOR_BIT_DEPTH_10));

    if (!format.mastering_display.has_value) return;
    amf::AMFBufferPtr buffer;
    if (context_->AllocBuffer(amf::AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &buffer) != AMF_OK || !buffer) return;
    const AMFHDRMetadata hdr = hdrMetadata(format.mastering_display, format.content_light_level);
    std::memcpy(buffer->GetNative(), &hdr, sizeof(hdr));
    encoder_->SetProperty(ids_->input_hdr, static_cast<amf::AMFInterface*>(buffer));
  }

  auto makeSurface(const Picture& picture, int64_t pts, amf::AMFSurfacePtr& surface) -> OMError {
    if (!std::get_if<HostPicture>(&picture.buffer)) return OM_CODEC_NOT_SUPPORTED;
    if (context_->AllocSurface(amf::AMF_MEMORY_HOST, surfaceFormat(picture.format),
                               static_cast<amf_int32>(width_), static_cast<amf_int32>(height_), &surface) != AMF_OK)
      return OM_COMMON_OUT_OF_MEMORY;

    const auto planes = std::min<uint32_t>(static_cast<uint32_t>(surface->GetPlanesCount()),
                                           picture.planes.getPlaneCount());
    for (uint32_t i = 0; i < planes; ++i) {
      amf::AMFPlane* plane = surface->GetPlaneAt(static_cast<amf_size>(i));
      if (!plane) break;
      const uint32_t src_stride = picture.planes.getLinesize(i);
      const auto [plane_width, plane_height] = picture.getPlaneDimensions(i);
      copyRows(static_cast<uint8_t*>(plane->GetNative()), static_cast<size_t>(plane->GetHPitch()),
               picture.planes.getData(i), src_stride,
               std::min<size_t>(static_cast<size_t>(plane->GetWidth()) * plane->GetPixelSizeInBytes(), src_stride),
               std::min<size_t>(static_cast<size_t>(plane->GetHeight()), plane_height));
    }
    surface->SetPts(static_cast<amf_pts>(pts));
    return OM_SUCCESS;
  }

  auto collectOutput(std::vector<Packet>& packets) -> OMError {
    while (true) {
      amf::AMFDataPtr data;
      const AMF_RESULT res = encoder_->QueryOutput(&data);
      if (res == AMF_EOF || res == AMF_REPEAT || res == AMF_NEED_MORE_INPUT || !data) return OM_SUCCESS;
      if (res != AMF_OK) return OM_CODEC_ENCODE_FAILED;

      amf::AMFBufferPtr buffer;
      if (data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer)) != AMF_OK || !buffer) continue;

      Packet packet = {};
      packet.allocate(buffer->GetSize());
      std::memcpy(packet.bytes.data(), buffer->GetNative(), buffer->GetSize());
      packet.pts = data->GetPts();
      packet.dts = packet.pts;
      packets.push_back(std::move(packet));
    }
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
