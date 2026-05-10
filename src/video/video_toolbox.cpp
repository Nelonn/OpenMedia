#include <codecs.hpp>

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <openmedia/video.hpp>
#include <span>
#include <vector>

namespace openmedia {

namespace {

constexpr int32_t kDefaultTimeScale = 1'000'000;

template<typename T>
class CFPtr {
public:
  CFPtr() = default;

  explicit CFPtr(T ref)
      : ref_(ref) {}

  CFPtr(const CFPtr&) = delete;
  auto operator=(const CFPtr&) -> CFPtr& = delete;

  CFPtr(CFPtr&& other) noexcept
      : ref_(other.ref_) {
    other.ref_ = nullptr;
  }

  auto operator=(CFPtr&& other) noexcept -> CFPtr& {
    if (this != &other) {
      reset(other.ref_);
      other.ref_ = nullptr;
    }
    return *this;
  }

  ~CFPtr() {
    reset();
  }

  auto get() const noexcept -> T { return ref_; }
  explicit operator bool() const noexcept { return ref_ != nullptr; }

  void reset(T ref = nullptr) noexcept {
    if (ref_) {
      CFRelease(ref_);
    }
    ref_ = ref;
  }

private:
  T ref_ = nullptr;
};

auto codecIdToVideoToolboxFormat(OMCodecId codec_id, OMProfile profile) noexcept -> CMVideoCodecType {
  switch (codec_id) {
    case OM_CODEC_H263:
      return kCMVideoCodecType_H263;
    case OM_CODEC_H264:
      return kCMVideoCodecType_H264;
    case OM_CODEC_H265:
      return kCMVideoCodecType_HEVC;
    case OM_CODEC_H262:
      return kCMVideoCodecType_MPEG2Video;
    case OM_CODEC_MPEG4:
      return kCMVideoCodecType_MPEG4Video;
    case OM_CODEC_AV1:
      return kCMVideoCodecType_AV1;
    case OM_CODEC_PRORES:
      switch (profile) {
        case OM_PROFILE_PRORES_PROXY: return kCMVideoCodecType_AppleProRes422Proxy;
        case OM_PROFILE_PRORES_LT: return kCMVideoCodecType_AppleProRes422LT;
        case OM_PROFILE_PRORES_STANDARD: return kCMVideoCodecType_AppleProRes422;
        case OM_PROFILE_PRORES_HQ: return kCMVideoCodecType_AppleProRes422HQ;
        case OM_PROFILE_PRORES_4444: return kCMVideoCodecType_AppleProRes4444;
        case OM_PROFILE_PRORES_XQ: return kCMVideoCodecType_AppleProRes4444XQ;
        default: return kCMVideoCodecType_AppleProRes422;
      }
    default:
      return 0;
  }
}

auto isParameterSetCodec(OMCodecId codec_id) noexcept -> bool {
  return codec_id == OM_CODEC_H264 || codec_id == OM_CODEC_H265;
}

auto isAnnexB(std::span<const uint8_t> bytes) noexcept -> bool {
  return bytes.size() >= 4 &&
         bytes[0] == 0x00 &&
         bytes[1] == 0x00 &&
         (bytes[2] == 0x01 || (bytes[2] == 0x00 && bytes[3] == 0x01));
}

auto nextStartCode(std::span<const uint8_t> bytes, size_t offset) noexcept -> size_t {
  for (size_t i = offset; i + 3 <= bytes.size(); ++i) {
    if (bytes[i] == 0x00 && bytes[i + 1] == 0x00) {
      if (bytes[i + 2] == 0x01) {
        return i;
      }
      if (i + 4 <= bytes.size() && bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01) {
        return i;
      }
    }
  }
  return bytes.size();
}

auto startCodeLength(std::span<const uint8_t> bytes, size_t offset) noexcept -> size_t {
  if (offset + 3 <= bytes.size() &&
      bytes[offset] == 0x00 &&
      bytes[offset + 1] == 0x00 &&
      bytes[offset + 2] == 0x01) {
    return 3;
  }
  if (offset + 4 <= bytes.size() &&
      bytes[offset] == 0x00 &&
      bytes[offset + 1] == 0x00 &&
      bytes[offset + 2] == 0x00 &&
      bytes[offset + 3] == 0x01) {
    return 4;
  }
  return 0;
}

template<typename Fn>
void forEachAnnexBNal(std::span<const uint8_t> bytes, Fn&& fn) {
  size_t start = nextStartCode(bytes, 0);
  while (start < bytes.size()) {
    const size_t prefix = startCodeLength(bytes, start);
    if (prefix == 0) {
      break;
    }

    const size_t nal_start = start + prefix;
    size_t next = nextStartCode(bytes, nal_start);
    size_t nal_end = next;
    while (nal_end > nal_start && bytes[nal_end - 1] == 0x00) {
      --nal_end;
    }

    if (nal_end > nal_start) {
      fn(bytes.subspan(nal_start, nal_end - nal_start));
    }
    start = next;
  }
}

auto annexBToLengthPrefixed(std::span<const uint8_t> bytes, size_t length_size = 4) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());

  forEachAnnexBNal(bytes, [&](std::span<const uint8_t> nal) {
    const auto size = static_cast<uint32_t>(nal.size());
    for (size_t i = 0; i < length_size; ++i) {
      const size_t shift = 8 * (length_size - i - 1);
      out.push_back(static_cast<uint8_t>((size >> shift) & 0xFF));
    }
    out.insert(out.end(), nal.begin(), nal.end());
  });

  return out;
}

auto h264NalType(std::span<const uint8_t> nal) noexcept -> uint8_t {
  return nal.empty() ? 0 : (nal[0] & 0x1Fu);
}

auto hevcNalType(std::span<const uint8_t> nal) noexcept -> uint8_t {
  return nal.empty() ? 0 : ((nal[0] >> 1u) & 0x3Fu);
}

struct ParameterSets {
  std::vector<std::vector<uint8_t>> sets;
  int nal_length_size = 4;
};

auto parseH264ParameterSets(std::span<const uint8_t> extradata) -> ParameterSets {
  ParameterSets result;

  if (extradata.size() >= 7 && extradata[0] == 1) {
    result.nal_length_size = (extradata[4] & 0x03u) + 1;
    size_t offset = 5;
    const uint8_t sps_count = extradata[offset++] & 0x1Fu;
    for (uint8_t i = 0; i < sps_count && offset + 2 <= extradata.size(); ++i) {
      const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
      offset += 2;
      if (offset + size > extradata.size()) break;
      result.sets.emplace_back(extradata.begin() + offset, extradata.begin() + offset + size);
      offset += size;
    }
    if (offset < extradata.size()) {
      const uint8_t pps_count = extradata[offset++];
      for (uint8_t i = 0; i < pps_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) break;
        result.sets.emplace_back(extradata.begin() + offset, extradata.begin() + offset + size);
        offset += size;
      }
    }
    return result;
  }

  if (isAnnexB(extradata)) {
    forEachAnnexBNal(extradata, [&](std::span<const uint8_t> nal) {
      const uint8_t type = h264NalType(nal);
      if (type == 7 || type == 8 || type == 13) {
        result.sets.emplace_back(nal.begin(), nal.end());
      }
    });
  }

  return result;
}

auto parseHEVCParameterSets(std::span<const uint8_t> extradata) -> ParameterSets {
  ParameterSets result;

  if (extradata.size() >= 23 && extradata[0] == 1) {
    result.nal_length_size = (extradata[21] & 0x03u) + 1;
    size_t offset = 22;
    const uint8_t array_count = extradata[offset++];
    for (uint8_t i = 0; i < array_count && offset + 3 <= extradata.size(); ++i) {
      offset += 1;
      const uint16_t nal_count = static_cast<uint16_t>((extradata[offset] << 8u) | extradata[offset + 1]);
      offset += 2;
      for (uint16_t j = 0; j < nal_count && offset + 2 <= extradata.size(); ++j) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) break;
        result.sets.emplace_back(extradata.begin() + offset, extradata.begin() + offset + size);
        offset += size;
      }
    }
    return result;
  }

  if (isAnnexB(extradata)) {
    forEachAnnexBNal(extradata, [&](std::span<const uint8_t> nal) {
      const uint8_t type = hevcNalType(nal);
      if (type == 32 || type == 33 || type == 34 || type == 39 || type == 40) {
        result.sets.emplace_back(nal.begin(), nal.end());
      }
    });
  }

  return result;
}

auto makeExtradataExtensions(OMCodecId codec_id, std::span<const uint8_t> extradata) -> CFPtr<CFDictionaryRef> {
  if (extradata.empty()) {
    return {};
  }

  const char* atom = nullptr;
  switch (codec_id) {
    case OM_CODEC_AV1: atom = "av1C"; break;
    case OM_CODEC_MPEG4: atom = "esds"; break;
    default: break;
  }
  if (!atom) {
    return {};
  }

  CFPtr<CFStringRef> atom_key(CFStringCreateWithCString(kCFAllocatorDefault, atom, kCFStringEncodingASCII));
  CFPtr<CFDataRef> atom_data(CFDataCreate(kCFAllocatorDefault,
                                          reinterpret_cast<const UInt8*>(extradata.data()),
                                          static_cast<CFIndex>(extradata.size())));
  if (!atom_key || !atom_data) {
    return {};
  }

  CFTypeRef atom_keys[] = {atom_key.get()};
  CFTypeRef atom_values[] = {atom_data.get()};
  CFPtr<CFDictionaryRef> atoms(CFDictionaryCreate(kCFAllocatorDefault,
                                                  atom_keys,
                                                  atom_values,
                                                  1,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks));
  if (!atoms) {
    return {};
  }

  CFTypeRef extension_keys[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms};
  CFTypeRef extension_values[] = {atoms.get()};
  return CFPtr<CFDictionaryRef>(CFDictionaryCreate(kCFAllocatorDefault,
                                                   extension_keys,
                                                   extension_values,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
}

auto createFormatDescription(OMCodecId codec_id,
                             OMProfile profile,
                             uint32_t width,
                             uint32_t height,
                             std::span<const uint8_t> extradata,
                             int* nal_length_size) -> CFPtr<CMVideoFormatDescriptionRef> {
  if (nal_length_size) {
    *nal_length_size = 4;
  }

  CMFormatDescriptionRef raw_parameter_format = nullptr;
  if (codec_id == OM_CODEC_H264) {
    auto parameter_sets = parseH264ParameterSets(extradata);
    if (nal_length_size) {
      *nal_length_size = parameter_sets.nal_length_size;
    }
    if (parameter_sets.sets.size() >= 2) {
      std::vector<const uint8_t*> pointers;
      std::vector<size_t> sizes;
      pointers.reserve(parameter_sets.sets.size());
      sizes.reserve(parameter_sets.sets.size());
      for (const auto& set : parameter_sets.sets) {
        pointers.push_back(set.data());
        sizes.push_back(set.size());
      }
      OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                                                                            pointers.size(),
                                                                            pointers.data(),
                                                                            sizes.data(),
                                                                            parameter_sets.nal_length_size,
                                                                            &raw_parameter_format);
      if (status == noErr && raw_parameter_format) {
        return CFPtr<CMVideoFormatDescriptionRef>(static_cast<CMVideoFormatDescriptionRef>(raw_parameter_format));
      }
      log(OM_CATEGORY_DECODER,
          OM_LEVEL_WARNING,
          std::format("VideoToolbox H.264 parameter-set format creation failed: {}", static_cast<int32_t>(status)));
    }
  }

  if (codec_id == OM_CODEC_H265) {
    auto parameter_sets = parseHEVCParameterSets(extradata);
    if (nal_length_size) {
      *nal_length_size = parameter_sets.nal_length_size;
    }
    if (parameter_sets.sets.size() >= 3) {
      std::vector<const uint8_t*> pointers;
      std::vector<size_t> sizes;
      pointers.reserve(parameter_sets.sets.size());
      sizes.reserve(parameter_sets.sets.size());
      for (const auto& set : parameter_sets.sets) {
        pointers.push_back(set.data());
        sizes.push_back(set.size());
      }
      OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,
                                                                            pointers.size(),
                                                                            pointers.data(),
                                                                            sizes.data(),
                                                                            parameter_sets.nal_length_size,
                                                                            nullptr,
                                                                            &raw_parameter_format);
      if (status == noErr && raw_parameter_format) {
        return CFPtr<CMVideoFormatDescriptionRef>(static_cast<CMVideoFormatDescriptionRef>(raw_parameter_format));
      }
      log(OM_CATEGORY_DECODER,
          OM_LEVEL_WARNING,
          std::format("VideoToolbox HEVC parameter-set format creation failed: {}", static_cast<int32_t>(status)));
    }
  }

  CMVideoFormatDescriptionRef raw_format = nullptr;
  auto extensions = makeExtradataExtensions(codec_id, extradata);
  OSStatus status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                                   codecIdToVideoToolboxFormat(codec_id, profile),
                                                   static_cast<int32_t>(width),
                                                   static_cast<int32_t>(height),
                                                   extensions.get(),
                                                   &raw_format);
  if (status != noErr || !raw_format) {
    log(OM_CATEGORY_DECODER,
        OM_LEVEL_ERROR,
        std::format("VideoToolbox format creation failed: {}", static_cast<int32_t>(status)));
    return {};
  }

  return CFPtr<CMVideoFormatDescriptionRef>(raw_format);
}

auto makeOutputAttributes() -> CFPtr<CFDictionaryRef> {
  int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  CFPtr<CFNumberRef> pixel_format_number(CFNumberCreate(kCFAllocatorDefault,
                                                        kCFNumberSInt32Type,
                                                        &pixel_format));
  if (!pixel_format_number) {
    return {};
  }

  CFTypeRef keys[] = {kCVPixelBufferPixelFormatTypeKey};
  CFTypeRef values[] = {pixel_format_number.get()};
  return CFPtr<CFDictionaryRef>(CFDictionaryCreate(kCFAllocatorDefault,
                                                   keys,
                                                   values,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
}

auto makeDecoderSpecification() -> CFPtr<CFDictionaryRef> {
  CFTypeRef keys[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
  CFTypeRef values[] = {kCFBooleanTrue};
  return CFPtr<CFDictionaryRef>(CFDictionaryCreate(kCFAllocatorDefault,
                                                   keys,
                                                   values,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
}

auto makeTime(int64_t value, int32_t timescale) -> CMTime {
  return value >= 0 ? CMTimeMake(value, timescale) : kCMTimeInvalid;
}

void copyPlaneWithStride(uint8_t* dst,
                         size_t dst_stride,
                         const uint8_t* src,
                         size_t src_stride,
                         size_t row_bytes,
                         size_t height) {
  for (size_t y = 0; y < height; ++y) {
    std::memcpy(dst, src, row_bytes);
    dst += dst_stride;
    src += src_stride;
  }
}

auto cvPixelFormatToOpenMedia(OSType pixel_format) noexcept -> OMPixelFormat {
  switch (pixel_format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
      return OM_FORMAT_NV12;
    case kCVPixelFormatType_32BGRA:
      return OM_FORMAT_B8G8R8A8;
    default:
      return OM_FORMAT_UNKNOWN;
  }
}

struct CallbackContext {
  std::vector<Frame>* frames = nullptr;
  OMColorSpace color_space = OM_COLOR_SPACE_BT709;
  OMTransferCharacteristic transfer_char = OM_TRANSFER_BT709;
};

struct FrameTiming {
  int64_t pts = -1;
  int64_t dts = -1;
  bool is_keyframe = false;
};

void decompressionCallback(void* decompression_output_ref_con,
                           void* source_frame_ref_con,
                           OSStatus status,
                           VTDecodeInfoFlags,
                           CVImageBufferRef image_buffer,
                           CMTime,
                           CMTime) {
  auto** context_ptr = static_cast<CallbackContext**>(decompression_output_ref_con);
  auto* context = context_ptr ? *context_ptr : nullptr;
  auto* timing = static_cast<FrameTiming*>(source_frame_ref_con);
  if (!context || !context->frames || status != noErr || !image_buffer) {
    if (status != noErr) {
      log(OM_CATEGORY_DECODER,
          OM_LEVEL_ERROR,
          std::format("VideoToolbox decode callback failed: {}", static_cast<int32_t>(status)));
    }
    return;
  }

  auto pixel_buffer = static_cast<CVPixelBufferRef>(image_buffer);
  const OSType cv_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
  const OMPixelFormat om_format = cvPixelFormatToOpenMedia(cv_format);
  if (om_format == OM_FORMAT_UNKNOWN) {
    log(OM_CATEGORY_DECODER,
        OM_LEVEL_ERROR,
        std::format("VideoToolbox produced unsupported pixel format: {}", static_cast<uint32_t>(cv_format)));
    return;
  }

  CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

  const auto width = static_cast<uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
  const auto height = static_cast<uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
  Picture picture(om_format, width, height);
  picture.color_space = context->color_space;
  picture.transfer_char = context->transfer_char;
  picture.is_keyframe = timing ? timing->is_keyframe : false;

  if (CVPixelBufferIsPlanar(pixel_buffer)) {
    const size_t plane_count = std::min<size_t>(CVPixelBufferGetPlaneCount(pixel_buffer), picture.planes.count);
    for (size_t i = 0; i < plane_count; ++i) {
      const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, i));
      const size_t src_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, i);
      const size_t plane_width = CVPixelBufferGetWidthOfPlane(pixel_buffer, i);
      const size_t plane_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, i);
      const size_t row_bytes = plane_width * getBytesPerPixel(om_format, static_cast<uint8_t>(i));
      copyPlaneWithStride(picture.planes.data[i],
                          picture.planes.linesize[i],
                          src,
                          src_stride,
                          row_bytes,
                          plane_height);
    }
  } else {
    const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const size_t src_stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    const size_t row_bytes = static_cast<size_t>(width) * getBytesPerPixel(om_format, 0);
    copyPlaneWithStride(picture.planes.data[0],
                        picture.planes.linesize[0],
                        src,
                        src_stride,
                        row_bytes,
                        height);
  }

  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

  Frame frame = {};
  frame.pts = timing && timing->pts >= 0 ? static_cast<uint64_t>(timing->pts) : 0;
  frame.dts = timing && timing->dts >= 0 ? static_cast<uint64_t>(timing->dts) : frame.pts;
  frame.data = std::move(picture);
  context->frames->push_back(std::move(frame));
}

class VideoToolboxDecoder final : public Decoder {
public:
  explicit VideoToolboxDecoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

  ~VideoToolboxDecoder() override {
    closeSession();
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    closeSession();

    if (options.format.type != OM_MEDIA_VIDEO || options.format.codec_id != codec_id_) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.format.video.width == 0 || options.format.video.height == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (codecIdToVideoToolboxFormat(codec_id_, options.format.profile) == 0) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    profile_ = options.format.profile;
    color_space_ = options.format.video.color_space;
    transfer_char_ = options.format.video.transfer_char;
    output_format_.width = options.format.video.width;
    output_format_.height = options.format.video.height;
    output_format_.format = OM_FORMAT_NV12;
    timescale_ = (options.time_base.num > 0 && options.time_base.den > 0)
                     ? options.time_base.den
                     : kDefaultTimeScale;

    format_description_ = createFormatDescription(codec_id_,
                                                  profile_,
                                                  output_format_.width,
                                                  output_format_.height,
                                                  options.extradata,
                                                  &nal_length_size_);
    if (!format_description_) {
      return OM_CODEC_OPEN_FAILED;
    }

    auto err = openSession();
    if (err != OM_SUCCESS) {
      return err;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) {
      return std::nullopt;
    }

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_ || !session_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    std::vector<Frame> frames;
    if (packet.bytes.empty()) {
      VTDecompressionSessionFinishDelayedFrames(session_.get());
      VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
      return Ok(std::move(frames));
    }

    std::span<const uint8_t> payload(packet.bytes.data(), packet.bytes.size());
    std::vector<uint8_t> converted;
    if (isParameterSetCodec(codec_id_) && isAnnexB(payload)) {
      converted = annexBToLengthPrefixed(payload, static_cast<size_t>(nal_length_size_));
      payload = converted;
      if (payload.empty()) {
        return Ok(std::move(frames));
      }
    }

    auto block_buffer = makeBlockBuffer(payload);
    if (!block_buffer) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    CMSampleTimingInfo timing = {};
    timing.duration = packet.duration > 0 ? CMTimeMake(packet.duration, timescale_) : kCMTimeInvalid;
    timing.presentationTimeStamp = makeTime(packet.pts, timescale_);
    timing.decodeTimeStamp = makeTime(packet.dts, timescale_);

    const size_t sample_size = payload.size();
    CMSampleBufferRef raw_sample_buffer = nullptr;
    OSStatus status = CMSampleBufferCreateReady(kCFAllocatorDefault,
                                                block_buffer.get(),
                                                format_description_.get(),
                                                1,
                                                1,
                                                &timing,
                                                1,
                                                &sample_size,
                                                &raw_sample_buffer);
    CFPtr<CMSampleBufferRef> sample_buffer(raw_sample_buffer);
    if (status != noErr || !sample_buffer) {
      logStatus(OM_LEVEL_ERROR, "CMSampleBufferCreateReady", status);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer.get(), true)) {
      auto attachment = const_cast<CFMutableDictionaryRef>(
          static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0)));
      if (attachment) {
        if (!packet.is_keyframe) {
          CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
        }
        CFDictionarySetValue(attachment,
                             kCMSampleAttachmentKey_DependsOnOthers,
                             packet.is_keyframe ? kCFBooleanFalse : kCFBooleanTrue);
      }
    }

    CallbackContext callback_context;
    callback_context.frames = &frames;
    callback_context.color_space = color_space_;
    callback_context.transfer_char = transfer_char_;

    FrameTiming frame_timing;
    frame_timing.pts = packet.pts;
    frame_timing.dts = packet.dts;
    frame_timing.is_keyframe = packet.is_keyframe;

    callback_context_ = &callback_context;
    VTDecodeInfoFlags info_flags = 0;
    status = VTDecompressionSessionDecodeFrame(session_.get(),
                                               sample_buffer.get(),
                                               0,
                                               &frame_timing,
                                               &info_flags);
    callback_context_ = nullptr;
    if (status != noErr) {
      logStatus(OM_LEVEL_ERROR, "VTDecompressionSessionDecodeFrame", status);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (session_) {
      VTDecompressionSessionFinishDelayedFrames(session_.get());
      VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
    }
  }

private:
  auto openSession() -> OMError {
    auto output_attributes = makeOutputAttributes();
    auto decoder_specification = makeDecoderSpecification();
    VTDecompressionOutputCallbackRecord callback = {};
    callback.decompressionOutputCallback = decompressionCallback;
    callback.decompressionOutputRefCon = &callback_context_;

    VTDecompressionSessionRef raw_session = nullptr;
    OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault,
                                                   format_description_.get(),
                                                   decoder_specification.get(),
                                                   output_attributes.get(),
                                                   &callback,
                                                   &raw_session);
    if (status != noErr || !raw_session) {
      logStatus(OM_LEVEL_ERROR, "VTDecompressionSessionCreate", status);
      return OM_CODEC_OPEN_FAILED;
    }

    session_.reset(raw_session);
    return OM_SUCCESS;
  }

  void closeSession() {
    initialized_ = false;
    callback_context_ = nullptr;
    if (session_) {
      VTDecompressionSessionInvalidate(session_.get());
      session_.reset();
    }
    format_description_.reset();
    output_format_ = {};
    profile_ = OM_PROFILE_NONE;
    nal_length_size_ = 4;
    timescale_ = kDefaultTimeScale;
  }

  auto makeBlockBuffer(std::span<const uint8_t> payload) const -> CFPtr<CMBlockBufferRef> {
    CMBlockBufferRef raw_block_buffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                                         nullptr,
                                                         payload.size(),
                                                         kCFAllocatorDefault,
                                                         nullptr,
                                                         0,
                                                         payload.size(),
                                                         0,
                                                         &raw_block_buffer);
    CFPtr<CMBlockBufferRef> block_buffer(raw_block_buffer);
    if (status != noErr || !block_buffer) {
      logStatus(OM_LEVEL_ERROR, "CMBlockBufferCreateWithMemoryBlock", status);
      return {};
    }

    status = CMBlockBufferReplaceDataBytes(payload.data(), block_buffer.get(), 0, payload.size());
    if (status != noErr) {
      logStatus(OM_LEVEL_ERROR, "CMBlockBufferReplaceDataBytes", status);
      return {};
    }

    return block_buffer;
  }

  void logStatus(OMLogLevel level, std::string_view operation, OSStatus status) const {
    log(OM_CATEGORY_DECODER,
        level,
        std::format("VideoToolbox {} failed: {}", operation, static_cast<int32_t>(status)));
  }

  OMCodecId codec_id_ = OM_CODEC_NONE;
  OMProfile profile_ = OM_PROFILE_NONE;
  CFPtr<CMVideoFormatDescriptionRef> format_description_;
  CFPtr<VTDecompressionSessionRef> session_;
  CallbackContext* callback_context_ = nullptr;
  VideoFormat output_format_ = {};
  OMColorSpace color_space_ = OM_COLOR_SPACE_BT709;
  OMTransferCharacteristic transfer_char_ = OM_TRANSFER_BT709;
  int nal_length_size_ = 4;
  int32_t timescale_ = kDefaultTimeScale;
  bool initialized_ = false;
};

auto createVideoToolboxDecoder(OMCodecId codec_id) -> std::unique_ptr<Decoder> {
  return std::make_unique<VideoToolboxDecoder>(codec_id);
}

} // namespace

const CodecDescriptor CODEC_VIDEOTOOLBOX_H263 = {
    .codec_id = OM_CODEC_H263,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_h263",
    .long_name = "H.263 (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {.media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}}},
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_H263); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_h264",
    .long_name = "H.264 / AVC (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE,
                     OM_PROFILE_H264_CONSTRAINED_BASELINE,
                     OM_PROFILE_H264_MAIN,
                     OM_PROFILE_H264_EXTENDED,
                     OM_PROFILE_H264_HIGH},
        .media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_H264); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_hevc",
    .long_name = "H.265 / HEVC (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H265_MAIN,
                     OM_PROFILE_H265_MAIN_10,
                     OM_PROFILE_H265_MAIN_STILL_PICTURE,
                     OM_PROFILE_H265_REXT},
        .media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_H265); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_MPEG2 = {
    .codec_id = OM_CODEC_MPEG2,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_mpeg2",
    .long_name = "MPEG-2 Video (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {.media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}}},
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_MPEG2); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_MPEG4 = {
    .codec_id = OM_CODEC_MPEG4,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_mpeg4",
    .long_name = "MPEG-4 Part 2 Video (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {.media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}}},
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_MPEG4); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_av1",
    .long_name = "AV1 (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_AV1_MAIN, OM_PROFILE_AV1_HIGH, OM_PROFILE_AV1_PROFESSIONAL},
        .media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_AV1); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_PRORES = {
    .codec_id = OM_CODEC_PRORES,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_prores",
    .long_name = "Apple ProRes (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_PRORES_PROXY,
                     OM_PROFILE_PRORES_LT,
                     OM_PROFILE_PRORES_STANDARD,
                     OM_PROFILE_PRORES_HQ,
                     OM_PROFILE_PRORES_4444,
                     OM_PROFILE_PRORES_XQ},
        .media = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_PRORES); },
};

} // namespace openmedia

#endif
