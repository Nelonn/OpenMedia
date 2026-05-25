#include <openmedia/hw_cuda.h>
#include <codecs.hpp>
#include <mutex>
#include <openmedia/codec_api.hpp>
#include <openmedia/video.hpp>
#include <queue>
#include <vector>
#include "dx_h264.hpp"
#include "nv_common.hpp"
#include "nv_loader.hpp"

namespace openmedia {

class OPENMEDIA_ABI NVDecPicture : public CudaHardwarePicture {
  CUvideodecoder decoder_;
  CUdeviceptr dev_ptr_;
  OMCudaPicture om_pic_;
public:
  NVDecPicture(CUvideodecoder decoder, CUdeviceptr dev_ptr, uint32_t pitch)
      : decoder_(decoder), dev_ptr_(dev_ptr) {
    data = dev_ptr;
    this->pitch = pitch;
    om_pic_.data = dev_ptr;
    om_pic_.pitch = pitch;
  }
  ~NVDecPicture() override {
    auto* cuvid = NVLoader::getInstance().cuvid();
    if (cuvid && decoder_ && dev_ptr_) {
      cuvid->cuvidUnmapVideoFrame(decoder_, dev_ptr_);
    }
  }

  auto getOMPicture() -> OMCudaPicture* override { return &om_pic_; }
};

class NVDec final : public Decoder {
  OMCudaContext* hw_context_ = nullptr;
  CUvideoparser parser_ = nullptr;
  CUvideodecoder decoder_ = nullptr;
  CUcontext cu_ctx_ = nullptr;

  VideoFormat output_format_ = {};
  std::vector<Frame> decoded_frames_;
  std::mutex frames_mutex_;
  OMMasteringDisplayMetadata pending_mastering_display_ = {};
  OMContentLightLevel pending_content_light_level_ = {};

  bool initialized_ = false;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  dx_h264::State h264_;

  static auto CUDAAPI handleVideoSequence(void* user_data, CUVIDEOFORMAT* format) -> int {
    return static_cast<NVDec*>(user_data)->onVideoSequence(format);
  }

  static auto CUDAAPI handlePictureDecode(void* user_data, CUVIDPICPARAMS* params) -> int {
    return static_cast<NVDec*>(user_data)->onPictureDecode(params);
  }

  static auto CUDAAPI handlePictureDisplay(void* user_data, CUVIDPARSERDISPINFO* info) -> int {
    return static_cast<NVDec*>(user_data)->onPictureDisplay(info);
  }

  static auto CUDAAPI handleSEIMessage(void* user_data, CUVIDSEIMESSAGEINFO* info) -> int {
    return static_cast<NVDec*>(user_data)->onSEIMessage(info);
  }

  auto onSEIMessage(CUVIDSEIMESSAGEINFO* info) -> int {
    auto read_be16 = [](const uint8_t* data) -> uint16_t {
      return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8u) | data[1]);
    };
    auto read_be32 = [](const uint8_t* data) -> uint32_t {
      return (static_cast<uint32_t>(data[0]) << 24u) |
             (static_cast<uint32_t>(data[1]) << 16u) |
             (static_cast<uint32_t>(data[2]) << 8u) |
             static_cast<uint32_t>(data[3]);
    };

    auto* sei_data = static_cast<const uint8_t*>(info->pSEIData);
    size_t offset = 0;
    for (uint32_t i = 0; i < info->sei_message_count; ++i) {
      const auto& msg = info->pSEIMessage[i];
      if (!sei_data) break;
      const auto* payload = sei_data + offset;
      if (msg.sei_message_type == 137 && msg.sei_message_size >= 24) {
        for (int p = 0; p < 3; ++p) {
          pending_mastering_display_.display_primaries[p][0] = read_be16(payload + p * 4);
          pending_mastering_display_.display_primaries[p][1] = read_be16(payload + p * 4 + 2);
        }
        pending_mastering_display_.white_point[0] = read_be16(payload + 12);
        pending_mastering_display_.white_point[1] = read_be16(payload + 14);
        pending_mastering_display_.max_display_mastering_luminance = read_be32(payload + 16);
        pending_mastering_display_.min_display_mastering_luminance = read_be32(payload + 20);
        pending_mastering_display_.has_value = true;
        output_format_.mastering_display = pending_mastering_display_;
      } else if (msg.sei_message_type == 144 && msg.sei_message_size >= 4) {
        pending_content_light_level_.max_content_light_level = read_be16(payload);
        pending_content_light_level_.max_pic_average_light_level = read_be16(payload + 2);
        pending_content_light_level_.has_value = true;
        output_format_.content_light_level = pending_content_light_level_;
      }
      offset += msg.sei_message_size;
    }
    return 1;
  }

  auto onVideoSequence(CUVIDEOFORMAT* format) -> int {
    auto* cuvid = NVLoader::getInstance().cuvid();

    if (decoder_) {
      cuvid->cuvidDestroyDecoder(decoder_);
      decoder_ = nullptr;
    }

    CUVIDDECODECREATEINFO create_info = {};
    create_info.CodecType = format->codec;
    create_info.ChromaFormat = format->chroma_format;
    create_info.OutputFormat = (format->bit_depth_luma_minus8 > 0) ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
    create_info.bitDepthMinus8 = format->bit_depth_luma_minus8;
    create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Adaptive;
    create_info.ulNumDecodeSurfaces = 16;
    create_info.ulNumOutputSurfaces = 1;
    create_info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    create_info.ulWidth = format->coded_width;
    create_info.ulHeight = format->coded_height;
    create_info.ulTargetWidth = format->display_area.right - format->display_area.left;
    create_info.ulTargetHeight = format->display_area.bottom - format->display_area.top;

    output_format_.width = create_info.ulTargetWidth;
    output_format_.height = create_info.ulTargetHeight;
    output_format_.format = (format->bit_depth_luma_minus8 > 0) ? OM_FORMAT_P010 : OM_FORMAT_NV12;

    // Report color properties
    auto map_primaries = [](uint8_t p) -> OMColorPrimaries {
      switch (p) {
        case 1: return OM_PRIMARIES_BT709;
        case 9: return OM_PRIMARIES_BT2020;
        default: return OM_PRIMARIES_UNKNOWN;
      }
    };
    auto map_transfer = [](uint8_t t) -> OMTransferCharacteristic {
      switch (t) {
        case 1: return OM_TRANSFER_BT709;
        case 16: return OM_TRANSFER_PQ;
        case 18: return OM_TRANSFER_HLG;
        default: return OM_TRANSFER_UNKNOWN;
      }
    };
    auto map_matrix = [](uint8_t m) -> OMColorSpace {
      switch (m) {
        case 1: return OM_COLOR_SPACE_BT709;
        case 9: return OM_COLOR_SPACE_BT2020;
        default: return OM_COLOR_SPACE_BT709;
      }
    };

    output_format_.color_primaries = map_primaries(format->video_signal_description.color_primaries);
    output_format_.transfer_char = map_transfer(format->video_signal_description.transfer_characteristics);
    output_format_.color_space = map_matrix(format->video_signal_description.matrix_coefficients);
    output_format_.color_range = format->video_signal_description.video_full_range_flag ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;

    if (format->seqhdr_data_length >= sizeof(CUVIDEOFORMATEX)) {
      CUVIDEOFORMATEX* ex = (CUVIDEOFORMATEX*) format;
      if (ex->format.display_area.right > 0) { // Check if we have extended info
          // CUVID structure for HDR varies by version. 
          // For now, we've enabled color volume reporting.
      }
    }

    if (cuvid->cuvidCreateDecoder(&decoder_, &create_info) != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: cuvidCreateDecoder failed");
      return 0;
    }

    openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, "NVDEC: cuvidCreateDecoder success");
    return 1;
  }

  auto onPictureDecode(CUVIDPICPARAMS* params) -> int {
    auto* cuvid = NVLoader::getInstance().cuvid();
    CUresult res = cuvid->cuvidDecodePicture(decoder_, params);
    if (res != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: cuvidDecodePicture failed with error {}", (int) res);
      return 0;
    }
    return 1;
  }

  auto onPictureDisplay(CUVIDPARSERDISPINFO* info) -> int {
    auto* cuvid = NVLoader::getInstance().cuvid();

    CUVIDPROCPARAMS proc_params = {};
    proc_params.progressive_frame = info->progressive_frame;
    proc_params.second_field = 0;
    proc_params.top_field_first = info->top_field_first;

    CUdeviceptr dev_ptr = 0;
    uint32_t pitch = 0;
    CUresult res = cuvid->cuvidMapVideoFrame(decoder_, info->picture_index, &dev_ptr, &pitch, &proc_params);
    if (res == CUDA_SUCCESS) {
      auto pic_buffer = std::make_shared<NVDecPicture>(decoder_, dev_ptr, pitch);
      pic_buffer->pitch = pitch;
      pic_buffer->width = output_format_.width;
      pic_buffer->height = output_format_.height;
      pic_buffer->format = output_format_.format;

      Frame frame = {};
      frame.pts = info->timestamp;
      Picture pic;
      pic.format = output_format_.format;
      pic.width = output_format_.width;
      pic.height = output_format_.height;
      pic.color_space = output_format_.color_space;
      pic.transfer_char = output_format_.transfer_char;
      pic.color_primaries = output_format_.color_primaries;
      pic.color_range = output_format_.color_range;
      pic.mastering_display = pending_mastering_display_;
      pic.content_light_level = pending_content_light_level_;
      pic.buffer = std::static_pointer_cast<HardwarePicture>(pic_buffer);
      frame.data = std::move(pic);

      {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        decoded_frames_.push_back(std::move(frame));
      }
    } else {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: cuvidMapVideoFrame failed with error {}", (int) res);
    }

    return 1;
  }

public:
  NVDec() = default;
  ~NVDec() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (!options.hw_device.has_value() || options.hw_device->type != HWDeviceType::CUDA) {
      return OM_CODEC_HWACCEL_FAILED;
    }
    hw_context_ = static_cast<OMCudaContext*>(options.hw_device->context);
    cu_ctx_ = (CUcontext) HWCudaContext_getContext(hw_context_);

    if (!NVLoader::getInstance().load()) return OM_CODEC_HWACCEL_FAILED;
    auto* cuvid = NVLoader::getInstance().cuvid();

    codec_id_ = options.format.codec_id;
    h264_ = {};
    output_format_ = {};
    output_format_.color_space = options.format.video.color_space;
    output_format_.transfer_char = options.format.video.transfer_char;
    output_format_.color_primaries = options.format.video.color_primaries;
    output_format_.color_range = OM_COLOR_RANGE_UNSPECIFIED;
    pending_mastering_display_ = {};
    pending_content_light_level_ = {};
    if (codec_id_ == OM_CODEC_H264) {
      h264_.parseExtradata(options.extradata);
    }

    CUVIDPARSERPARAMS parser_params = {};
    switch (codec_id_) {
      case OM_CODEC_H264: parser_params.CodecType = cudaVideoCodec_H264; break;
      case OM_CODEC_H265: parser_params.CodecType = cudaVideoCodec_HEVC; break;
      case OM_CODEC_VP9: parser_params.CodecType = cudaVideoCodec_VP9; break;
      case OM_CODEC_AV1: parser_params.CodecType = cudaVideoCodec_AV1; break;
      default: return OM_CODEC_NOT_SUPPORTED;
    }
    parser_params.ulMaxNumDecodeSurfaces = 16;
    parser_params.pUserData = this;
    parser_params.pfnSequenceCallback = NVDec::handleVideoSequence;
    parser_params.pfnDecodePicture = NVDec::handlePictureDecode;
    parser_params.pfnDisplayPicture = NVDec::handlePictureDisplay;
    parser_params.pfnGetSEIMsg = NVDec::handleSEIMessage;

    CUVIDEOFORMATEX ext_format = {};
    if (!options.extradata.empty()) {
      ext_format.format.seqhdr_data_length = std::min<uint32_t>((uint32_t) options.extradata.size(), 1024);
      std::memcpy(ext_format.raw_seqhdr_data, options.extradata.data(), ext_format.format.seqhdr_data_length);
      parser_params.pExtVideoInfo = &ext_format;
    }

    if (cuvid->cuvidCreateVideoParser(&parser_, &parser_params) != CUDA_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

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
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);

    auto* cuvid = NVLoader::getInstance().cuvid();
    CUVIDSOURCEDATAPACKET cupkt = {};

    std::span<const uint8_t> bytes = packet.bytes;

    static bool first_packet = true;
    if (first_packet) {
      if (bytes.size() >= 4) {
        openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, "NVDEC: pkt size {} first bytes: {:02x} {:02x} {:02x} {:02x}", bytes.size(), bytes[0], bytes[1], bytes[2], bytes[3]);
        openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, "NVDEC: nal_length_size: {}", h264_.nal_length_size);
      }
      first_packet = false;
    }

    cupkt.payload = bytes.data();
    cupkt.payload_size = (uint32_t) bytes.size();
    cupkt.timestamp = packet.pts;
    cupkt.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    if (packet.bytes.empty()) cupkt.flags |= CUVID_PKT_ENDOFSTREAM;

    CUresult res = cuvid->cuvidParseVideoData(parser_, &cupkt);
    if (res != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: cuvidParseVideoData failed with error {}", (int) res);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    std::vector<Frame> result;
    {
      std::lock_guard<std::mutex> lock(frames_mutex_);
      result = std::move(decoded_frames_);
      decoded_frames_.clear();
    }
    return Ok(std::move(result));
  }

  void flush() override {
    decode(Packet {});
  }

  void release() {
    auto* cuvid = NVLoader::getInstance().cuvid();
    if (cuvid) {
      if (parser_) cuvid->cuvidDestroyVideoParser(parser_);
      if (decoder_) cuvid->cuvidDestroyDecoder(decoder_);
    }
    parser_ = nullptr;
    decoder_ = nullptr;
    initialized_ = false;
  }
};

const CodecDescriptor CODEC_NVDEC_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "nvdec_h264",
    .long_name = "NVIDIA NVDEC H.264",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .decoder_factory = []() -> std::unique_ptr<Decoder> { return std::make_unique<NVDec>(); },
};

const CodecDescriptor CODEC_NVDEC_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "nvdec_h265",
    .long_name = "NVIDIA NVDEC H.265",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .decoder_factory = []() -> std::unique_ptr<Decoder> { return std::make_unique<NVDec>(); },
};

const CodecDescriptor CODEC_NVDEC_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "nvdec_vp9",
    .long_name = "NVIDIA NVDEC VP9",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .decoder_factory = []() -> std::unique_ptr<Decoder> { return std::make_unique<NVDec>(); },
};

const CodecDescriptor CODEC_NVDEC_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "nvdec_av1",
    .long_name = "NVIDIA NVDEC AV1",
    .vendor = "NVIDIA",
    .flags = HARDWARE,
    .decoder_factory = []() -> std::unique_ptr<Decoder> { return std::make_unique<NVDec>(); },
};

} // namespace openmedia
