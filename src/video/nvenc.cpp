#include <openmedia/hw_cuda.h>
#ifdef _WIN32
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#endif
#include <openmedia/hw_vulkan.h>
#include <codecs.hpp>
#include <mutex>
#include <openmedia/codec_api.hpp>
#include <openmedia/video.hpp>
#include <queue>
#include <variant>
#include <vector>
#include "nv_common.hpp"
#include "nv_loader.hpp"

#include "hw_common.hpp"

namespace openmedia {

class NVEnc final : public Encoder {
  OMCudaContext* hw_context_ = nullptr;
  void* encoder_ = nullptr;
  NV_ENCODE_API_FUNCTION_LIST nvenc_ = {};
  CUcontext cu_ctx_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  OMPixelFormat format_ = OM_FORMAT_NV12;

  struct InputSurface {
    NV_ENC_INPUT_PTR input_ptr = nullptr;
    NV_ENC_REGISTERED_PTR registered_ptr = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  struct OutputBuffer {
    NV_ENC_OUTPUT_PTR output_ptr = nullptr;
    void* event = nullptr;
  };

  std::vector<InputSurface> input_surfaces_;
  std::vector<OutputBuffer> output_buffers_;
  uint32_t current_index_ = 0;

  bool initialized_ = false;

public:
  NVEnc() = default;
  ~NVEnc() override { release(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (!options.hw_device.has_value() || options.hw_device->type != HWDeviceType::CUDA) {
      return OM_CODEC_HWACCEL_FAILED;
    }
    hw_context_ = static_cast<OMCudaContext*>(options.hw_device->context);
    cu_ctx_ = (CUcontext) HWCudaContext_getContext(hw_context_);

    if (!NVLoader::getInstance().load()) return OM_CODEC_HWACCEL_FAILED;
    auto* nvenc_funcs = NVLoader::getInstance().nvenc();

    nvenc_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (nvenc_funcs->NvEncodeAPICreateInstance(&nvenc_) != NV_ENC_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    width_ = options.video_format.width;
    height_ = options.video_format.height;
    format_ = options.video_format.format;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params = {};
    open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.device = cu_ctx_;
    open_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open_params.apiVersion = NVENCAPI_VERSION;

    if (nvenc_.nvEncOpenEncodeSessionEx(&open_params, &encoder_) != NV_ENC_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    NV_ENC_CONFIG config = {};
    config.version = NV_ENC_CONFIG_VER;

    NV_ENC_INITIALIZE_PARAMS init_params = {};
    init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params.encodeWidth = width_;
    init_params.encodeHeight = height_;
    init_params.darWidth = width_;
    init_params.darHeight = height_;
    init_params.frameRateNum = options.format.video.framerate.num > 0 ? options.format.video.framerate.num : 30;
    init_params.frameRateDen = options.format.video.framerate.den > 0 ? options.format.video.framerate.den : 1;
    init_params.enablePTD = 1;
    init_params.encodeConfig = &config;

    switch (options.format.codec_id) {
      case OM_CODEC_H264: init_params.encodeGUID = NV_ENC_CODEC_H264_GUID; break;
      case OM_CODEC_H265: init_params.encodeGUID = NV_ENC_CODEC_HEVC_GUID; break;
      case OM_CODEC_AV1: init_params.encodeGUID = NV_ENC_CODEC_AV1_GUID; break;
      default: return OM_CODEC_NOT_SUPPORTED;
    }

    NV_ENC_PRESET_CONFIG preset_config = {};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
    nvenc_.nvEncGetEncodePresetConfig(encoder_, init_params.encodeGUID, NV_ENC_PRESET_P4_GUID, &preset_config);
    std::memcpy(&config, &preset_config.presetCfg, sizeof(config));
    init_params.presetGUID = NV_ENC_PRESET_P4_GUID;

    // Set VUI for color space
    if (options.format.codec_id == OM_CODEC_H264) {
      config.encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag = 1;
      config.encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries = (NV_ENC_VUI_COLOR_PRIMARIES) map_color_primaries(options.format.video.color_primaries);
      config.encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics = (NV_ENC_VUI_TRANSFER_CHARACTERISTIC) map_transfer_characteristics(options.format.video.transfer_char);
      config.encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix = (NV_ENC_VUI_MATRIX_COEFFS) map_color_space(options.format.video.color_space);
      config.encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag = (options.video_format.format == OM_FORMAT_YUVJ420P);
    } else if (options.format.codec_id == OM_CODEC_H265) {
      config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag = 1;
      config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries = (NV_ENC_VUI_COLOR_PRIMARIES) map_color_primaries(options.format.video.color_primaries);
      config.encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics = (NV_ENC_VUI_TRANSFER_CHARACTERISTIC) map_transfer_characteristics(options.format.video.transfer_char);
      config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix = (NV_ENC_VUI_MATRIX_COEFFS) map_color_space(options.format.video.color_space);
      config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag = (options.video_format.format == OM_FORMAT_YUVJ420P);
    }

    applyRateControl(options.rate_control, config);

    if (nvenc_.nvEncInitializeEncoder(encoder_, &init_params) != NV_ENC_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    for (int i = 0; i < 3; ++i) {
      NV_ENC_CREATE_INPUT_BUFFER input_buf = {};
      input_buf.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
      input_buf.width = width_;
      input_buf.height = height_;
      input_buf.bufferFmt = (format_ == OM_FORMAT_P010) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
      if (nvenc_.nvEncCreateInputBuffer(encoder_, &input_buf) == NV_ENC_SUCCESS) {
        InputSurface surf = {};
        surf.input_ptr = input_buf.inputBuffer;
        surf.width = width_;
        surf.height = height_;
        input_surfaces_.push_back(surf);
      }

      NV_ENC_CREATE_BITSTREAM_BUFFER bitstream_buf = {};
      bitstream_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      if (nvenc_.nvEncCreateBitstreamBuffer(encoder_, &bitstream_buf) == NV_ENC_SUCCESS) {
        OutputBuffer out = {};
        out.output_ptr = bitstream_buf.bitstreamBuffer;
        output_buffers_.push_back(out);
      }
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override { return {}; }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (!std::holds_alternative<Picture>(frame.data)) return Err(OM_CODEC_INVALID_PARAMS);
    const auto& picture = std::get<Picture>(frame.data);

    InputSurface& surf = input_surfaces_[current_index_];

    if (std::holds_alternative<std::shared_ptr<HardwarePicture>>(picture.buffer)) {
      auto hw_pic = std::get<std::shared_ptr<HardwarePicture>>(picture.buffer);
      if (hw_pic->getType() == HWDeviceType::CUDA) {
        auto cuda_pic = std::static_pointer_cast<CudaHardwarePicture>(hw_pic);
        return encodeCuda(cuda_pic, frame.pts, picture);
#if defined(_WIN32)
      } else if (hw_pic->getType() == HWDeviceType::DX11) {
        auto dx11_pic = std::static_pointer_cast<DX11HardwarePicture>(hw_pic);
        return encodeDX11(dx11_pic, frame.pts, picture);
#endif
      }
    }

    NV_ENC_LOCK_INPUT_BUFFER lock_params = {};
    lock_params.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock_params.inputBuffer = surf.input_ptr;
    if (nvenc_.nvEncLockInputBuffer(encoder_, &lock_params) == NV_ENC_SUCCESS) {
      uint8_t* dst = (uint8_t*) lock_params.bufferDataPtr;
      for (int i = 0; i < picture.planes.getPlaneCount(); ++i) {
        auto dims = picture.getPlaneDimensions(i);
        auto src_ptr = picture.planes.getData(i);
        auto src_stride = picture.planes.getLinesize(i);
        auto dst_stride = lock_params.pitch;
        size_t bpp = getBytesPerPixel(picture.format, i);
        for (uint32_t y = 0; y < dims.second; ++y) {
          std::memcpy(dst + y * dst_stride, src_ptr + y * src_stride, dims.first * bpp);
        }
        dst += dims.second * dst_stride;
      }
      nvenc_.nvEncUnlockInputBuffer(encoder_, surf.input_ptr);
    }

    return encodeCommon(surf.input_ptr, frame.pts, picture);
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!encoder_) return OM_COMMON_NOT_INITIALIZED;
    NV_ENC_CONFIG config = {};
    config.version = NV_ENC_CONFIG_VER;
    applyRateControl(rc, config);
    NV_ENC_RECONFIGURE_PARAMS reconfig = {};
    reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfig.reInitEncodeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    reconfig.reInitEncodeParams.encodeConfig = &config;
    if (nvenc_.nvEncReconfigureEncoder(encoder_, &reconfig) != NV_ENC_SUCCESS) return OM_CODEC_HWACCEL_FAILED;
    return OM_SUCCESS;
  }

private:
  auto encodeCommon(NV_ENC_INPUT_PTR input, int64_t pts, const Picture& picture) -> Result<std::vector<Packet>, OMError> {
    OutputBuffer& out = output_buffers_[current_index_];
    NV_ENC_PIC_PARAMS pic_params = {};
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.inputBuffer = input;
    pic_params.bufferFmt = (picture.format == OM_FORMAT_P010) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    pic_params.inputWidth = width_;
    pic_params.inputHeight = height_;
    pic_params.outputBitstream = out.output_ptr;
    pic_params.inputTimeStamp = pts;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    MASTERING_DISPLAY_INFO md = {};
    CONTENT_LIGHT_LEVEL cll = {};
    NV_ENC_PIC_PARAMS_HEVC hevc_params = {};

    if (picture.mastering_display.has_value || picture.content_light_level.has_value) {
      if (picture.mastering_display.has_value) {
        md.r.x = picture.mastering_display.display_primaries[0][0];
        md.r.y = picture.mastering_display.display_primaries[0][1];
        md.g.x = picture.mastering_display.display_primaries[1][0];
        md.g.y = picture.mastering_display.display_primaries[1][1];
        md.b.x = picture.mastering_display.display_primaries[2][0];
        md.b.y = picture.mastering_display.display_primaries[2][1];
        md.whitePoint.x = picture.mastering_display.white_point[0];
        md.whitePoint.y = picture.mastering_display.white_point[1];
        md.maxLuma = picture.mastering_display.max_display_mastering_luminance;
        md.minLuma = picture.mastering_display.min_display_mastering_luminance;
        hevc_params.pMasteringDisplay = &md;
      }
      if (picture.content_light_level.has_value) {
        cll.maxContentLightLevel = picture.content_light_level.max_content_light_level;
        cll.maxPicAverageLightLevel = picture.content_light_level.max_pic_average_light_level;
        hevc_params.pMaxCll = &cll;
      }
      pic_params.codecPicParams.hevcPicParams = hevc_params;
    }

    if (nvenc_.nvEncEncodePicture(encoder_, &pic_params) != NV_ENC_SUCCESS) return Err(OM_CODEC_ENCODE_FAILED);

    std::vector<Packet> packets;
    NV_ENC_LOCK_BITSTREAM lock_bit = {};
    lock_bit.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock_bit.outputBitstream = out.output_ptr;
    if (nvenc_.nvEncLockBitstream(encoder_, &lock_bit) == NV_ENC_SUCCESS) {
      Packet pkt;
      pkt.allocate(lock_bit.bitstreamSizeInBytes);
      std::memcpy(pkt.bytes.data(), lock_bit.bitstreamBufferPtr, lock_bit.bitstreamSizeInBytes);
      pkt.pts = lock_bit.outputTimeStamp;
      packets.push_back(std::move(pkt));
      nvenc_.nvEncUnlockBitstream(encoder_, out.output_ptr);
    }

    current_index_ = (current_index_ + 1) % input_surfaces_.size();
    return Ok(std::move(packets));
  }

#if defined(_WIN32)
  auto encodeDX11(std::shared_ptr<DX11HardwarePicture> pic, int64_t pts, const Picture& picture) -> Result<std::vector<Packet>, OMError> {
    auto* cu = NVLoader::getInstance().cuda();
    CUgraphicsResource cu_resource;
    if (cu->cuGraphicsD3D11RegisterResource(&cu_resource, pic->pic->texture, CU_GRAPHICS_REGISTER_FLAGS_NONE) != CUDA_SUCCESS) return Err(OM_CODEC_ENCODE_FAILED);
    CUstream stream = (CUstream) HWCudaContext_getStream(hw_context_);
    if (cu->cuGraphicsMapResources(1, &cu_resource, stream) != CUDA_SUCCESS) {
      cu->cuGraphicsUnregisterResource(cu_resource);
      return Err(OM_CODEC_ENCODE_FAILED);
    }
    CUdeviceptr dev_ptr;
    size_t size;
    if (cu->cuGraphicsResourceGetMappedPointer(&dev_ptr, &size, cu_resource) != CUDA_SUCCESS) {
      cu->cuGraphicsUnmapResources(1, &cu_resource, stream);
      cu->cuGraphicsUnregisterResource(cu_resource);
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.resourceToRegister = (void*) dev_ptr;
    reg.width = width_;
    reg.height = height_;
    reg.pitch = width_;
    reg.bufferFormat = (picture.format == OM_FORMAT_P010) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (nvenc_.nvEncRegisterResource(encoder_, &reg) != NV_ENC_SUCCESS) {
      cu->cuGraphicsUnmapResources(1, &cu_resource, stream);
      cu->cuGraphicsUnregisterResource(cu_resource);
      return Err(OM_CODEC_ENCODE_FAILED);
    }
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = reg.registeredResource;
    nvenc_.nvEncMapInputResource(encoder_, &map);

    auto res = encodeCommon(map.mappedResource, pts, picture);

    nvenc_.nvEncUnmapInputResource(encoder_, map.mappedResource);
    nvenc_.nvEncUnregisterResource(encoder_, reg.registeredResource);
    cu->cuGraphicsUnmapResources(1, &cu_resource, stream);
    cu->cuGraphicsUnregisterResource(cu_resource);
    return res;
  }
#endif

  auto encodeCuda(std::shared_ptr<CudaHardwarePicture> pic, int64_t pts, const Picture& picture) -> Result<std::vector<Packet>, OMError> {
    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.resourceToRegister = (void*) pic->data;
    reg.width = pic->width;
    reg.height = pic->height;
    reg.pitch = pic->pitch;
    reg.bufferFormat = (picture.format == OM_FORMAT_P010) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (nvenc_.nvEncRegisterResource(encoder_, &reg) != NV_ENC_SUCCESS) return Err(OM_CODEC_ENCODE_FAILED);
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = reg.registeredResource;
    if (nvenc_.nvEncMapInputResource(encoder_, &map) != NV_ENC_SUCCESS) {
      nvenc_.nvEncUnregisterResource(encoder_, reg.registeredResource);
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    auto res = encodeCommon(map.mappedResource, pts, picture);

    nvenc_.nvEncUnmapInputResource(encoder_, map.mappedResource);
    nvenc_.nvEncUnregisterResource(encoder_, reg.registeredResource);
    return res;
  }

  auto map_color_primaries(OMColorPrimaries p) -> uint32_t {
    switch (p) {
      case OM_PRIMARIES_BT709: return 1;
      case OM_PRIMARIES_BT2020: return 9;
      case OM_PRIMARIES_BT601: return 6;
      default: return 2;
    }
  }

  auto map_transfer_characteristics(OMTransferCharacteristic t) -> uint32_t {
    switch (t) {
      case OM_TRANSFER_BT709: return 1;
      case OM_TRANSFER_PQ: return 16;
      case OM_TRANSFER_HLG: return 18;
      default: return 2;
    }
  }

  auto map_color_space(OMColorSpace c) -> uint32_t {
    switch (c) {
      case OM_COLOR_SPACE_BT709: return 1;
      case OM_COLOR_SPACE_BT2020: return 9;
      case OM_COLOR_SPACE_BT601: return 6;
      default: return 2;
    }
  }

  void applyRateControl(const RateControlParams& rc, NV_ENC_CONFIG& config) {
    config.rcParams.version = NV_ENC_RC_PARAMS_VER;
    switch (rc.getMode()) {
      case RateControlMode::CBR:
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        config.rcParams.averageBitRate = (uint32_t) std::get<CbrParams>(rc.params).bitrate.target_bitrate;
        config.rcParams.maxBitRate = config.rcParams.averageBitRate;
        break;
      case RateControlMode::VBR:
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        config.rcParams.averageBitRate = (uint32_t) std::get<VbrParams>(rc.params).bitrate.target_bitrate;
        config.rcParams.maxBitRate = (uint32_t) std::get<VbrParams>(rc.params).bitrate.max_bitrate.value_or(config.rcParams.averageBitRate * 2);
        break;
      case RateControlMode::CQP:
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        config.rcParams.constQP.qpInterP = std::get<CqpParams>(rc.params).qp_p;
        config.rcParams.constQP.qpInterB = std::get<CqpParams>(rc.params).qp_b.value_or(config.rcParams.constQP.qpInterP);
        config.rcParams.constQP.qpIntra = std::get<CqpParams>(rc.params).qp_i;
        break;
      default:
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        config.rcParams.averageBitRate = 5000000;
        break;
    }
  }

  void release() {
    if (encoder_) {
      for (auto& surf : input_surfaces_)
        if (surf.input_ptr) nvenc_.nvEncDestroyInputBuffer(encoder_, surf.input_ptr);
      for (auto& out : output_buffers_)
        if (out.output_ptr) nvenc_.nvEncDestroyBitstreamBuffer(encoder_, out.output_ptr);
      nvenc_.nvEncDestroyEncoder(encoder_);
    }
    encoder_ = nullptr;
    input_surfaces_.clear();
    output_buffers_.clear();
    initialized_ = false;
  }
};

const CodecDescriptor CODEC_NVENC_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "nvenc_h264",
    .long_name = "NVIDIA NVENC H.264",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<NVEnc>(); },
};
const CodecDescriptor CODEC_NVENC_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "nvenc_h265",
    .long_name = "NVIDIA NVENC H.265",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<NVEnc>(); },
};
const CodecDescriptor CODEC_NVENC_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "nvenc_av1",
    .long_name = "NVIDIA NVENC AV1",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .encoder_factory = [] { return std::make_unique<NVEnc>(); },
};

} // namespace openmedia
