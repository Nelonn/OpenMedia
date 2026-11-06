#include <openmedia/hw_cuda.h>
#ifdef _WIN32
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#include <wrl/client.h>
#endif
#include <openmedia/hw_vulkan.h>
#include <codecs.hpp>
#include <algorithm>
#include <deque>
#include <openmedia/codec_api.hpp>
#include <openmedia/video.hpp>
#include <optional>
#include <variant>
#include <vector>
#include "av1_quant.hpp"
#include "enc_preset.hpp"
#include "hw_common.hpp"
#include "nv_common.hpp"
#include "nv_loader.hpp"

namespace openmedia {

namespace {

// How a picture format reaches NVENC, and the stream it makes.
struct InputLayout {
  NV_ENC_BUFFER_FORMAT buffer;
  uint32_t chroma_format_idc; // 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  bool deep;                  // 16-bit samples, 10 valid bits in the MSBs
};

auto inputLayout(OMPixelFormat format) -> std::optional<InputLayout> {
  switch (format) {
    case OM_FORMAT_NV12: return InputLayout {NV_ENC_BUFFER_FORMAT_NV12, 1, false};
    case OM_FORMAT_YUV420P:
    case OM_FORMAT_YUVJ420P: return InputLayout {NV_ENC_BUFFER_FORMAT_IYUV, 1, false};
    case OM_FORMAT_P010:
    case OM_FORMAT_P016: return InputLayout {NV_ENC_BUFFER_FORMAT_YUV420_10BIT, 1, true};
    case OM_FORMAT_NV16: return InputLayout {NV_ENC_BUFFER_FORMAT_NV16, 2, false};
    case OM_FORMAT_P216: return InputLayout {NV_ENC_BUFFER_FORMAT_P210, 2, true};
    case OM_FORMAT_YUV444P:
    case OM_FORMAT_YUVJ444P: return InputLayout {NV_ENC_BUFFER_FORMAT_YUV444, 3, false};
    case OM_FORMAT_YUV444P16: return InputLayout {NV_ENC_BUFFER_FORMAT_YUV444_10BIT, 3, true};
    // V,U,Y,A bytes: NVENC's word-ordered A8Y8U8V8 and D3D11's AYUV alike.
    case OM_FORMAT_AYUV: return InputLayout {NV_ENC_BUFFER_FORMAT_AYUV, 3, false};
    default: return std::nullopt;
  }
}

auto codecGuid(OMCodecId codec) -> std::optional<GUID> {
  switch (codec) {
    case OM_CODEC_H264: return NV_ENC_CODEC_H264_GUID;
    case OM_CODEC_H265: return NV_ENC_CODEC_HEVC_GUID;
    case OM_CODEC_AV1: return NV_ENC_CODEC_AV1_GUID;
    default: return std::nullopt;
  }
}

// NVENC's own 1-7 scale: P1 the fastest, P7 the one that spends most on the picture.
auto presetGuid(int32_t preset) -> GUID {
  switch (preset) {
    case 1: return NV_ENC_PRESET_P1_GUID;
    case 2: return NV_ENC_PRESET_P2_GUID;
    case 3: return NV_ENC_PRESET_P3_GUID;
    case 5: return NV_ENC_PRESET_P5_GUID;
    case 6: return NV_ENC_PRESET_P6_GUID;
    case 7: return NV_ENC_PRESET_P7_GUID;
    default: return NV_ENC_PRESET_P4_GUID;
  }
}

auto colorPrimaries(OMColorPrimaries p) -> NV_ENC_VUI_COLOR_PRIMARIES {
  switch (p) {
    case OM_PRIMARIES_BT709: return NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    case OM_PRIMARIES_BT2020: return NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
    case OM_PRIMARIES_BT601: return NV_ENC_VUI_COLOR_PRIMARIES_SMPTE170M;
    default: return NV_ENC_VUI_COLOR_PRIMARIES_UNSPECIFIED;
  }
}

auto transferCharacteristics(OMTransferCharacteristic t) -> NV_ENC_VUI_TRANSFER_CHARACTERISTIC {
  switch (t) {
    case OM_TRANSFER_BT709: return NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    case OM_TRANSFER_PQ: return NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
    case OM_TRANSFER_HLG: return NV_ENC_VUI_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
    default: return NV_ENC_VUI_TRANSFER_CHARACTERISTIC_UNSPECIFIED;
  }
}

auto matrixCoefficients(OMColorSpace c) -> NV_ENC_VUI_MATRIX_COEFFS {
  switch (c) {
    case OM_COLOR_SPACE_RGB: return NV_ENC_VUI_MATRIX_COEFFS_RGB;
    case OM_COLOR_SPACE_BT709: return NV_ENC_VUI_MATRIX_COEFFS_BT709;
    case OM_COLOR_SPACE_BT2020: return NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
    case OM_COLOR_SPACE_BT601: return NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
    default: return NV_ENC_VUI_MATRIX_COEFFS_UNSPECIFIED;
  }
}

// AV1 quantises on 0..255; everything else on 0..51.
auto nativeQp(OMCodecId codec, int32_t qp) -> uint32_t {
  return static_cast<uint32_t>(codec == OM_CODEC_AV1 ? av1_quant::qindexFromQp(qp) : std::clamp(qp, 0, 51));
}

auto qpTriple(uint32_t qp) -> NV_ENC_QP { return {qp, qp, qp}; }

// The presets' profiles are 4:2:0 8-bit; anything else needs the one that allows it.
auto h264Profile(const InputLayout& layout) -> std::optional<GUID> {
  if (layout.chroma_format_idc == 3) return NV_ENC_H264_PROFILE_HIGH_444_GUID;
  if (layout.chroma_format_idc == 2) return NV_ENC_H264_PROFILE_HIGH_422_GUID;
  if (layout.deep) return NV_ENC_H264_PROFILE_HIGH_10_GUID;
  return std::nullopt;
}

auto hevcProfile(const InputLayout& layout) -> std::optional<GUID> {
  if (layout.chroma_format_idc != 1) return NV_ENC_HEVC_PROFILE_FREXT_GUID;
  if (layout.deep) return NV_ENC_HEVC_PROFILE_MAIN10_GUID;
  return std::nullopt;
}

#ifdef _WIN32
auto textureBufferFormat(DXGI_FORMAT format) -> NV_ENC_BUFFER_FORMAT {
  switch (format) {
    case DXGI_FORMAT_NV12: return NV_ENC_BUFFER_FORMAT_NV12;
    case DXGI_FORMAT_P010: return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    case DXGI_FORMAT_AYUV: return NV_ENC_BUFFER_FORMAT_AYUV;
    default: return NV_ENC_BUFFER_FORMAT_UNDEFINED;
  }
}

// A decoder's own surfaces are slices of an array texture; a pooled copy is slice 0.
auto arraySlice(const DX11HardwarePicture& picture) -> UINT {
  if (!picture.pic || !picture.pic->decoder_output) return 0;
  D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view = {};
  picture.pic->decoder_output->GetDesc(&view);
  return view.Texture2D.ArraySlice;
}
#endif

auto fail(OMError error, std::string_view what) -> OMError {
  openmedia::log(OM_CATEGORY_ENCODER, OM_LEVEL_ERROR, "nvenc: {}", what);
  return error;
}

} // namespace

class NVEnc final : public Encoder {
  NV_ENCODE_API_FUNCTION_LIST nvenc_ = {};
  void* encoder_ = nullptr;
  // A CUDA session takes CUDA pictures, a D3D11 one D3D11 textures; both take host pictures.
  HWDeviceType session_device_ = HWDeviceType::NONE;

  OMCodecId codec_id_ = OM_CODEC_NONE;
  GUID codec_guid_ = {};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  InputLayout layout_ = {};
  VideoFormat input_format_ = {};
  // Reconfiguring takes the whole set again, not only what changes.
  NV_ENC_CONFIG config_ = {};
  NV_ENC_INITIALIZE_PARAMS init_params_ = {};
  std::vector<uint8_t> extradata_;

#ifdef _WIN32
  // Registering is costly and a decoder's pool hands out the same few textures
  // again and again. The reference keeps the texture alive, so its address can
  // never come back as a different texture behind a stale registration.
  struct TextureRegistration {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    uint32_t in_use = 0; // pending slots mapping it
  };
  std::vector<TextureRegistration> textures_;
  static constexpr size_t kMaxRegisteredTextures = 64;
#endif

  // One per picture in flight. With B-frames and lookahead NVENC holds pictures
  // back (NV_ENC_ERR_NEED_MORE_INPUT) and then completes several at once, so a
  // slot and the input it was given stay untouched until its bitstream is read.
  struct Slot {
    NV_ENC_INPUT_PTR input_buffer = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;          // a CUDA picture's, unregistered with the slot
    NV_ENC_REGISTERED_PTR cached_registration = nullptr; // a texture's, kept in textures_
    std::shared_ptr<HardwarePicture> held_picture;
  };
  std::vector<Slot> slots_;
  uint32_t next_slot_ = 0;
  std::deque<uint32_t> pending_; // submitted, oldest first, output not read yet

  // Packets come out in decode order, one per input. With B-frames packet k
  // takes the pts of the picture submitted reorder_delay_ places earlier, so dts
  // never passes pts; the first few count down to the first pts a tick at a time,
  // as the IDR is out before a frame step is known.
  std::deque<int64_t> input_pts_;
  int64_t first_pts_ = 0;
  int64_t output_index_ = 0;
  uint32_t reorder_delay_ = 0;

  bool initialized_ = false;
  bool force_keyframe_ = false;
  bool finished_ = false;

public:
  NVEnc() = default;
  ~NVEnc() override { release(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    release();
    const auto layout = inputLayout(options.video_format.format);
    const auto guid = codecGuid(options.format.codec_id);
    if (!guid) return OM_CODEC_NOT_SUPPORTED;
    if (!layout) return fail(OM_CODEC_NOT_SUPPORTED, "the input pixel format is not one NVENC takes");
    if (options.format.codec_id == OM_CODEC_AV1 && layout->chroma_format_idc != 1)
      return fail(OM_CODEC_NOT_SUPPORTED, "AV1 is encoded from 4:2:0 only");

    codec_id_ = options.format.codec_id;
    codec_guid_ = *guid;
    layout_ = *layout;
    width_ = options.video_format.width;
    height_ = options.video_format.height;
    input_format_ = options.video_format;

    if (const OMError error = openSession(options); error != OM_SUCCESS) return error;
    if (const OMError error = checkCapabilities(); error != OM_SUCCESS) return error;
    if (const OMError error = initialize(options); error != OM_SUCCESS) return error;
    if (const OMError error = createSlots(); error != OM_SUCCESS) return error;
    captureSequenceParams();
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    info.mastering_display = input_format_.mastering_display;
    info.content_light_level = input_format_.content_light_level;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (finished_) return Err(OM_CODEC_FLUSHED);
    const auto* picture = std::get_if<Picture>(&frame.data);
    if (!picture) return Err(OM_CODEC_INVALID_PARAMS);
    if (picture->width == 0) return finish(); // the older way of asking for the tail
    if (pending_.size() >= slots_.size()) return Err(fail(OM_CODEC_ENCODE_FAILED, "every slot is still held by the encoder"));

    const uint32_t index = next_slot_;
    Slot& slot = slots_[index];
    auto input = submitInput(*picture, slot);
    if (input.isErr()) return Err(input.unwrapErr());

    NV_ENC_PIC_PARAMS pic_params = {};
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.inputBuffer = input.unwrap();
    pic_params.bufferFmt = layout_.buffer;
    pic_params.inputWidth = width_;
    pic_params.inputHeight = height_;
    pic_params.outputBitstream = slot.bitstream;
    pic_params.inputTimeStamp = static_cast<uint64_t>(frame.pts);
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (force_keyframe_) pic_params.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    MASTERING_DISPLAY_INFO md = {};
    CONTENT_LIGHT_LEVEL cll = {};
    if (codec_id_ == OM_CODEC_H265) attachHdrMetadata(*picture, md, cll, pic_params.codecPicParams.hevcPicParams);

    if (input_pts_.empty() && output_index_ == 0) first_pts_ = frame.pts;
    input_pts_.push_back(frame.pts);
    next_slot_ = (next_slot_ + 1) % static_cast<uint32_t>(slots_.size());
    pending_.push_back(index);

    const NVENCSTATUS status = nvenc_.nvEncEncodePicture(encoder_, &pic_params);
    force_keyframe_ = false;
    if (status == NV_ENC_ERR_NEED_MORE_INPUT) return Ok(std::vector<Packet> {});
    if (status != NV_ENC_SUCCESS) {
      pending_.pop_back();
      input_pts_.pop_back();
      releaseInput(slot);
      return Err(fail(OM_CODEC_ENCODE_FAILED, std::format("nvEncEncodePicture failed ({})", static_cast<int>(status))));
    }
    return readPending();
  }

  // Sends end of stream and returns every picture NVENC still holds back.
  auto finish() -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (finished_) return Ok(std::vector<Packet> {});
    finished_ = true;
    NV_ENC_PIC_PARAMS pic_params = {};
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    if (const NVENCSTATUS status = nvenc_.nvEncEncodePicture(encoder_, &pic_params); status != NV_ENC_SUCCESS) {
      return Err(fail(OM_CODEC_ENCODE_FAILED, std::format("end of stream failed ({})", static_cast<int>(status))));
    }
    return readPending();
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!initialized_) return OM_COMMON_NOT_INITIALIZED;
    NV_ENC_CONFIG config = config_;
    applyRateControl(rc, config);
    NV_ENC_RECONFIGURE_PARAMS reconfig = {};
    reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfig.reInitEncodeParams = init_params_;
    reconfig.reInitEncodeParams.encodeConfig = &config;
    if (const NVENCSTATUS status = nvenc_.nvEncReconfigureEncoder(encoder_, &reconfig); status != NV_ENC_SUCCESS) {
      return fail(OM_CODEC_HWACCEL_FAILED, std::format("nvEncReconfigureEncoder failed ({})", static_cast<int>(status)));
    }
    config_ = config;
    return OM_SUCCESS;
  }

  auto requestKeyframe() -> OMError override {
    if (!initialized_) return OM_COMMON_NOT_INITIALIZED;
    force_keyframe_ = true;
    return OM_SUCCESS;
  }

private:
  // --- configure steps ----------------------------------------------------------

  auto openSession(const EncoderOptions& options) -> OMError {
    if (!options.hw_device || !options.hw_device->context) return OM_CODEC_HWACCEL_FAILED;
    void* device = nullptr;
    NV_ENC_DEVICE_TYPE device_type = NV_ENC_DEVICE_TYPE_CUDA;
    switch (options.hw_device->type) {
      case HWDeviceType::CUDA:
        device = HWCudaContext_getContext(static_cast<OMCudaContext*>(options.hw_device->context));
        break;
#ifdef _WIN32
      case HWDeviceType::DX11:
        device = HWD3D11Context_getDevice(static_cast<OMDX11Context*>(options.hw_device->context));
        device_type = NV_ENC_DEVICE_TYPE_DIRECTX;
        break;
#endif
      default: return OM_CODEC_HWACCEL_FAILED;
    }
    if (!device) return OM_CODEC_HWACCEL_FAILED;
    session_device_ = options.hw_device->type;

    if (!NVLoader::getInstance().load()) return OM_CODEC_HWACCEL_FAILED;
    auto* functions = NVLoader::getInstance().nvenc();
    if (!functions) return fail(OM_CODEC_HWACCEL_FAILED, "the NVENC library is not available");
    nvenc_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (functions->NvEncodeAPICreateInstance(&nvenc_) != NV_ENC_SUCCESS)
      return fail(OM_CODEC_HWACCEL_FAILED, "NvEncodeAPICreateInstance failed");

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.device = device;
    params.deviceType = device_type;
    params.apiVersion = NVENCAPI_VERSION;
    if (nvenc_.nvEncOpenEncodeSessionEx(&params, &encoder_) != NV_ENC_SUCCESS) {
      encoder_ = nullptr;
      return fail(OM_CODEC_HWACCEL_FAILED, "nvEncOpenEncodeSessionEx failed");
    }
    return OM_SUCCESS;
  }

  auto capability(NV_ENC_CAPS cap) -> bool {
    NV_ENC_CAPS_PARAM param = {};
    param.version = NV_ENC_CAPS_PARAM_VER;
    param.capsToQuery = cap;
    int value = 0;
    return nvenc_.nvEncGetEncodeCaps(encoder_, codec_guid_, &param, &value) == NV_ENC_SUCCESS && value != 0;
  }

  // Turns "this GPU can't" into NOT_SUPPORTED instead of a bare initialise failure.
  auto checkCapabilities() -> OMError {
    if (layout_.chroma_format_idc == 3 && !capability(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE))
      return fail(OM_CODEC_NOT_SUPPORTED, "this GPU cannot encode 4:4:4 in this codec");
    if (layout_.chroma_format_idc == 2 && !capability(NV_ENC_CAPS_SUPPORT_YUV422_ENCODE))
      return fail(OM_CODEC_NOT_SUPPORTED, "this GPU cannot encode 4:2:2 in this codec");
    if (layout_.deep && !capability(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE))
      return fail(OM_CODEC_NOT_SUPPORTED, "this GPU cannot encode 10-bit in this codec");
    return OM_SUCCESS;
  }

  auto initialize(const EncoderOptions& options) -> OMError {
    // Low latency keeps the encoder frame-in frame-out, so a sender can raise the
    // preset without paying in delay.
    const NV_ENC_TUNING_INFO tuning = options.extra.getBool("low_latency", false) ? NV_ENC_TUNING_INFO_LOW_LATENCY
                                                                                  : NV_ENC_TUNING_INFO_HIGH_QUALITY;
    const GUID preset = presetGuid(enc_preset::requestedOr(options.extra, enc_preset::kBalanced));

    NV_ENC_PRESET_CONFIG preset_config = {};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
    // Without the tuning in the query the config is the latency-tolerant one, B-frames and all.
    if (nvenc_.nvEncGetEncodePresetConfigEx) {
      nvenc_.nvEncGetEncodePresetConfigEx(encoder_, codec_guid_, preset, tuning, &preset_config);
    } else {
      nvenc_.nvEncGetEncodePresetConfig(encoder_, codec_guid_, preset, &preset_config);
    }
    config_ = preset_config.presetCfg;
    config_.version = NV_ENC_CONFIG_VER;
    applyFormat(options, config_);
    applyRateControl(options.rate_control, config_);
    if (const auto* gop = options.extra.get("gop_size")) {
      if (const auto n = gop->getInt64(); n && *n > 0) config_.gopLength = static_cast<uint32_t>(*n);
    }

    const auto& rate = options.format.video.framerate;
    init_params_ = {};
    init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params_.encodeGUID = codec_guid_;
    init_params_.presetGUID = preset;
    init_params_.tuningInfo = tuning;
    init_params_.encodeWidth = width_;
    init_params_.encodeHeight = height_;
    init_params_.darWidth = width_;
    init_params_.darHeight = height_;
    init_params_.frameRateNum = rate.num > 0 ? rate.num : 30;
    init_params_.frameRateDen = rate.den > 0 ? rate.den : 1;
    init_params_.enablePTD = 1;
    init_params_.encodeConfig = &config_;
    if (const NVENCSTATUS status = nvenc_.nvEncInitializeEncoder(encoder_, &init_params_); status != NV_ENC_SUCCESS) {
      return fail(OM_CODEC_HWACCEL_FAILED, std::format("nvEncInitializeEncoder failed ({})", static_cast<int>(status)));
    }

    const uint32_t b_frames = config_.frameIntervalP > 1 ? config_.frameIntervalP - 1 : 0;
    // AV1 hides its out-of-order frames inside packets that come out in display order.
    reorder_delay_ = codec_id_ == OM_CODEC_AV1 ? 0 : b_frames;
    return OM_SUCCESS;
  }

  // Every picture NVENC may hold back needs a slot, plus the one being submitted.
  auto createSlots() -> OMError {
    const uint32_t b_frames = config_.frameIntervalP > 1 ? config_.frameIntervalP - 1 : 0;
    const uint32_t lookahead = config_.rcParams.enableLookahead ? config_.rcParams.lookaheadDepth : 0;
    slots_.resize(b_frames + lookahead + 4);
    for (auto& slot : slots_) {
      NV_ENC_CREATE_INPUT_BUFFER input = {};
      input.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
      input.width = width_;
      input.height = height_;
      input.bufferFmt = layout_.buffer;
      if (nvenc_.nvEncCreateInputBuffer(encoder_, &input) != NV_ENC_SUCCESS)
        return fail(OM_CODEC_HWACCEL_FAILED, "nvEncCreateInputBuffer failed");
      slot.input_buffer = input.inputBuffer;

      NV_ENC_CREATE_BITSTREAM_BUFFER bitstream = {};
      bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      if (nvenc_.nvEncCreateBitstreamBuffer(encoder_, &bitstream) != NV_ENC_SUCCESS)
        return fail(OM_CODEC_HWACCEL_FAILED, "nvEncCreateBitstreamBuffer failed");
      slot.bitstream = bitstream.bitstreamBuffer;
    }
    return OM_SUCCESS;
  }

  void applyFormat(const EncoderOptions& options, NV_ENC_CONFIG& config) const {
    const auto depth = layout_.deep ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    const auto& video = options.format.video;
    const OMPixelFormat format = options.video_format.format;
    const bool full_range = video.color_range == OM_COLOR_RANGE_FULL || format == OM_FORMAT_YUVJ420P ||
                            format == OM_FORMAT_YUVJ444P;

    // The colour description only counts inside a video signal type, so both flags go together.
    const auto set_vui = [&](auto& vui) {
      vui.videoSignalTypePresentFlag = 1;
      vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
      vui.videoFullRangeFlag = full_range;
      vui.colourDescriptionPresentFlag = 1;
      vui.colourPrimaries = colorPrimaries(video.color_primaries);
      vui.transferCharacteristics = transferCharacteristics(video.transfer_char);
      vui.colourMatrix = matrixCoefficients(video.color_space);
    };
    const auto set_depth = [&](auto& codec) {
      codec.inputBitDepth = depth;
      codec.outputBitDepth = depth;
    };

    switch (codec_id_) {
      case OM_CODEC_H264: {
        auto& h264 = config.encodeCodecConfig.h264Config;
        set_vui(h264.h264VUIParameters);
        set_depth(h264);
        h264.chromaFormatIDC = layout_.chroma_format_idc;
        if (const auto profile = h264Profile(layout_)) config.profileGUID = *profile;
        break;
      }
      case OM_CODEC_H265: {
        auto& hevc = config.encodeCodecConfig.hevcConfig;
        set_vui(hevc.hevcVUIParameters);
        set_depth(hevc);
        hevc.chromaFormatIDC = layout_.chroma_format_idc;
        if (const auto profile = hevcProfile(layout_)) config.profileGUID = *profile;
        break;
      }
      case OM_CODEC_AV1: {
        auto& av1 = config.encodeCodecConfig.av1Config;
        set_depth(av1);
        av1.chromaFormatIDC = 1;
        av1.colorPrimaries = colorPrimaries(video.color_primaries);
        av1.transferCharacteristics = transferCharacteristics(video.transfer_char);
        av1.matrixCoefficients = matrixCoefficients(video.color_space);
        av1.colorRange = full_range;
        break;
      }
      default: break;
    }
  }

  void applyRateControl(const RateControlParams& rc, NV_ENC_CONFIG& config) const {
    auto& p = config.rcParams;
    p.version = NV_ENC_RC_PARAMS_VER;
    std::visit(
        [&](const auto& params) {
          using T = std::decay_t<decltype(params)>;
          if constexpr (std::is_same_v<T, CbrParams>) {
            p.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            p.averageBitRate = static_cast<uint32_t>(params.bitrate.target_bitrate);
            p.maxBitRate = p.averageBitRate;
          } else if constexpr (std::is_same_v<T, VbrParams>) {
            p.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            p.averageBitRate = static_cast<uint32_t>(params.bitrate.target_bitrate);
            p.maxBitRate = static_cast<uint32_t>(params.bitrate.max_bitrate.value_or(int64_t(p.averageBitRate) * 2));
          } else if constexpr (std::is_same_v<T, CqpParams>) {
            p.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            p.constQP.qpIntra = nativeQp(codec_id_, params.qp_i);
            p.constQP.qpInterP = nativeQp(codec_id_, params.qp_p);
            p.constQP.qpInterB = nativeQp(codec_id_, params.qp_b.value_or(params.qp_p));
          } else if constexpr (std::is_same_v<T, CrfParams>) {
            // Constant quality: VBR with no bitrate target, steered by the CQ level.
            p.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            p.averageBitRate = 0;
            p.maxBitRate = 0;
            p.targetQuality = static_cast<uint8_t>(std::clamp(params.quality, 0.0f, 51.0f));
          } else if constexpr (std::is_same_v<T, AbrParams>) {
            p.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            p.averageBitRate = static_cast<uint32_t>(params.target_bitrate);
          } else {
            p.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            p.averageBitRate = 5000000;
          }
        },
        rc.params);
    if (rc.min_qp) {
      p.enableMinQP = 1;
      p.minQP = qpTriple(nativeQp(codec_id_, *rc.min_qp));
    }
    if (rc.max_qp) {
      p.enableMaxQP = 1;
      p.maxQP = qpTriple(nativeQp(codec_id_, *rc.max_qp));
    }
  }

  // SPS/PPS (and VPS) for a container's codec configuration. AV1's sequence
  // header travels in the first packet instead.
  void captureSequenceParams() {
    extradata_.clear();
    if (codec_id_ == OM_CODEC_AV1) return;
    std::vector<uint8_t> buffer(1024);
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = {};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<uint32_t>(buffer.size());
    payload.spsppsBuffer = buffer.data();
    payload.outSPSPPSPayloadSize = &size;
    if (nvenc_.nvEncGetSequenceParams(encoder_, &payload) == NV_ENC_SUCCESS && size <= buffer.size()) {
      buffer.resize(size);
      extradata_ = std::move(buffer);
    }
  }

  // --- input ------------------------------------------------------------------

  auto submitInput(const Picture& picture, Slot& slot) -> Result<NV_ENC_INPUT_PTR, OMError> {
    if (const auto* hw = std::get_if<std::shared_ptr<HardwarePicture>>(&picture.buffer)) {
      if (!*hw) return Err(OM_CODEC_INVALID_PARAMS);
      auto mapped = mapHardwarePicture(**hw, slot);
      if (mapped.isOk()) slot.held_picture = *hw; // NVENC may read it until the slot's output is out
      return mapped;
    }
    if (const OMError error = upload(picture, slot.input_buffer); error != OM_SUCCESS) return Err(error);
    return Ok(slot.input_buffer);
  }

  // Copies a host picture into an input buffer, plane after plane at the buffer's pitch.
  auto upload(const Picture& picture, NV_ENC_INPUT_PTR buffer) -> OMError {
    const auto layout = inputLayout(picture.format);
    if (!layout || layout->buffer != layout_.buffer)
      return fail(OM_CODEC_INVALID_PARAMS, "the picture's format differs from the configured one");
    if (picture.width != width_ || picture.height != height_)
      return fail(OM_CODEC_INVALID_PARAMS, "the picture's size differs from the configured one");

    NV_ENC_LOCK_INPUT_BUFFER lock = {};
    lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock.inputBuffer = buffer;
    if (nvenc_.nvEncLockInputBuffer(encoder_, &lock) != NV_ENC_SUCCESS)
      return fail(OM_CODEC_ENCODE_FAILED, "nvEncLockInputBuffer failed");
    auto* dst = static_cast<uint8_t*>(lock.bufferDataPtr);
    for (uint32_t plane = 0; plane < picture.planes.getPlaneCount(); ++plane) {
      const auto [plane_width, plane_height] = picture.getPlaneDimensions(plane);
      const uint8_t* src = picture.planes.getData(plane);
      const size_t src_stride = picture.planes.getLinesize(plane);
      const size_t row_bytes = size_t(plane_width) * getBytesPerPixel(picture.format, uint8_t(plane));
      // IYUV's chroma planes are half as wide as luma, and so is their pitch.
      const size_t dst_pitch = layout_.buffer == NV_ENC_BUFFER_FORMAT_IYUV && plane > 0 ? lock.pitch / 2 : lock.pitch;
      for (uint32_t y = 0; y < plane_height; ++y) std::memcpy(dst + y * dst_pitch, src + y * src_stride, row_bytes);
      dst += size_t(plane_height) * dst_pitch;
    }
    nvenc_.nvEncUnlockInputBuffer(encoder_, buffer);
    return OM_SUCCESS;
  }

  auto mapHardwarePicture(HardwarePicture& picture, Slot& slot) -> Result<NV_ENC_INPUT_PTR, OMError> {
#ifdef _WIN32
    if (picture.getType() == HWDeviceType::DX11 && session_device_ == HWDeviceType::DX11)
      return mapTexture(static_cast<DX11HardwarePicture&>(picture), slot);
#endif
    if (picture.getType() != HWDeviceType::CUDA || session_device_ != HWDeviceType::CUDA)
      return Err(fail(OM_CODEC_NOT_SUPPORTED, "the picture lives on a device this encoder was not configured on"));

    auto& cuda_pic = static_cast<CudaHardwarePicture&>(picture);
    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.resourceToRegister = reinterpret_cast<void*>(cuda_pic.data);
    reg.width = width_;
    reg.height = height_;
    reg.pitch = cuda_pic.pitch;
    reg.bufferFormat = layout_.buffer;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (nvenc_.nvEncRegisterResource(encoder_, &reg) != NV_ENC_SUCCESS)
      return Err(fail(OM_CODEC_ENCODE_FAILED, "nvEncRegisterResource failed"));
    auto mapped = map(reg.registeredResource);
    if (mapped.isErr()) {
      nvenc_.nvEncUnregisterResource(encoder_, reg.registeredResource);
      return mapped;
    }
    slot.registered = reg.registeredResource;
    slot.mapped = mapped.unwrap();
    return mapped;
  }

#ifdef _WIN32
  auto mapTexture(DX11HardwarePicture& picture, Slot& slot) -> Result<NV_ENC_INPUT_PTR, OMError> {
    ID3D11Texture2D* texture = picture.texture();
    if (!texture) return Err(fail(OM_CODEC_INVALID_PARAMS, "the D3D11 picture has no texture"));
    auto registration = textureRegistration(texture, arraySlice(picture));
    if (registration.isErr()) return Err(registration.unwrapErr());
    TextureRegistration& entry = *registration.unwrap();

    auto mapped = map(entry.registered);
    if (mapped.isErr()) return mapped;
    ++entry.in_use;
    slot.cached_registration = entry.registered;
    slot.mapped = mapped.unwrap();
    return mapped;
  }

  // The cached registration of a texture slice, registering it the first time it is seen.
  auto textureRegistration(ID3D11Texture2D* texture, UINT subresource) -> Result<TextureRegistration*, OMError> {
    const auto found = std::ranges::find_if(textures_, [&](const TextureRegistration& r) {
      return r.texture.Get() == texture && r.subresource == subresource;
    });
    if (found != textures_.end()) return Ok(&*found);

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (textureBufferFormat(desc.Format) != layout_.buffer)
      return Err(fail(OM_CODEC_INVALID_PARAMS, "the texture's format differs from the configured input format"));
    if (textures_.size() >= kMaxRegisteredTextures) {
      // Forget the oldest registration nothing is waiting on.
      const auto idle = std::ranges::find_if(textures_, [](const TextureRegistration& r) { return r.in_use == 0; });
      if (idle != textures_.end()) {
        nvenc_.nvEncUnregisterResource(encoder_, idle->registered);
        textures_.erase(idle);
      }
    }

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = texture;
    reg.subResourceIndex = subresource;
    reg.width = width_;
    reg.height = height_;
    reg.bufferFormat = layout_.buffer;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (const NVENCSTATUS status = nvenc_.nvEncRegisterResource(encoder_, &reg); status != NV_ENC_SUCCESS)
      return Err(fail(OM_CODEC_ENCODE_FAILED, std::format("cannot register the D3D11 texture ({})", static_cast<int>(status))));
    textures_.push_back({texture, subresource, reg.registeredResource, 0});
    return Ok(&textures_.back());
  }
#endif

  auto map(NV_ENC_REGISTERED_PTR registered) -> Result<NV_ENC_INPUT_PTR, OMError> {
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered;
    if (nvenc_.nvEncMapInputResource(encoder_, &map) != NV_ENC_SUCCESS)
      return Err(fail(OM_CODEC_ENCODE_FAILED, "nvEncMapInputResource failed"));
    return Ok(map.mappedResource);
  }

  void releaseInput(Slot& slot) {
    if (slot.mapped) nvenc_.nvEncUnmapInputResource(encoder_, slot.mapped);
    if (slot.registered) nvenc_.nvEncUnregisterResource(encoder_, slot.registered);
#ifdef _WIN32
    if (slot.cached_registration) {
      const auto entry = std::ranges::find(textures_, slot.cached_registration, &TextureRegistration::registered);
      if (entry != textures_.end()) --entry->in_use;
    }
#endif
    slot.mapped = nullptr;
    slot.registered = nullptr;
    slot.cached_registration = nullptr;
    slot.held_picture.reset();
  }

  static void attachHdrMetadata(const Picture& picture, MASTERING_DISPLAY_INFO& md, CONTENT_LIGHT_LEVEL& cll,
                                NV_ENC_PIC_PARAMS_HEVC& hevc) {
    if (const auto& m = picture.mastering_display; m.has_value) {
      md.r = {m.display_primaries[0][0], m.display_primaries[0][1]};
      md.g = {m.display_primaries[1][0], m.display_primaries[1][1]};
      md.b = {m.display_primaries[2][0], m.display_primaries[2][1]};
      md.whitePoint = {m.white_point[0], m.white_point[1]};
      md.maxLuma = m.max_display_mastering_luminance;
      md.minLuma = m.min_display_mastering_luminance;
      hevc.pMasteringDisplay = &md;
    }
    if (const auto& l = picture.content_light_level; l.has_value) {
      cll.maxContentLightLevel = l.max_content_light_level;
      cll.maxPicAverageLightLevel = l.max_pic_average_light_level;
      hevc.pMaxCll = &cll;
    }
  }

  // --- output -----------------------------------------------------------------

  // Reads every completed slot, oldest first: NVENC finishes pictures in decode order.
  auto readPending() -> Result<std::vector<Packet>, OMError> {
    std::vector<Packet> packets;
    while (!pending_.empty()) {
      Slot& slot = slots_[pending_.front()];
      NV_ENC_LOCK_BITSTREAM lock = {};
      lock.version = NV_ENC_LOCK_BITSTREAM_VER;
      lock.outputBitstream = slot.bitstream;
      if (nvenc_.nvEncLockBitstream(encoder_, &lock) != NV_ENC_SUCCESS)
        return Err(fail(OM_CODEC_ENCODE_FAILED, "nvEncLockBitstream failed"));
      Packet packet;
      packet.allocate(lock.bitstreamSizeInBytes);
      std::memcpy(packet.bytes.data(), lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
      packet.pts = static_cast<int64_t>(lock.outputTimeStamp);
      packet.dts = std::min(nextDts(packet.pts), packet.pts);
      packet.is_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                           (codec_id_ == OM_CODEC_AV1 && lock.pictureType == NV_ENC_PIC_TYPE_I);
      nvenc_.nvEncUnlockBitstream(encoder_, slot.bitstream);
      releaseInput(slot);
      pending_.pop_front();
      packets.push_back(std::move(packet));
    }
    return Ok(std::move(packets));
  }

  auto nextDts(int64_t pts) -> int64_t {
    const int64_t index = output_index_++;
    const auto delay = static_cast<int64_t>(reorder_delay_);
    if (index < delay) return first_pts_ - (delay - index);
    if (input_pts_.empty()) return pts;
    const int64_t dts = input_pts_.front();
    input_pts_.pop_front();
    return dts;
  }

  void release() {
    if (encoder_) {
      for (auto& slot : slots_) {
        releaseInput(slot);
        if (slot.input_buffer) nvenc_.nvEncDestroyInputBuffer(encoder_, slot.input_buffer);
        if (slot.bitstream) nvenc_.nvEncDestroyBitstreamBuffer(encoder_, slot.bitstream);
      }
#ifdef _WIN32
      for (const auto& texture : textures_) nvenc_.nvEncUnregisterResource(encoder_, texture.registered);
#endif
      nvenc_.nvEncDestroyEncoder(encoder_);
    }
#ifdef _WIN32
    textures_.clear();
#endif
    encoder_ = nullptr;
    session_device_ = HWDeviceType::NONE;
    slots_.clear();
    pending_.clear();
    input_pts_.clear();
    next_slot_ = 0;
    output_index_ = 0;
    reorder_delay_ = 0;
    extradata_.clear();
    initialized_ = false;
    finished_ = false;
    force_keyframe_ = false;
  }
};

// Whether a given GPU encodes 4:2:2, 4:4:4 or 10-bit is asked at configure time.
static const std::vector<OMPixelFormat> kNvencFormats = {
    OM_FORMAT_NV12, OM_FORMAT_YUV420P, OM_FORMAT_P010, OM_FORMAT_NV16,
    OM_FORMAT_P216, OM_FORMAT_YUV444P, OM_FORMAT_YUV444P16, OM_FORMAT_AYUV};
static const std::vector<OMPixelFormat> kNvencAv1Formats = {OM_FORMAT_NV12, OM_FORMAT_YUV420P, OM_FORMAT_P010};

const CodecDescriptor CODEC_NVENC_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "nvenc_h264",
    .long_name = "NVIDIA NVENC H.264",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = kNvencFormats}},
    .encoder_factory = [] { return std::make_unique<NVEnc>(); },
};
const CodecDescriptor CODEC_NVENC_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "nvenc_h265",
    .long_name = "NVIDIA NVENC H.265",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = kNvencFormats}},
    .encoder_factory = [] { return std::make_unique<NVEnc>(); },
};
const CodecDescriptor CODEC_NVENC_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "nvenc_av1",
    .long_name = "NVIDIA NVENC AV1",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = kNvencAv1Formats}},
    .encoder_factory = [] { return std::make_unique<NVEnc>(); },
};

} // namespace openmedia
