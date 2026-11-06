#include <openmedia/hw_cuda.h>
#include <codecs.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <openmedia/codec_api.hpp>
#include <openmedia/video.hpp>
#include <queue>
#include <vector>
#include "dx_h264.hpp"
#include "nv_common.hpp"
#include "nv_loader.hpp"

namespace openmedia {

namespace {

// How NVDEC lays a picture out: luma, then one interleaved (4:2:0, 4:2:2) or
// two separate (4:4:4) chroma planes. Deep samples are 16-bit, MSB-aligned.
struct SurfaceLayout {
  cudaVideoSurfaceFormat surface;
  OMPixelFormat format;
  uint32_t chroma_planes;
  bool full_height_chroma;
};

auto surfaceLayout(cudaVideoChromaFormat chroma, bool deep) -> std::optional<SurfaceLayout> {
  switch (chroma) {
    case cudaVideoChromaFormat_420:
      return deep ? SurfaceLayout {cudaVideoSurfaceFormat_P016, OM_FORMAT_P010, 1, false}
                  : SurfaceLayout {cudaVideoSurfaceFormat_NV12, OM_FORMAT_NV12, 1, false};
    case cudaVideoChromaFormat_422:
      return deep ? SurfaceLayout {cudaVideoSurfaceFormat_P216, OM_FORMAT_P216, 1, true}
                  : SurfaceLayout {cudaVideoSurfaceFormat_NV16, OM_FORMAT_NV16, 1, true};
    case cudaVideoChromaFormat_444:
      return deep ? SurfaceLayout {cudaVideoSurfaceFormat_YUV444_16Bit, OM_FORMAT_YUV444P16, 2, true}
                  : SurfaceLayout {cudaVideoSurfaceFormat_YUV444, OM_FORMAT_YUV444P, 2, true};
    default: return std::nullopt;
  }
}

auto gpuDecodes(const CUVIDEOFORMAT& format, cudaVideoSurfaceFormat surface) -> bool {
  CUVIDDECODECAPS caps = {};
  caps.eCodecType = format.codec;
  caps.eChromaFormat = format.chroma_format;
  caps.nBitDepthMinus8 = format.bit_depth_luma_minus8;
  // A driver too old to answer gets the benefit of the doubt; creating the decoder will tell.
  if (NVLoader::getInstance().cuvid()->cuvidGetDecoderCaps(&caps) != CUDA_SUCCESS) return true;
  return caps.bIsSupported && (caps.nOutputFormatMask & (1u << surface)) != 0;
}

} // namespace

// Device buffers decoded pictures are copied into, so that a picture never holds
// one of NVDEC's few mapped surfaces and outlives the decoder that made it.
class NVDecFramePool {
  CUcontext ctx_;
  uint32_t row_bytes_;
  uint32_t rows_;
  size_t pitch_ = 0;
  std::mutex mutex_;
  std::vector<CUdeviceptr> free_;
  std::vector<CUdeviceptr> all_;

public:
  NVDecFramePool(CUcontext ctx, uint32_t row_bytes, uint32_t rows) : ctx_(ctx), row_bytes_(row_bytes), rows_(rows) {}
  NVDecFramePool(const NVDecFramePool&) = delete;
  auto operator=(const NVDecFramePool&) -> NVDecFramePool& = delete;

  ~NVDecFramePool() {
    auto* cu = NVLoader::getInstance().cuda();
    if (!cu || all_.empty()) return;
    // The last picture may go away on a thread with no context current.
    cu->cuCtxPushCurrent(ctx_);
    for (CUdeviceptr ptr : all_) cu->cuMemFree(ptr);
    CUcontext popped = nullptr;
    cu->cuCtxPopCurrent(&popped);
  }

  auto pitch() const -> size_t { return pitch_; }

  auto acquire() -> CUdeviceptr {
    {
      std::lock_guard lock(mutex_);
      if (!free_.empty()) {
        const CUdeviceptr ptr = free_.back();
        free_.pop_back();
        return ptr;
      }
    }
    CUdeviceptr ptr = 0;
    size_t pitch = 0;
    auto* cu = NVLoader::getInstance().cuda();
    if (cu->cuMemAllocPitch(&ptr, &pitch, row_bytes_, rows_, 16) != CUDA_SUCCESS) return 0;
    std::lock_guard lock(mutex_);
    pitch_ = pitch;
    all_.push_back(ptr);
    return ptr;
  }

  void release(CUdeviceptr ptr) {
    std::lock_guard lock(mutex_);
    free_.push_back(ptr);
  }
};

class OPENMEDIA_ABI NVDecPicture : public CudaHardwarePicture {
  std::shared_ptr<NVDecFramePool> pool_;
  OMCudaPicture om_pic_;

public:
  NVDecPicture(std::shared_ptr<NVDecFramePool> pool, CUdeviceptr dev_ptr, uint32_t pitch)
      : pool_(std::move(pool)) {
    data = dev_ptr;
    this->pitch = pitch;
    om_pic_.data = dev_ptr;
    om_pic_.pitch = pitch;
  }
  ~NVDecPicture() override { pool_->release(data); }

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
  // Why the last sequence header produced no decoder; the parser only sees a 0.
  OMError sequence_error_ = OM_SUCCESS;
  OMCodecId codec_id_ = OM_CODEC_NONE;

  SurfaceLayout layout_ = {};
  uint32_t row_bytes_ = 0;
  std::shared_ptr<NVDecFramePool> pool_;
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

    const bool deep = format->bit_depth_luma_minus8 > 0;
    const auto layout = surfaceLayout(format->chroma_format, deep);
    if (!layout) {
      return rejectSequence(OM_CODEC_NOT_SUPPORTED,
                            std::format("chroma format {} is not supported", static_cast<int>(format->chroma_format)));
    }
    if (!gpuDecodes(*format, layout->surface)) {
      return rejectSequence(OM_CODEC_NOT_SUPPORTED,
                            std::format("this GPU cannot decode codec {} chroma {} at {} bits",
                                        static_cast<int>(format->codec), static_cast<int>(format->chroma_format),
                                        format->bit_depth_luma_minus8 + 8));
    }

    CUVIDDECODECREATEINFO create_info = {};
    create_info.CodecType = format->codec;
    create_info.ChromaFormat = format->chroma_format;
    create_info.OutputFormat = layout->surface;
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
    output_format_.format = layout->format;

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

    if (CUresult res = cuvid->cuvidCreateDecoder(&decoder_, &create_info); res != CUDA_SUCCESS) {
      return rejectSequence(OM_CODEC_HWACCEL_FAILED, std::format("cuvidCreateDecoder failed with error {}", static_cast<int>(res)));
    }
    sequence_error_ = OM_SUCCESS;
    layout_ = *layout;
    row_bytes_ = output_format_.width * (deep ? 2 : 1);
    pool_ = std::make_shared<NVDecFramePool>(cu_ctx_, row_bytes_,
                                             output_format_.height + layout_.chroma_planes * chromaRows());

    openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, "NVDEC: cuvidCreateDecoder success");
    return 1;
  }

  auto rejectSequence(OMError error, std::string_view why) -> int {
    openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: {}", why);
    sequence_error_ = error;
    return 0;
  }

  auto chromaRows() const -> uint32_t {
    const uint32_t height = output_format_.height;
    return layout_.full_height_chroma ? height : (height + 1) / 2;
  }

  // Copies a mapped surface into a pool buffer laid out the way copyPlanesToHost reads it.
  auto copyOut(CUdeviceptr mapped, uint32_t mapped_pitch) -> std::optional<CUdeviceptr> {
    const CUdeviceptr copy = pool_->acquire();
    if (!copy) return std::nullopt;
    auto* cu = NVLoader::getInstance().cuda();
    auto stream = static_cast<CUstream>(HWCudaContext_getStream(hw_context_));
    const uint32_t height = output_format_.height;
    // The driver pads luma to an even number of rows.
    const size_t src_plane = size_t(mapped_pitch) * ((height + 1) & ~1u);
    for (uint32_t plane = 0; plane <= layout_.chroma_planes; ++plane) {
      CUDA_MEMCPY2D c = {};
      c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      c.srcDevice = mapped + src_plane * plane;
      c.srcPitch = mapped_pitch;
      c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      c.dstDevice = copy + pool_->pitch() * height * plane;
      c.dstPitch = pool_->pitch();
      c.WidthInBytes = row_bytes_;
      c.Height = plane == 0 ? height : chromaRows();
      cu->cuMemcpy2DAsync(&c, stream);
    }
    if (cu->cuStreamSynchronize(stream) != CUDA_SUCCESS) {
      pool_->release(copy);
      return std::nullopt;
    }
    return copy;
  }

  auto makeFrame(CUdeviceptr data, int64_t pts) const -> Frame {
    auto buffer = std::make_shared<NVDecPicture>(pool_, data, static_cast<uint32_t>(pool_->pitch()));
    buffer->width = output_format_.width;
    buffer->height = output_format_.height;
    buffer->format = output_format_.format;

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
    pic.buffer = std::static_pointer_cast<HardwarePicture>(std::move(buffer));

    Frame frame = {};
    frame.pts = pts;
    frame.data = std::move(pic);
    return frame;
  }

  auto onPictureDecode(CUVIDPICPARAMS* params) -> int {
    if (!decoder_) return 0;
    auto* cuvid = NVLoader::getInstance().cuvid();
    CUresult res = cuvid->cuvidDecodePicture(decoder_, params);
    if (res != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: cuvidDecodePicture failed with error {}", (int) res);
      return 0;
    }
    return 1;
  }

  auto onPictureDisplay(CUVIDPARSERDISPINFO* info) -> int {
    if (!decoder_ || !pool_) return 1;
    auto* cuvid = NVLoader::getInstance().cuvid();

    CUVIDPROCPARAMS proc_params = {};
    proc_params.progressive_frame = info->progressive_frame;
    proc_params.second_field = 0;
    proc_params.top_field_first = info->top_field_first;

    CUdeviceptr mapped = 0;
    uint32_t mapped_pitch = 0;
    if (CUresult res = cuvid->cuvidMapVideoFrame(decoder_, info->picture_index, &mapped, &mapped_pitch, &proc_params);
        res != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: cuvidMapVideoFrame failed with error {}", (int) res);
      return 1;
    }
    // Unmapped straight away: there is one output surface, and a flush displays
    // several pictures before anyone could release the first.
    const auto copy = copyOut(mapped, mapped_pitch);
    cuvid->cuvidUnmapVideoFrame(decoder_, mapped);
    if (!copy) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "NVDEC: copying picture {} out of the decoder failed",
                     info->picture_index);
      return 1;
    }

    std::lock_guard lock(frames_mutex_);
    decoded_frames_.push_back(makeFrame(*copy, info->timestamp));
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
    sequence_error_ = OM_SUCCESS;
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

    cupkt.payload = bytes.data();
    cupkt.payload_size = (uint32_t) bytes.size();
    cupkt.timestamp = packet.pts;
    cupkt.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    if (packet.bytes.empty()) cupkt.flags |= CUVID_PKT_ENDOFSTREAM;

    CUresult res = cuvid->cuvidParseVideoData(parser_, &cupkt);
    if (sequence_error_ != OM_SUCCESS) return Err(sequence_error_);
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
    pool_.reset();
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
