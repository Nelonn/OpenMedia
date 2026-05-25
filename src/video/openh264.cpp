#include <wels/codec_api.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <memory>
#include <mutex>
#include <openmedia/codec_extra.hpp>
#include <openmedia/video.hpp>
#include <type_traits>
#include <util/cpp.hpp>
#include <util/dynamic_loader.hpp>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

using PFN_WelsCreateSVCEncoder = fn_ptr<int EXTAPI(ISVCEncoder**)>;
using PFN_WelsDestroySVCEncoder = fn_ptr<void EXTAPI(ISVCEncoder*)>;
using PFN_WelsCreateDecoder = fn_ptr<long EXTAPI(ISVCDecoder**)>;
using PFN_WelsDestroyDecoder = fn_ptr<void EXTAPI(ISVCDecoder*)>;

class OpenH264Loader {
public:
  static auto getInstance() -> OpenH264Loader& {
    static OpenH264Loader instance;
    return instance;
  }

  auto load() -> bool {
    if (loaded_) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return true;

#if defined(_WIN32)
    lib_.open("openh264.dll");
    if (!lib_.success()) {
      lib_.open("openh264-2.6.0-win64.dll");
    }
#elif defined(__APPLE__)
    lib_.open("libopenh264.dylib");
#else
    lib_.open("libopenh264.so");
#endif

    if (!lib_.success()) return false;

    WelsCreateSVCEncoder = lib_.getProcAddress<PFN_WelsCreateSVCEncoder>("WelsCreateSVCEncoder");
    WelsDestroySVCEncoder = lib_.getProcAddress<PFN_WelsDestroySVCEncoder>("WelsDestroySVCEncoder");
    WelsCreateDecoder = lib_.getProcAddress<PFN_WelsCreateDecoder>("WelsCreateDecoder");
    WelsDestroyDecoder = lib_.getProcAddress<PFN_WelsDestroyDecoder>("WelsDestroyDecoder");

    if (!WelsCreateSVCEncoder || !WelsDestroySVCEncoder || !WelsCreateDecoder || !WelsDestroyDecoder) {
      return false;
    }

    loaded_ = true;
    return true;
  }

  PFN_WelsCreateSVCEncoder WelsCreateSVCEncoder = nullptr;
  PFN_WelsDestroySVCEncoder WelsDestroySVCEncoder = nullptr;
  PFN_WelsCreateDecoder WelsCreateDecoder = nullptr;
  PFN_WelsDestroyDecoder WelsDestroyDecoder = nullptr;

private:
  OpenH264Loader() = default;

  DynamicLoader lib_;
  bool loaded_ = false;
  std::mutex mutex_;
};

struct OpenH264DecoderDeleter {
  void operator()(ISVCDecoder* decoder) const noexcept {
    if (!decoder) return;
    decoder->Uninitialize();
    OpenH264Loader::getInstance().WelsDestroyDecoder(decoder);
  }
};

struct OpenH264EncoderDeleter {
  void operator()(ISVCEncoder* encoder) const noexcept {
    if (!encoder) return;
    encoder->Uninitialize();
    OpenH264Loader::getInstance().WelsDestroySVCEncoder(encoder);
  }
};

using OpenH264DecoderPtr = std::unique_ptr<ISVCDecoder, OpenH264DecoderDeleter>;
using OpenH264EncoderPtr = std::unique_ptr<ISVCEncoder, OpenH264EncoderDeleter>;

class OpenH264Decoder final : public Decoder {
  OpenH264DecoderPtr decoder_;
  bool initialized_ = false;
  VideoFormat output_format_ = {};

public:
  OpenH264Decoder() = default;
  ~OpenH264Decoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    decoder_.reset();
    initialized_ = false;

    if (options.format.codec_id != OM_CODEC_H264) {
      return OM_CODEC_INVALID_PARAMS;
    }

    auto& loader = OpenH264Loader::getInstance();
    if (!loader.load()) {
      return OM_CODEC_OPEN_FAILED;
    }

    ISVCDecoder* raw_decoder = nullptr;
    if (loader.WelsCreateDecoder(&raw_decoder) != 0 || !raw_decoder) {
      return OM_CODEC_OPEN_FAILED;
    }
    OpenH264DecoderPtr decoder(raw_decoder);

    SDecodingParam param = {};
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

    if (decoder->Initialize(&param) != 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    if (!options.extradata.empty()) {
      unsigned char* data[3] = {nullptr, nullptr, nullptr};
      SBufferInfo buf_info = {};
      decoder->DecodeFrame2(options.extradata.data(), static_cast<int>(options.extradata.size()), data, &buf_info);
    }

    output_format_.width = options.format.video.width;
    output_format_.height = options.format.video.height;
    output_format_.format = OM_FORMAT_YUV420P;
    decoder_ = std::move(decoder);
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
    if (!decoder_) return Err(OM_CODEC_DECODE_FAILED);

    std::vector<Frame> frames;
    SBufferInfo buf_info = {};
    unsigned char* data[3] = {nullptr, nullptr, nullptr};

    std::span<const uint8_t> raw = packet.bytes;

    int ret;
    if (packet.bytes.empty()) {
      ret = decoder_->DecodeFrame2(nullptr, 0, data, &buf_info);
    } else {
      ret = decoder_->DecodeFrame2(raw.data(), static_cast<int>(raw.size()), data, &buf_info);
    }

    if (ret != 0) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (buf_info.iBufferStatus == 1) {
      output_format_.width = buf_info.UsrData.sSystemBuffer.iWidth;
      output_format_.height = buf_info.UsrData.sSystemBuffer.iHeight;

      Picture pic(OM_FORMAT_YUV420P, output_format_.width, output_format_.height);

      const auto [luma_w, luma_h] = getPlaneDimensions(OM_FORMAT_YUV420P, 0, output_format_.width, output_format_.height);
      const auto [chroma_w, chroma_h] = getPlaneDimensions(OM_FORMAT_YUV420P, 1, output_format_.width, output_format_.height);

      copyPlane(pic.planes.data[0], pic.planes.linesize[0], data[0], buf_info.UsrData.sSystemBuffer.iStride[0], luma_w, luma_h);
      copyPlane(pic.planes.data[1], pic.planes.linesize[1], data[1], buf_info.UsrData.sSystemBuffer.iStride[1], chroma_w, chroma_h);
      copyPlane(pic.planes.data[2], pic.planes.linesize[2], data[2], buf_info.UsrData.sSystemBuffer.iStride[1], chroma_w, chroma_h);

      Frame frame = {};
      frame.pts = packet.pts;
      frame.dts = packet.dts;
      frame.data = std::move(pic);
      frames.push_back(std::move(frame));
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (decoder_) {
      unsigned char* data[3] = {nullptr, nullptr, nullptr};
      SBufferInfo buf_info = {};
      decoder_->DecodeFrame2(nullptr, 0, data, &buf_info);
    }
  }
};

class OpenH264Encoder final : public Encoder {
  OpenH264EncoderPtr encoder_;
  VideoFormat format_ = {};
  bool initialized_ = false;

public:
  OpenH264Encoder() = default;
  ~OpenH264Encoder() override = default;

  auto configure(const EncoderOptions& options) -> OMError override {
    encoder_.reset();
    initialized_ = false;

    if (options.format.codec_id != OM_CODEC_H264) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.format.profile != OM_PROFILE_NONE &&
        options.format.profile != OM_PROFILE_H264_CONSTRAINED_BASELINE) {
      return OM_CODEC_NOT_SUPPORTED;
    }
    if (options.video_format.format != OM_FORMAT_YUV420P) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    auto& loader = OpenH264Loader::getInstance();
    if (!loader.load()) {
      return OM_CODEC_OPEN_FAILED;
    }

    ISVCEncoder* raw_encoder = nullptr;
    if (loader.WelsCreateSVCEncoder(&raw_encoder) != 0 || !raw_encoder) {
      return OM_CODEC_OPEN_FAILED;
    }
    OpenH264EncoderPtr encoder(raw_encoder);

    format_ = options.video_format;

    SEncParamExt param = {};
    encoder->GetDefaultParams(&param);

    const auto& extra = options.extra;
    auto set_int32 = [&extra]<typename T0>(const Key& key, T0& field) {
      if (!extra.contains(key)) return;
      using Field = std::remove_reference_t<T0>;
      field = static_cast<Field>(extra.getInt32(key));
    };
    auto set_int64 = [&extra]<typename T0>(const Key& key, T0& field) {
      if (!extra.contains(key)) return;
      using Field = std::remove_reference_t<T0>;
      field = static_cast<Field>(extra.getInt64(key));
    };
    auto set_bool = [&extra](const Key& key, bool& field) {
      if (extra.contains(key)) field = extra.getBool(key);
    };

    param.iUsageType = static_cast<EUsageType>(extra.getInt32(OPENH264_ENC_USAGE_TYPE, CAMERA_VIDEO_REAL_TIME));
    param.iComplexityMode = static_cast<ECOMPLEXITY_MODE>(extra.getInt32(OPENH264_ENC_COMPLEXITY, LOW_COMPLEXITY));

    set_int32(OPENH264_ENC_IDR_INTERVAL, param.uiIntraPeriod);
    set_int32(OPENH264_ENC_NUM_REF_FRAME, param.iNumRefFrame);
    set_int32(OPENH264_ENC_SPS_PPS_ID_STRATEGY, param.eSpsPpsIdStrategy);
    set_bool(OPENH264_ENC_PREFIX_NAL, param.bPrefixNalAddingCtrl);
    set_bool(OPENH264_ENC_SSEI, param.bEnableSSEI);
    set_bool(OPENH264_ENC_SIMULCAST_AVC, param.bSimulcastAVC);
    set_int32(OPENH264_ENC_PADDING, param.iPaddingFlag);
    set_int32(OPENH264_ENC_ENTROPY_CODING, param.iEntropyCodingModeFlag);
    set_bool(OPENH264_ENC_FRAME_SKIP, param.bEnableFrameSkip);
    set_int32(OPENH264_ENC_MAX_QP, param.iMaxQp);
    set_int32(OPENH264_ENC_MIN_QP, param.iMinQp);
    set_int32(OPENH264_ENC_MAX_NAL_SIZE, param.uiMaxNalSize);
    set_bool(OPENH264_ENC_LTR, param.bEnableLongTermReference);
    set_int32(OPENH264_ENC_LTR_REF_NUM, param.iLTRRefNum);
    set_int32(OPENH264_ENC_LTR_PERIOD, param.iLtrMarkPeriod);
    set_int32(OPENH264_ENC_THREADS, param.iMultipleThreadIdc);
    set_bool(OPENH264_ENC_LOAD_BALANCING, param.bUseLoadBalancing);
    set_int32(OPENH264_ENC_LOOP_FILTER, param.iLoopFilterDisableIdc);
    set_int32(OPENH264_ENC_LOOP_FILTER_ALPHA, param.iLoopFilterAlphaC0Offset);
    set_int32(OPENH264_ENC_LOOP_FILTER_BETA, param.iLoopFilterBetaOffset);
    set_bool(OPENH264_ENC_DENOISE, param.bEnableDenoise);
    set_bool(OPENH264_ENC_BGD, param.bEnableBackgroundDetection);
    set_bool(OPENH264_ENC_AQ, param.bEnableAdaptiveQuant);
    set_bool(OPENH264_ENC_FRAME_CROPPING, param.bEnableFrameCroppingFlag);
    set_bool(OPENH264_ENC_SCENE_CHANGE, param.bEnableSceneChangeDetect);
    set_bool(OPENH264_ENC_LOSSLESS_LINK, param.bIsLosslessLink);
    set_bool(OPENH264_ENC_FIX_RC_OVERSHOOT, param.bFixRCOverShoot);
    set_int32(OPENH264_ENC_IDR_BITRATE_RATIO, param.iIdrBitrateRatio);
    set_bool(OPENH264_ENC_PSNR_Y, param.bPsnrY);
    set_bool(OPENH264_ENC_PSNR_U, param.bPsnrU);
    set_bool(OPENH264_ENC_PSNR_V, param.bPsnrV);

    // Rate Control, from options.rate_control (overrides extra, or supplements it)
    int32_t target_bitrate = 1000000; // 1 Mbps default
    int32_t max_bitrate = UNSPECIFIED_BIT_RATE;
    if (auto* rc = std::get_if<CbrParams>(&options.rate_control.params)) {
      target_bitrate = static_cast<int32_t>(rc->bitrate.target_bitrate);
      max_bitrate = target_bitrate;
      param.iRCMode = RC_BITRATE_MODE;
    } else if (auto* vbr = std::get_if<VbrParams>(&options.rate_control.params)) {
      target_bitrate = static_cast<int32_t>(vbr->bitrate.target_bitrate);
      max_bitrate = static_cast<int32_t>(vbr->bitrate.max_bitrate.value_or(vbr->bitrate.target_bitrate));
      param.iRCMode = RC_BITRATE_MODE; // OpenH264's RC_BITRATE_MODE is essentially VBR-ish with target
    } else {
      param.iRCMode = RC_OFF_MODE;
    }

    if (options.rate_control.max_qp) {
      param.iMaxQp = *options.rate_control.max_qp;
    }
    if (options.rate_control.min_qp) {
      param.iMinQp = *options.rate_control.min_qp;
    }

    set_int64(OPENH264_ENC_MAX_BITRATE, max_bitrate);

    const double frame_rate = options.format.video.framerate.toDouble();
    param.fMaxFrameRate = frame_rate > 0.0 ? static_cast<float>(frame_rate) : 30.0f;
    param.iPicWidth = format_.width;
    param.iPicHeight = format_.height;
    param.iTargetBitrate = target_bitrate;
    param.iMaxBitrate = max_bitrate;
    param.iTemporalLayerNum = std::clamp(extra.getInt32(OPENH264_ENC_TEMPORAL_LAYERS, 1), 1, MAX_TEMPORAL_LAYER_NUM);
    param.iSpatialLayerNum = 1;

    param.sSpatialLayers[0].iVideoWidth = param.iPicWidth;
    param.sSpatialLayers[0].iVideoHeight = param.iPicHeight;
    param.sSpatialLayers[0].fFrameRate = param.fMaxFrameRate;
    param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;
    param.sSpatialLayers[0].iMaxSpatialBitrate = max_bitrate > 0 ? max_bitrate : param.iTargetBitrate;
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    if (options.format.level != 0) {
      param.sSpatialLayers[0].uiLevelIdc = static_cast<ELevelIdc>(options.format.level);
    }
    set_int32(OPENH264_ENC_LAYER_QP, param.sSpatialLayers[0].iDLayerQp);
    set_int32(OPENH264_ENC_SLICE_MODE, param.sSpatialLayers[0].sSliceArgument.uiSliceMode);
    set_int32(OPENH264_ENC_SLICE_NUM, param.sSpatialLayers[0].sSliceArgument.uiSliceNum);
    set_int32(OPENH264_ENC_SLICE_SIZE, param.sSpatialLayers[0].sSliceArgument.uiSliceSizeConstraint);
    set_bool(OPENH264_ENC_VIDEO_SIGNAL_TYPE_PRESENT, param.sSpatialLayers[0].bVideoSignalTypePresent);
    set_int32(OPENH264_ENC_VIDEO_FORMAT, param.sSpatialLayers[0].uiVideoFormat);
    set_bool(OPENH264_ENC_FULL_RANGE, param.sSpatialLayers[0].bFullRange);
    set_bool(OPENH264_ENC_COLOR_DESCRIPTION_PRESENT, param.sSpatialLayers[0].bColorDescriptionPresent);
    set_int32(OPENH264_ENC_COLOR_PRIMARIES, param.sSpatialLayers[0].uiColorPrimaries);
    set_int32(OPENH264_ENC_TRANSFER_CHARACTERISTICS, param.sSpatialLayers[0].uiTransferCharacteristics);
    set_int32(OPENH264_ENC_COLOR_MATRIX, param.sSpatialLayers[0].uiColorMatrix);
    set_bool(OPENH264_ENC_ASPECT_RATIO_PRESENT, param.sSpatialLayers[0].bAspectRatioPresent);
    set_int32(OPENH264_ENC_ASPECT_RATIO, param.sSpatialLayers[0].eAspectRatio);
    set_int32(OPENH264_ENC_ASPECT_RATIO_EXT_WIDTH, param.sSpatialLayers[0].sAspectRatioExtWidth);
    set_int32(OPENH264_ENC_ASPECT_RATIO_EXT_HEIGHT, param.sSpatialLayers[0].sAspectRatioExtHeight);

    if (auto* value = extra.get(OPENH264_ENC_SLICE_MB_NUM)) {
      if (auto values = value->getArray()) {
        const auto& array = values->get();
        const size_t count = std::min(array.size(), static_cast<size_t>(MAX_SLICES_NUM_TMP));
        for (size_t i = 0; i < count; ++i) {
          if (auto slice_mb_num = array[i].toInt64()) {
            param.sSpatialLayers[0].sSliceArgument.uiSliceMbNum[i] = static_cast<unsigned int>(*slice_mb_num);
          }
        }
      }
    }

    if (encoder->InitializeExt(&param) != 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    encoder_ = std::move(encoder);
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    if (!encoder_ || !initialized_) return info;

    SFrameBSInfo bs_info = {};
    if (encoder_->EncodeParameterSets(&bs_info) != 0 || bs_info.eFrameType == videoFrameTypeSkip) {
      return info;
    }

    appendBitstream(bs_info, info.extradata);
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!encoder_) return Err(OM_CODEC_ENCODE_FAILED);

    const auto& pic = std::get<Picture>(frame.data);
    if (pic.format != OM_FORMAT_YUV420P) {
      return Err(OM_CODEC_NOT_SUPPORTED);
    }

    SSourcePicture src_pic = {};
    src_pic.iColorFormat = videoFormatI420;
    src_pic.iPicWidth = pic.width;
    src_pic.iPicHeight = pic.height;
    src_pic.iStride[0] = pic.planes.linesize[0];
    src_pic.iStride[1] = pic.planes.linesize[1];
    src_pic.iStride[2] = pic.planes.linesize[2];
    src_pic.pData[0] = const_cast<uint8_t*>(pic.planes.data[0]);
    src_pic.pData[1] = const_cast<uint8_t*>(pic.planes.data[1]);
    src_pic.pData[2] = const_cast<uint8_t*>(pic.planes.data[2]);

    SFrameBSInfo bs_info = {};
    if (encoder_->EncodeFrame(&src_pic, &bs_info) != 0) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    std::vector<Packet> packets;
    if (bs_info.eFrameType != videoFrameTypeSkip) {
      Packet pkt;
      pkt.allocate(bitstreamSize(bs_info));
      copyBitstream(bs_info, pkt.bytes);
      pkt.pts = frame.pts;
      pkt.dts = frame.dts;
      pkt.is_keyframe = (bs_info.eFrameType == videoFrameTypeIDR);
      packets.push_back(std::move(pkt));
    }

    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!encoder_ || !initialized_) return OM_COMMON_NOT_INITIALIZED;

    int32_t target_bitrate = 0;
    if (auto* cbr = std::get_if<CbrParams>(&rc.params)) {
      target_bitrate = static_cast<int32_t>(cbr->bitrate.target_bitrate);
    } else if (auto* vbr = std::get_if<VbrParams>(&rc.params)) {
      target_bitrate = static_cast<int32_t>(vbr->bitrate.target_bitrate);
    }

    if (target_bitrate > 0) {
      SBitrateInfo info = {SPATIAL_LAYER_ALL, target_bitrate};
      if (encoder_->SetOption(ENCODER_OPTION_BITRATE, &info) != 0) {
        return OM_CODEC_INVALID_PARAMS;
      }
      return OM_SUCCESS;
    }

    return OM_COMMON_NOT_IMPLEMENTED;
  }

private:
  static auto bitstreamSize(const SFrameBSInfo& bs_info) -> size_t {
    size_t total_size = 0;
    for (int i = 0; i < bs_info.iLayerNum; ++i) {
      const SLayerBSInfo& layer = bs_info.sLayerInfo[i];
      for (int j = 0; j < layer.iNalCount; ++j) {
        total_size += layer.pNalLengthInByte[j];
      }
    }
    return total_size;
  }

  static void appendBitstream(const SFrameBSInfo& bs_info, std::vector<uint8_t>& out) {
    for (int i = 0; i < bs_info.iLayerNum; ++i) {
      const SLayerBSInfo& layer = bs_info.sLayerInfo[i];
      size_t layer_size = 0;
      for (int j = 0; j < layer.iNalCount; ++j) {
        layer_size += layer.pNalLengthInByte[j];
      }
      out.insert(out.end(), layer.pBsBuf, layer.pBsBuf + layer_size);
    }
  }

  static void copyBitstream(const SFrameBSInfo& bs_info, std::span<uint8_t> out) {
    size_t offset = 0;
    for (int i = 0; i < bs_info.iLayerNum; ++i) {
      const SLayerBSInfo& layer = bs_info.sLayerInfo[i];
      size_t layer_size = 0;
      for (int j = 0; j < layer.iNalCount; ++j) {
        layer_size += layer.pNalLengthInByte[j];
      }
      std::memcpy(out.data() + offset, layer.pBsBuf, layer_size);
      offset += layer_size;
    }
  }
};

const CodecDescriptor CODEC_OPENH264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "openh264",
    .long_name = "OpenH264",
    .vendor = "Cisco",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_CONSTRAINED_BASELINE},
        .levels = {
            LEVEL_1_B,
            LEVEL_1_0,
            LEVEL_1_1,
            LEVEL_1_2,
            LEVEL_1_3,
            LEVEL_2_0,
            LEVEL_2_1,
            LEVEL_2_2,
            LEVEL_3_0,
            LEVEL_3_1,
            LEVEL_3_2,
            LEVEL_4_0,
            LEVEL_4_1,
            LEVEL_4_2,
            LEVEL_5_0,
            LEVEL_5_1,
            LEVEL_5_2,
        },
        .threading = true,
        .video = VideoCodecCaps {
            .pix_fmts = {OM_FORMAT_YUV420P},
        },
    },
    .options = {
        OPENH264_ENC_USAGE_TYPE,
        OPENH264_ENC_COMPLEXITY,
        OPENH264_ENC_IDR_INTERVAL,
        OPENH264_ENC_TEMPORAL_LAYERS,
        OPENH264_ENC_NUM_REF_FRAME,
        OPENH264_ENC_SPS_PPS_ID_STRATEGY,
        OPENH264_ENC_PREFIX_NAL,
        OPENH264_ENC_SSEI,
        OPENH264_ENC_SIMULCAST_AVC,
        OPENH264_ENC_PADDING,
        OPENH264_ENC_ENTROPY_CODING,
        OPENH264_ENC_FRAME_SKIP,
        OPENH264_ENC_MAX_BITRATE,
        OPENH264_ENC_MAX_QP,
        OPENH264_ENC_MIN_QP,
        OPENH264_ENC_MAX_NAL_SIZE,
        OPENH264_ENC_LTR,
        OPENH264_ENC_LTR_REF_NUM,
        OPENH264_ENC_LTR_PERIOD,
        OPENH264_ENC_THREADS,
        OPENH264_ENC_LOAD_BALANCING,
        OPENH264_ENC_DENOISE,
        OPENH264_ENC_BGD,
        OPENH264_ENC_AQ,
        OPENH264_ENC_SCENE_CHANGE,
        OPENH264_ENC_LOOP_FILTER,
        OPENH264_ENC_LOOP_FILTER_ALPHA,
        OPENH264_ENC_LOOP_FILTER_BETA,
        OPENH264_ENC_FRAME_CROPPING,
        OPENH264_ENC_LOSSLESS_LINK,
        OPENH264_ENC_FIX_RC_OVERSHOOT,
        OPENH264_ENC_IDR_BITRATE_RATIO,
        OPENH264_ENC_PSNR_Y,
        OPENH264_ENC_PSNR_U,
        OPENH264_ENC_PSNR_V,
        OPENH264_ENC_LAYER_QP,
        OPENH264_ENC_SLICE_MODE,
        OPENH264_ENC_SLICE_NUM,
        OPENH264_ENC_SLICE_MB_NUM,
        OPENH264_ENC_SLICE_SIZE,
        OPENH264_ENC_VIDEO_SIGNAL_TYPE_PRESENT,
        OPENH264_ENC_VIDEO_FORMAT,
        OPENH264_ENC_FULL_RANGE,
        OPENH264_ENC_COLOR_DESCRIPTION_PRESENT,
        OPENH264_ENC_COLOR_PRIMARIES,
        OPENH264_ENC_TRANSFER_CHARACTERISTICS,
        OPENH264_ENC_COLOR_MATRIX,
        OPENH264_ENC_ASPECT_RATIO_PRESENT,
        OPENH264_ENC_ASPECT_RATIO,
        OPENH264_ENC_ASPECT_RATIO_EXT_WIDTH,
        OPENH264_ENC_ASPECT_RATIO_EXT_HEIGHT,
    },
    .decoder_factory = [] { return std::make_unique<OpenH264Decoder>(); },
    .encoder_factory = [] { return std::make_unique<OpenH264Encoder>(); },
};

} // namespace openmedia
